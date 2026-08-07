#pragma once
#include "QtLLM_base.h"
#include "UsageSample.h"
#include <QWidget>
#include <QComboBox>
#include <QPushButton>
#include <QVector>
#include <QString>

namespace QtLLM {

class UsageHistory;

// Aggregated bucket for chart rendering.
struct ChartBucket
{
    qint64 fromMs = 0;
    qint64 toMs   = 0;

    // Category mode values
    int uncachedInput  = 0;
    int cacheRead      = 0;
    int cacheWrite     = 0;
    int output         = 0;

    // Model mode values: model name -> total tokens
    QMap<QString, int> perModel;

    double costUsd = 0.0;
    int    totalTokens() const { return uncachedInput + cacheRead + cacheWrite + output; }
};

// Per-model summary tile data.
struct ModelSummary
{
    QString model;
    int     inputTokens              = 0;
    int     outputTokens             = 0;
    int     cacheReadInputTokens     = 0;
    int     cacheCreationInputTokens = 0;
    int     toolCalls                = 0;
    int     turns                    = 0;
    double  costUsd                  = 0.0;

    double cacheHitRate() const
    {
        int denom = cacheReadInputTokens + inputTokens + cacheCreationInputTokens;
        if (denom <= 0) return 0.0;
        return static_cast<double>(cacheReadInputTokens) / denom;
    }
};

class QT_LLM_API UsageStatsWidget : public QWidget
{
    Q_OBJECT
public:
    explicit UsageStatsWidget(QWidget* parent = nullptr);
    ~UsageStatsWidget() override;

    void setUsageHistory(UsageHistory* history);
    void setDarkMode(bool dark);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;

private slots:
    void onRangeChanged(int index);
    void onColourByChanged(int index);
    void onModelFilterChanged(int index);
    void onAppFilterChanged(int index);
    void onSampleAppended(const QtLLM::UsageSample& sample);
    void onCopyCsv();

private:
    enum class TimeRange { Session, Days7, Days30, All };
    enum class ColourBy  { Category, Model };

    void rebuildData();
    void rebuildModelFilter();
    void rebuildAppFilter();
    QVector<UsageSample> filteredSamples() const;
    QVector<ChartBucket> buildBuckets(const QVector<UsageSample>& samples) const;
    QVector<ModelSummary> buildSummaries(const QVector<UsageSample>& samples) const;

    void paintChart(QPainter& p, const QRect& area);
    void paintLegend(QPainter& p, const QRect& area);
    void paintSummaryTiles(QPainter& p, const QRect& area);
    void paintTooltip(QPainter& p);

    // Colour helpers
    QColor categoryColour(int index) const;  // 0=uncached, 1=cacheRead, 2=cacheWrite, 3=output
    QColor modelColour(int index) const;
    bool   isDark() const;

    static QString humanTokens(int tokens);
    static QString humanTime(qint64 ms);

    // Layout rects (computed in paintEvent)
    QRect m_chartRect;
    QRect m_legendRect;
    QRect m_tilesRect;

    // Data
    UsageHistory*         m_history = nullptr;
    UsageHistory*         m_ownedHistory = nullptr;  // self-loading fallback when no external history is set
    QVector<ChartBucket>  m_buckets;
    QVector<ModelSummary> m_summaries;
    QStringList           m_allModels;  // stable ordering for colour assignment

    // Controls
    QComboBox*   m_rangeCombo       = nullptr;
    QComboBox*   m_colourByCombo    = nullptr;
    QComboBox*   m_modelFilterCombo = nullptr;
    QComboBox*   m_appFilterCombo   = nullptr;
    QPushButton* m_copyCsvBtn       = nullptr;

    // State
    TimeRange m_range    = TimeRange::Session;
    ColourBy  m_colourBy = ColourBy::Category;
    QString   m_modelFilter;  // empty = all
    QString   m_appFilter;    // empty = all
    int       m_hoverBucket = -1;
    QPoint    m_hoverPos;
    bool      m_darkOverride  = false;
    bool      m_darkExplicit  = false;

    // Category labels (fixed order)
    static constexpr int kCategoryCount = 4;
    static const char* kCategoryLabels[kCategoryCount];
};

} // namespace QtLLM
