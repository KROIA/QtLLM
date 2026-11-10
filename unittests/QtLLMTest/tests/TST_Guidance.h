#pragma once

// GUI tests of the guidance widgets: GuideOverlay (highlights and shapes over
// a window) and SpeechBubble / BubbleManager (bubbles and tours). A small
// fixture window with named buttons; widgets found by objectName.
#ifdef UNITTEST_GUI_ENABLED

#include "UnitTest.h"
#include "UnitTest_Gui.h"
#include "GuideOverlay.h"
#include "SpeechBubble.h"
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMouseEvent>
#include <QPointer>
#include <QPushButton>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWindow>

class TST_Guidance : public UnitTest::Test
{
    TEST_CLASS(TST_Guidance)
public:
    TST_Guidance()
        : Test("TST_Guidance")
    {
        ADD_TEST(TST_Guidance::testOverlayOnePerWindow);
        ADD_TEST(TST_Guidance::testHighlightFollowsWidget);
        ADD_TEST(TST_Guidance::testItemRemovedWithWidget);
        ADD_TEST(TST_Guidance::testClickPassesThroughOverlay);
        ADD_TEST(TST_Guidance::testItemLimitAndClamps);
        ADD_TEST(TST_Guidance::testTimedItemDisappears);
        ADD_TEST(TST_Guidance::testOverlayJson);
        ADD_TEST(TST_Guidance::testBubbleTailAndPlacement);
        ADD_TEST(TST_Guidance::testBubbleDrag);
        ADD_TEST(TST_Guidance::testBubbleCloseButton);
        ADD_TEST(TST_Guidance::testTourOneAtATime);
        ADD_TEST(TST_Guidance::testTourAllAtOnce);
        ADD_TEST(TST_Guidance::testBubbleInsideSmallWindow);
    }

private:
    using GuideOverlay = QtLLM::GuideOverlay;
    using OverlayItem = QtLLM::OverlayItem;
    using OverlayAnimation = QtLLM::OverlayAnimation;
    using SpeechBubble = QtLLM::SpeechBubble;
    using BubbleManager = QtLLM::BubbleManager;

    // Buttons at their natural width on the left, one on the right that moves
    // with the window width, and a free-positioned button inside a panel.
    struct Fixture
    {
        QWidget window;
        QPushButton* alpha = nullptr;
        QPushButton* beta = nullptr;
        QPushButton* gamma = nullptr;
        QPushButton* right = nullptr;
        QWidget* panel = nullptr;
        QPushButton* free = nullptr;

        Fixture()
        {
            window.setObjectName("guidanceFixture");
            QVBoxLayout* layout = new QVBoxLayout(&window);
            alpha = addButton(layout, "Alpha", "btnAlpha", Qt::AlignLeft);
            beta = addButton(layout, "Beta", "btnBeta", Qt::AlignLeft);
            gamma = addButton(layout, "Gamma", "btnGamma", Qt::AlignLeft);
            right = addButton(layout, "Right", "btnRight", Qt::AlignRight);
            panel = new QWidget(&window);
            panel->setMinimumHeight(80);
            layout->addWidget(panel);
            free = new QPushButton("Free", panel);
            free->setObjectName("btnFree");
            free->setGeometry(10, 10, 80, 28);
            layout->addStretch();
            window.resize(560, 420);
        }

        QPushButton* addButton(QVBoxLayout* layout, const char* text, const char* name, Qt::Alignment alignment)
        {
            QPushButton* button = new QPushButton(QString::fromLatin1(text), &window);
            button->setObjectName(QString::fromLatin1(name));
            layout->addWidget(button, 0, alignment);
            return button;
        }
    };

    static QRect inWindow(QWidget* widget)
    {
        return QRect(widget->mapTo(widget->window(), QPoint()), widget->size());
    }

    template<class T>
    static T* find(const char* name, QWidget* root)
    {
        return UnitTest::Gui::find<T>(QString::fromLatin1(name), root);
    }

    // ------------------------------------------------------------ GuideOverlay

    TEST_FUNCTION(testOverlayOnePerWindow)
    {
        TEST_START;
        if (!UnitTest::Gui::isAvailable()) { TEST_MESSAGE("Skipping: no GUI session"); return; }

        Fixture f;
        TEST_ASSERT(UnitTest::Gui::showAndWait(&f.window));
        GuideOverlay* overlay = GuideOverlay::forWindow(f.alpha);
        TEST_ASSERT_NOT_NULL(overlay);
        if (!overlay) return;
        TEST_ASSERT(GuideOverlay::forWindow(f.free) == overlay);
        TEST_ASSERT(GuideOverlay::forWindow(&f.window) == overlay);
        TEST_ASSERT(overlay->parentWidget() == &f.window);
        TEST_ASSERT(overlay->testAttribute(Qt::WA_TransparentForMouseEvents));
        TEST_COMPARE(overlay->geometry(), f.window.rect());

        QWidget other;
        GuideOverlay* otherOverlay = GuideOverlay::forWindow(&other);
        TEST_ASSERT(otherOverlay != nullptr && otherOverlay != overlay);

        f.window.resize(f.window.width() + 80, f.window.height() + 40);
        TEST_ASSERT(UnitTest::Gui::waitFor([&]() { return overlay->geometry() == f.window.rect(); }));
    }

    TEST_FUNCTION(testHighlightFollowsWidget)
    {
        TEST_START;
        if (!UnitTest::Gui::isAvailable()) { TEST_MESSAGE("Skipping: no GUI session"); return; }

        Fixture f;
        TEST_ASSERT(UnitTest::Gui::showAndWait(&f.window));
        GuideOverlay* overlay = GuideOverlay::forWindow(&f.window);

        // A moved widget.
        OverlayItem item;
        item.anchor = f.free;
        item.padding = 0;
        const QString id = overlay->addItem(item);
        TEST_ASSERT(!id.isEmpty());
        TEST_COMPARE(overlay->itemRect(id), inWindow(f.free));
        f.free->move(120, 30);
        TEST_COMPARE(overlay->itemRect(id), inWindow(f.free));
        TEST_COMPARE(overlay->itemRect(id).topLeft(), f.panel->mapTo(&f.window, QPoint(120, 30)));

        // Hidden: not painted; shown again: back.
        f.free->hide();
        TEST_ASSERT(overlay->itemRect(id).isEmpty());
        f.free->show();
        TEST_COMPARE(overlay->itemRect(id), inWindow(f.free));

        // A layout-managed widget after the window was resized.
        const QString highlight = overlay->highlight(f.right);
        const QRect before = overlay->itemRect(highlight);
        TEST_COMPARE(before, inWindow(f.right).adjusted(-4, -4, 4, 4));
        f.window.resize(f.window.width() + 120, f.window.height() + 60);
        TEST_ASSERT(UnitTest::Gui::waitFor([&]() {
            return overlay->itemRect(highlight) != before
                && overlay->itemRect(highlight) == inWindow(f.right).adjusted(-4, -4, 4, 4);
        }));
    }

    TEST_FUNCTION(testItemRemovedWithWidget)
    {
        TEST_START;
        if (!UnitTest::Gui::isAvailable()) { TEST_MESSAGE("Skipping: no GUI session"); return; }

        Fixture f;
        TEST_ASSERT(UnitTest::Gui::showAndWait(&f.window));
        GuideOverlay* overlay = GuideOverlay::forWindow(&f.window);
        QStringList removed;
        QObject::connect(overlay, &GuideOverlay::itemRemoved, [&](const QString& id) { removed.append(id); });

        QPushButton* temp = new QPushButton("Temporary", f.panel);
        temp->show();
        const QString id = overlay->highlight(temp);
        TEST_ASSERT(overlay->itemIds().contains(id));
        delete temp;
        TEST_ASSERT(!overlay->itemIds().contains(id));
        TEST_COMPARE(removed, QStringList{ id });
    }

    // Sent to the native window, so Qt picks the receiver the way it does for
    // a real click - the overlay on top must not swallow it.
    TEST_FUNCTION(testClickPassesThroughOverlay)
    {
        TEST_START;
        if (!UnitTest::Gui::isAvailable()) { TEST_MESSAGE("Skipping: no GUI session"); return; }

        Fixture f;
        TEST_ASSERT(UnitTest::Gui::showAndWait(&f.window));
        GuideOverlay* overlay = GuideOverlay::forWindow(&f.window);
        OverlayItem spotlight;
        spotlight.shape = OverlayItem::Shape::Spotlight;
        spotlight.anchor = f.alpha;
        TEST_ASSERT(!overlay->addItem(spotlight).isEmpty());
        OverlayItem box;
        box.shape = OverlayItem::Shape::Rect;
        box.rect = QRectF(inWindow(f.beta).adjusted(-10, -10, 10, 10));
        box.fill = QColor(255, 0, 0, 80);
        TEST_ASSERT(!overlay->addItem(box).isEmpty());

        int clicks = 0;
        QObject::connect(f.beta, &QPushButton::clicked, [&]() { ++clicks; });
        const QPoint center = inWindow(f.beta).center();
        TEST_ASSERT(f.window.childAt(center) == f.beta);

        QWindow* handle = f.window.windowHandle();
        TEST_ASSERT_NOT_NULL(handle);
        if (!handle) return;
        const QPointF global = f.window.mapToGlobal(center);
        QMouseEvent press(QEvent::MouseButtonPress, center, center, global,
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QMouseEvent release(QEvent::MouseButtonRelease, center, center, global,
                            Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(handle, &press);
        QCoreApplication::sendEvent(handle, &release);
        TEST_COMPARE(clicks, 1);
    }

    TEST_FUNCTION(testItemLimitAndClamps)
    {
        TEST_START;
        if (!UnitTest::Gui::isAvailable()) { TEST_MESSAGE("Skipping: no GUI session"); return; }

        Fixture f;
        TEST_ASSERT(UnitTest::Gui::showAndWait(&f.window));
        GuideOverlay* overlay = GuideOverlay::forWindow(&f.window);

        QStringList ids;
        for (int i = 0; i < int(GuideOverlay::kMaxItems); ++i)
        {
            OverlayItem item;
            item.rect = QRectF(5 + i * 10, 5, 20, 20);
            ids.append(overlay->addItem(item));
        }
        TEST_COMPARE(overlay->itemIds().size(), 20);
        TEST_ASSERT(!ids.contains(QString()));

        OverlayItem extra;
        extra.rect = QRectF(10, 10, 20, 20);
        TEST_ASSERT(overlay->addItem(extra).isEmpty());
        // Replacing an existing id is not an addition.
        extra.id = ids.first();
        TEST_COMPARE(overlay->addItem(extra), ids.first());
        TEST_COMPARE(overlay->itemIds().size(), 20);

        overlay->clear();
        TEST_ASSERT(overlay->itemIds().isEmpty());

        // No geometry, or an anchor in another window: refused.
        TEST_ASSERT(overlay->addItem(OverlayItem()).isEmpty());
        QWidget otherWindow;
        QPushButton otherButton("Elsewhere", &otherWindow);
        TEST_ASSERT(overlay->highlight(&otherButton).isEmpty());

        OverlayItem blink;
        blink.anchor = f.alpha;
        blink.animation.type = OverlayAnimation::Type::Blink;
        blink.animation.periodMs = 50;
        blink.durationMs = 60 * 60 * 1000;
        const QString id = overlay->addItem(blink);
        TEST_ASSERT(!id.isEmpty());
        TEST_COMPARE(overlay->item(id).animation.periodMs, int(GuideOverlay::kMinBlinkPeriodMs));
        TEST_COMPARE(overlay->item(id).durationMs, int(GuideOverlay::kMaxDurationMs));
    }

    TEST_FUNCTION(testTimedItemDisappears)
    {
        TEST_START;
        if (!UnitTest::Gui::isAvailable()) { TEST_MESSAGE("Skipping: no GUI session"); return; }

        Fixture f;
        TEST_ASSERT(UnitTest::Gui::showAndWait(&f.window));
        GuideOverlay* overlay = GuideOverlay::forWindow(&f.window);
        QStringList removed;
        QObject::connect(overlay, &GuideOverlay::itemRemoved, [&](const QString& id) { removed.append(id); });

        OverlayItem item;
        item.anchor = f.alpha;
        item.durationMs = 200;
        item.animation.type = OverlayAnimation::Type::Fade;
        item.animation.periodMs = 60;
        item.animation.fadeOut = true;
        const QString id = overlay->addItem(item);
        TEST_ASSERT(overlay->itemIds().contains(id));
        TEST_ASSERT(UnitTest::Gui::waitFor([&]() { return !overlay->itemIds().contains(id); }, 3000));
        TEST_COMPARE(removed, QStringList{ id });
    }

    TEST_FUNCTION(testOverlayJson)
    {
        TEST_START;

        const QJsonObject json{
            { "id", "arrow1" },
            { "shape", "arrow" },
            { "stroke", "#ff0000" },
            { "fill", "#8000ff00" },
            { "strokeWidth", 4 },
            { "penStyle", "dash" },
            { "from", QJsonObject{ { "x", 10 }, { "y", 20 } } },
            { "to", QJsonObject{ { "x", 110 }, { "y", 70 } } },
            { "animation", QJsonObject{ { "type", "colorLoop" }, { "periodMs", 800 }, { "count", 3 },
                                        { "colors", QJsonArray{ "red", "#0000ff" } } } },
            { "durationMs", 5000 },
        };
        OverlayItem item;
        QString error;
        TEST_ASSERT(GuideOverlay::overlayItemFromJson(json, &item, &error));
        TEST_COMPARE(item.id, QString("arrow1"));
        TEST_ASSERT(item.shape == OverlayItem::Shape::Arrow);
        TEST_ASSERT(item.stroke == QColor(255, 0, 0));
        TEST_COMPARE(item.fill.alpha(), 0x80);
        TEST_ASSERT(item.strokeWidth == 4);
        TEST_ASSERT(item.penStyle == Qt::DashLine);
        TEST_ASSERT(item.from == QPointF(10, 20));
        TEST_ASSERT(item.to == QPointF(110, 70));
        TEST_ASSERT(item.animation.type == OverlayAnimation::Type::ColorLoop);
        TEST_COMPARE(item.animation.periodMs, 800);
        TEST_COMPARE(item.animation.count, 3);
        TEST_COMPARE(item.animation.colors.size(), 2);
        TEST_COMPARE(item.durationMs, 5000);

        // Round trip.
        const QJsonObject out = GuideOverlay::overlayItemToJson(item);
        OverlayItem again;
        TEST_ASSERT(GuideOverlay::overlayItemFromJson(out, &again, &error));
        TEST_ASSERT(GuideOverlay::overlayItemToJson(again) == out);
        TEST_COMPARE(out["stroke"].toString(), QString("#ff0000"));

        // Absent keys keep what the caller pre-filled; "color" is an alias of "stroke".
        OverlayItem preset;
        preset.strokeWidth = 7;
        TEST_ASSERT(GuideOverlay::overlayItemFromJson(QJsonObject{ { "shape", "ellipse" }, { "color", "blue" } },
                                                      &preset, &error));
        TEST_ASSERT(preset.shape == OverlayItem::Shape::Ellipse);
        TEST_ASSERT(preset.strokeWidth == 7);
        TEST_ASSERT(preset.stroke == QColor(Qt::blue));

        const QJsonObject bad[] = {
            QJsonObject{ { "shape", "hexagon" } },
            QJsonObject{ { "stroke", "notacolor" } },
            QJsonObject{ { "fill", 12 } },
            QJsonObject{ { "strokeWidth", -1 } },
            QJsonObject{ { "strokeWidth", "wide" } },
            QJsonObject{ { "penStyle", "wavy" } },
            QJsonObject{ { "animation", "blink" } },
            QJsonObject{ { "animation", QJsonObject{ { "type", "spin" } } } },
            QJsonObject{ { "animation", QJsonObject{ { "type", "blink" }, { "count", -2 } } } },
            QJsonObject{ { "animation", QJsonObject{ { "type", "colorLoop" }, { "colors", QJsonArray{ "nope" } } } } },
            QJsonObject{ { "rect", QJsonObject{ { "x", 0 }, { "y", 0 }, { "width", 0 }, { "height", 10 } } } },
            QJsonObject{ { "rect", QJsonArray{ 1, 2, 3, 4 } } },
            QJsonObject{ { "from", QJsonObject{ { "x", 1 } } } },
            QJsonObject{ { "durationMs", -5 } },
            QJsonObject{ { "durationMs", 10 * 60 * 1000 } },
            QJsonObject{ { "id", 3 } },
        };
        for (const QJsonObject& input : bad)
        {
            OverlayItem target;
            target.strokeWidth = 9;
            QString message;
            const bool accepted = GuideOverlay::overlayItemFromJson(input, &target, &message);
            TEST_ASSERT_M(!accepted && !message.isEmpty() && target.strokeWidth == 9,
                          "accepted: " + QJsonDocument(input).toJson(QJsonDocument::Compact).toStdString());
        }
    }

    // ------------------------------------------------------------ SpeechBubble

    TEST_FUNCTION(testBubbleTailAndPlacement)
    {
        TEST_START;
        if (!UnitTest::Gui::isAvailable()) { TEST_MESSAGE("Skipping: no GUI session"); return; }

        Fixture f;
        TEST_ASSERT(UnitTest::Gui::showAndWait(&f.window));
        BubbleManager* manager = BubbleManager::forWindow(f.alpha);
        TEST_ASSERT(manager != nullptr && manager == BubbleManager::forWindow(&f.window));
        if (!manager) return;

        const QString id = manager->showBubble(f.alpha, "Click <b>Alpha</b> to start.", "Step one");
        SpeechBubble* bubble = manager->bubble(id);
        TEST_ASSERT_NOT_NULL(bubble);
        if (!bubble) return;
        TEST_ASSERT(UnitTest::Gui::waitFor([&]() { return bubble->isVisible(); }));
        TEST_ASSERT(inWindow(f.alpha).contains(bubble->tailTip()));
        TEST_ASSERT(!bubble->bubbleRect().intersects(inWindow(f.alpha)));
        TEST_ASSERT(f.window.rect().contains(bubble->bubbleRect()));
        TEST_ASSERT(bubble->geometry().contains(bubble->tailTip()));

        // A second bubble keeps clear of both targets.
        SpeechBubble* second = manager->bubble(manager->showBubble(f.beta, "Then Beta."));
        TEST_ASSERT_NOT_NULL(second);
        if (!second) return;
        TEST_ASSERT(!second->bubbleRect().intersects(inWindow(f.alpha)));
        TEST_ASSERT(!second->bubbleRect().intersects(inWindow(f.beta)));
        TEST_ASSERT(!bubble->bubbleRect().intersects(inWindow(f.beta)));

        // Explicit placement.
        SpeechBubble* below = manager->bubble(manager->showBubble(f.free, "Below.", QString(), 0,
                                                                  SpeechBubble::Placement::Below));
        TEST_ASSERT_NOT_NULL(below);
        if (!below) return;
        TEST_ASSERT(below->bubbleRect().top() > inWindow(f.free).bottom());
        TEST_ASSERT(inWindow(f.free).contains(below->tailTip()));

        // Point anchor: the tail ends exactly there.
        SpeechBubble* atPoint = manager->bubble(manager->showBubble(QPoint(400, 300), "Here."));
        TEST_ASSERT_NOT_NULL(atPoint);
        if (!atPoint) return;
        TEST_COMPARE(atPoint->tailTip(), QPoint(400, 300));
        TEST_COMPARE(manager->bubbleIds().size(), 4);
        TEST_COMPARE(manager->describe().size(), 4);
    }

    TEST_FUNCTION(testBubbleDrag)
    {
        TEST_START;
        if (!UnitTest::Gui::isAvailable()) { TEST_MESSAGE("Skipping: no GUI session"); return; }

        Fixture f;
        TEST_ASSERT(UnitTest::Gui::showAndWait(&f.window));
        BubbleManager* manager = BubbleManager::forWindow(&f.window);
        SpeechBubble* bubble = manager->bubble(manager->showBubble(f.alpha, "Drag me somewhere."));
        TEST_ASSERT_NOT_NULL(bubble);
        if (!bubble) return;
        int moves = 0;
        QObject::connect(bubble, &SpeechBubble::moved, [&]() { ++moves; });

        const QRect before = bubble->bubbleRect();
        const QPoint from = before.center() - bubble->pos();
        TEST_ASSERT(UnitTest::Gui::drag(bubble, from, from + QPoint(40, 60)));
        TEST_COMPARE(bubble->bubbleRect().topLeft(), before.topLeft() + QPoint(40, 60));
        TEST_ASSERT(inWindow(f.alpha).contains(bubble->tailTip()));
        TEST_ASSERT(bubble->geometry().contains(bubble->tailTip()));
        TEST_COMPARE(moves, 1);
        TEST_ASSERT(bubble->isUserMoved());

        // Where the user put it survives a window resize.
        const QPoint kept = bubble->bubbleRect().topLeft();
        f.window.resize(f.window.width() + 50, f.window.height());
        TEST_ASSERT(UnitTest::Gui::waitFor([&]() { return f.window.width() >= 610; }));
        TEST_COMPARE(bubble->bubbleRect().topLeft(), kept);
    }

    TEST_FUNCTION(testBubbleCloseButton)
    {
        TEST_START;
        if (!UnitTest::Gui::isAvailable()) { TEST_MESSAGE("Skipping: no GUI session"); return; }

        Fixture f;
        TEST_ASSERT(UnitTest::Gui::showAndWait(&f.window));
        BubbleManager* manager = BubbleManager::forWindow(&f.window);
        const QString id = manager->showBubble(f.alpha, "Close me.");
        QPointer<SpeechBubble> bubble = manager->bubble(id);
        TEST_ASSERT_NOT_NULL(bubble.data());
        if (!bubble) return;
        QStringList closed, reported;
        QObject::connect(bubble.data(), &SpeechBubble::closed, [&](const QString& c) { closed.append(c); });
        QObject::connect(manager, &BubbleManager::bubbleClosed, [&](const QString& c) { reported.append(c); });

        TEST_ASSERT(UnitTest::Gui::click(find<QToolButton>("qtllm_bubbleClose", bubble.data())));
        TEST_COMPARE(closed, QStringList{ id });
        TEST_COMPARE(reported, QStringList{ id });
        TEST_ASSERT(!bubble || !bubble->isVisible());
        TEST_ASSERT(manager->bubbleIds().isEmpty());

        bool listedAsClosed = false;
        for (const QJsonValue& entry : manager->describe())
        {
            listedAsClosed = listedAsClosed
                || (entry["id"].toString() == id && entry["status"].toString() == "closedByUser");
        }
        TEST_ASSERT(listedAsClosed);
        // A program-side close is not reported as the user's.
        TEST_ASSERT(manager->closeBubble(manager->showBubble(f.beta, "Gone soon.")));
        TEST_COMPARE(reported.size(), 1);
    }

    TEST_FUNCTION(testTourOneAtATime)
    {
        TEST_START;
        if (!UnitTest::Gui::isAvailable()) { TEST_MESSAGE("Skipping: no GUI session"); return; }

        Fixture f;
        TEST_ASSERT(UnitTest::Gui::showAndWait(&f.window));
        BubbleManager* manager = BubbleManager::forWindow(&f.window);
        GuideOverlay* overlay = GuideOverlay::forWindow(&f.window);
        QList<QtLLM::TourStep> steps;
        steps.append(QtLLM::TourStep{ f.alpha, "First Alpha.", "One" });
        steps.append(QtLLM::TourStep{ f.beta, "Then Beta.", "Two" });
        steps.append(QtLLM::TourStep{ f.gamma, "Finally Gamma.", QString() });

        const QString tour = manager->showTour(steps, BubbleManager::TourMode::OneAtATime);
        TEST_ASSERT(!tour.isEmpty());
        TEST_COMPARE(manager->bubbleIds().size(), 1);
        SpeechBubble* bubble = manager->bubble(manager->bubbleIds().value(0));
        auto* next = find<QPushButton>("qtllm_tourNext", &f.window);
        auto* back = find<QPushButton>("qtllm_tourBack", &f.window);
        auto* progress = find<QLabel>("qtllm_tourProgress", &f.window);
        TEST_ASSERT(bubble && next && back && progress);
        if (!bubble || !next || !back || !progress) return;
        auto highlighted = [&]() {
            const QStringList ids = overlay->itemIds();
            return ids.size() == 1 ? overlay->itemRect(ids.first()) : QRect();
        };

        TEST_COMPARE(progress->text(), QString("1 / 3"));
        TEST_ASSERT(!back->isEnabled());
        TEST_COMPARE(bubble->number(), 1);
        TEST_ASSERT(highlighted().contains(inWindow(f.alpha)));
        TEST_ASSERT(inWindow(f.alpha).contains(bubble->tailTip()));

        TEST_ASSERT(UnitTest::Gui::click(next));
        TEST_COMPARE(manager->tourStep(tour), 1);
        TEST_COMPARE(progress->text(), QString("2 / 3"));
        TEST_COMPARE(bubble->text(), QString("Then Beta."));
        TEST_COMPARE(bubble->number(), 2);
        TEST_ASSERT(back->isEnabled());
        TEST_ASSERT(highlighted().contains(inWindow(f.beta)));
        TEST_ASSERT(!highlighted().contains(inWindow(f.alpha)));
        TEST_ASSERT(inWindow(f.beta).contains(bubble->tailTip()));
        TEST_ASSERT(!bubble->bubbleRect().intersects(inWindow(f.beta)));

        TEST_ASSERT(UnitTest::Gui::click(back));
        TEST_COMPARE(manager->tourStep(tour), 0);
        TEST_ASSERT(highlighted().contains(inWindow(f.alpha)));

        TEST_ASSERT(UnitTest::Gui::click(next));
        TEST_ASSERT(UnitTest::Gui::click(next));
        TEST_COMPARE(progress->text(), QString("3 / 3"));
        TEST_COMPARE(next->text(), QString("Done"));
        TEST_ASSERT(inWindow(f.gamma).contains(bubble->tailTip()));

        TEST_ASSERT(UnitTest::Gui::click(next));   // Done ends the tour; the bubble goes away
        TEST_ASSERT(manager->tourIds().isEmpty());
        TEST_ASSERT(manager->bubbleIds().isEmpty());
        TEST_ASSERT(overlay->itemIds().isEmpty());
    }

    TEST_FUNCTION(testTourAllAtOnce)
    {
        TEST_START;
        if (!UnitTest::Gui::isAvailable()) { TEST_MESSAGE("Skipping: no GUI session"); return; }

        Fixture f;
        TEST_ASSERT(UnitTest::Gui::showAndWait(&f.window));
        BubbleManager* manager = BubbleManager::forWindow(&f.window);
        const QList<QPushButton*> targets{ f.alpha, f.beta, f.gamma };
        QList<QtLLM::TourStep> steps;
        for (QPushButton* target : targets)
            steps.append(QtLLM::TourStep{ target, "Press " + target->text() + ".", QString() });

        const QString tour = manager->showTour(steps, BubbleManager::TourMode::AllAtOnce);
        TEST_ASSERT(!tour.isEmpty());
        const QStringList ids = manager->bubbleIds();
        TEST_COMPARE(ids.size(), 3);
        if (ids.size() != 3) return;
        for (int i = 0; i < ids.size(); ++i)
        {
            SpeechBubble* bubble = manager->bubble(ids[i]);
            TEST_ASSERT_NOT_NULL(bubble);
            if (!bubble) return;
            TEST_COMPARE(bubble->number(), i + 1);
            TEST_ASSERT(bubble->isVisible());
            TEST_ASSERT(inWindow(targets[i]).contains(bubble->tailTip()));
            for (QPushButton* target : targets)
                TEST_ASSERT(!bubble->bubbleRect().intersects(inWindow(target)));
        }

        TEST_ASSERT(manager->closeTour(tour));
        TEST_ASSERT(manager->bubbleIds().isEmpty());
        TEST_ASSERT(manager->tourIds().isEmpty());
    }

    TEST_FUNCTION(testBubbleInsideSmallWindow)
    {
        TEST_START;
        if (!UnitTest::Gui::isAvailable()) { TEST_MESSAGE("Skipping: no GUI session"); return; }

        QWidget window;
        QVBoxLayout* layout = new QVBoxLayout(&window);
        QPushButton* go = new QPushButton("Go", &window);
        go->setObjectName("btnGo");
        layout->addWidget(go, 0, Qt::AlignCenter);
        window.resize(260, 200);
        TEST_ASSERT(UnitTest::Gui::showAndWait(&window));

        BubbleManager* manager = BubbleManager::forWindow(&window);
        SpeechBubble* bubble = manager->bubble(manager->showBubble(go,
            "A longer explanation that has to wrap over several lines in a small window.", "Title"));
        TEST_ASSERT_NOT_NULL(bubble);
        if (!bubble) return;
        TEST_ASSERT(window.rect().contains(bubble->bubbleRect()));
        TEST_ASSERT(inWindow(go).contains(bubble->tailTip()));

        window.resize(220, 240);
        TEST_ASSERT(UnitTest::Gui::waitFor([&]() {
            return window.width() == 220 && window.rect().contains(bubble->bubbleRect());
        }));
        TEST_ASSERT(inWindow(go).contains(bubble->tailTip()));
    }
};

TEST_INSTANTIATE(TST_Guidance);

#endif // UNITTEST_GUI_ENABLED
