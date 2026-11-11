#pragma once

#include "UnitTest.h"
#include "QtLLM.h"
#include "ClaudeCodeProtocol.h"
#include <QEventLoop>
#include <QJsonObject>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidget>
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
        ADD_TEST(TST_AppInterfaceIntegration::testModelOperatesControls);
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

    // Operating an application that has no code for the assistant at all:
    // fill a form and click, read and edit a table, deal with a dialog.
    TEST_FUNCTION(testModelOperatesControls)
    {
        TEST_START;

        if (!loggedInOrSkip())
            return;

        QWidget window;
        window.setObjectName("orderWindow");
        window.setWindowTitle("Orders");
        auto* layout = new QVBoxLayout(&window);
        auto* name = new QLineEdit(&window);
        name->setObjectName("nameEdit");
        name->setPlaceholderText("Your name");
        auto* color = new QComboBox(&window);
        color->setObjectName("colorCombo");
        color->addItems({"Red", "Green", "Blue"});
        auto* shout = new QCheckBox("Shout", &window);
        shout->setObjectName("shoutCheck");
        auto* greet = new QPushButton("Greet", &window);
        greet->setObjectName("greetButton");
        auto* greeting = new QLabel("No greeting yet.", &window);
        greeting->setObjectName("greetingLabel");
        auto* about = new QPushButton("About...", &window);
        about->setObjectName("aboutButton");
        auto* table = new QTableWidget(0, 2, &window);
        table->setObjectName("inventoryTable");
        table->setHorizontalHeaderLabels({"Item", "Quantity"});
        const QList<QStringList> rows = {{"Screws", "120"}, {"Nails", "45"}, {"Washers", "300"}, {"Hinges", "8"}};
        for (const QStringList& row : rows) {
            const int index = table->rowCount();
            table->insertRow(index);
            table->setItem(index, 0, new QTableWidgetItem(row[0]));
            table->setItem(index, 1, new QTableWidgetItem(row[1]));
        }
        for (QWidget* widget : QList<QWidget*>{name, color, shout, greet, greeting, about, table})
            layout->addWidget(widget);
        int greetClicks = 0;
        QObject::connect(greet, &QPushButton::clicked, [&]() {
            ++greetClicks;
            QString text = QString("Hello, %1 (%2)!").arg(name->text(), color->currentText());
            greeting->setText(shout->isChecked() ? text.toUpper() : text);
        });
        QObject::connect(about, &QPushButton::clicked, [&]() {
            auto* box = new QMessageBox(QMessageBox::Information, "About Orders",
                                        "Orders version 7 - codename Pineapple.", QMessageBox::Ok, &window);
            box->setObjectName("aboutDialog");
            box->setAttribute(Qt::WA_DeleteOnClose);
            box->open();
        });
        window.resize(520, 520);
        window.show();
        QEventLoop settle;
        QTimer::singleShot(200, &settle, &QEventLoop::quit);
        settle.exec();

        QtLLM::Client client(QtLLM::Provider::ClaudeCode, QString());
        client.setModel("haiku");
        client.setSystemPrompt(
            "You are an assistant inside a desktop application. Operate the application's window "
            "with the ui_* tools on the user's behalf. Answer in one short line.");
        QtLLM::AppInterface ui(&client, &window);
        using Cap = QtLLM::AppInterface::Capability;
        ui.setCapabilities(Cap::ReadWidgets | Cap::ReadProperties | Cap::SimulateInput | Cap::ReadItems
                           | Cap::SelectItems | Cap::EditItems | Cap::WaitForUi);
        ui.setWindowDiscoveryEnabled(true);

        QStringList called;
        QObject::connect(&client, &QtLLM::Client::toolInvoked,
                         [&](const QString& tool, const QJsonObject&) { called.append(tool); });
        auto report = [&](const QString& answer) {
            std::cout << "  Response: " << answer.toStdString() << "\n";
            std::cout << "  Tools used: " << called.join(", ").toStdString() << "\n";
            called.clear();
        };

        QString error;
        const QString first = ask(client,
            "Type 'Ada' into the name field, choose Blue, tick Shout, then click Greet. "
            "What does the greeting say?", error);
        TEST_ASSERT_M(error.isEmpty(), ("error: " + error).toStdString().c_str());
        TEST_ASSERT(called.contains("ui_type_text"));
        TEST_ASSERT(called.contains("ui_click"));
        report(first);
        TEST_COMPARE(greeting->text(), QString("HELLO, ADA (BLUE)!"));
        TEST_ASSERT(greetClicks >= 1);
        TEST_ASSERT(first.contains("HELLO, ADA", Qt::CaseInsensitive));

        const QString second = ask(client,
            "Which item in the inventory table has the lowest quantity? Then set the quantity "
            "of Screws to 500.", error);
        TEST_ASSERT_M(error.isEmpty(), ("error: " + error).toStdString().c_str());
        TEST_ASSERT(called.contains("ui_get_items"));
        TEST_ASSERT(called.contains("ui_set_item_text"));
        report(second);
        TEST_ASSERT(second.contains("Hinges", Qt::CaseInsensitive));
        TEST_COMPARE(table->item(0, 1)->text(), QString("500"));

        const QString third = ask(client,
            "Click the About button, tell me what the dialog that opens says, then close it.", error);
        TEST_ASSERT_M(error.isEmpty(), ("error: " + error).toStdString().c_str());
        report(third);
        TEST_ASSERT(third.contains("Pineapple", Qt::CaseInsensitive));
        bool dialogOpen = false;
        for (QWidget* top : QApplication::topLevelWidgets())
            dialogOpen = dialogOpen || (qobject_cast<QMessageBox*>(top) && top->isVisible());
        TEST_ASSERT(!dialogOpen);

        std::cout << "  Action log: " << ui.actionLog().size() << " entries\n";
        TEST_ASSERT(ui.actionLog().size() >= 5);
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
        ui.setCapabilities(Cap::ReadWidgets | Cap::SpeechBubbles);

        QStringList called;
        QObject::connect(&client, &QtLLM::Client::toolInvoked,
                         [&](const QString& name, const QJsonObject&) { called.append(name); });
        auto inWindow = [&](QWidget* widget) {
            return QRect(widget->mapTo(&window, QPoint(0, 0)), widget->size());
        };

        QString error;
        const QString answer = ask(client,
            "Attach a speech bubble to the Print button that says what it does.", error);
        TEST_ASSERT_M(error.isEmpty(), ("error: " + error).toStdString().c_str());
        std::cout << "  Response: " << answer.toStdString() << "\n";
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
