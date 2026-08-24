#pragma once
#include "QtLLM_base.h"
#include "UsageStats.h"
#include <QWidget>

namespace QtLLM
{
    // Horizontal stacked bar visualizing the current context breakdown
    // (system prompt / tools / conversation messages / free space), each in
    // its own color. Hovering a segment highlights it and shows a tooltip
    // with its token estimate and share of the context window.
    class QT_LLM_API ContextUsageBar : public QWidget
    {
        Q_OBJECT
    public:
        explicit ContextUsageBar(QWidget* parent = nullptr);

        void setBreakdown(const ContextBreakdown& breakdown);
        ContextBreakdown breakdown() const { return m_breakdown; }

        // Bar thickness in px; the widget's fixed height. Chat view uses
        // ~10-15px, the Settings "Context" tab uses ~20-25px for more detail.
        void setBarHeight(int px);

    protected:
        void paintEvent(QPaintEvent* event) override;
        void mouseMoveEvent(QMouseEvent* event) override;
        void leaveEvent(QEvent* event) override;

    private:
        struct Segment {
            QString label;
            int tokens = 0;
            QColor color;
        };
        QList<Segment> segments() const;
        int segmentAt(int x) const;

        ContextBreakdown m_breakdown;
        int m_hoveredSegment = -1;
    };
}
