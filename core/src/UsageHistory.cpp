#include "UsageHistory.h"

#include <QDir>
#include <QFile>
#include <QSaveFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QDateTime>
#include <QCoreApplication>
#include <QSet>
#include <QHash>
#include <QPointer>

namespace QtLLM {

// Process-wide session anchor: every UsageHistory instance created in this
// process reports the same sessionStartMs(), so "This Session" in the stats
// widget covers the full process lifetime regardless of how many Client /
// UsageHistory objects are created or destroyed.
static qint64 s_processSessionStartMs = 0;

// One sample store per file path, shared by every instance using that path,
// so the file is parsed once per process and an append through one instance
// (e.g. a background Agent's Client) is visible to all others (e.g. the
// statistics tab bound to the chat Client's history).
struct UsageHistoryStore
{
    QVector<UsageSample>  samples;
    bool                  loaded = false;
    QList<UsageHistory*>  instances;
};

// ponytail: stores live for the process lifetime (normally exactly one path);
// freeing them when the last instance goes would force a re-parse on the next.
static QHash<QString, UsageHistoryStore*>& sharedStores()
{
    static QHash<QString, UsageHistoryStore*> stores;
    return stores;
}

UsageHistory::UsageHistory(QObject* parent)
    : QObject(parent)
{
    if (s_processSessionStartMs == 0)
        s_processSessionStartMs = QDateTime::currentMSecsSinceEpoch();
    m_sessionStartMs = s_processSessionStartMs;
    attachStore();
}

UsageHistory::~UsageHistory()
{
    detachStore();
}

void UsageHistory::attachStore()
{
    UsageHistoryStore*& store = sharedStores()[defaultFilePath()];
    if (!store)
        store = new UsageHistoryStore;
    store->instances.append(this);
    m_store = store;
}

void UsageHistory::detachStore()
{
    if (m_store)
        m_store->instances.removeAll(this);
    m_store = nullptr;
}

UsageHistoryStore& UsageHistory::store() const
{
    // Loaded lazily so setFilePath() right after construction does not parse
    // the default file for nothing.
    if (!m_store->loaded) {
        m_store->loaded = true;
        loadFromFile();
    }
    return *m_store;
}

void UsageHistory::setFilePath(const QString& path)
{
    detachStore();
    m_filePath = path;
    attachStore();
}

QString UsageHistory::defaultFilePath() const
{
    if (!m_filePath.isEmpty())
        return m_filePath;
    // Intentionally global (GenericDataLocation has no org/app suffix) so all
    // QtLLM-consuming apps for this user share one usage history file.
    // Pre-existing per-app files under AppDataLocation are not migrated (future).
    QString base = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    return base + "/QtLLM/usage_history.jsonl";
}

void UsageHistory::loadFromFile() const
{
    QVector<UsageSample>& samples = m_store->samples;
    samples.clear();

    QString path = defaultFilePath();
    QFile file(path);
    if (!file.exists())
        return;
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return;

    const qint64 cutoffMs = QDateTime::currentDateTime().addYears(-2).toMSecsSinceEpoch();
    bool dropped = false;
    while (!file.atEnd()) {
        QByteArray line = file.readLine().trimmed();
        if (line.isEmpty())
            continue;
        QJsonDocument doc = QJsonDocument::fromJson(line);
        if (!doc.isObject())
            continue;
        const QJsonObject obj = doc.object();
        // Valid JSON that is not a sample would otherwise become a ts=0 entry
        if (!obj.contains("ts") || !obj.contains("model"))
            continue;
        UsageSample s = sampleFromJson(obj);
        if (s.timestampMsEpoch < cutoffMs) {
            dropped = true;
            continue;
        }
        samples.append(s);
    }
    file.close();

    // Rotation: rewrite only when something aged out, so a normal start never
    // touches a file other processes may be appending to.
    if (dropped) {
        QSaveFile out(path);
        if (out.open(QIODevice::WriteOnly | QIODevice::Text)) {
            for (const UsageSample& s : samples) {
                QByteArray l = QJsonDocument(sampleToJson(s)).toJson(QJsonDocument::Compact);
                l.append('\n');
                out.write(l);
            }
            out.commit();
        }
    }
}

void UsageHistory::append(const UsageSample& sample)
{
    UsageSample s = sample;
    if (s.app.isEmpty()) {
        QString name = QCoreApplication::applicationName();
        s.app = name.isEmpty() ? QStringLiteral("unknown") : name;
    }
    store().samples.append(s);

    QString path = defaultFilePath();
    QDir().mkpath(QFileInfo(path).absolutePath());

    QFile file(path);
    if (file.open(QIODevice::Append | QIODevice::Text)) {
        QByteArray line = QJsonDocument(sampleToJson(s)).toJson(QJsonDocument::Compact);
        line.append('\n');
        file.write(line);
        file.flush();
    }

    // Every view of this store sees the new sample; QPointer because a slot
    // may delete another instance mid-loop.
    QList<QPointer<UsageHistory>> targets;
    for (UsageHistory* h : m_store->instances)
        targets.append(h);
    for (const QPointer<UsageHistory>& h : targets) {
        if (h)
            emit h->sampleAppended(s);
    }
}

const QVector<UsageSample>& UsageHistory::samples() const
{
    return store().samples;
}

QVector<UsageSample> UsageHistory::samplesInRange(qint64 fromMs, qint64 toMs) const
{
    QVector<UsageSample> result;
    for (const UsageSample& s : store().samples) {
        if (s.timestampMsEpoch >= fromMs && s.timestampMsEpoch <= toMs)
            result.append(s);
    }
    return result;
}

QStringList UsageHistory::distinctModels() const
{
    QSet<QString> seen;
    QStringList result;
    for (const UsageSample& s : store().samples) {
        if (!seen.contains(s.model)) {
            seen.insert(s.model);
            result.append(s.model);
        }
    }
    return result;
}

QStringList UsageHistory::distinctApps() const
{
    QSet<QString> seen;
    QStringList result;
    for (const UsageSample& s : store().samples) {
        if (!seen.contains(s.app)) {
            seen.insert(s.app);
            result.append(s.app);
        }
    }
    return result;
}

qint64 UsageHistory::earliestTimestamp() const
{
    const QVector<UsageSample>& samples = store().samples;
    if (samples.isEmpty())
        return 0;
    return samples.first().timestampMsEpoch;
}

qint64 UsageHistory::sessionStartMs() const
{
    return m_sessionStartMs;
}

void UsageHistory::reload()
{
    m_store->loaded = true;
    loadFromFile();
}

void UsageHistory::clear()
{
    m_store->samples.clear();
    m_store->loaded = true;
    QString path = defaultFilePath();
    QFile::remove(path);
}

QJsonObject UsageHistory::sampleToJson(const UsageSample& s)
{
    QJsonObject obj;
    obj["ts"]           = s.timestampMsEpoch;
    obj["model"]        = s.model;
    obj["provider"]     = s.provider;
    obj["app"]          = s.app;
    obj["in"]           = s.inputTokens;
    obj["out"]          = s.outputTokens;
    obj["cache_read"]   = s.cacheReadInputTokens;
    obj["cache_create"] = s.cacheCreationInputTokens;
    obj["tools"]        = s.toolCalls;
    obj["ms"]           = s.durationMs;
    obj["cost"]         = s.costUsd;
    return obj;
}

UsageSample UsageHistory::sampleFromJson(const QJsonObject& obj)
{
    UsageSample s;
    s.timestampMsEpoch         = static_cast<qint64>(obj["ts"].toDouble());
    s.model                    = obj["model"].toString();
    s.provider                 = obj["provider"].toString();
    s.app                      = obj["app"].toString();
    s.inputTokens              = obj["in"].toInt();
    s.outputTokens             = obj["out"].toInt();
    s.cacheReadInputTokens     = obj["cache_read"].toInt();
    s.cacheCreationInputTokens = obj["cache_create"].toInt();
    s.toolCalls                = obj["tools"].toInt();
    s.durationMs               = static_cast<qint64>(obj["ms"].toDouble());
    s.costUsd                  = obj["cost"].toDouble();
    return s;
}

} // namespace QtLLM
