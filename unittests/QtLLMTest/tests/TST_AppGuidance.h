#pragma once

// Guidance tools of AppInterface: speech bubbles and tours as the model calls them, behind their capability switches.
#ifdef UNITTEST_GUI_ENABLED

#include "UnitTest.h"
#include "UnitTest_Gui.h"
#include "QtLLM.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPushButton>
#include <QVBoxLayout>
#include <QWidget>

class TST_AppGuidance : public UnitTest::Test
{
    TEST_CLASS(TST_AppGuidance)
public:
    TST_AppGuidance()
        : Test("TST_AppGuidance")
    {
        ADD_TEST(TST_AppGuidance::testToolsFollowCapabilities);
        ADD_TEST(TST_AppGuidance::testBubbleAndTourTools);
        ADD_TEST(TST_AppGuidance::testClearGuidance);
    }

private:
    using Cap = QtLLM::AppInterface::Capability;

    struct Fixture
    {
        QWidget      window;
        QPushButton* alpha = nullptr;
        QPushButton* beta = nullptr;
        QPushButton* gamma = nullptr;
        QtLLM::Client client{QtLLM::Provider::Ollama, "http://127.0.0.1:9"};

        Fixture()
        {
            window.setObjectName("guidanceWindow");
            auto* layout = new QVBoxLayout(&window);
            alpha = new QPushButton("Alpha", &window);
            beta  = new QPushButton("Beta", &window);
            gamma = new QPushButton("Gamma", &window);
            alpha->setObjectName("alpha");
            beta->setObjectName("beta");
            gamma->setObjectName("gamma");
            for (QPushButton* button : {alpha, beta, gamma}) {
                button->setFixedWidth(120);
                layout->addWidget(button, 0, Qt::AlignLeft);
            }
            layout->addStretch();
            window.resize(520, 420);
        }

        QRect inWindow(QWidget* widget) const
        {
            return QRect(widget->mapTo(const_cast<QWidget*>(&window), QPoint(0, 0)), widget->size());
        }
    };

    static bool ok(const QJsonObject& result) { return result["status"].toString() == "ok"; }

    static QStringList uiTools(const QtLLM::Client& client)
    {
        QStringList names;
        for (const QString& name : client.toolNames()) {
            if (name.startsWith("ui_"))
                names.append(name);
        }
        names.sort();
        return names;
    }

    TEST_FUNCTION(testToolsFollowCapabilities)
    {
        TEST_START;
        if (!UnitTest::Gui::isAvailable()) { TEST_MESSAGE("Skipping: no GUI session"); return; }

        Fixture f;
        {
            QtLLM::AppInterface ui(&f.client, &f.window);
            TEST_ASSERT(uiTools(f.client).isEmpty());             // everything off by default

            ui.setCapabilityEnabled(Cap::SpeechBubbles, true);
            TEST_COMPARE(uiTools(f.client), (QStringList{"ui_clear_bubbles", "ui_close_bubble",
                                                         "ui_list_guidance", "ui_show_bubble"}));

            ui.setCapabilityEnabled(Cap::Tours, true);
            TEST_ASSERT(uiTools(f.client).contains("ui_show_tour"));

            ui.setCapabilityEnabled(Cap::SpeechBubbles, false);
            TEST_ASSERT(!uiTools(f.client).contains("ui_show_bubble"));
            TEST_ASSERT(uiTools(f.client).contains("ui_close_bubble"));     // still needed for tours

            // They draw on top of the application and change nothing in it.
            for (const QtLLM::Tool& tool : f.client.registeredTools()) {
                if (tool.name().startsWith("ui_")) {
                    TEST_ASSERT(tool.isReadOnly());
                    TEST_COMPARE(tool.group(), QString("App UI guidance"));
                }
            }
        }
        TEST_ASSERT(uiTools(f.client).isEmpty());                 // gone with the interface
    }

    TEST_FUNCTION(testBubbleAndTourTools)
    {
        TEST_START;
        if (!UnitTest::Gui::isAvailable()) { TEST_MESSAGE("Skipping: no GUI session"); return; }

        Fixture f;
        TEST_ASSERT(UnitTest::Gui::showAndWait(&f.window));
        QtLLM::AppInterface ui(&f.client, &f.window);
        ui.setCapabilityEnabled(Cap::SpeechBubbles, true);
        ui.setCapabilityEnabled(Cap::Tours, true);
        QtLLM::BubbleManager* manager = QtLLM::BubbleManager::forWindow(&f.window);

        const QJsonObject shown = ui.showBubble({{"target", ui.idForWidget(f.alpha)},
            {"text", "Click here to start."}, {"title", "Start"}, {"number", 1}, {"placement", "right"}});
        TEST_ASSERT(ok(shown));
        const QString bubbleId = shown["bubble"].toString();
        QtLLM::SpeechBubble* bubble = manager->bubble(bubbleId);
        TEST_ASSERT_NOT_NULL(bubble);
        if (!bubble) return;
        TEST_COMPARE(bubble->text(), QString("Click here to start."));
        TEST_COMPARE(bubble->number(), 1);
        TEST_ASSERT(f.inWindow(f.alpha).contains(bubble->tailTip()));
        TEST_ASSERT(!bubble->bubbleRect().intersects(f.inWindow(f.alpha)));

        // A point instead of a widget.
        TEST_ASSERT(ok(ui.showBubble({{"point", QJsonObject{{"x", 300}, {"y", 300}}}, {"text", "Here."}})));
        TEST_ASSERT(!ok(ui.showBubble({{"text", "Where?"}})));                // neither target nor point
        TEST_ASSERT(!ok(ui.showBubble({{"target", ui.idForWidget(f.alpha)}}))); // no text
        TEST_COMPARE(manager->bubbleIds().size(), 2);

        // The user closes one; the model can find that out.
        TEST_ASSERT(UnitTest::Gui::click(UnitTest::Gui::find<QWidget>("qtllm_bubbleClose", bubble)));
        TEST_COMPARE(manager->bubbleIds().size(), 1);
        TEST_ASSERT(QJsonDocument(ui.listGuidance({})).toJson().contains(bubbleId.toUtf8()));

        TEST_ASSERT(ok(ui.clearBubbles({})));
        TEST_ASSERT(manager->bubbleIds().isEmpty());

        // A numbered tour over three widgets.
        const QJsonObject tour = ui.showTour({{"steps", QJsonArray{
            QJsonObject{{"target", ui.idForWidget(f.alpha)}, {"text", "First this."}},
            QJsonObject{{"target", ui.idForWidget(f.beta)}, {"text", "Then this."}},
            QJsonObject{{"target", ui.idForWidget(f.gamma)}, {"text", "Finally this."}}}}});
        TEST_ASSERT(ok(tour));
        TEST_COMPARE(tour["steps"].toInt(), 3);
        TEST_COMPARE(manager->bubbleIds().size(), 3);

        const QJsonObject broken = ui.showTour({{"steps", QJsonArray{
            QJsonObject{{"target", ui.idForWidget(f.alpha)}, {"text", "ok"}},
            QJsonObject{{"target", "missing"}, {"text", "no"}}}}});
        TEST_ASSERT(!ok(broken));
        TEST_COMPARE(broken["step"].toInt(), 2);                              // says which step
        TEST_ASSERT(!ok(ui.showTour({{"steps", QJsonArray()}})));

        TEST_ASSERT(ok(ui.closeBubble({{"id", tour["tour"].toString()}})));
        TEST_ASSERT(manager->bubbleIds().isEmpty());
        TEST_ASSERT(!ok(ui.closeBubble({{"id", "nothing"}})));
    }

    TEST_FUNCTION(testClearGuidance)
    {
        TEST_START;
        if (!UnitTest::Gui::isAvailable()) { TEST_MESSAGE("Skipping: no GUI session"); return; }

        Fixture f;
        TEST_ASSERT(UnitTest::Gui::showAndWait(&f.window));
        QtLLM::AppInterface ui(&f.client, &f.window);
        ui.setCapabilities(Cap::SpeechBubbles | Cap::Tours);

        TEST_ASSERT(ok(ui.showBubble({{"target", ui.idForWidget(f.beta)}, {"text", "x"}})));
        TEST_ASSERT(ok(ui.showTour({{"mode", "one_at_a_time"}, {"steps", QJsonArray{
            QJsonObject{{"target", ui.idForWidget(f.alpha)}, {"text", "a"}},
            QJsonObject{{"target", ui.idForWidget(f.gamma)}, {"text", "b"}}}}})));
        TEST_ASSERT(!QtLLM::GuideOverlay::forWindow(&f.window)->itemIds().isEmpty());  // the step's highlight
        TEST_ASSERT(!QtLLM::BubbleManager::forWindow(&f.window)->bubbleIds().isEmpty());

        ui.clearGuidance();
        TEST_ASSERT(QtLLM::GuideOverlay::forWindow(&f.window)->itemIds().isEmpty());
        TEST_ASSERT(QtLLM::BubbleManager::forWindow(&f.window)->bubbleIds().isEmpty());
        TEST_ASSERT(ui.listGuidance({})["bubbles"].toArray().isEmpty());
    }
};

TEST_INSTANTIATE(TST_AppGuidance);

#endif // UNITTEST_GUI_ENABLED
