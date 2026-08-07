#pragma once
#include "QtLLM_base.h"
#include "UsageSample.h"
#include <QObject>
#include <QVector>
#include <QString>
#include <QStringList>

namespace QtLLM {

// Persistent per-turn usage history stored as JSONL.
// Owned by Client; provides data for UsageStatsWidget.
class QT_LLM_API UsageHistory : public QObject
{
    Q_OBJECT
public:
    explicit UsageHistory(QObject* parent = nullptr);
    ~UsageHistory() override;

    // Override the default storage path (must be called before first append/load).
    void setFilePath(const QString& path);

    // Append a sample to memory and persist to JSONL file.
    void append(const UsageSample& sample);

    // All recorded samples.
    const QVector<UsageSample>& samples() const;

    // Samples within [fromMs, toMs] inclusive.
    QVector<UsageSample> samplesInRange(qint64 fromMs, qint64 toMs) const;

    // Distinct model identifiers across all samples.
    QStringList distinctModels() const;

    // Epoch of the earliest sample, or 0.
    qint64 earliestTimestamp() const;

    // Session start epoch (set at construction time).
    qint64 sessionStartMs() const;

    // TODO: rotation/cap is a future concern.
    void clear();

signals:
    void sampleAppended(const QtLLM::UsageSample& sample);

private:
    void loadFromFile();
    static QJsonObject sampleToJson(const UsageSample& s);
    static UsageSample sampleFromJson(const QJsonObject& obj);
    QString defaultFilePath() const;

    QVector<UsageSample> m_samples;
    QString              m_filePath;
    qint64               m_sessionStartMs = 0;
};

} // namespace QtLLM
