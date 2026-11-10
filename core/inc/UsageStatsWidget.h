#pragma once
#include "QtLLM_base.h"
#include "UsageSample.h"
#include <QWidget>
#include <QComboBox>
#include <QPushButton>
#include <QVector>
#include <QString>
#include <QScrollBar>
#include <QPointer>
#include <QMap>

namespace QtLLM {

class UsageHistory;

// Aggregated bucket for chart rendering.
struct ChartBucket
{
    qint64 fromMs = 0;
    qint64 toMs   = 0;

    // Category mode values
    // qint64: summed over long ranges, cache-read tokens alone exceed 2^31
    qint64 uncachedInput  = 0;
    qint64 cacheRead      = 0;
    qint64 cacheWrite     = 0;
    qint64 output         = 0;

    // Model mode values: model name -> total tokens
    QMap<QString, qint64> perModel;

    double costUsd = 0.0;
    qint64 totalTokens() const { return uncachedInput + cacheRead + cacheWrite + output; }
};

// Per-model summary tile data.
struct ModelSummary
{
    QString model;
    qint64  inputTokens              = 0;
    qint64  outputTokens             = 0;
    qint64  cacheReadInputTokens     = 0;
    qint64  cacheCreationInputTokens = 0;
    qint64  toolCalls                = 0;
    qint64  turns                    = 0;
    double  costUsd                  = 0.0;

    double cacheHitRate() const
    {
        qint64 denom = cacheReadInputTokens + inputTokens + cacheCreationInputTokens;
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
    void wheelEvent(QWheelEvent* event) override;

private slots:
    void onRangeChanged(int index);
    void onColourByChanged(int index);
    void onModelFilterChanged(int index);
    void onAppFilterChanged(int index);
    void onSampleAppended(const QtLLM::UsageSample& sample);
    void onCopyCsv();
    void onChartTypeChanged(int index);
    void onScrollBarChanged(int value);

private:
    enum class TimeRange { Session, Hour1, Hour6, Days7, Days30, All };
    enum class ColourBy  { Category, Model };
    enum class ChartType { Bars, Lines };

    void rebuildData();
    void rebuildModelFilter();
    void rebuildAppFilter();
    QVector<UsageSample> filteredSamples() const;
    QVector<ChartBucket> buildBuckets(const QVector<UsageSample>& samples) const;
    QVector<ModelSummary> buildSummaries(const QVector<UsageSample>& samples) const;

    void paintChart(QPainter& p, const QRect& area);
    void paintLineChart(QPainter& p, const QRect& area);
    void paintCostChart(QPainter& p, const QRect& area);
    void paintLegend(QPainter& p, const QRect& area);
    void paintSummaryTiles(QPainter& p, const QRect& area);
    void paintTooltip(QPainter& p);

    void updateScrollBar();

    // Colour helpers
    QColor categoryColour(int index) const;  // 0=uncached, 1=cacheRead, 2=cacheWrite, 3=output
    QColor modelColour(int index) const;
    bool   isDark() const;

    static QString humanTokens(qint64 tokens);
    static QString humanTime(qint64 ms);

    // Layout rects (computed in paintEvent)
    QRect m_chartRect;
    QRect m_costChartRect;
    QRect m_legendRect;
    QRect m_tilesRect;

    // Data
    QPointer<UsageHistory> m_history;  // owned by a Client that may die first
    UsageHistory*         m_ownedHistory = nullptr;  // self-loading fallback when no external history is set
    QVector<ChartBucket>  m_buckets;
    QVector<ModelSummary> m_summaries;
    QStringList           m_allModels;  // stable ordering for colour assignment

    // Controls
    QComboBox*   m_rangeCombo       = nullptr;
    QComboBox*   m_chartTypeCombo   = nullptr;
    QComboBox*   m_colourByCombo    = nullptr;
    QComboBox*   m_modelFilterCombo = nullptr;
    QComboBox*   m_appFilterCombo   = nullptr;
    QPushButton* m_copyCsvBtn       = nullptr;
    QScrollBar*  m_scrollBar        = nullptr;

    // State
    TimeRange m_range     = TimeRange::Session;
    ChartType m_chartType = ChartType::Bars;
    ColourBy  m_colourBy  = ColourBy::Category;
    QString   m_modelFilter;  // empty = all
    QString   m_appFilter;    // empty = all
    int       m_hoverBucket = -1;
    QPoint    m_hoverPos;
    bool      m_darkOverride  = false;
    bool      m_darkExplicit  = false;

    // Zoom/pan: visible time window (0 = show all / no zoom)
    qint64 m_viewStartMs = 0;
    qint64 m_viewEndMs   = 0;

    // Full data-range buckets (before view clipping)
    QVector<ChartBucket> m_allBuckets;

    // Category labels (fixed order)
    static constexpr int kCategoryCount = 4;
    static const char* kCategoryLabels[kCategoryCount];
};

} // namespace QtLLM
