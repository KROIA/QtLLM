#include "SpeechBubble.h"
#include "GuideOverlay.h"

#include <QApplication>
#include <QBitmap>
#include <QEvent>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QTextDocument>
#include <QToolButton>
#include <QVBoxLayout>

#include <cmath>

namespace QtLLM
{

namespace
{

constexpr int kMargin = 6;       // kept free between bubble and window edge
constexpr int kGap = 16;         // between box and target; the tail spans it
constexpr int kTipInset = 4;     // the tail ends this far inside the target
constexpr int kTailBase = 16;
constexpr qreal kRadius = 8;
constexpr int kEdge = 3;         // room for the antialiased border around the shape
constexpr int kMinWidth = 140;
constexpr int kMaxWidth = 320;

QPoint globalPosOf(const QMouseEvent* event)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    return event->globalPosition().toPoint();
#else
    return event->globalPos();
#endif
}

QPoint localPosOf(const QMouseEvent* event)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    return event->position().toPoint();
#else
    return event->pos();
#endif
}

QRect clampInto(QRect box, const QRect& area)
{
    box.moveLeft(qBound(area.left(), box.left(), qMax(area.left(), area.right() - box.width() + 1)));
    box.moveTop(qBound(area.top(), box.top(), qMax(area.top(), area.bottom() - box.height() + 1)));
    return box;
}

} // namespace

// ------------------------------------------------------------------ SpeechBubble

SpeechBubble::SpeechBubble(QWidget* target, const QString& text, const QString& title, int number, Placement placement)
    : QWidget(target ? target->window() : nullptr)
{
    init(target ? target->window() : nullptr, text, title, number, placement);
    setTarget(target);
}

SpeechBubble::SpeechBubble(QWidget* anyWidgetInWindow, const QPoint& point, const QString& text,
                           const QString& title, int number, Placement placement)
    : QWidget(anyWidgetInWindow ? anyWidgetInWindow->window() : nullptr)
    , m_point(point)
{
    init(parentWidget(), text, title, number, placement);
    setTarget(nullptr);
}

void SpeechBubble::init(QWidget* window, const QString& text, const QString& title, int number, Placement placement)
{
    setObjectName("qtllm_speechBubble");
    // GuideOverlay keeps widgets with this property above itself.
    setProperty("qtllm_aboveOverlay", true);
    m_placement = placement;
    m_number = number;

    m_content = new QWidget(this);
    m_layout = new QVBoxLayout(m_content);
    m_layout->setSizeConstraint(QLayout::SetNoConstraint);
    m_layout->setContentsMargins(10, 6, 6, 10);
    m_layout->setSpacing(4);

    m_header = new QHBoxLayout();
    m_header->setSpacing(6);
    // The bubble has a fixed light background, so its text must not follow a
    // dark application palette.
    QPalette cardPalette = palette();
    for (QPalette::ColorRole role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText}) {
        cardPalette.setColor(role, QColor(0x1a, 0x1a, 0x1a));
        cardPalette.setColor(QPalette::Disabled, role, QColor(0x80, 0x80, 0x80));
    }
    // Controls on it (tour buttons, check boxes, line edits) stay light as well.
    cardPalette.setColor(QPalette::Button, QColor(0xe1, 0xe1, 0xe1));
    cardPalette.setColor(QPalette::Base, Qt::white);
    cardPalette.setColor(QPalette::Window, QColor(0xf0, 0xf0, 0xf0));
    setPalette(cardPalette);
    m_badge = new QLabel(m_content);
    m_badge->setObjectName("qtllm_bubbleNumber");
    m_badge->setAlignment(Qt::AlignCenter);
    m_badge->setMinimumWidth(18);
    m_badge->setFixedHeight(18);
    m_badge->setStyleSheet("QLabel { background-color: #2980b9; color: white; border-radius: 9px;"
                           " font-weight: bold; padding: 0 4px; }");
    m_titleLabel = new QLabel(m_content);
    m_titleLabel->setObjectName("qtllm_bubbleTitle");
    m_titleLabel->setStyleSheet("QLabel { font-weight: bold; }");
    m_closeButton = new QToolButton(m_content);
    m_closeButton->setObjectName("qtllm_bubbleClose");
    m_closeButton->setText(QString(QChar(0x00D7)));
    m_closeButton->setToolTip(tr("Close"));
    m_closeButton->setAutoRaise(true);
    m_header->addWidget(m_badge);
    m_header->addWidget(m_titleLabel);
    m_header->addStretch();
    m_header->addWidget(m_closeButton, 0, Qt::AlignTop);
    m_layout->addLayout(m_header);

    m_textLabel = new QLabel(m_content);
    m_textLabel->setObjectName("qtllm_bubbleText");
    m_textLabel->setWordWrap(true);
    m_layout->addWidget(m_textLabel);

    // Pressing on text starts a drag of the whole bubble.
    for (QLabel* label : { m_badge, m_titleLabel, m_textLabel })
        label->setAttribute(Qt::WA_TransparentForMouseEvents);

    m_badge->setText(QString::number(number));
    m_titleLabel->setText(title);
    m_textLabel->setText(text);

    connect(m_closeButton, &QToolButton::clicked, this, [this]() {
        dismiss();
        emit closed(m_id);
    });

    if (window)
        window->installEventFilter(this);
}

QString SpeechBubble::id() const
{
    return m_id;
}

void SpeechBubble::setId(const QString& id)
{
    m_id = id;
}

QString SpeechBubble::text() const
{
    return m_textLabel->text();
}

void SpeechBubble::setText(const QString& text)
{
    m_textLabel->setText(text);
    updateContent();
}

QString SpeechBubble::title() const
{
    return m_titleLabel->text();
}

void SpeechBubble::setTitle(const QString& title)
{
    m_titleLabel->setText(title);
    updateContent();
}

int SpeechBubble::number() const
{
    return m_number;
}

void SpeechBubble::setNumber(int number)
{
    m_number = number;
    m_badge->setText(QString::number(number));
    updateContent();
}

SpeechBubble::Placement SpeechBubble::placement() const
{
    return m_placement;
}

void SpeechBubble::setPlacement(Placement placement)
{
    m_placement = placement;
    m_userMoved = false;
    reposition();
}

QWidget* SpeechBubble::target() const
{
    return m_target;
}

void SpeechBubble::setTarget(QWidget* target)
{
    QWidget* window = parentWidget();
    if (target && target->window() != window)
        return;
    for (QWidget* w = m_target; w && w != window; w = w->parentWidget())
        w->removeEventFilter(this);
    disconnect(m_targetDestroyed);

    m_target = target;
    if (target)
    {
        m_targetDestroyed = connect(target, &QObject::destroyed, this, [this]() { dismiss(); });
        // Ancestors too: a scroll area moves the parent, not the target.
        for (QWidget* w = target; w && w != window; w = w->parentWidget())
            w->installEventFilter(this);
    }
    m_userMoved = false;
    updateContent();
    raise();
}

void SpeechBubble::setFooter(QWidget* footer)
{
    delete m_footer;
    m_footer = footer;
    if (footer)
    {
        footer->setParent(m_content);
        m_layout->addWidget(footer);
        // A widget given to a parent that is already on screen stays hidden
        // until it is shown explicitly.
        footer->show();
    }
    updateContent();
}

bool SpeechBubble::isUserMoved() const
{
    return m_userMoved;
}

QPoint SpeechBubble::tailTip() const
{
    return m_tip;
}

QRect SpeechBubble::bubbleRect() const
{
    return m_box;
}

QRect SpeechBubble::targetRect() const
{
    QWidget* window = parentWidget();
    if (m_target && window && (m_target == window || window->isAncestorOf(m_target)))
        return QRect(m_target->mapTo(window, QPoint()), m_target->size());
    return QRect(m_point, QSize(1, 1));
}

void SpeechBubble::reposition()
{
    QWidget* window = parentWidget();
    if (!window || m_closed || m_box.isEmpty())
        return;
    if (m_target && !m_target->isVisibleTo(window))
    {
        hide();
        return;
    }
    if (m_userMoved)
        m_box = clampInto(m_box, windowArea());
    else
        m_box = placeBox(m_box.size());
    applyGeometry();
    show();
}

void SpeechBubble::dismiss()
{
    if (m_closed)
        return;
    m_closed = true;
    hide();
    deleteLater();
}

bool SpeechBubble::eventFilter(QObject* watched, QEvent* event)
{
    switch (event->type())
    {
    case QEvent::Resize:
        // The window's width caps the bubble's width.
        if (watched == parentWidget())
            updateContent();
        else
            reposition();
        break;
    case QEvent::Move:
    case QEvent::Show:
    case QEvent::Hide:
        if (watched != parentWidget())
            reposition();
        break;
    default:
        break;
    }
    return QWidget::eventFilter(watched, event);
}

void SpeechBubble::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(QColor(0x7B, 0xA7, 0xD7), 1.5));
    painter.setBrush(QColor(0xF0, 0xF6, 0xFF));
    painter.drawPath(shapePath(pos()));
}

void SpeechBubble::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton || !m_box.contains(localPosOf(event) + pos()))
    {
        event->ignore();
        return;
    }
    m_dragging = true;
    m_dragMoved = false;
    // Global positions: the bubble moves under the cursor while dragging.
    m_pressGlobal = globalPosOf(event);
    m_pressBoxPos = m_box.topLeft();
    event->accept();
}

void SpeechBubble::mouseMoveEvent(QMouseEvent* event)
{
    if (!m_dragging)
    {
        event->ignore();
        return;
    }
    const QPoint delta = globalPosOf(event) - m_pressGlobal;
    if (!m_dragMoved && delta.manhattanLength() < QApplication::startDragDistance())
        return;
    m_dragMoved = true;
    m_userMoved = true;
    m_box = clampInto(QRect(m_pressBoxPos + delta, m_box.size()), windowArea());
    applyGeometry();
}

void SpeechBubble::mouseReleaseEvent(QMouseEvent* event)
{
    if (!m_dragging)
    {
        event->ignore();
        return;
    }
    m_dragging = false;
    if (m_dragMoved)
        emit moved();
}

void SpeechBubble::updateContent()
{
    m_badge->setVisible(m_number > 0);
    m_titleLabel->setVisible(!m_titleLabel->text().isEmpty());

    // As wide as the unwrapped text, within limits and never wider than the window.
    QTextDocument doc;
    doc.setDefaultFont(m_textLabel->font());
    if (Qt::mightBeRichText(m_textLabel->text()))
        doc.setHtml(m_textLabel->text());
    else
        doc.setPlainText(m_textLabel->text());
    const QMargins margins = m_layout->contentsMargins();
    int width = qMax(int(std::ceil(doc.idealWidth())), m_header->sizeHint().width());
    if (m_footer)
        width = qMax(width, m_footer->sizeHint().width());
    width = qBound(kMinWidth, width + margins.left() + margins.right(), kMaxWidth);
    if (parentWidget())
        width = qMax(60, qMin(width, parentWidget()->width() - 2 * kMargin));

    m_layout->invalidate();
    int height = m_layout->totalHeightForWidth(width);
    if (height <= 0)
        height = m_layout->totalSizeHint().height();
    m_content->setFixedSize(width, height);
    m_box.setSize(QSize(width, height));
    reposition();
}

QRect SpeechBubble::windowArea() const
{
    return parentWidget() ? parentWidget()->rect().adjusted(kMargin, kMargin, -kMargin, -kMargin) : QRect();
}

QRect SpeechBubble::placeBox(const QSize& size) const
{
    QWidget* window = parentWidget();
    const QRect area = windowArea();
    const QRect t = targetRect();
    auto candidate = [&](Placement placement) {
        QRect box(QPoint(), size);
        switch (placement)
        {
        case Placement::Left:
            box.moveTopLeft(QPoint(t.left() - kGap - size.width(), t.center().y() - size.height() / 2));
            break;
        case Placement::Below:
            box.moveTopLeft(QPoint(t.center().x() - size.width() / 2, t.bottom() + 1 + kGap));
            break;
        case Placement::Above:
            box.moveTopLeft(QPoint(t.center().x() - size.width() / 2, t.top() - kGap - size.height()));
            break;
        default:
            box.moveTopLeft(QPoint(t.right() + 1 + kGap, t.center().y() - size.height() / 2));
            break;
        }
        // Slide along the target's side to stay inside the window.
        const QRect inside = clampInto(box, area);
        if (placement == Placement::Left || placement == Placement::Right || placement == Placement::Auto)
            box.moveTop(inside.top());
        else
            box.moveLeft(inside.left());
        return box;
    };
    if (m_placement != Placement::Auto)
        return clampInto(candidate(m_placement), area);

    QList<QRect> otherTargets, otherBoxes;
    for (SpeechBubble* other : window->findChildren<SpeechBubble*>(QString(), Qt::FindDirectChildrenOnly))
    {
        if (other == this || other->m_closed || other->m_box.isEmpty()
            || (other->m_target && !other->m_target->isVisibleTo(window)))
            continue;
        otherTargets.append(other->targetRect());
        otherBoxes.append(other->m_box);
    }
    auto overlaps = [](const QRect& box, const QList<QRect>& rects) {
        for (const QRect& r : rects)
        {
            if (box.intersects(r))
                return true;
        }
        return false;
    };

    // Ideally clear of everything; failing that, never over another bubble's
    // target; failing that, at least inside the window and off our own target.
    const Placement order[] = { Placement::Right, Placement::Below, Placement::Left, Placement::Above };
    for (int strictness = 2; strictness >= 0; --strictness)
    {
        for (Placement placement : order)
        {
            const QRect box = candidate(placement);
            if (!area.contains(box) || box.intersects(t)
                || (strictness >= 1 && overlaps(box, otherTargets))
                || (strictness >= 2 && overlaps(box, otherBoxes)))
                continue;
            return box;
        }
    }
    return clampInto(candidate(Placement::Right), area);
}

QPoint SpeechBubble::computeTip(const QRect& box) const
{
    if (!m_target)
        return m_point;
    const QRect t = targetRect();
    const int ix = qMin(kTipInset, t.width() / 4);
    const int iy = qMin(kTipInset, t.height() / 4);
    const QRect inner = t.adjusted(ix, iy, -ix, -iy);
    const QPoint c = box.center();
    return QPoint(qBound(inner.left(), c.x(), inner.right()), qBound(inner.top(), c.y(), inner.bottom()));
}

QPainterPath SpeechBubble::shapePath(const QPoint& origin) const
{
    const QRectF box = QRectF(m_box.translated(-origin));
    const QPointF tip = QPointF(m_tip - origin);
    QPainterPath path;
    path.addRoundedRect(box, kRadius, kRadius);
    if (box.contains(tip))
        return path;

    // The tail leaves the box edge the tip is furthest outside of.
    const qreal half = kTailBase / 2.0;
    const qreal dx = tip.x() < box.left() ? box.left() - tip.x() : (tip.x() > box.right() ? tip.x() - box.right() : 0);
    const qreal dy = tip.y() < box.top() ? box.top() - tip.y() : (tip.y() > box.bottom() ? tip.y() - box.bottom() : 0);
    QPointF a, b;
    if (dx >= dy)
    {
        const qreal x = tip.x() < box.left() ? box.left() + 1 : box.right() - 1;
        const qreal y = box.height() > 2 * (kRadius + half)
            ? qBound(box.top() + kRadius + half, tip.y(), box.bottom() - kRadius - half) : box.center().y();
        a = QPointF(x, y - half);
        b = QPointF(x, y + half);
    }
    else
    {
        const qreal y = tip.y() < box.top() ? box.top() + 1 : box.bottom() - 1;
        const qreal x = box.width() > 2 * (kRadius + half)
            ? qBound(box.left() + kRadius + half, tip.x(), box.right() - kRadius - half) : box.center().x();
        a = QPointF(x - half, y);
        b = QPointF(x + half, y);
    }
    QPainterPath tail;
    tail.moveTo(a);
    tail.lineTo(tip);
    tail.lineTo(b);
    tail.closeSubpath();
    return path.united(tail);
}

void SpeechBubble::applyGeometry()
{
    m_tip = computeTip(m_box);
    const QRect bounds = m_box.united(QRect(m_tip, QSize(1, 1))).adjusted(-kEdge, -kEdge, kEdge, kEdge);
    setGeometry(bounds);
    m_content->move(m_box.topLeft() - bounds.topLeft());

    // Clicks outside the drawn shape reach the widgets underneath; the thick
    // pen keeps the antialiased border inside the mask.
    QBitmap mask(bounds.size());
    mask.fill(Qt::color0);
    QPainter painter(&mask);
    painter.setPen(QPen(Qt::color1, 2 * kEdge));
    painter.setBrush(Qt::color1);
    painter.drawPath(shapePath(bounds.topLeft()));
    painter.end();
    setMask(mask);
    update();
}

// ------------------------------------------------------------------ BubbleManager

BubbleManager::BubbleManager(QWidget* window)
    : QObject(window)
    , m_window(window)
{
    setObjectName("qtllm_bubbleManager");
}

BubbleManager* BubbleManager::forWindow(QWidget* anyWidgetInThatWindow)
{
    if (!anyWidgetInThatWindow)
        return nullptr;
    QWidget* window = anyWidgetInThatWindow->window();
    if (BubbleManager* existing = window->findChild<BubbleManager*>(QString(), Qt::FindDirectChildrenOnly))
        return existing;
    return new BubbleManager(window);
}

QString BubbleManager::showBubble(QWidget* target, const QString& text, const QString& title, int number,
                                  SpeechBubble::Placement placement)
{
    if (!target || !m_window || target->window() != m_window)
        return QString();
    return addBubble(new SpeechBubble(target, text, title, number, placement));
}

QString BubbleManager::showBubble(const QPoint& windowPoint, const QString& text, const QString& title, int number,
                                  SpeechBubble::Placement placement)
{
    if (!m_window || !m_window->rect().contains(windowPoint))
        return QString();
    return addBubble(new SpeechBubble(m_window, windowPoint, text, title, number, placement));
}

bool BubbleManager::closeBubble(const QString& id)
{
    SpeechBubble* b = bubble(id);
    if (!b)
        return false;
    b->dismiss();
    forget(id);
    return true;
}

void BubbleManager::clear()
{
    for (const QString& id : tourIds())
        closeTour(id);
    for (const QString& id : bubbleIds())
        closeBubble(id);
    m_closedByUser.clear();
}

QStringList BubbleManager::bubbleIds() const
{
    QStringList ids;
    for (const QPointer<SpeechBubble>& b : m_bubbles)
    {
        if (b)
            ids.append(b->id());
    }
    return ids;
}

SpeechBubble* BubbleManager::bubble(const QString& id) const
{
    for (const QPointer<SpeechBubble>& b : m_bubbles)
    {
        if (b && b->id() == id)
            return b;
    }
    return nullptr;
}

QJsonArray BubbleManager::describe() const
{
    QJsonArray result;
    for (const QPointer<SpeechBubble>& b : m_bubbles)
    {
        if (!b)
            continue;
        QJsonObject entry{ { "kind", "bubble" }, { "id", b->id() }, { "status", "shown" }, { "text", b->text() } };
        if (!b->title().isEmpty())
            entry["title"] = b->title();
        if (b->number() > 0)
            entry["number"] = b->number();
        if (QWidget* target = b->target())
        {
            entry["target"] = target->objectName().isEmpty()
                ? QString::fromLatin1(target->metaObject()->className()) : target->objectName();
        }
        else
        {
            const QPoint p = b->targetRect().topLeft();
            entry["point"] = QJsonObject{ { "x", p.x() }, { "y", p.y() } };
        }
        for (auto it = m_tours.cbegin(); it != m_tours.cend(); ++it)
        {
            if (it->bubbleIds.contains(b->id()))
                entry["tour"] = it.key();
        }
        result.append(entry);
    }
    for (auto it = m_tours.cbegin(); it != m_tours.cend(); ++it)
    {
        result.append(QJsonObject{
            { "kind", "tour" }, { "id", it.key() }, { "status", "shown" },
            { "mode", it->mode == TourMode::OneAtATime ? "oneAtATime" : "allAtOnce" },
            { "step", it->current + 1 }, { "steps", int(it->steps.size()) } });
    }
    for (const QString& id : m_closedByUser)
        result.append(QJsonObject{ { "kind", "bubble" }, { "id", id }, { "status", "closedByUser" } });
    return result;
}

QString BubbleManager::showTour(const QList<TourStep>& steps, TourMode mode)
{
    QList<TourStep> valid;
    for (const TourStep& step : steps)
    {
        if (step.target && m_window && step.target->window() == m_window)
            valid.append(step);
    }
    if (valid.isEmpty())
        return QString();

    const QString tourId = QString("tour_%1").arg(m_nextTour++);
    Tour tour;
    tour.mode = mode;
    tour.steps = valid;

    if (mode == TourMode::AllAtOnce)
    {
        for (int i = 0; i < valid.size(); ++i)
            tour.bubbleIds.append(addBubble(new SpeechBubble(valid[i].target, valid[i].text, valid[i].title, i + 1)));
        m_tours.insert(tourId, tour);
        // Placed one by one, earlier bubbles could not yet avoid later targets.
        for (const QString& id : tour.bubbleIds)
        {
            if (SpeechBubble* b = bubble(id))
                b->reposition();
        }
        return tourId;
    }

    QWidget* footer = new QWidget();
    QHBoxLayout* row = new QHBoxLayout(footer);
    row->setContentsMargins(0, 4, 0, 0);
    tour.back = new QPushButton(tr("Back"), footer);
    tour.back->setObjectName("qtllm_tourBack");
    tour.progress = new QLabel(footer);
    tour.progress->setObjectName("qtllm_tourProgress");
    tour.next = new QPushButton(tr("Next"), footer);
    tour.next->setObjectName("qtllm_tourNext");
    row->addWidget(tour.back);
    row->addStretch();
    row->addWidget(tour.progress);
    row->addStretch();
    row->addWidget(tour.next);
    connect(tour.back, &QPushButton::clicked, this, [this, tourId]() { setTourStep(tourId, tourStep(tourId) - 1); });
    connect(tour.next, &QPushButton::clicked, this, [this, tourId]() {
        const int step = tourStep(tourId);
        if (step + 1 >= m_tours.value(tourId).steps.size())
            closeTour(tourId);
        else
            setTourStep(tourId, step + 1);
    });

    SpeechBubble* b = new SpeechBubble(valid[0].target, valid[0].text, valid[0].title, 1);
    b->setFooter(footer);
    tour.bubbleIds.append(addBubble(b));
    m_tours.insert(tourId, tour);
    setTourStep(tourId, 0);
    return tourId;
}

bool BubbleManager::closeTour(const QString& id)
{
    const auto it = m_tours.find(id);
    if (it == m_tours.end())
        return false;
    const Tour tour = it.value();
    m_tours.erase(it);
    if (!tour.highlightId.isEmpty() && m_window)
        GuideOverlay::forWindow(m_window)->removeItem(tour.highlightId);
    for (const QString& bubbleId : tour.bubbleIds)
        closeBubble(bubbleId);
    return true;
}

QStringList BubbleManager::tourIds() const
{
    return m_tours.keys();
}

int BubbleManager::tourStep(const QString& id) const
{
    const auto it = m_tours.constFind(id);
    return it == m_tours.cend() ? -1 : it->current;
}

bool BubbleManager::setTourStep(const QString& id, int index)
{
    const auto it = m_tours.find(id);
    if (it == m_tours.end() || it->mode != TourMode::OneAtATime || index < 0 || index >= it->steps.size())
        return false;
    SpeechBubble* b = bubble(it->bubbleIds.value(0));
    const TourStep step = it->steps[index];
    if (!b || !step.target)
    {
        closeTour(id);
        return false;
    }
    it->current = index;
    const int count = int(it->steps.size());
    if (it->progress)
        it->progress->setText(QString("%1 / %2").arg(index + 1).arg(count));
    if (it->back)
        it->back->setEnabled(index > 0);
    if (it->next)
        it->next->setText(index + 1 == count ? tr("Done") : tr("Next"));

    GuideOverlay* overlay = GuideOverlay::forWindow(m_window);
    if (!it->highlightId.isEmpty())
        overlay->removeItem(it->highlightId);
    it->highlightId = overlay->highlight(step.target);

    b->setTitle(step.title);
    b->setText(step.text);
    b->setNumber(index + 1);
    b->setTarget(step.target);
    return true;
}

QString BubbleManager::addBubble(SpeechBubble* bubble)
{
    const QString id = QString("bubble_%1").arg(m_nextBubble++);
    bubble->setId(id);
    connect(bubble, &SpeechBubble::closed, this, [this](const QString& closedId) {
        m_closedByUser.append(closedId);
        // Bounded: the list is reported to the model on request.
        while (m_closedByUser.size() > 50)
            m_closedByUser.removeFirst();
        emit bubbleClosed(closedId);
        forget(closedId);
    });
    connect(bubble, &QObject::destroyed, this, [this, id]() { forget(id); });
    m_bubbles.append(bubble);

    // Move earlier bubbles off the new target, where they were placed without knowing it.
    const QRect target = bubble->targetRect();
    for (const QPointer<SpeechBubble>& other : m_bubbles)
    {
        if (other && other != bubble && !other->isUserMoved() && other->bubbleRect().intersects(target))
            other->reposition();
    }
    return id;
}

void BubbleManager::forget(const QString& id)
{
    for (int i = m_bubbles.size() - 1; i >= 0; --i)
    {
        if (!m_bubbles[i] || m_bubbles[i]->id() == id)
            m_bubbles.removeAt(i);
    }
    for (auto it = m_tours.begin(); it != m_tours.end(); ++it)
    {
        if (!it->bubbleIds.contains(id))
            continue;
        if (it->mode == TourMode::OneAtATime)
        {
            const QString tourId = it.key();
            closeTour(tourId);
        }
        else
        {
            it->bubbleIds.removeAll(id);
            if (it->bubbleIds.isEmpty())
                m_tours.erase(it);
        }
        return;
    }
}

} // namespace QtLLM
