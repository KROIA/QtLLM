#include "GuideOverlay.h"

#include <QApplication>
#include <QEvent>
#include <QJsonArray>
#include <QLineF>
#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>
#include <QTimer>
#include <QVariantAnimation>

#include <cstddef>
#include <iterator>

namespace QtLLM
{

namespace
{

bool s_animationsEnabled = true;

// Indexed by the enum values.
const char* const kShapeNames[] = { "rect", "roundedRect", "ellipse", "line", "arrow", "spotlight" };
const char* const kAnimationNames[] = { "none", "blink", "fade", "pulse", "colorLoop", "marchingAnts", "drawOn" };
const char* const kPenStyleNames[] = { "solid", "dash", "dot", "dashDot" };
const Qt::PenStyle kPenStyles[] = { Qt::SolidLine, Qt::DashLine, Qt::DotLine, Qt::DashDotLine };

// Length of one Qt::DashLine cycle (4 on, 2 off), in pen widths.
constexpr qreal kDashCycle = 6;

template<std::size_t N>
int nameIndex(const char* const (&names)[N], const QString& name)
{
    for (std::size_t i = 0; i < N; ++i)
    {
        if (name.compare(QLatin1String(names[i]), Qt::CaseInsensitive) == 0)
            return int(i);
    }
    return -1;
}

bool fail(QString* error, const QString& message)
{
    if (error)
        *error = message;
    return false;
}

bool readColor(const QJsonValue& value, const QString& key, QColor* out, QString* error)
{
    const QColor color(value.toString());
    if (!value.isString() || !color.isValid())
        return fail(error, key + ": expected a colour such as \"#ff8c00\", \"#80ff8c00\" or \"red\"");
    *out = color;
    return true;
}

// Leaves *out unchanged when the key is absent.
bool optionalNumber(const QJsonObject& json, const char* key, double min, double max, double* out, QString* error)
{
    const QJsonValue value = json.value(QLatin1String(key));
    if (value.isUndefined())
        return true;
    if (!value.isDouble() || value.toDouble() < min || value.toDouble() > max)
        return fail(error, QString("%1: expected a number from %2 to %3").arg(QLatin1String(key)).arg(min).arg(max));
    *out = value.toDouble();
    return true;
}

bool readPoint(const QJsonValue& value, const QString& key, QPointF* out, QString* error)
{
    const QJsonObject obj = value.toObject();
    if (!value.isObject() || !obj.value("x").isDouble() || !obj.value("y").isDouble())
        return fail(error, key + ": expected { \"x\": number, \"y\": number }");
    *out = QPointF(obj.value("x").toDouble(), obj.value("y").toDouble());
    return true;
}

QJsonObject pointToJson(const QPointF& point)
{
    return QJsonObject{ { "x", point.x() }, { "y", point.y() } };
}

QString colorName(const QColor& color)
{
    return color.alpha() == 255 ? color.name() : color.name(QColor::HexArgb);
}

QColor mix(const QColor& a, const QColor& b, qreal t)
{
    return QColor::fromRgbF(a.redF() + (b.redF() - a.redF()) * t,
                            a.greenF() + (b.greenF() - a.greenF()) * t,
                            a.blueF() + (b.blueF() - a.blueF()) * t,
                            a.alphaF() + (b.alphaF() - a.alphaF()) * t);
}

bool isLine(OverlayItem::Shape shape)
{
    return shape == OverlayItem::Shape::Line || shape == OverlayItem::Shape::Arrow;
}

bool readAnimation(const QJsonValue& value, OverlayAnimation* out, QString* error)
{
    if (!value.isObject())
        return fail(error, "animation: expected an object such as { \"type\": \"blink\", \"periodMs\": 600 }");
    const QJsonObject json = value.toObject();
    OverlayAnimation a = *out;
    if (json.contains("type"))
    {
        const int index = nameIndex(kAnimationNames, json.value("type").toString());
        if (index < 0)
            return fail(error, "animation.type: expected one of none, blink, fade, pulse, colorLoop, marchingAnts, drawOn");
        a.type = OverlayAnimation::Type(index);
    }
    double period = a.periodMs, count = a.count;
    if (!optionalNumber(json, "periodMs", 1, 60000, &period, error)
        || !optionalNumber(json, "count", 0, 1000, &count, error))
        return false;
    a.periodMs = int(period);
    a.count = int(count);
    if (json.contains("fadeOut"))
    {
        if (!json.value("fadeOut").isBool())
            return fail(error, "animation.fadeOut: expected true or false");
        a.fadeOut = json.value("fadeOut").toBool();
    }
    if (json.contains("colors"))
    {
        const QJsonArray colors = json.value("colors").toArray();
        if (!json.value("colors").isArray() || colors.size() > 16)
            return fail(error, "animation.colors: expected an array of at most 16 colours");
        a.colors.clear();
        for (const QJsonValue& c : colors)
        {
            QColor color;
            if (!readColor(c, "animation.colors", &color, error))
                return false;
            a.colors.append(color);
        }
    }
    *out = a;
    return true;
}

} // namespace

GuideOverlay::GuideOverlay(QWidget* window)
    : QWidget(window)
{
    setObjectName("qtllm_guideOverlay");
    // Purely visual: clicks, hover and focus belong to the widgets underneath.
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_NoSystemBackground);
    setFocusPolicy(Qt::NoFocus);
    setGeometry(window->rect());
    window->installEventFilter(this);
    raiseAboveSiblings();
    show();
}

GuideOverlay* GuideOverlay::forWindow(QWidget* anyWidgetInThatWindow)
{
    if (!anyWidgetInThatWindow)
        return nullptr;
    QWidget* window = anyWidgetInThatWindow->window();
    if (GuideOverlay* existing = window->findChild<GuideOverlay*>(QString(), Qt::FindDirectChildrenOnly))
        return existing;
    return new GuideOverlay(window);
}

void GuideOverlay::setAnimationsEnabled(bool enabled)
{
    if (s_animationsEnabled == enabled)
        return;
    s_animationsEnabled = enabled;
    for (QWidget* window : QApplication::topLevelWidgets())
    {
        if (GuideOverlay* overlay = window->findChild<GuideOverlay*>(QString(), Qt::FindDirectChildrenOnly))
            overlay->refreshAnimations();
    }
}

bool GuideOverlay::animationsEnabled()
{
    return s_animationsEnabled;
}

QString GuideOverlay::addItem(const OverlayItem& newItem)
{
    OverlayItem item = newItem;
    QWidget* window = parentWidget();
    if (item.anchor)
    {
        if (item.anchor->window() != window)
            return QString();
    }
    else if (isLine(item.shape) ? item.from == item.to : !item.rect.isValid())
    {
        return QString();
    }

    if (item.id.isEmpty())
    {
        do
            item.id = QString("overlay_%1").arg(m_nextId++);
        while (entry(item.id));
    }
    if (entry(item.id))
        removeItem(item.id);
    else if (m_items.size() >= kMaxItems)
        return QString();

    if (item.animation.type == OverlayAnimation::Type::Blink)
        item.animation.periodMs = qMax(item.animation.periodMs, int(kMinBlinkPeriodMs));
    // A zero-length looping animation would spin the event loop.
    item.animation.periodMs = qMax(item.animation.periodMs, 50);
    item.durationMs = qBound(0, item.durationMs, int(kMaxDurationMs));

    Entry e;
    e.item = item;
    const QString id = item.id;
    if (item.anchor)
    {
        e.anchorDestroyed = connect(item.anchor.data(), &QObject::destroyed, this, [this, id]() { removeItem(id); });
        // Ancestors too: scrolling moves a parent, not the anchor itself.
        for (QWidget* w = item.anchor; w && w != window; w = w->parentWidget())
            w->installEventFilter(this);
    }
    if (item.durationMs > 0)
    {
        e.timer = new QTimer(this);
        e.timer->setSingleShot(true);
        connect(e.timer, &QTimer::timeout, this, [this, id]() { removeItem(id); });
        e.timer->start(item.durationMs);
    }
    m_items.append(e);
    startAnimation(m_items.last());
    raiseAboveSiblings();
    update(repaintRect(m_items.last()));
    return id;
}

bool GuideOverlay::removeItem(const QString& id)
{
    for (int i = 0; i < m_items.size(); ++i)
    {
        if (m_items[i].item.id != id)
            continue;
        const Entry e = m_items.takeAt(i);
        const QString removedId = e.item.id;
        disconnect(e.anchorDestroyed);
        // deleteLater: removeItem may run inside the timer's or animation's own signal.
        if (e.animation)
        {
            e.animation->stop();
            e.animation->deleteLater();
        }
        if (e.timer)
        {
            e.timer->stop();
            e.timer->deleteLater();
        }
        update();
        emit itemRemoved(removedId);
        return true;
    }
    return false;
}

void GuideOverlay::clear()
{
    for (const QString& id : itemIds())
        removeItem(id);
}

QStringList GuideOverlay::itemIds() const
{
    QStringList ids;
    for (const Entry& e : m_items)
        ids.append(e.item.id);
    return ids;
}

OverlayItem GuideOverlay::item(const QString& id) const
{
    const Entry* e = entry(id);
    return e ? e->item : OverlayItem();
}

QRect GuideOverlay::itemRect(const QString& id) const
{
    const Entry* e = entry(id);
    QRectF r;
    if (!e || !geometryOf(*e, &r, nullptr, nullptr))
        return QRect();
    return r.toRect();
}

QString GuideOverlay::highlight(QWidget* target, const QColor& color, const OverlayAnimation& animation, int durationMs)
{
    OverlayItem item;
    item.anchor = target;
    item.stroke = color;
    item.strokeWidth = 3;
    item.animation = animation;
    item.durationMs = durationMs;
    return addItem(item);
}

bool GuideOverlay::overlayItemFromJson(const QJsonObject& json, OverlayItem* item, QString* error)
{
    if (!item)
        return fail(error, "no item");
    OverlayItem r = *item;

    if (json.contains("id"))
    {
        if (!json.value("id").isString())
            return fail(error, "id: expected a string");
        r.id = json.value("id").toString();
    }
    if (json.contains("shape"))
    {
        const int index = nameIndex(kShapeNames, json.value("shape").toString());
        if (index < 0)
            return fail(error, "shape: expected one of rect, roundedRect, ellipse, line, arrow, spotlight");
        r.shape = OverlayItem::Shape(index);
    }
    const QString strokeKey = json.contains("stroke") ? "stroke" : "color";
    if (json.contains(strokeKey) && !readColor(json.value(strokeKey), strokeKey, &r.stroke, error))
        return false;
    if (json.contains("fill") && !readColor(json.value("fill"), "fill", &r.fill, error))
        return false;

    double strokeWidth = r.strokeWidth, cornerRadius = r.cornerRadius, padding = r.padding, duration = r.durationMs;
    if (!optionalNumber(json, "strokeWidth", 0.5, 20, &strokeWidth, error)
        || !optionalNumber(json, "cornerRadius", 0, 100, &cornerRadius, error)
        || !optionalNumber(json, "padding", 0, 50, &padding, error)
        || !optionalNumber(json, "durationMs", 0, kMaxDurationMs, &duration, error))
        return false;
    r.strokeWidth = strokeWidth;
    r.cornerRadius = cornerRadius;
    r.padding = int(padding);
    r.durationMs = int(duration);

    if (json.contains("penStyle"))
    {
        const int index = nameIndex(kPenStyleNames, json.value("penStyle").toString());
        if (index < 0)
            return fail(error, "penStyle: expected one of solid, dash, dot, dashDot");
        r.penStyle = kPenStyles[index];
    }
    if (json.contains("rect"))
    {
        const QJsonObject rect = json.value("rect").toObject();
        const char* const keys[] = { "x", "y", "width", "height" };
        for (const char* key : keys)
        {
            if (!rect.value(QLatin1String(key)).isDouble())
                return fail(error, "rect: expected { \"x\", \"y\", \"width\", \"height\" } numbers in window coordinates");
        }
        r.rect = QRectF(rect.value("x").toDouble(), rect.value("y").toDouble(),
                        rect.value("width").toDouble(), rect.value("height").toDouble());
        if (!r.rect.isValid())
            return fail(error, "rect: width and height must be greater than 0");
    }
    if (json.contains("from") && !readPoint(json.value("from"), "from", &r.from, error))
        return false;
    if (json.contains("to") && !readPoint(json.value("to"), "to", &r.to, error))
        return false;
    if (json.contains("animation") && !readAnimation(json.value("animation"), &r.animation, error))
        return false;

    *item = r;
    return true;
}

QJsonObject GuideOverlay::overlayItemToJson(const OverlayItem& item)
{
    int penIndex = 0;
    for (int i = 0; i < int(std::size(kPenStyles)); ++i)
    {
        if (kPenStyles[i] == item.penStyle)
            penIndex = i;
    }
    QJsonObject json{
        { "shape", QLatin1String(kShapeNames[int(item.shape)]) },
        { "stroke", colorName(item.stroke) },
        { "strokeWidth", item.strokeWidth },
        { "cornerRadius", item.cornerRadius },
        { "padding", item.padding },
        { "penStyle", QLatin1String(kPenStyleNames[penIndex]) },
        { "durationMs", item.durationMs },
    };
    if (!item.id.isEmpty())
        json["id"] = item.id;
    if (item.fill.isValid())
        json["fill"] = colorName(item.fill);
    if (item.anchor)
        json["anchor"] = item.anchor->objectName();
    if (!item.rect.isNull())
    {
        json["rect"] = QJsonObject{ { "x", item.rect.x() }, { "y", item.rect.y() },
                                    { "width", item.rect.width() }, { "height", item.rect.height() } };
    }
    if (isLine(item.shape))
    {
        json["from"] = pointToJson(item.from);
        json["to"] = pointToJson(item.to);
    }
    const OverlayAnimation& a = item.animation;
    if (a.type != OverlayAnimation::Type::None)
    {
        QJsonObject animation{
            { "type", QLatin1String(kAnimationNames[int(a.type)]) },
            { "periodMs", a.periodMs },
            { "count", a.count },
        };
        if (a.fadeOut)
            animation["fadeOut"] = true;
        if (!a.colors.isEmpty())
        {
            QJsonArray colors;
            for (const QColor& c : a.colors)
                colors.append(colorName(c));
            animation["colors"] = colors;
        }
        json["animation"] = animation;
    }
    return json;
}

bool GuideOverlay::eventFilter(QObject* watched, QEvent* event)
{
    QWidget* window = parentWidget();
    switch (event->type())
    {
    case QEvent::Resize:
        if (watched == window)
            setGeometry(window->rect());
        update();
        break;
    case QEvent::ChildAdded:
        if (watched == window)
        {
            raiseAboveSiblings();
            // A widget under construction is stacked on top afterwards; raise again then.
            QTimer::singleShot(0, this, [this]() { raiseAboveSiblings(); });
        }
        break;
    case QEvent::Move:
    case QEvent::Show:
    case QEvent::Hide:
    case QEvent::ParentChange:
        // ponytail: repaints everything on any anchor change; per-item old/new rects if overlays get busy.
        if (watched != window)
            update();
        break;
    default:
        break;
    }
    return QWidget::eventFilter(watched, event);
}

void GuideOverlay::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    for (const Entry& e : m_items)
        paintItem(painter, e);
}

GuideOverlay::Entry* GuideOverlay::entry(const QString& id)
{
    for (Entry& e : m_items)
    {
        if (e.item.id == id)
            return &e;
    }
    return nullptr;
}

const GuideOverlay::Entry* GuideOverlay::entry(const QString& id) const
{
    return const_cast<GuideOverlay*>(this)->entry(id);
}

bool GuideOverlay::geometryOf(const Entry& entry, QRectF* rect, QPointF* from, QPointF* to) const
{
    const OverlayItem& it = entry.item;
    QWidget* window = parentWidget();
    QRectF r = it.rect;
    if (it.anchor)
    {
        QWidget* anchor = it.anchor;
        if (!window || (anchor != window && !window->isAncestorOf(anchor)) || !anchor->isVisibleTo(window))
            return false;
        const QRect anchorRect(anchor->mapTo(window, QPoint()), anchor->size());
        // A widget scrolled out of its viewport exists but cannot be seen.
        QRect visible = anchorRect;
        for (QWidget* p = anchor->parentWidget(); p && p != window; p = p->parentWidget())
            visible &= QRect(p->mapTo(window, QPoint()), p->size());
        if (visible.isEmpty())
            return false;
        r = QRectF(anchorRect).adjusted(-it.padding, -it.padding, it.padding, it.padding);
    }
    QPointF a = it.from, b = it.to;
    if (isLine(it.shape))
    {
        if (it.anchor)
            b = QPointF(qBound(r.left(), a.x(), r.right()), qBound(r.top(), a.y(), r.bottom()));
        r = QRectF(a, b).normalized();
    }
    if (rect)
        *rect = r;
    if (from)
        *from = a;
    if (to)
        *to = b;
    return true;
}

QRect GuideOverlay::repaintRect(const Entry& entry) const
{
    if (entry.item.shape == OverlayItem::Shape::Spotlight)
        return rect();
    QRectF r;
    if (!geometryOf(entry, &r, nullptr, nullptr))
        return QRect();
    // Room for the pulse glow and an arrow head.
    const qreal margin = entry.item.strokeWidth * 4 + 16;
    return r.adjusted(-margin, -margin, margin, margin).toAlignedRect();
}

void GuideOverlay::startAnimation(Entry& entry)
{
    setEndState(entry);
    const OverlayAnimation& a = entry.item.animation;
    if (!s_animationsEnabled || a.type == OverlayAnimation::Type::None)
        return;

    QVariantAnimation* animation = new QVariantAnimation(this);
    animation->setStartValue(0.0);
    animation->setEndValue(1.0);
    animation->setDuration(a.periodMs);
    animation->setLoopCount(a.count > 0 ? a.count : -1);
    switch (a.type)
    {
    case OverlayAnimation::Type::Fade:
        animation->setLoopCount(1);
        if (a.fadeOut && entry.item.durationMs > 0)
        {
            // One animation over the item's whole life: in, hold, out.
            const qreal edge = qMin<qreal>(0.5, qreal(a.periodMs) / entry.item.durationMs);
            animation->setDuration(entry.item.durationMs);
            animation->setKeyValueAt(edge, 1.0);
            animation->setKeyValueAt(1 - edge, 1.0);
            animation->setEndValue(0.0);
        }
        break;
    case OverlayAnimation::Type::Pulse:
        animation->setKeyValueAt(0.5, 1.0);
        animation->setEndValue(0.0);
        break;
    case OverlayAnimation::Type::DrawOn:
        animation->setLoopCount(1);
        break;
    default:
        break;
    }

    const QString id = entry.item.id;
    connect(animation, &QVariantAnimation::valueChanged, this, [this, id](const QVariant& value) {
        if (Entry* e = this->entry(id))
        {
            applyAnimationValue(*e, value.toReal());
            update(repaintRect(*e));
        }
    });
    connect(animation, &QVariantAnimation::finished, this, [this, id]() {
        Entry* e = this->entry(id);
        if (!e)
            return;
        // A fade-out ends invisible; its timer removes the item.
        const OverlayAnimation& anim = e->item.animation;
        if (!(anim.type == OverlayAnimation::Type::Fade && anim.fadeOut && e->item.durationMs > 0))
            setEndState(*e);
        update(repaintRect(*e));
    });
    entry.animation = animation;
    animation->start();
}

void GuideOverlay::setEndState(Entry& entry)
{
    entry.opacity = 1;
    entry.extraWidth = 0;
    entry.color = QColor();
    entry.dashOffset = 0;
    entry.progress = 1;
}

void GuideOverlay::applyAnimationValue(Entry& entry, qreal value)
{
    const OverlayAnimation& a = entry.item.animation;
    switch (a.type)
    {
    case OverlayAnimation::Type::Blink:
        entry.opacity = value < 0.5 ? 1 : 0;
        break;
    case OverlayAnimation::Type::Fade:
        entry.opacity = value;
        break;
    case OverlayAnimation::Type::Pulse:
        entry.extraWidth = value * qMax<qreal>(2, entry.item.strokeWidth);
        break;
    case OverlayAnimation::Type::ColorLoop:
    {
        QList<QColor> colors = a.colors;
        if (colors.size() < 2)
            colors = { entry.item.stroke, entry.item.stroke.lighter(170) };
        const qreal pos = value * colors.size();
        const int i = qMin(int(pos), int(colors.size()) - 1);
        entry.color = mix(colors[i], colors[(i + 1) % colors.size()], pos - i);
        break;
    }
    case OverlayAnimation::Type::MarchingAnts:
        entry.dashOffset = -value * kDashCycle;
        break;
    case OverlayAnimation::Type::DrawOn:
        entry.progress = value;
        break;
    default:
        break;
    }
}

void GuideOverlay::paintItem(QPainter& painter, const Entry& entry) const
{
    QRectF r;
    QPointF from, to;
    if (entry.opacity <= 0 || !geometryOf(entry, &r, &from, &to))
        return;
    const OverlayItem& it = entry.item;

    painter.save();
    painter.setOpacity(entry.opacity);
    QPen pen(entry.color.isValid() ? entry.color : it.stroke, it.strokeWidth + entry.extraWidth,
             it.penStyle, Qt::RoundCap, Qt::RoundJoin);
    if (it.animation.type == OverlayAnimation::Type::MarchingAnts)
    {
        pen.setStyle(Qt::DashLine);
        pen.setCapStyle(Qt::FlatCap);
        pen.setDashOffset(entry.dashOffset);
    }

    if (isLine(it.shape))
    {
        const QPointF end = from + (to - from) * entry.progress;
        const qreal headLength = qMax<qreal>(10, pen.widthF() * 4);
        const QLineF back(end, from);
        if (it.shape == OverlayItem::Shape::Arrow && back.length() > headLength)
        {
            QPolygonF head;
            head << end << QLineF::fromPolar(headLength, back.angle() + 25).translated(end).p2()
                 << QLineF::fromPolar(headLength, back.angle() - 25).translated(end).p2();
            painter.setPen(pen);
            // Stop the shaft inside the head so its round cap does not poke through the tip.
            painter.drawLine(from, back.pointAt(headLength * 0.6 / back.length()));
            painter.setPen(Qt::NoPen);
            painter.setBrush(pen.color());
            painter.drawPolygon(head);
        }
        else
        {
            painter.setPen(pen);
            painter.drawLine(from, end);
        }
        painter.restore();
        return;
    }

    QPainterPath path;
    if (it.shape == OverlayItem::Shape::Rect)
        path.addRect(r);
    else if (it.shape == OverlayItem::Shape::Ellipse)
        path.addEllipse(r);
    else
        path.addRoundedRect(r, it.cornerRadius, it.cornerRadius);

    if (it.shape == OverlayItem::Shape::Spotlight)
    {
        QPainterPath dim;
        dim.addRect(QRectF(rect()));
        painter.fillPath(dim.subtracted(path), it.fill.isValid() ? it.fill : QColor(0, 0, 0, 120));
    }
    else if (it.fill.isValid())
    {
        painter.fillPath(path, it.fill);
    }

    if (entry.extraWidth > 0)
    {
        QColor glowColor = pen.color();
        glowColor.setAlphaF(0.3);
        QPen glow = pen;
        glow.setColor(glowColor);
        glow.setWidthF(pen.widthF() + entry.extraWidth * 2);
        painter.strokePath(path, glow);
    }
    if (entry.progress < 1)
    {
        // Draw-on of a closed shape: one dash as long as the drawn part, then a gap longer than the path.
        const qreal length = path.length() / pen.widthF();
        pen.setCapStyle(Qt::FlatCap);
        pen.setDashPattern({ qMax<qreal>(entry.progress * length, 0.01), length + 1 });
    }
    painter.strokePath(path, pen);
    painter.restore();
}

void GuideOverlay::raiseAboveSiblings()
{
    raise();
    // Speech bubbles take input and belong above the overlay.
    for (QWidget* sibling : parentWidget()->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly))
    {
        if (sibling->property("qtllm_aboveOverlay").toBool())
            sibling->raise();
    }
}

void GuideOverlay::refreshAnimations()
{
    for (Entry& e : m_items)
    {
        if (e.animation)
        {
            QVariantAnimation* animation = e.animation;
            e.animation = nullptr;
            animation->stop();
            animation->deleteLater();
        }
        startAnimation(e);
    }
    update();
}

} // namespace QtLLM
