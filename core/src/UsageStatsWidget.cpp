#include "UsageStatsWidget.h"
#include "UsageHistory.h"

#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QShowEvent>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QLabel>
#include <QDateTime>
#include <QClipboard>
#include <QApplication>
#include <QScrollBar>
#include <QtMath>

namespace QtLLM {

const char* UsageStatsWidget::kCategoryLabels[kCategoryCount] = {
    "Uncached Input", "Cache Read", "Cache Write", "Output"
};

// ---------------------------------------------------------------------------
// Category colours — visually distinct, colourblind-reasonable
// Index: 0=uncached input, 1=cache read, 2=cache write, 3=output
// ---------------------------------------------------------------------------
static const QColor kCatLight[] = {
    QColor(66, 133, 244),   // blue
    QColor(52, 168, 83),    // green
    QColor(251, 188, 4),    // amber
    QColor(154, 80, 194)    // purple
};
static const QColor kCatDark[] = {
    QColor(100, 160, 255),
    QColor(80, 200, 120),
    QColor(255, 210, 60),
    QColor(180, 120, 220)
};

// Model colours — up to 8 distinct, 9th+ folds into "Other"
static const QColor kModelLight[] = {
    QColor(66, 133, 244), QColor(234, 67, 53), QColor(52, 168, 83),
    QColor(251, 188, 4),  QColor(154, 80, 194), QColor(0, 172, 193),
    QColor(255, 112, 67), QColor(124, 179, 66)
};
static const QColor kModelDark[] = {
    QColor(100, 160, 255), QColor(255, 100, 90), QColor(80, 200, 120),
    QColor(255, 210, 60),  QColor(180, 120, 220), QColor(60, 210, 230),
    QColor(255, 140, 100), QColor(160, 210, 100)
};
static const int kMaxModelColours = 8;

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

UsageStatsWidget::UsageStatsWidget(QWidget* parent)
    : QWidget(parent)
{
    setMouseTracking(true);
    setMinimumSize(500, 400);

    // Controls row at the top
    auto* controlsLayout = new QHBoxLayout();
    controlsLayout->setContentsMargins(8, 8, 8, 0);

    m_rangeCombo = new QComboBox(this);
    m_rangeCombo->addItems({"This Session", "Last Hour", "Last 6 Hours",
                            "7 Days", "30 Days", "All"});
    controlsLayout->addWidget(new QLabel("Range:", this));
    controlsLayout->addWidget(m_rangeCombo);

    m_chartTypeCombo = new QComboBox(this);
    m_chartTypeCombo->addItems({"Stacked bars", "Lines"});
    controlsLayout->addWidget(new QLabel("Chart:", this));
    controlsLayout->addWidget(m_chartTypeCombo);

    m_colourByCombo = new QComboBox(this);
    m_colourByCombo->addItems({"Category", "Model"});
    controlsLayout->addWidget(new QLabel("Colour by:", this));
    controlsLayout->addWidget(m_colourByCombo);

    m_modelFilterCombo = new QComboBox(this);
    m_modelFilterCombo->addItem("All");
    controlsLayout->addWidget(new QLabel("Model:", this));
    controlsLayout->addWidget(m_modelFilterCombo);

    m_appFilterCombo = new QComboBox(this);
    m_appFilterCombo->addItem("All");
    controlsLayout->addWidget(new QLabel("App:", this));
    controlsLayout->addWidget(m_appFilterCombo);

    controlsLayout->addStretch();

    m_copyCsvBtn = new QPushButton("Copy CSV", this);
    controlsLayout->addWidget(m_copyCsvBtn);

    // Horizontal scroll bar for panning when zoomed in
    m_scrollBar = new QScrollBar(Qt::Horizontal, this);
    m_scrollBar->setVisible(false);

    // Place controls; painting happens below them
    auto* topLayout = new QVBoxLayout(this);
    topLayout->setContentsMargins(0, 0, 0, 0);
    topLayout->addLayout(controlsLayout);
    topLayout->addStretch();
    topLayout->addWidget(m_scrollBar);

    connect(m_rangeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &UsageStatsWidget::onRangeChanged);
    connect(m_colourByCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &UsageStatsWidget::onColourByChanged);
    connect(m_modelFilterCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &UsageStatsWidget::onModelFilterChanged);
    connect(m_appFilterCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &UsageStatsWidget::onAppFilterChanged);
    connect(m_copyCsvBtn, &QPushButton::clicked,
            this, &UsageStatsWidget::onCopyCsv);
    connect(m_chartTypeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &UsageStatsWidget::onChartTypeChanged);
    connect(m_scrollBar, &QScrollBar::valueChanged,
            this, &UsageStatsWidget::onScrollBarChanged);

    // Fallback history: a self-loading, read-only view of the persisted JSONL so
    // the panel shows data even if the consumer never calls setUsageHistory().
    // It reloads on show (see showEvent) to pick up samples written this run.
    m_ownedHistory = new UsageHistory(this);
    setUsageHistory(m_ownedHistory);
}

UsageStatsWidget::~UsageStatsWidget() = default;

void UsageStatsWidget::setUsageHistory(UsageHistory* history)
{
    if (m_history)
        disconnect(m_history, nullptr, this, nullptr);

    // If an external history is supplied, drop the owned fallback.
    if (m_ownedHistory && history != m_ownedHistory) {
        delete m_ownedHistory;
        m_ownedHistory = nullptr;
    }

    m_history = history;
    if (m_history) {
        connect(m_history, &UsageHistory::sampleAppended,
                this, &UsageStatsWidget::onSampleAppended);
    }
    rebuildModelFilter();
    rebuildAppFilter();
    rebuildData();
    update();
}

void UsageStatsWidget::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    // When running on the self-loading fallback, re-read the file each time the
    // panel is shown so freshly recorded turns appear without an app restart.
    if (m_history && m_history == m_ownedHistory) {
        m_ownedHistory->reload();
        rebuildModelFilter();
        rebuildAppFilter();
        rebuildData();
    }
    update();
}

void UsageStatsWidget::setDarkMode(bool dark)
{
    m_darkOverride = dark;
    m_darkExplicit = true;
    update();
}

// ---------------------------------------------------------------------------
// Slots
// ---------------------------------------------------------------------------

void UsageStatsWidget::onRangeChanged(int index)
{
    m_range = static_cast<TimeRange>(index);
    // Reset zoom/pan on range change
    m_viewStartMs = 0;
    m_viewEndMs   = 0;
    rebuildData();
    updateScrollBar();
    update();
}

void UsageStatsWidget::onColourByChanged(int index)
{
    m_colourBy = static_cast<ColourBy>(index);
    update();
}

void UsageStatsWidget::onModelFilterChanged(int index)
{
    m_modelFilter = (index <= 0) ? QString() : m_modelFilterCombo->itemText(index);
    rebuildData();
    update();
}

void UsageStatsWidget::onAppFilterChanged(int index)
{
    m_appFilter = (index <= 0) ? QString() : m_appFilterCombo->itemText(index);
    rebuildData();
    update();
}

void UsageStatsWidget::onSampleAppended(const QtLLM::UsageSample& /*sample*/)
{
    rebuildModelFilter();
    rebuildAppFilter();
    rebuildData();
    update();
}

void UsageStatsWidget::onCopyCsv()
{
    if (!m_history) return;
    auto samples = filteredSamples();
    QString csv;
    csv += "timestamp,model,provider,input_tokens,output_tokens,"
           "cache_read,cache_write,tool_calls,duration_ms,cost_usd\n";
    for (const UsageSample& s : samples) {
        csv += QDateTime::fromMSecsSinceEpoch(s.timestampMsEpoch).toString(Qt::ISODate)
            + "," + s.model
            + "," + s.provider
            + "," + QString::number(s.inputTokens)
            + "," + QString::number(s.outputTokens)
            + "," + QString::number(s.cacheReadInputTokens)
            + "," + QString::number(s.cacheCreationInputTokens)
            + "," + QString::number(s.toolCalls)
            + "," + QString::number(s.durationMs)
            + "," + QString::number(s.costUsd, 'f', 6)
            + "\n";
    }
    QApplication::clipboard()->setText(csv);
}

void UsageStatsWidget::onChartTypeChanged(int index)
{
    m_chartType = static_cast<ChartType>(index);
    update();
}

void UsageStatsWidget::onScrollBarChanged(int value)
{
    if (m_allBuckets.isEmpty() || m_viewStartMs == 0)
        return;

    qint64 fullStart = m_allBuckets.first().fromMs;
    qint64 fullEnd   = m_allBuckets.last().toMs;
    qint64 viewSpan  = m_viewEndMs - m_viewStartMs;

    qint64 fullSpan  = fullEnd - fullStart;
    if (fullSpan <= 0) return;

    // Map scrollbar value (0..1000) to offset within the full range
    double frac = value / 1000.0;
    qint64 newStart = fullStart + static_cast<qint64>(frac * (fullSpan - viewSpan));
    qint64 newEnd   = newStart + viewSpan;
    if (newEnd > fullEnd) {
        newEnd   = fullEnd;
        newStart = newEnd - viewSpan;
    }
    if (newStart < fullStart)
        newStart = fullStart;

    m_viewStartMs = newStart;
    m_viewEndMs   = newEnd;
    rebuildData();
    update();
}

// ---------------------------------------------------------------------------
// Data rebuilding
// ---------------------------------------------------------------------------

QVector<UsageSample> UsageStatsWidget::filteredSamples() const
{
    if (!m_history) return {};

    qint64 now = QDateTime::currentMSecsSinceEpoch();
    qint64 fromMs = 0;
    switch (m_range) {
    case TimeRange::Session: fromMs = m_history->sessionStartMs(); break;
    case TimeRange::Hour1:   fromMs = now - qint64(3600)  * 1000; break;
    case TimeRange::Hour6:   fromMs = now - qint64(21600) * 1000; break;
    case TimeRange::Days7:   fromMs = now - qint64(7)  * 86400 * 1000; break;
    case TimeRange::Days30:  fromMs = now - qint64(30) * 86400 * 1000; break;
    case TimeRange::All:     fromMs = 0; break;
    }

    QVector<UsageSample> result;
    for (const UsageSample& s : m_history->samples()) {
        if (s.timestampMsEpoch < fromMs)
            continue;
        if (!m_modelFilter.isEmpty() && s.model != m_modelFilter)
            continue;
        if (!m_appFilter.isEmpty() && s.app != m_appFilter)
            continue;
        result.append(s);
    }
    return result;
}

void UsageStatsWidget::rebuildData()
{
    auto samples = filteredSamples();
    m_allBuckets = buildBuckets(samples);
    m_summaries  = buildSummaries(samples);

    // Build stable model list
    QSet<QString> seen;
    m_allModels.clear();
    for (const UsageSample& s : samples) {
        if (!seen.contains(s.model)) {
            seen.insert(s.model);
            m_allModels.append(s.model);
        }
    }

    // Apply view window (zoom clipping)
    if (m_viewStartMs > 0 && m_viewEndMs > m_viewStartMs && !m_allBuckets.isEmpty()) {
        m_buckets.clear();
        for (const ChartBucket& b : m_allBuckets) {
            if (b.toMs >= m_viewStartMs && b.fromMs <= m_viewEndMs)
                m_buckets.append(b);
        }
    } else {
        m_buckets = m_allBuckets;
    }
}

void UsageStatsWidget::rebuildModelFilter()
{
    if (!m_history) return;
    QString current = m_modelFilterCombo->currentText();
    m_modelFilterCombo->blockSignals(true);
    m_modelFilterCombo->clear();
    m_modelFilterCombo->addItem("All");
    for (const QString& m : m_history->distinctModels())
        m_modelFilterCombo->addItem(m);
    int idx = m_modelFilterCombo->findText(current);
    m_modelFilterCombo->setCurrentIndex(idx >= 0 ? idx : 0);
    m_modelFilterCombo->blockSignals(false);
}

void UsageStatsWidget::rebuildAppFilter()
{
    if (!m_history) return;
    QString current = m_appFilterCombo->currentText();
    m_appFilterCombo->blockSignals(true);
    m_appFilterCombo->clear();
    m_appFilterCombo->addItem("All");
    for (const QString& a : m_history->distinctApps())
        m_appFilterCombo->addItem(a);
    int idx = m_appFilterCombo->findText(current);
    m_appFilterCombo->setCurrentIndex(idx >= 0 ? idx : 0);
    m_appFilterCombo->blockSignals(false);
}

QVector<ChartBucket> UsageStatsWidget::buildBuckets(const QVector<UsageSample>& samples) const
{
    if (samples.isEmpty()) return {};

    // Determine bucket width
    qint64 now = QDateTime::currentMSecsSinceEpoch();
    qint64 spanMs = 0;
    switch (m_range) {
    case TimeRange::Session: {
        qint64 sessionStart = m_history ? m_history->sessionStartMs() : 0;
        spanMs = now - sessionStart;
        break;
    }
    case TimeRange::Hour1:  spanMs = qint64(3600)  * 1000; break;
    case TimeRange::Hour6:  spanMs = qint64(21600) * 1000; break;
    case TimeRange::Days7:  spanMs = qint64(7)  * 86400 * 1000; break;
    case TimeRange::Days30: spanMs = qint64(30) * 86400 * 1000; break;
    case TimeRange::All: {
        if (!samples.isEmpty())
            spanMs = now - samples.first().timestampMsEpoch;
        break;
    }
    }
    if (spanMs <= 0) spanMs = 1;

    // For "This session" with few samples, use per-turn buckets
    if (m_range == TimeRange::Session && samples.size() <= 180) {
        QVector<ChartBucket> buckets;
        for (const UsageSample& s : samples) {
            ChartBucket b;
            b.fromMs        = s.timestampMsEpoch;
            b.toMs          = s.timestampMsEpoch;
            b.uncachedInput = s.inputTokens;
            b.cacheRead     = s.cacheReadInputTokens;
            b.cacheWrite    = s.cacheCreationInputTokens;
            b.output        = s.outputTokens;
            b.costUsd       = s.costUsd;
            b.perModel[s.model] += s.totalTokens();
            buckets.append(b);
        }
        return buckets;
    }

    // Adaptive bucket widths
    qint64 bucketMs;
    if (spanMs <= qint64(2) * 86400 * 1000)
        bucketMs = 60 * 1000;          // per minute
    else if (spanMs <= qint64(8) * 86400 * 1000)
        bucketMs = 3600 * 1000;        // per hour
    else
        bucketMs = qint64(86400) * 1000; // per day

    // Widen if too many buckets
    while (spanMs / bucketMs > 180)
        bucketMs *= 2;

    qint64 startMs = now - spanMs;
    int numBuckets = static_cast<int>((spanMs + bucketMs - 1) / bucketMs);
    if (numBuckets <= 0) numBuckets = 1;

    QVector<ChartBucket> buckets(numBuckets);
    for (int i = 0; i < numBuckets; ++i) {
        buckets[i].fromMs = startMs + i * bucketMs;
        buckets[i].toMs   = startMs + (i + 1) * bucketMs - 1;
    }

    for (const UsageSample& s : samples) {
        int idx = static_cast<int>((s.timestampMsEpoch - startMs) / bucketMs);
        if (idx < 0) idx = 0;
        if (idx >= numBuckets) idx = numBuckets - 1;

        buckets[idx].uncachedInput += s.inputTokens;
        buckets[idx].cacheRead     += s.cacheReadInputTokens;
        buckets[idx].cacheWrite    += s.cacheCreationInputTokens;
        buckets[idx].output        += s.outputTokens;
        buckets[idx].costUsd       += s.costUsd;
        buckets[idx].perModel[s.model] += s.totalTokens();
    }

    // Remove trailing empty buckets
    while (!buckets.isEmpty() && buckets.last().totalTokens() == 0
           && buckets.last().perModel.isEmpty())
        buckets.removeLast();

    return buckets;
}

QVector<ModelSummary> UsageStatsWidget::buildSummaries(const QVector<UsageSample>& samples) const
{
    QMap<QString, ModelSummary> map;
    for (const UsageSample& s : samples) {
        ModelSummary& ms = map[s.model];
        ms.model = s.model;
        ms.inputTokens              += s.inputTokens;
        ms.outputTokens             += s.outputTokens;
        ms.cacheReadInputTokens     += s.cacheReadInputTokens;
        ms.cacheCreationInputTokens += s.cacheCreationInputTokens;
        ms.toolCalls                += s.toolCalls;
        ms.turns                    += 1;
        ms.costUsd                  += s.costUsd;
    }
    return map.values().toVector();
}

// ---------------------------------------------------------------------------
// Painting
// ---------------------------------------------------------------------------

bool UsageStatsWidget::isDark() const
{
    if (m_darkExplicit)
        return m_darkOverride;
    return palette().window().color().lightnessF() < 0.5;
}

QColor UsageStatsWidget::categoryColour(int index) const
{
    if (index < 0 || index >= kCategoryCount) return Qt::gray;
    return isDark() ? kCatDark[index] : kCatLight[index];
}

QColor UsageStatsWidget::modelColour(int index) const
{
    if (index < 0) return Qt::gray;
    if (index >= kMaxModelColours) return QColor(158, 158, 158); // "Other" grey
    return isDark() ? kModelDark[index] : kModelLight[index];
}

QString UsageStatsWidget::humanTokens(int tokens)
{
    if (tokens < 1000)
        return QString::number(tokens);
    if (tokens < 1000000)
        return QString::number(tokens / 1000.0, 'f', 1) + "k";
    return QString::number(tokens / 1000000.0, 'f', 2) + "M";
}

QString UsageStatsWidget::humanTime(qint64 ms)
{
    QDateTime dt = QDateTime::fromMSecsSinceEpoch(ms);
    return dt.toString("MM-dd hh:mm");
}

void UsageStatsWidget::paintEvent(QPaintEvent* /*event*/)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    QColor bgColor    = palette().window().color();
    QColor textColor  = palette().windowText().color();
    p.fillRect(rect(), bgColor);
    p.setPen(textColor);

    int controlsHeight = 40;
    int scrollBarHeight = m_scrollBar->isVisible() ? m_scrollBar->height() + 4 : 0;
    int margin = 12;
    int legendHeight = 30;
    int costChartHeight = 100;  // fixed height for the cost line chart
    int tilesHeight  = 0;

    // Compute tiles height: one row per model, ~50px each
    if (!m_summaries.isEmpty())
        tilesHeight = qMin(static_cast<int>(m_summaries.size()), 4) * 55 + 10;

    m_legendRect = QRect(margin, controlsHeight + margin,
                         width() - margin * 2, legendHeight);
    m_tilesRect  = QRect(margin, height() - tilesHeight - margin - scrollBarHeight,
                         width() - margin * 2, tilesHeight);

    // Cost chart sits above the tiles
    m_costChartRect = QRect(margin, m_tilesRect.top() - costChartHeight - 8,
                            width() - margin * 2, costChartHeight);

    // Token chart fills the remaining space
    m_chartRect  = QRect(margin, m_legendRect.bottom() + 8,
                         width() - margin * 2,
                         m_costChartRect.top() - m_legendRect.bottom() - 16);

    if (m_chartRect.height() < 60) {
        // Not enough room; drop tiles and cost chart
        int availH = height() - controlsHeight - margin * 2 - scrollBarHeight;
        m_chartRect.setHeight(availH - legendHeight - 20);
        m_tilesRect = QRect();
        m_costChartRect = QRect();
    }

    paintLegend(p, m_legendRect);
    if (m_chartType == ChartType::Lines)
        paintLineChart(p, m_chartRect);
    else
        paintChart(p, m_chartRect);

    if (!m_costChartRect.isNull())
        paintCostChart(p, m_costChartRect);
    if (!m_tilesRect.isNull())
        paintSummaryTiles(p, m_tilesRect);
    if (m_hoverBucket >= 0)
        paintTooltip(p);
}

void UsageStatsWidget::paintLegend(QPainter& p, const QRect& area)
{
    int x = area.x();
    int y = area.y() + 4;
    int chipSize = 12;
    int spacing  = 8;

    QFont f = font();
    f.setPointSize(8);
    p.setFont(f);

    if (m_colourBy == ColourBy::Category) {
        for (int i = 0; i < kCategoryCount; ++i) {
            p.fillRect(x, y, chipSize, chipSize, categoryColour(i));
            p.setPen(palette().windowText().color());
            p.drawRect(x, y, chipSize, chipSize);
            x += chipSize + 4;
            QString label = kCategoryLabels[i];
            QRect textRect(x, y - 1, 200, chipSize + 2);
            p.drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft, label);
            x += p.fontMetrics().horizontalAdvance(label) + spacing;
        }
    } else {
        for (int i = 0; i < m_allModels.size() && i < kMaxModelColours; ++i) {
            p.fillRect(x, y, chipSize, chipSize, modelColour(i));
            p.setPen(palette().windowText().color());
            p.drawRect(x, y, chipSize, chipSize);
            x += chipSize + 4;
            QRect textRect(x, y - 1, 200, chipSize + 2);
            p.drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft, m_allModels[i]);
            x += p.fontMetrics().horizontalAdvance(m_allModels[i]) + spacing;
        }
        if (m_allModels.size() > kMaxModelColours) {
            p.fillRect(x, y, chipSize, chipSize, modelColour(kMaxModelColours));
            p.setPen(palette().windowText().color());
            p.drawRect(x, y, chipSize, chipSize);
            x += chipSize + 4;
            QRect textRect(x, y - 1, 200, chipSize + 2);
            p.drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft, "Other");
        }
    }
}

void UsageStatsWidget::paintChart(QPainter& p, const QRect& area)
{
    QColor textColor = palette().windowText().color();
    QColor gridColor = textColor;
    gridColor.setAlphaF(0.15);

    if (m_buckets.isEmpty()) {
        p.setPen(textColor);
        QFont f = font();
        f.setPointSize(12);
        p.setFont(f);
        p.drawText(area, Qt::AlignCenter, "No usage data in this range");
        return;
    }

    // Find max Y
    int maxY = 1;
    for (const ChartBucket& b : m_buckets) {
        int val = b.totalTokens();
        if (val > maxY) maxY = val;
    }

    // Round up to nice number
    int magnitude = static_cast<int>(qPow(10, qFloor(qLn(maxY) / qLn(10))));
    if (magnitude < 1) magnitude = 1;
    maxY = ((maxY / magnitude) + 1) * magnitude;

    int leftAxisWidth = 60;
    int bottomAxisHeight = 20;
    QRect plotArea(area.x() + leftAxisWidth, area.y(),
                   area.width() - leftAxisWidth, area.height() - bottomAxisHeight);

    // Grid lines (4 lines)
    QFont axisFont = font();
    axisFont.setPointSize(7);
    p.setFont(axisFont);
    p.setPen(gridColor);
    for (int i = 1; i <= 4; ++i) {
        int y = plotArea.bottom() - (plotArea.height() * i / 4);
        p.drawLine(plotArea.left(), y, plotArea.right(), y);

        int val = maxY * i / 4;
        p.setPen(textColor);
        p.drawText(QRect(area.x(), y - 8, leftAxisWidth - 6, 16),
                   Qt::AlignRight | Qt::AlignVCenter, humanTokens(val));
        p.setPen(gridColor);
    }

    // Bars
    int numBuckets = m_buckets.size();
    double barTotalWidth = static_cast<double>(plotArea.width()) / numBuckets;
    double barWidth = qMax(1.0, barTotalWidth - 2.0);  // 2px gap
    int gap = 2;

    for (int i = 0; i < numBuckets; ++i) {
        const ChartBucket& b = m_buckets[i];
        double x = plotArea.x() + i * barTotalWidth + (barTotalWidth - barWidth) / 2.0;
        int barBottom = plotArea.bottom();

        bool hovered = (i == m_hoverBucket);

        if (m_colourBy == ColourBy::Category) {
            int vals[kCategoryCount] = { b.uncachedInput, b.cacheRead, b.cacheWrite, b.output };
            for (int c = 0; c < kCategoryCount; ++c) {
                if (vals[c] <= 0) continue;
                int segH = static_cast<int>(static_cast<double>(vals[c]) / maxY * plotArea.height());
                if (segH < 1) segH = 1;
                QRectF segRect(x, barBottom - segH, barWidth, segH - gap);
                QColor col = categoryColour(c);
                if (hovered) col = col.lighter(130);
                p.fillRect(segRect, col);
                barBottom -= segH;
            }
        } else {
            // Model mode — stack per model
            for (int mi = 0; mi < m_allModels.size(); ++mi) {
                int colIdx = (mi < kMaxModelColours) ? mi : kMaxModelColours;
                QString model = m_allModels[mi];
                int val = b.perModel.value(model, 0);
                if (val <= 0) continue;
                int segH = static_cast<int>(static_cast<double>(val) / maxY * plotArea.height());
                if (segH < 1) segH = 1;
                QRectF segRect(x, barBottom - segH, barWidth, segH - gap);
                QColor col = modelColour(colIdx);
                if (hovered) col = col.lighter(130);
                p.fillRect(segRect, col);
                barBottom -= segH;
            }
        }
    }

    // X axis labels (evenly spaced, ~6 labels)
    p.setPen(textColor);
    p.setFont(axisFont);
    int labelCount = qMin(6, numBuckets);
    for (int i = 0; i < labelCount; ++i) {
        int bucketIdx = (numBuckets <= 1) ? 0
                        : i * (numBuckets - 1) / (labelCount - 1);
        double x = plotArea.x() + bucketIdx * barTotalWidth + barTotalWidth / 2.0;
        QString label = humanTime(m_buckets[bucketIdx].fromMs);
        QRect labelRect(static_cast<int>(x) - 40, plotArea.bottom() + 2, 80, 16);
        p.drawText(labelRect, Qt::AlignHCenter | Qt::AlignTop, label);
    }
}

void UsageStatsWidget::paintSummaryTiles(QPainter& p, const QRect& area)
{
    if (m_summaries.isEmpty()) return;

    QColor textColor = palette().windowText().color();
    QColor tileColor = palette().base().color();
    QColor borderColor = textColor;
    borderColor.setAlphaF(0.2);

    QFont titleFont = font();
    titleFont.setPointSize(9);
    titleFont.setBold(true);
    QFont bodyFont = font();
    bodyFont.setPointSize(7);

    int tileWidth  = qMin(220, (area.width() - 8) / qMax(1, m_summaries.size()));
    int tileHeight = 50;

    for (int i = 0; i < m_summaries.size() && i < 4; ++i) {
        const ModelSummary& ms = m_summaries[i];
        QRect tile(area.x() + i * (tileWidth + 6), area.y(), tileWidth, tileHeight);

        p.setPen(borderColor);
        p.setBrush(tileColor);
        p.drawRoundedRect(tile, 4, 4);

        p.setPen(textColor);
        p.setFont(titleFont);
        p.drawText(tile.adjusted(6, 4, -4, 0), Qt::AlignLeft | Qt::AlignTop, ms.model);

        p.setFont(bodyFont);
        QString info = QString("In: %1  Out: %2  CR: %3  CW: %4\n"
                               "Hit: %5%  Tools: %6  Turns: %7  Est. $%8")
            .arg(humanTokens(ms.inputTokens))
            .arg(humanTokens(ms.outputTokens))
            .arg(humanTokens(ms.cacheReadInputTokens))
            .arg(humanTokens(ms.cacheCreationInputTokens))
            .arg(QString::number(ms.cacheHitRate() * 100, 'f', 1))
            .arg(ms.toolCalls)
            .arg(ms.turns)
            .arg(QString::number(ms.costUsd, 'f', 4));
        p.drawText(tile.adjusted(6, 20, -4, -2), Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap, info);
    }
}

void UsageStatsWidget::paintTooltip(QPainter& p)
{
    if (m_hoverBucket < 0 || m_hoverBucket >= m_buckets.size())
        return;

    const ChartBucket& b = m_buckets[m_hoverBucket];

    QStringList lines;
    lines << humanTime(b.fromMs);
    if (m_colourBy == ColourBy::Category) {
        lines << QString("Uncached: %1").arg(humanTokens(b.uncachedInput));
        lines << QString("Cache Read: %1").arg(humanTokens(b.cacheRead));
        lines << QString("Cache Write: %1").arg(humanTokens(b.cacheWrite));
        lines << QString("Output: %1").arg(humanTokens(b.output));
    } else {
        for (auto it = b.perModel.cbegin(); it != b.perModel.cend(); ++it)
            lines << QString("%1: %2").arg(it.key(), humanTokens(it.value()));
    }
    lines << QString("Total: %1").arg(humanTokens(b.totalTokens()));
    if (b.costUsd > 0)
        lines << QString("Est. $%1").arg(QString::number(b.costUsd, 'f', 4));

    QFont tipFont = font();
    tipFont.setPointSize(8);
    p.setFont(tipFont);
    QFontMetrics fm(tipFont);

    int lineH = fm.height() + 2;
    int tipW = 0;
    for (const QString& l : lines)
        tipW = qMax(tipW, fm.horizontalAdvance(l));
    tipW += 16;
    int tipH = lines.size() * lineH + 8;

    int tx = m_hoverPos.x() + 12;
    int ty = m_hoverPos.y() - tipH / 2;
    if (tx + tipW > width())  tx = m_hoverPos.x() - tipW - 12;
    if (ty < 0) ty = 4;
    if (ty + tipH > height()) ty = height() - tipH - 4;

    QRect tipRect(tx, ty, tipW, tipH);
    QColor tipBg = isDark() ? QColor(50, 50, 55, 230) : QColor(255, 255, 255, 230);
    QColor tipBorder = isDark() ? QColor(100, 100, 110) : QColor(180, 180, 190);

    p.setPen(tipBorder);
    p.setBrush(tipBg);
    p.drawRoundedRect(tipRect, 4, 4);

    p.setPen(palette().windowText().color());
    int y = tipRect.y() + 4;
    for (const QString& l : lines) {
        p.drawText(tipRect.x() + 8, y + fm.ascent(), l);
        y += lineH;
    }
}

// ---------------------------------------------------------------------------
// Line chart rendering
// ---------------------------------------------------------------------------

void UsageStatsWidget::paintLineChart(QPainter& p, const QRect& area)
{
    QColor textColor = palette().windowText().color();
    QColor gridColor = textColor;
    gridColor.setAlphaF(0.15);

    if (m_buckets.isEmpty()) {
        p.setPen(textColor);
        QFont f = font();
        f.setPointSize(12);
        p.setFont(f);
        p.drawText(area, Qt::AlignCenter, "No usage data in this range");
        return;
    }

    // Find max Y
    int maxY = 1;
    for (const ChartBucket& b : m_buckets) {
        int val = b.totalTokens();
        if (val > maxY) maxY = val;
    }
    int magnitude = static_cast<int>(qPow(10, qFloor(qLn(maxY) / qLn(10))));
    if (magnitude < 1) magnitude = 1;
    maxY = ((maxY / magnitude) + 1) * magnitude;

    int leftAxisWidth = 60;
    int bottomAxisHeight = 20;
    QRect plotArea(area.x() + leftAxisWidth, area.y(),
                   area.width() - leftAxisWidth, area.height() - bottomAxisHeight);

    // Grid lines
    QFont axisFont = font();
    axisFont.setPointSize(7);
    p.setFont(axisFont);
    p.setPen(gridColor);
    for (int i = 1; i <= 4; ++i) {
        int y = plotArea.bottom() - (plotArea.height() * i / 4);
        p.drawLine(plotArea.left(), y, plotArea.right(), y);
        int val = maxY * i / 4;
        p.setPen(textColor);
        p.drawText(QRect(area.x(), y - 8, leftAxisWidth - 6, 16),
                   Qt::AlignRight | Qt::AlignVCenter, humanTokens(val));
        p.setPen(gridColor);
    }

    int numBuckets = m_buckets.size();
    double stepX = (numBuckets <= 1) ? 0 : static_cast<double>(plotArea.width()) / (numBuckets - 1);

    auto xForBucket = [&](int i) -> double {
        if (numBuckets <= 1) return plotArea.center().x();
        return plotArea.x() + i * stepX;
    };
    auto yForValue = [&](int val) -> double {
        return plotArea.bottom() - static_cast<double>(val) / maxY * plotArea.height();
    };

    if (m_colourBy == ColourBy::Category) {
        // Draw 4 polylines: uncached, cacheRead, cacheWrite, output
        auto drawLine = [&](int catIdx, std::function<int(const ChartBucket&)> valueFn) {
            if (numBuckets < 1) return;
            QPainterPath path;
            path.moveTo(xForBucket(0), yForValue(valueFn(m_buckets[0])));
            for (int i = 1; i < numBuckets; ++i)
                path.lineTo(xForBucket(i), yForValue(valueFn(m_buckets[i])));

            QPen pen(categoryColour(catIdx), 2);
            p.setPen(pen);
            p.setBrush(Qt::NoBrush);
            p.drawPath(path);
        };

        drawLine(0, [](const ChartBucket& b) { return b.uncachedInput; });
        drawLine(1, [](const ChartBucket& b) { return b.cacheRead; });
        drawLine(2, [](const ChartBucket& b) { return b.cacheWrite; });
        drawLine(3, [](const ChartBucket& b) { return b.output; });
    } else {
        // One polyline per model
        for (int mi = 0; mi < m_allModels.size() && mi <= kMaxModelColours; ++mi) {
            int colIdx = (mi < kMaxModelColours) ? mi : kMaxModelColours;
            const QString& model = m_allModels[mi];

            QPainterPath path;
            bool started = false;
            for (int i = 0; i < numBuckets; ++i) {
                int val = m_buckets[i].perModel.value(model, 0);
                double px = xForBucket(i);
                double py = yForValue(val);
                if (!started) { path.moveTo(px, py); started = true; }
                else          path.lineTo(px, py);
            }
            QPen pen(modelColour(colIdx), 2);
            p.setPen(pen);
            p.setBrush(Qt::NoBrush);
            p.drawPath(path);
        }
    }

    // Draw data points as small circles on hover
    if (m_hoverBucket >= 0 && m_hoverBucket < numBuckets) {
        p.setPen(Qt::NoPen);
        double hx = xForBucket(m_hoverBucket);
        if (m_colourBy == ColourBy::Category) {
            int vals[kCategoryCount] = {
                m_buckets[m_hoverBucket].uncachedInput,
                m_buckets[m_hoverBucket].cacheRead,
                m_buckets[m_hoverBucket].cacheWrite,
                m_buckets[m_hoverBucket].output
            };
            for (int c = 0; c < kCategoryCount; ++c) {
                p.setBrush(categoryColour(c));
                p.drawEllipse(QPointF(hx, yForValue(vals[c])), 4, 4);
            }
        } else {
            for (int mi = 0; mi < m_allModels.size() && mi <= kMaxModelColours; ++mi) {
                int colIdx = (mi < kMaxModelColours) ? mi : kMaxModelColours;
                int val = m_buckets[m_hoverBucket].perModel.value(m_allModels[mi], 0);
                p.setBrush(modelColour(colIdx));
                p.drawEllipse(QPointF(hx, yForValue(val)), 4, 4);
            }
        }
    }

    // X axis labels
    p.setPen(textColor);
    p.setFont(axisFont);
    int labelCount = qMin(6, numBuckets);
    for (int i = 0; i < labelCount; ++i) {
        int bucketIdx = (numBuckets <= 1) ? 0
                        : i * (numBuckets - 1) / (labelCount - 1);
        double x = xForBucket(bucketIdx);
        QString label = humanTime(m_buckets[bucketIdx].fromMs);
        QRect labelRect(static_cast<int>(x) - 40, plotArea.bottom() + 2, 80, 16);
        p.drawText(labelRect, Qt::AlignHCenter | Qt::AlignTop, label);
    }
}

// ---------------------------------------------------------------------------
// Cost line chart
// ---------------------------------------------------------------------------

void UsageStatsWidget::paintCostChart(QPainter& p, const QRect& area)
{
    QColor textColor = palette().windowText().color();
    QColor gridColor = textColor;
    gridColor.setAlphaF(0.15);

    // Title
    QFont titleFont = font();
    titleFont.setPointSize(8);
    titleFont.setBold(true);
    p.setFont(titleFont);
    p.setPen(textColor);
    p.drawText(area.x(), area.y() - 2, QString::fromUtf16(u"Kosten (est.)"));

    if (m_buckets.isEmpty()) return;

    // Find max cost
    double maxCost = 0.0;
    for (const ChartBucket& b : m_buckets) {
        if (b.costUsd > maxCost) maxCost = b.costUsd;
    }
    if (maxCost <= 0.0) {
        p.setPen(gridColor);
        QFont f = font();
        f.setPointSize(9);
        p.setFont(f);
        p.drawText(area, Qt::AlignCenter, "No cost data");
        return;
    }

    // Round up max cost to a nice number
    double costMag = qPow(10, qFloor(qLn(maxCost) / qLn(10)));
    if (costMag < 0.0001) costMag = 0.0001;
    maxCost = qCeil(maxCost / costMag) * costMag;

    int leftAxisWidth = 60;
    int bottomAxisHeight = 14;
    QRect plotArea(area.x() + leftAxisWidth, area.y() + 12,
                   area.width() - leftAxisWidth, area.height() - bottomAxisHeight - 12);

    // Grid lines (2 lines)
    QFont axisFont = font();
    axisFont.setPointSize(7);
    p.setFont(axisFont);
    for (int i = 1; i <= 2; ++i) {
        int y = plotArea.bottom() - (plotArea.height() * i / 2);
        p.setPen(gridColor);
        p.drawLine(plotArea.left(), y, plotArea.right(), y);
        double val = maxCost * i / 2;
        p.setPen(textColor);
        QString label = (val >= 0.01) ? QString("$%1").arg(val, 0, 'f', 2)
                                      : QString("$%1").arg(val, 0, 'f', 4);
        p.drawText(QRect(area.x(), y - 8, leftAxisWidth - 6, 16),
                   Qt::AlignRight | Qt::AlignVCenter, label);
    }

    // Draw cost polyline
    int numBuckets = m_buckets.size();
    double stepX = (numBuckets <= 1) ? 0 : static_cast<double>(plotArea.width()) / (numBuckets - 1);

    QColor costColour = isDark() ? QColor(255, 160, 60) : QColor(230, 120, 20);

    QPainterPath path;
    for (int i = 0; i < numBuckets; ++i) {
        double px = (numBuckets <= 1) ? plotArea.center().x() : plotArea.x() + i * stepX;
        double py = plotArea.bottom() - (m_buckets[i].costUsd / maxCost) * plotArea.height();
        if (i == 0) path.moveTo(px, py);
        else        path.lineTo(px, py);
    }

    QPen costPen(costColour, 2);
    p.setPen(costPen);
    p.setBrush(Qt::NoBrush);
    p.drawPath(path);

    // Fill under the curve with translucent colour
    if (numBuckets > 1) {
        QPainterPath fillPath = path;
        double lastX = plotArea.x() + (numBuckets - 1) * stepX;
        fillPath.lineTo(lastX, plotArea.bottom());
        fillPath.lineTo(plotArea.x(), plotArea.bottom());
        fillPath.closeSubpath();
        QColor fillCol = costColour;
        fillCol.setAlphaF(0.15);
        p.fillPath(fillPath, fillCol);
    }

    // Hover dot on cost chart
    if (m_hoverBucket >= 0 && m_hoverBucket < numBuckets) {
        double hx = (numBuckets <= 1) ? plotArea.center().x() : plotArea.x() + m_hoverBucket * stepX;
        double hy = plotArea.bottom() - (m_buckets[m_hoverBucket].costUsd / maxCost) * plotArea.height();
        p.setPen(Qt::NoPen);
        p.setBrush(costColour);
        p.drawEllipse(QPointF(hx, hy), 4, 4);
    }
}

// ---------------------------------------------------------------------------
// Zoom / pan
// ---------------------------------------------------------------------------

void UsageStatsWidget::wheelEvent(QWheelEvent* event)
{
    if (m_allBuckets.isEmpty()) {
        QWidget::wheelEvent(event);
        return;
    }

    qint64 fullStart = m_allBuckets.first().fromMs;
    qint64 fullEnd   = m_allBuckets.last().toMs;
    qint64 fullSpan  = fullEnd - fullStart;
    if (fullSpan <= 0) {
        QWidget::wheelEvent(event);
        return;
    }

    // Current view or full range
    qint64 viewStart = (m_viewStartMs > 0) ? m_viewStartMs : fullStart;
    qint64 viewEnd   = (m_viewEndMs > 0)   ? m_viewEndMs   : fullEnd;
    qint64 viewSpan  = viewEnd - viewStart;

    // Zoom factor: scroll up = zoom in, scroll down = zoom out
    double zoomFactor = (event->angleDelta().y() > 0) ? 0.8 : 1.25;
    qint64 newSpan = static_cast<qint64>(viewSpan * zoomFactor);

    // Clamp: don't zoom out past full range, don't zoom in below 5 seconds
    if (newSpan >= fullSpan) {
        // Fully zoomed out — reset
        m_viewStartMs = 0;
        m_viewEndMs   = 0;
        rebuildData();
        updateScrollBar();
        update();
        event->accept();
        return;
    }
    if (newSpan < 5000) newSpan = 5000;

    // Anchor zoom on cursor X position within the chart area
    double anchorFrac = 0.5;
    if (m_chartRect.width() > 0) {
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
        int mx = static_cast<int>(event->position().x());
#else
        int mx = event->x();
#endif
        anchorFrac = static_cast<double>(mx - m_chartRect.x()) / m_chartRect.width();
        anchorFrac = qBound(0.0, anchorFrac, 1.0);
    }

    qint64 anchorMs = viewStart + static_cast<qint64>(anchorFrac * viewSpan);
    qint64 newStart = anchorMs - static_cast<qint64>(anchorFrac * newSpan);
    qint64 newEnd   = newStart + newSpan;

    // Clamp to full range
    if (newStart < fullStart) { newStart = fullStart; newEnd = newStart + newSpan; }
    if (newEnd > fullEnd)     { newEnd = fullEnd; newStart = newEnd - newSpan; }
    if (newStart < fullStart) newStart = fullStart;

    m_viewStartMs = newStart;
    m_viewEndMs   = newEnd;
    rebuildData();
    updateScrollBar();
    update();
    event->accept();
}

void UsageStatsWidget::updateScrollBar()
{
    if (m_viewStartMs <= 0 || m_allBuckets.isEmpty()) {
        m_scrollBar->setVisible(false);
        return;
    }

    qint64 fullStart = m_allBuckets.first().fromMs;
    qint64 fullEnd   = m_allBuckets.last().toMs;
    qint64 fullSpan  = fullEnd - fullStart;
    qint64 viewSpan  = m_viewEndMs - m_viewStartMs;

    if (viewSpan >= fullSpan || fullSpan <= 0) {
        m_scrollBar->setVisible(false);
        return;
    }

    m_scrollBar->setVisible(true);
    m_scrollBar->blockSignals(true);
    m_scrollBar->setRange(0, 1000);
    m_scrollBar->setPageStep(static_cast<int>(1000.0 * viewSpan / fullSpan));

    double frac = static_cast<double>(m_viewStartMs - fullStart) / (fullSpan - viewSpan);
    m_scrollBar->setValue(static_cast<int>(frac * 1000));
    m_scrollBar->blockSignals(false);
}

// ---------------------------------------------------------------------------
// Mouse tracking
// ---------------------------------------------------------------------------

void UsageStatsWidget::mouseMoveEvent(QMouseEvent* event)
{
    m_hoverPos = event->pos();
    if (m_chartRect.contains(event->pos()) && !m_buckets.isEmpty()) {
        int leftAxisWidth = 60;
        QRect plotArea(m_chartRect.x() + leftAxisWidth, m_chartRect.y(),
                       m_chartRect.width() - leftAxisWidth, m_chartRect.height() - 20);
        double barTotalWidth = static_cast<double>(plotArea.width()) / m_buckets.size();
        int idx = static_cast<int>((event->pos().x() - plotArea.x()) / barTotalWidth);
        if (idx >= 0 && idx < m_buckets.size())
            m_hoverBucket = idx;
        else
            m_hoverBucket = -1;
    } else {
        m_hoverBucket = -1;
    }
    update();
}

void UsageStatsWidget::leaveEvent(QEvent* /*event*/)
{
    m_hoverBucket = -1;
    update();
}

void UsageStatsWidget::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    update();
}

} // namespace QtLLM
