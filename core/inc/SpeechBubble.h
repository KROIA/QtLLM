#pragma once
#include "QtLLM_base.h"
#include <QJsonArray>
#include <QList>
#include <QMap>
#include <QMetaObject>
#include <QObject>
#include <QPoint>
#include <QPointer>
#include <QRect>
#include <QString>
#include <QStringList>
#include <QWidget>

class QHBoxLayout;
class QLabel;
class QPainterPath;
class QPushButton;
class QToolButton;
class QVBoxLayout;

namespace QtLLM
{

// A text box with a tail pointing at a widget (or a fixed point) of its
// window. Child of the target's window; unlike GuideOverlay it takes mouse
// input: drag the body to move it (the tail stays on the target), x closes it.
// Follows the target on resize / move unless the user dragged it, hides while
// the target is hidden and deletes itself when the target is destroyed.
// Owner thread only.
class QT_LLM_API SpeechBubble : public QWidget
{
    Q_OBJECT
public:
    enum class Placement { Auto, Right, Below, Left, Above };

    // target must not be null. text may be rich text. number > 0 shows a numbered badge.
    SpeechBubble(QWidget* target, const QString& text, const QString& title = QString(),
                 int number = 0, Placement placement = Placement::Auto);
    // Anchored to a fixed point in window coordinates of anyWidgetInWindow's window.
    SpeechBubble(QWidget* anyWidgetInWindow, const QPoint& point, const QString& text,
                 const QString& title = QString(), int number = 0, Placement placement = Placement::Auto);

    QString id() const;
    void setId(const QString& id);
    QString text() const;
    void setText(const QString& text);
    QString title() const;
    void setTitle(const QString& title);
    int number() const;
    void setNumber(int number);
    Placement placement() const;
    void setPlacement(Placement placement);
    QWidget* target() const;
    // Points the bubble at another widget of the same window and places it anew.
    void setTarget(QWidget* target);
    // Extra controls under the text (tour buttons); the bubble takes ownership.
    void setFooter(QWidget* footer);
    bool isUserMoved() const;

    // Window coordinates.
    QPoint tailTip() const;
    QRect bubbleRect() const;
    QRect targetRect() const;

    // Places the bubble for the current target geometry (keeps a dragged one where the user put it).
    void reposition();
    // Programmatic close: hides and deletes later, without emitting closed().
    void dismiss();

signals:
    // The user clicked x. The bubble deletes itself afterwards.
    void closed(const QString& id);
    // The user finished dragging the bubble.
    void moved();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    void init(QWidget* window, const QString& text, const QString& title, int number, Placement placement);
    void updateContent();
    QRect placeBox(const QSize& size) const;
    QRect windowArea() const;
    QPoint computeTip(const QRect& box) const;
    QPainterPath shapePath(const QPoint& origin) const;
    void applyGeometry();

    QString m_id;
    QPointer<QWidget> m_target;
    QPoint m_point;               // anchor without a target, window coordinates
    Placement m_placement = Placement::Auto;
    QRect m_box;                  // window coordinates
    QPoint m_tip;
    int m_number = 0;
    bool m_userMoved = false;
    bool m_dragging = false;
    bool m_dragMoved = false;
    bool m_closed = false;
    QPoint m_pressGlobal;
    QPoint m_pressBoxPos;
    QWidget* m_content = nullptr;
    QVBoxLayout* m_layout = nullptr;
    QHBoxLayout* m_header = nullptr;
    QLabel* m_badge = nullptr;
    QLabel* m_titleLabel = nullptr;
    QLabel* m_textLabel = nullptr;
    QToolButton* m_closeButton = nullptr;
    QWidget* m_footer = nullptr;
    QMetaObject::Connection m_targetDestroyed;
};

struct QT_LLM_API TourStep
{
    QPointer<QWidget> target;
    QString text;
    QString title;
};

// The bubbles and tours of one window, addressed by id - the layer a tool
// handler talks to. One per window, created on demand. Owner thread only.
class QT_LLM_API BubbleManager : public QObject
{
    Q_OBJECT
public:
    enum class TourMode { AllAtOnce, OneAtATime };

    static BubbleManager* forWindow(QWidget* anyWidgetInThatWindow);

    // Return the bubble id, or an empty string when the target is not in this
    // window (or the point lies outside it).
    QString showBubble(QWidget* target, const QString& text, const QString& title = QString(),
                       int number = 0, SpeechBubble::Placement placement = SpeechBubble::Placement::Auto);
    QString showBubble(const QPoint& windowPoint, const QString& text, const QString& title = QString(),
                       int number = 0, SpeechBubble::Placement placement = SpeechBubble::Placement::Auto);
    bool closeBubble(const QString& id);
    // Closes every bubble and tour and forgets which ones the user closed.
    void clear();
    QStringList bubbleIds() const;
    SpeechBubble* bubble(const QString& id) const;

    // [{ "kind": "bubble", "id", "status": "shown", "text", "title", "number",
    //    "target": objectName | className, "point": {x, y}, "tour": id },
    //  { "kind": "tour", "id", "status": "shown", "mode": "allAtOnce" | "oneAtATime", "step", "steps" },
    //  { "kind": "bubble", "id", "status": "closedByUser" }]
    QJsonArray describe() const;

    // Steps whose target is gone or in another window are skipped; empty id
    // when none is left. AllAtOnce: bubbles numbered 1..N. OneAtATime: one
    // bubble with Back / Next and "2 / 5"; the current target is highlighted
    // on the GuideOverlay. The tour ends after the last step or when its
    // bubble is closed.
    QString showTour(const QList<TourStep>& steps, TourMode mode = TourMode::AllAtOnce);
    bool closeTour(const QString& id);
    QStringList tourIds() const;
    // Zero-based current step of a OneAtATime tour, -1 for an unknown id.
    int tourStep(const QString& id) const;
    bool setTourStep(const QString& id, int index);

signals:
    void bubbleClosed(const QString& id);   // by the user

private:
    struct Tour
    {
        TourMode mode = TourMode::AllAtOnce;
        QList<TourStep> steps;
        int current = 0;
        QStringList bubbleIds;
        QString highlightId;
        QPointer<QLabel> progress;
        QPointer<QPushButton> back;
        QPointer<QPushButton> next;
    };

    explicit BubbleManager(QWidget* window);
    QString addBubble(SpeechBubble* bubble);
    void forget(const QString& id);

    QPointer<QWidget> m_window;
    QList<QPointer<SpeechBubble>> m_bubbles;   // creation order
    QMap<QString, Tour> m_tours;
    QStringList m_closedByUser;
    int m_nextBubble = 1;
    int m_nextTour = 1;
};

} // namespace QtLLM
