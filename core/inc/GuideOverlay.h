#pragma once
#include "QtLLM_base.h"
#include <QColor>
#include <QJsonObject>
#include <QList>
#include <QMetaObject>
#include <QPointF>
#include <QPointer>
#include <QRectF>
#include <QString>
#include <QStringList>
#include <QWidget>

class QPainter;
class QTimer;
class QVariantAnimation;

namespace QtLLM
{

// How an overlay item moves. One animation per item. With animations disabled
// (GuideOverlay::setAnimationsEnabled) every item shows its static end state.
struct QT_LLM_API OverlayAnimation
{
    enum class Type { None, Blink, Fade, Pulse, ColorLoop, MarchingAnts, DrawOn };

    Type type = Type::None;
    // Blink, Pulse, ColorLoop, MarchingAnts: one cycle. Fade, DrawOn: the transition.
    int periodMs = 1000;
    // Cycles of Blink, Pulse, ColorLoop and MarchingAnts; 0 = until the item is removed.
    int count = 0;
    // ColorLoop; with fewer than two the stroke alternates with a lighter stroke.
    QList<QColor> colors;
    // Fade of a timed item: also fade out during its last periodMs.
    bool fadeOut = false;
};

// One shape on a GuideOverlay. Anchored items follow their widget, are not
// painted while it is hidden and are removed when it is destroyed; the others
// use rect (or from/to for Line and Arrow) in window coordinates.
struct QT_LLM_API OverlayItem
{
    enum class Shape { Rect, RoundedRect, Ellipse, Line, Arrow, Spotlight };

    QString id;                   // generated when empty
    Shape shape = Shape::RoundedRect;
    QPointer<QWidget> anchor;
    QRectF rect;
    QPointF from, to;             // anchored Line / Arrow end on the anchor's edge nearest "from"
    QColor stroke = QColor(255, 140, 0);
    QColor fill;                  // invalid = none; Spotlight: the dimming colour
    qreal strokeWidth = 2;
    qreal cornerRadius = 6;
    Qt::PenStyle penStyle = Qt::SolidLine;
    int padding = 4;              // space between an anchored widget and the outline
    OverlayAnimation animation;
    int durationMs = 0;           // 0 = until removed
};

// Transparent, mouse-transparent layer over a whole top-level window on which
// the model points at things. One per window, created on demand. Owner thread only.
//
// ponytail: a sibling widget cannot paint over content drawn by a native child
// (QOpenGLWidget in native mode, embedded native windows). Upgrade path: a
// frameless, translucent, input-transparent tool window tracking the target's
// screen geometry.
class QT_LLM_API GuideOverlay : public QWidget
{
    Q_OBJECT
public:
    // Hard limits, deliberately not configurable by the caller.
    static constexpr int kMaxItems = 20;
    static constexpr int kMinBlinkPeriodMs = 334;          // at most 3 blinks per second
    static constexpr int kMaxDurationMs = 5 * 60 * 1000;

    // The overlay of the window containing the widget; created on first use.
    static GuideOverlay* forWindow(QWidget* anyWidgetInThatWindow);

    // Applies to every overlay; false shows each item in its static end state.
    static void setAnimationsEnabled(bool enabled);
    static bool animationsEnabled();

    // Returns the id, or an empty string when refused (item limit reached,
    // anchor in another window, no geometry). An existing item with the same
    // id is replaced. Blink period and durationMs are clamped to the limits.
    QString addItem(const OverlayItem& item);
    bool removeItem(const QString& id);
    void clear();
    QStringList itemIds() const;
    // The stored item, after clamping; default item for an unknown id.
    OverlayItem item(const QString& id) const;
    // Currently painted geometry in window coordinates (outline rect, or the
    // from/to bounding rect of a Line / Arrow); empty when not painted.
    QRect itemRect(const QString& id) const;

    QString highlight(QWidget* target, const QColor& color = QColor(255, 140, 0),
                      const OverlayAnimation& animation = {}, int durationMs = 0);

    // Applies the keys present in json onto *item; absent keys leave it
    // unchanged, so a caller pre-fills defaults and sets the anchor itself.
    // On error *item is untouched and *error says what was wrong.
    //   { "id": "...", "shape": "rect|roundedRect|ellipse|line|arrow|spotlight",
    //     "stroke" (or "color"): "#rrggbb" | "#aarrggbb" | "red", "fill": ...,
    //     "strokeWidth": 2, "cornerRadius": 6, "padding": 4,
    //     "penStyle": "solid|dash|dot|dashDot",
    //     "rect": { "x", "y", "width", "height" }, "from": { "x", "y" }, "to": { "x", "y" },
    //     "animation": { "type": "none|blink|fade|pulse|colorLoop|marchingAnts|drawOn",
    //                    "periodMs": 1000, "count": 0, "colors": ["#f00", "#00f"], "fadeOut": false },
    //     "durationMs": 0 }
    static bool overlayItemFromJson(const QJsonObject& json, OverlayItem* item, QString* error = nullptr);
    // Same format; an anchored item additionally reports "anchor": objectName.
    static QJsonObject overlayItemToJson(const OverlayItem& item);

signals:
    // Every removal: explicit, clear(), timed expiry, anchor destroyed, replaced.
    void itemRemoved(const QString& id);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    struct Entry
    {
        OverlayItem item;
        qreal opacity = 1;
        qreal extraWidth = 0;          // Pulse
        QColor color;                  // ColorLoop; invalid = item.stroke
        qreal dashOffset = 0;          // MarchingAnts
        qreal progress = 1;            // DrawOn
        QVariantAnimation* animation = nullptr;
        QTimer* timer = nullptr;
        QMetaObject::Connection anchorDestroyed;
    };

    explicit GuideOverlay(QWidget* window);

    Entry* entry(const QString& id);
    const Entry* entry(const QString& id) const;
    bool geometryOf(const Entry& entry, QRectF* rect, QPointF* from, QPointF* to) const;
    QRect repaintRect(const Entry& entry) const;
    void startAnimation(Entry& entry);
    static void setEndState(Entry& entry);
    void applyAnimationValue(Entry& entry, qreal value);
    void paintItem(QPainter& painter, const Entry& entry) const;
    void raiseAboveSiblings();
    void refreshAnimations();

    QList<Entry> m_items;   // paint order
    int m_nextId = 1;
};

} // namespace QtLLM
