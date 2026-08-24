#include "ContextUsageBar.h"
#include <QPainter>
#include <QMouseEvent>
#include <QToolTip>

namespace QtLLM
{
    ContextUsageBar::ContextUsageBar(QWidget* parent)
        : QWidget(parent)
    {
        setBarHeight(13);
        setMouseTracking(true);
        setMinimumWidth(80);
    }

    void ContextUsageBar::setBarHeight(int px)
    {
        setFixedHeight(px);
        update();
    }

    void ContextUsageBar::setBreakdown(const ContextBreakdown& breakdown)
    {
        m_breakdown = breakdown;
        update();
    }

    QList<ContextUsageBar::Segment> ContextUsageBar::segments() const
    {
        return {
            { QStringLiteral("System prompt"), m_breakdown.systemPromptTokens, QColor("#3b82f6") },
            { QStringLiteral("Tools"),         m_breakdown.toolsTokens,        QColor("#f59e0b") },
            { QStringLiteral("Messages"),      m_breakdown.messagesTokens,     QColor("#22c55e") },
        };
    }

    int ContextUsageBar::segmentAt(int x) const
    {
        const int total = qMax(m_breakdown.contextWindowTokens, 1);
        const QList<Segment> segs = segments();
        double cursor = 0.0;
        for (int i = 0; i < segs.size(); ++i) {
            if (segs[i].tokens <= 0)
                continue;
            double start = cursor / total * width();
            cursor += segs[i].tokens;
            double end = cursor / total * width();
            if (x >= start && x < end)
                return i;
        }
        return -1;
    }

    void ContextUsageBar::paintEvent(QPaintEvent*)
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);

        const QRectF r(0, 0, width(), height());
        p.setPen(Qt::NoPen);
        p.setBrush(QColor("#e5e7eb"));
        p.drawRoundedRect(r, 3, 3);

        const int total = qMax(m_breakdown.contextWindowTokens, 1);
        const QList<Segment> segs = segments();
        double x = 0.0;
        for (int i = 0; i < segs.size(); ++i) {
            if (segs[i].tokens <= 0)
                continue;
            double w = double(segs[i].tokens) / total * width();
            QRectF segRect(x, 0, w, height());
            QColor c = segs[i].color;
            if (i == m_hoveredSegment)
                c = c.lighter(125);
            p.setBrush(c);
            p.drawRect(segRect);
            if (i == m_hoveredSegment) {
                p.setPen(QPen(Qt::black, 1));
                p.setBrush(Qt::NoBrush);
                p.drawRect(segRect.adjusted(0.5, 0.5, -0.5, -0.5));
            }
            x += w;
        }

        // Rounded outline on top so square segment joins don't poke past the corners.
        p.setPen(QPen(QColor("#9ca3af"), 1));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(r.adjusted(0.5, 0.5, -0.5, -0.5), 3, 3);
    }

    void ContextUsageBar::mouseMoveEvent(QMouseEvent* event)
    {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        const QPoint globalPos = event->globalPosition().toPoint();
#else
        const QPoint globalPos = event->globalPos();
#endif
        int seg = segmentAt(event->pos().x());
        if (seg != m_hoveredSegment) {
            m_hoveredSegment = seg;
            update();
        }

        const int total = qMax(m_breakdown.contextWindowTokens, 1);
        const bool exact = m_breakdown.exactUsedTokens >= 0;

        if (seg >= 0) {
            const QList<Segment> segs = segments();
            const double pct = 100.0 * segs[seg].tokens / total;
            const QString text = QString("%1: ~%2 tokens (%3% of context)\nTotal used: %4%5 / %6 tokens")
                .arg(segs[seg].label)
                .arg(segs[seg].tokens)
                .arg(pct, 0, 'f', 1)
                .arg(exact ? "" : "~")
                .arg(m_breakdown.bestUsedTokens())
                .arg(m_breakdown.contextWindowTokens);
            QToolTip::showText(globalPos, text, this);
        } else {
            const double pct = 100.0 * m_breakdown.bestUsedTokens() / total;
            const QString text = QString("Context usage: %1%2 / %3 tokens (%4%)\n%5")
                .arg(exact ? "" : "~")
                .arg(m_breakdown.bestUsedTokens())
                .arg(m_breakdown.contextWindowTokens)
                .arg(pct, 0, 'f', 1)
                .arg(exact
                    ? "Total confirmed by the provider's own tokenizer; per-segment split below is still a character-count estimate."
                    : "Estimated from character counts - not the model's exact tokenizer.");
            QToolTip::showText(globalPos, text, this);
        }

        QWidget::mouseMoveEvent(event);
    }

    void ContextUsageBar::leaveEvent(QEvent* event)
    {
        if (m_hoveredSegment != -1) {
            m_hoveredSegment = -1;
            update();
        }
        QWidget::leaveEvent(event);
    }
}
