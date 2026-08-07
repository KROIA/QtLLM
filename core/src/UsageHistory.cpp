#include "UsageHistory.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QDateTime>
#include <QCoreApplication>
#include <QSet>

namespace QtLLM {

// Process-wide session anchor: every UsageHistory instance created in this
// process reports the same sessionStartMs(), so "This Session" in the stats
// widget covers the full process lifetime regardless of how many Client /
// UsageHistory objects are created or destroyed.
static qint64 s_processSessionStartMs = 0;

UsageHistory::UsageHistory(QObject* parent)
    : QObject(parent)
{
    if (s_processSessionStartMs == 0)
        s_processSessionStartMs = QDateTime::currentMSecsSinceEpoch();
    m_sessionStartMs = s_processSessionStartMs;
    loadFromFile();
}

UsageHistory::~UsageHistory() = default;

void UsageHistory::setFilePath(const QString& path)
{
    m_filePath = path;
    m_samples.clear();
    loadFromFile();
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

void UsageHistory::loadFromFile()
{
    QString path = defaultFilePath();
    QFile file(path);
    if (!file.exists())
        return;
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return;

    while (!file.atEnd()) {
        QByteArray line = file.readLine().trimmed();
        if (line.isEmpty())
            continue;
        QJsonDocument doc = QJsonDocument::fromJson(line);
        if (doc.isNull())
            continue;
        m_samples.append(sampleFromJson(doc.object()));
    }
}

void UsageHistory::append(const UsageSample& sample)
{
    UsageSample s = sample;
    if (s.app.isEmpty()) {
        QString name = QCoreApplication::applicationName();
        s.app = name.isEmpty() ? QStringLiteral("unknown") : name;
    }
    m_samples.append(s);

    QString path = defaultFilePath();
    QDir().mkpath(QFileInfo(path).absolutePath());

    QFile file(path);
    if (file.open(QIODevice::Append | QIODevice::Text)) {
        QByteArray line = QJsonDocument(sampleToJson(s)).toJson(QJsonDocument::Compact);
        line.append('\n');
        file.write(line);
        file.flush();
    }

    emit sampleAppended(s);
}

const QVector<UsageSample>& UsageHistory::samples() const
{
    return m_samples;
}

QVector<UsageSample> UsageHistory::samplesInRange(qint64 fromMs, qint64 toMs) const
{
    QVector<UsageSample> result;
    for (const UsageSample& s : m_samples) {
        if (s.timestampMsEpoch >= fromMs && s.timestampMsEpoch <= toMs)
            result.append(s);
    }
    return result;
}

QStringList UsageHistory::distinctModels() const
{
    QSet<QString> seen;
    QStringList result;
    for (const UsageSample& s : m_samples) {
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
    for (const UsageSample& s : m_samples) {
        if (!seen.contains(s.app)) {
            seen.insert(s.app);
            result.append(s.app);
        }
    }
    return result;
}

qint64 UsageHistory::earliestTimestamp() const
{
    if (m_samples.isEmpty())
        return 0;
    return m_samples.first().timestampMsEpoch;
}

qint64 UsageHistory::sessionStartMs() const
{
    return m_sessionStartMs;
}

void UsageHistory::reload()
{
    m_samples.clear();
    loadFromFile();
}

void UsageHistory::clear()
{
    m_samples.clear();
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
