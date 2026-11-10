#pragma once

#include "UnitTest.h"
#include "QtLLM.h"
#include "ClaudeCodeProtocol.h"
#include <QEventLoop>
#include <QJsonObject>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>
#include <iostream>

// The whole point of the application interface, end to end: a real model gets
// a plain request ("draw a border around the Export button"), has to find the
// widget with the ui_* tools and then point at it. This is what the demo
// buttons of the example application do.
//
// Runs on the subscription provider, so it costs no API money; skipped unless
// Claude Code is installed and logged in.
class TST_AppInterfaceIntegration : public UnitTest::Test
{
    TEST_CLASS(TST_AppInterfaceIntegration)
public:
    TST_AppInterfaceIntegration()
        : Test("TST_AppInterfaceIntegration")
    {
        ADD_TEST(TST_AppInterfaceIntegration::testModelPointsAtWidgets);
    }

private:

    static bool loggedInOrSkip()
    {
        QJsonObject status;
        QEventLoop loop;
        QObject context;
        QtLLM::ClaudeCodeProtocol::queryAuthStatus(&context, [&](const QJsonObject& s) {
            status = s;
            loop.quit();
        });
        QTimer::singleShot(15000, &loop, &QEventLoop::quit);
        loop.exec();
        if (!status["loggedIn"].toBool()) {
            std::cout << "  Skipping: Claude Code CLI not installed or not logged in\n";
            return false;
        }
        return true;
    }

    static QString ask(QtLLM::Client& client, const QString& prompt, QString& error)
    {
        QString text;
        QEventLoop loop;
        auto c1 = QObject::connect(&client, &QtLLM::Client::responseReady, [&](const QString& t) {
            text = t;
            loop.quit();
        });
        auto c2 = QObject::connect(&client, &QtLLM::Client::errorOccurred, [&](const QString& m) {
            error = m;
            loop.quit();
        });
        QTimer::singleShot(240000, &loop, &QEventLoop::quit);
        client.sendPrompt(prompt);
        loop.exec();
        QObject::disconnect(c1);
        QObject::disconnect(c2);
        return text;
    }

    TEST_FUNCTION(testModelPointsAtWidgets)
    {
        TEST_START;

        if (!loggedInOrSkip())
            return;

        QWidget window;
        window.setObjectName("documentWindow");
        auto* layout = new QVBoxLayout(&window);
        auto* save   = new QPushButton("Save", &window);
        auto* exportButton = new QPushButton("Export", &window);
        auto* print  = new QPushButton("Print", &window);
        save->setObjectName("saveButton");
        exportButton->setObjectName("exportButton");
        print->setObjectName("printButton");
        for (QPushButton* button : {save, exportButton, print}) {
            button->setFixedWidth(120);
            layout->addWidget(button, 0, Qt::AlignLeft);
        }
        layout->addStretch();
        window.resize(520, 400);
        window.show();
        QEventLoop settle;
        QTimer::singleShot(200, &settle, &QEventLoop::quit);
        settle.exec();

        QtLLM::Client client(QtLLM::Provider::ClaudeCode, QString());
        client.setModel("haiku");
        client.setSystemPrompt(
            "You are an assistant inside a desktop application. Use the ui_* tools to look at "
            "the application's window and to point things out to the user. Answer in one short line.");

        QtLLM::AppInterface ui(&client, &window);
        using Cap = QtLLM::AppInterface::Capability;
        ui.setCapabilities(Cap::ReadWidgets | Cap::Highlights | Cap::SpeechBubbles);

        QStringList called;
        QObject::connect(&client, &QtLLM::Client::toolInvoked,
                         [&](const QString& name, const QJsonObject&) { called.append(name); });
        auto inWindow = [&](QWidget* widget) {
            return QRect(widget->mapTo(&window, QPoint(0, 0)), widget->size());
        };

        QString error;
        const QString first = ask(client, "Draw a border around the Export button.", error);
        TEST_ASSERT_M(error.isEmpty(), ("error: " + error).toStdString().c_str());
        std::cout << "  Response 1: " << first.toStdString() << "\n";
        std::cout << "  Tools used: " << called.join(", ").toStdString() << "\n";

        QtLLM::GuideOverlay* overlay = QtLLM::GuideOverlay::forWindow(&window);
        TEST_ASSERT_M(called.contains("ui_highlight_widget"), "Expected the model to call ui_highlight_widget");
        TEST_COMPARE(overlay->itemIds().size(), 1);
        if (overlay->itemIds().size() == 1) {
            const QRect drawn = overlay->itemRect(overlay->itemIds().first());
            TEST_ASSERT(drawn.contains(inWindow(exportButton)));       // the right widget
            TEST_ASSERT(!drawn.intersects(inWindow(save)));
        }

        called.clear();
        const QString second = ask(client,
            "Now attach a speech bubble to the Print button that says what it does.", error);
        TEST_ASSERT_M(error.isEmpty(), ("error: " + error).toStdString().c_str());
        std::cout << "  Response 2: " << second.toStdString() << "\n";
        std::cout << "  Tools used: " << called.join(", ").toStdString() << "\n";

        QtLLM::BubbleManager* bubbles = QtLLM::BubbleManager::forWindow(&window);
        TEST_ASSERT_M(called.contains("ui_show_bubble"), "Expected the model to call ui_show_bubble");
        TEST_COMPARE(bubbles->bubbleIds().size(), 1);
        if (bubbles->bubbleIds().size() == 1) {
            QtLLM::SpeechBubble* bubble = bubbles->bubble(bubbles->bubbleIds().first());
            TEST_ASSERT(bubble && inWindow(print).contains(bubble->tailTip()));
            TEST_ASSERT(bubble && !bubble->text().isEmpty());
            std::cout << "  Bubble text: " << (bubble ? bubble->text().toStdString() : std::string()) << "\n";
        }
    }
};

TEST_INSTANTIATE(TST_AppInterfaceIntegration);
