#include "UsageHistory.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QDateTime>
#include <QSet>

namespace QtLLM {

UsageHistory::UsageHistory(QObject* parent)
    : QObject(parent)
    , m_sessionStartMs(QDateTime::currentMSecsSinceEpoch())
{
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
    QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
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
    m_samples.append(sample);

    QString path = defaultFilePath();
    QDir().mkpath(QFileInfo(path).absolutePath());

    QFile file(path);
    if (file.open(QIODevice::Append | QIODevice::Text)) {
        QByteArray line = QJsonDocument(sampleToJson(sample)).toJson(QJsonDocument::Compact);
        line.append('\n');
        file.write(line);
        file.flush();
    }

    emit sampleAppended(sample);
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
