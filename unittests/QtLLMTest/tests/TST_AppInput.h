#pragma once

// Input tools of AppInterface: clicks, typing and key presses delivered as real
// events, waiting for the UI, window discovery, property undo and the action log.
#ifdef UNITTEST_GUI_ENABLED

#include "UnitTest.h"
#include "UnitTest_Gui.h"
#include "QtLLM.h"
#include "AppInterface.h"
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMenu>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>
#include <functional>

class TST_AppInput : public UnitTest::Test
{
    TEST_CLASS(TST_AppInput)
public:
    TST_AppInput()
        : Test("TST_AppInput")
    {
        ADD_TEST(TST_AppInput::testInputToolsRegister);
        ADD_TEST(TST_AppInput::testClick);
        ADD_TEST(TST_AppInput::testClickRefused);
        ADD_TEST(TST_AppInput::testClickOpeningModalDialogDoesNotBlock);
        ADD_TEST(TST_AppInput::testTypeText);
        ADD_TEST(TST_AppInput::testPressKey);
        ADD_TEST(TST_AppInput::testWaitFor);
        ADD_TEST(TST_AppInput::testWindowDiscovery);
        ADD_TEST(TST_AppInput::testPropertyUndo);
        ADD_TEST(TST_AppInput::testActionLog);
    }

private:
    using Cap = QtLLM::AppInterface::Capability;

    struct Fixture
    {
        QMainWindow     window;
        QPushButton*    button = nullptr;
        QPushButton*    disabledButton = nullptr;
        QPushButton*    hiddenButton = nullptr;
        QCheckBox*      check = nullptr;
        QLineEdit*      edit = nullptr;
        QLineEdit*      password = nullptr;
        QLineEdit*      readOnly = nullptr;
        QPlainTextEdit* plain = nullptr;
        QSpinBox*       spin = nullptr;
        QComboBox*      combo = nullptr;
        QLabel*         label = nullptr;
        int             clicks = 0;

        Fixture()
        {
            window.setObjectName("win");
            window.resize(400, 500);
            auto* central = new QWidget;
            central->setObjectName("central");
            window.setCentralWidget(central);
            auto* layout = new QVBoxLayout(central);
            auto make = [&](QWidget* widget, const char* name) {
                widget->setObjectName(name);
                layout->addWidget(widget);
            };
            make(button = new QPushButton("Go"), "button");
            make(disabledButton = new QPushButton("Off"), "disabledButton");
            make(hiddenButton = new QPushButton("Hidden"), "hiddenButton");
            make(check = new QCheckBox("Check"), "check");
            make(edit = new QLineEdit("abc"), "edit");
            make(password = new QLineEdit, "password");
            make(readOnly = new QLineEdit("fixed"), "readOnly");
            make(plain = new QPlainTextEdit, "plain");
            make(spin = new QSpinBox, "spin");
            make(combo = new QComboBox, "combo");
            make(label = new QLabel("Label"), "label");
            disabledButton->setEnabled(false);
            hiddenButton->hide();
            password->setEchoMode(QLineEdit::Password);
            readOnly->setReadOnly(true);
            spin->setRange(0, 100);
            spin->setValue(5);
            combo->addItems({"one", "two"});
            QObject::connect(button, &QPushButton::clicked, [this]() { ++clicks; });
        }

        QString id(const char* name) const { return QString("win/central/") + name; }
    };

    static bool isOk(const QJsonObject& result)
    {
        return result.value("status").toString() == "ok";
    }

    static QString errorOf(const QJsonObject& result)
    {
        return result.value("status").toString() == "error" ? result.value("message").toString()
                                                             : QString();
    }

    static bool spinUntil(const std::function<bool()>& condition, int timeoutMs = 3000)
    {
        QEventLoop loop;
        QTimer poll;
        QObject::connect(&poll, &QTimer::timeout, [&]() {
            if (condition())
                loop.quit();
        });
        poll.start(10);
        QTimer::singleShot(timeoutMs, &loop, &QEventLoop::quit);
        if (!condition())
            loop.exec();
        return condition();
    }

    static void spinFor(int ms)
    {
        QEventLoop loop;
        QTimer::singleShot(ms, &loop, &QEventLoop::quit);
        loop.exec();
    }

    // Runs waitFor() and spins until it answers.
    static QJsonObject waitResult(QtLLM::AppInterface& ui, const QJsonObject& args)
    {
        QJsonObject result;
        ui.waitFor(args, [&](const QJsonObject& r) { result = r; });
        spinUntil([&]() { return !result.isEmpty(); }, 5000);
        return result;
    }

    static QStringList windowIds(QtLLM::AppInterface& ui)
    {
        QStringList ids;
        for (const QJsonValue& value : ui.listWindows({}).value("windows").toArray())
            ids.append(value.toObject().value("id").toString());
        return ids;
    }

    // ------------------------------------------------------------ registration

    TEST_FUNCTION(testInputToolsRegister)
    {
        TEST_START;
        if (!UnitTest::Gui::isAvailable()) { TEST_MESSAGE("Skipping: no GUI session"); return; }

        QtLLM::Client client(QtLLM::Provider::Ollama, "http://127.0.0.1:9");
        QWidget window;
        {
            QtLLM::AppInterface ui(&client, &window);
            ui.setCapabilityEnabled(Cap::SimulateInput, true);
            ui.setCapabilityEnabled(Cap::WaitForUi, true);
            ui.setCapabilityEnabled(Cap::ReadWidgets, true);
            ui.setCapabilityEnabled(Cap::WriteProperties, true);
            QStringList names = client.toolNames();
            for (const char* name : {"ui_click", "ui_type_text", "ui_press_key", "ui_wait_for",
                                     "ui_list_windows", "ui_undo_property_change"})
                TEST_ASSERT(names.contains(name));
            for (const QtLLM::Tool& tool : client.registeredTools()) {
                if (tool.name() == "ui_wait_for" || tool.name() == "ui_list_windows")
                    TEST_ASSERT(tool.isReadOnly());
                if (tool.name() == "ui_click" || tool.name() == "ui_undo_property_change")
                    TEST_ASSERT(!tool.isReadOnly());
            }
            ui.setCapabilityEnabled(Cap::SimulateInput, false);
            names = client.toolNames();
            TEST_ASSERT(!names.contains("ui_click"));
            TEST_ASSERT(names.contains("ui_wait_for"));
        }
        TEST_ASSERT(!client.toolNames().contains("ui_wait_for"));
    }

    // ------------------------------------------------------------ click

    TEST_FUNCTION(testClick)
    {
        TEST_START;
        if (!UnitTest::Gui::isAvailable()) { TEST_MESSAGE("Skipping: no GUI session"); return; }

        Fixture f;
        QtLLM::AppInterface ui(nullptr, &f.window);
        TEST_ASSERT(UnitTest::Gui::showAndWait(&f.window));

        TEST_ASSERT(isOk(ui.click({{"id", f.id("button")}})));
        TEST_COMPARE(f.clicks, 0);   // queued: delivered after the call returned
        TEST_ASSERT(spinUntil([&]() { return f.clicks == 1; }));

        TEST_ASSERT(!f.check->isChecked());
        TEST_ASSERT(isOk(ui.click({{"id", f.id("check")}})));
        TEST_ASSERT(spinUntil([&]() { return f.check->isChecked(); }));

        // A right click is no click for a button.
        TEST_ASSERT(isOk(ui.click({{"id", f.id("button")}, {"button", "right"}})));
        spinFor(50);
        TEST_COMPARE(f.clicks, 1);

        TEST_ASSERT(!errorOf(ui.click({{"id", f.id("button")}, {"x", 10000}})).isEmpty());
        TEST_ASSERT(!errorOf(ui.click({{"id", f.id("button")}, {"button", "fourth"}})).isEmpty());
    }

    TEST_FUNCTION(testClickRefused)
    {
        TEST_START;
        if (!UnitTest::Gui::isAvailable()) { TEST_MESSAGE("Skipping: no GUI session"); return; }

        Fixture f;
        QtLLM::AppInterface ui(nullptr, &f.window);
        TEST_ASSERT(UnitTest::Gui::showAndWait(&f.window));

        TEST_ASSERT(errorOf(ui.click({{"id", f.id("disabledButton")}})).contains("disabled"));
        TEST_ASSERT(errorOf(ui.click({{"id", f.id("hiddenButton")}})).contains("not visible"));
        TEST_ASSERT(errorOf(ui.click({{"id", f.id("nothing")}})).contains("no widget"));
    }

    TEST_FUNCTION(testClickOpeningModalDialogDoesNotBlock)
    {
        TEST_START;
        if (!UnitTest::Gui::isAvailable()) { TEST_MESSAGE("Skipping: no GUI session"); return; }

        Fixture f;
        QtLLM::AppInterface ui(nullptr, &f.window);
        TEST_ASSERT(UnitTest::Gui::showAndWait(&f.window));
        bool opened = false;
        bool returned = false;
        QObject::connect(f.button, &QPushButton::clicked, [&]() {
            QDialog dialog(&f.window);
            dialog.setWindowTitle("Question");
            opened = true;
            dialog.exec();
            returned = true;
        });

        TEST_ASSERT(isOk(ui.click({{"id", f.id("button")}})));
        TEST_ASSERT(!opened);
        // The test plays the user: it closes the dialog once it is up.
        TEST_ASSERT(spinUntil([&]() {
            if (QWidget* modal = QApplication::activeModalWidget()) {
                // While it is open, the main window is out of reach.
                if (ui.click({{"id", f.id("check")}}).value("message").toString().contains("modal"))
                    modal->close();
            }
            return returned;
        }));
        TEST_ASSERT(opened);
    }

    // ------------------------------------------------------------ typing

    TEST_FUNCTION(testTypeText)
    {
        TEST_START;
        if (!UnitTest::Gui::isAvailable()) { TEST_MESSAGE("Skipping: no GUI session"); return; }

        Fixture f;
        QtLLM::AppInterface ui(nullptr, &f.window);
        TEST_ASSERT(UnitTest::Gui::showAndWait(&f.window));
        int edited = 0;
        QObject::connect(f.edit, &QLineEdit::textEdited, [&]() { ++edited; });

        QJsonObject result = ui.typeText({{"id", f.id("edit")}, {"text", "hello"}});
        TEST_ASSERT(isOk(result));
        TEST_COMPARE(result.value("text").toString(), QString("hello"));
        TEST_COMPARE(f.edit->text(), QString("hello"));
        TEST_ASSERT(edited > 0);

        result = ui.typeText({{"id", f.id("edit")}, {"text", " world"}, {"replace", false}});
        TEST_COMPARE(f.edit->text(), QString("hello world"));

        ui.typeText({{"id", f.id("plain")}, {"text", "one\ntwo"}});
        TEST_COMPARE(f.plain->toPlainText(), QString("one\ntwo"));

        ui.typeText({{"id", f.id("spin")}, {"text", "42"}});
        TEST_COMPARE(f.spin->value(), 42);

        // The password is typed but never returned.
        result = ui.typeText({{"id", f.id("password")}, {"text", "s3cret"}});
        TEST_ASSERT(isOk(result));
        TEST_COMPARE(f.password->text(), QString("s3cret"));
        TEST_COMPARE(result.value("text").toString(), QString("[redacted]"));

        TEST_ASSERT(errorOf(ui.typeText({{"id", f.id("readOnly")}, {"text", "x"}})).contains("read-only"));
        TEST_COMPARE(f.readOnly->text(), QString("fixed"));
        TEST_ASSERT(errorOf(ui.typeText({{"id", f.id("combo")}, {"text", "x"}})).contains("not editable"));
        TEST_ASSERT(errorOf(ui.typeText({{"id", f.id("label")}, {"text", "x"}})).contains("does not take text"));

        // submit presses Enter afterwards, queued.
        int returns = 0;
        QObject::connect(f.edit, &QLineEdit::returnPressed, [&]() { ++returns; });
        ui.typeText({{"id", f.id("edit")}, {"text", "query"}, {"submit", true}});
        TEST_COMPARE(returns, 0);
        TEST_ASSERT(spinUntil([&]() { return returns == 1; }));
    }

    // ------------------------------------------------------------ keys

    TEST_FUNCTION(testPressKey)
    {
        TEST_START;
        if (!UnitTest::Gui::isAvailable()) { TEST_MESSAGE("Skipping: no GUI session"); return; }

        Fixture f;
        QtLLM::AppInterface ui(nullptr, &f.window);
        int triggered = 0;
        auto* action = new QAction("Shortcut", &f.window);
        action->setShortcut(QKeySequence("F7"));
        f.window.addAction(action);
        QObject::connect(action, &QAction::triggered, [&]() { ++triggered; });
        int returns = 0;
        QObject::connect(f.edit, &QLineEdit::returnPressed, [&]() { ++returns; });
        TEST_ASSERT(UnitTest::Gui::showAndWait(&f.window));

        TEST_ASSERT(isOk(ui.pressKey({{"key", "Enter"}, {"id", f.id("edit")}})));
        TEST_ASSERT(spinUntil([&]() { return returns == 1; }));

        // Keys move the cursor like real ones.
        f.edit->setText("abc");
        f.edit->setCursorPosition(3);
        ui.pressKey({{"key", "Home"}, {"id", f.id("edit")}});
        TEST_ASSERT(spinUntil([&]() { return f.edit->cursorPosition() == 0; }));

        TEST_ASSERT(!errorOf(ui.pressKey({{"key", "Ctrl+S, Ctrl+Q"}})).isEmpty());
        TEST_ASSERT(!errorOf(ui.pressKey({{"key", ""}})).isEmpty());
        TEST_ASSERT(errorOf(ui.pressKey({{"key", "A"}, {"id", f.id("disabledButton")}})).contains("disabled"));

        // Application shortcuts fire while the window is active, as for a real key.
        f.window.activateWindow();
        f.edit->setFocus();
        if (!spinUntil([&]() { return f.window.isActiveWindow(); }, 1000)) {
            TEST_MESSAGE("Skipping the shortcut check: the window could not be activated");
            return;
        }
        TEST_ASSERT(isOk(ui.pressKey({{"key", "F7"}})));
        TEST_ASSERT(spinUntil([&]() { return triggered == 1; }));
        TEST_COMPARE(f.edit->text(), QString("abc"));
    }

    // ------------------------------------------------------------ waiting

    TEST_FUNCTION(testWaitFor)
    {
        TEST_START;
        if (!UnitTest::Gui::isAvailable()) { TEST_MESSAGE("Skipping: no GUI session"); return; }

        Fixture f;
        QtLLM::AppInterface ui(nullptr, &f.window);
        TEST_ASSERT(UnitTest::Gui::showAndWait(&f.window));

        QTimer::singleShot(200, f.button, [&]() { f.button->hide(); });
        QJsonObject result = waitResult(ui, {{"condition", "hidden"}, {"id", f.id("button")}});
        TEST_ASSERT(isOk(result));
        TEST_ASSERT(result.value("waited_ms").toInt() >= 150);

        // Already true: answers at once.
        result = waitResult(ui, {{"condition", "disabled"}, {"id", f.id("disabledButton")}});
        TEST_ASSERT(isOk(result));

        QTimer::singleShot(150, f.label, [&]() { f.label->setText("Done"); });
        result = waitResult(ui, {{"condition", "property_equals"}, {"id", f.id("label")},
                                 {"property", "text"}, {"value", "Done"}});
        TEST_ASSERT(isOk(result));

        // Timeout: an error that reports the current state.
        result = waitResult(ui, {{"condition", "visible"}, {"id", f.id("hiddenButton")},
                                 {"timeoutSeconds", 0.3}});
        TEST_ASSERT(errorOf(result).contains("not met"));
        TEST_ASSERT(result.value("state").toObject().value("exists").toBool());
        TEST_ASSERT(!result.value("state").toObject().value("visible").toBool(true));

        TEST_ASSERT(!errorOf(waitResult(ui, {{"condition", "visible"}})).isEmpty());
        TEST_ASSERT(!errorOf(waitResult(ui, {{"condition", "soon"}, {"id", f.id("button")}})).isEmpty());

        // A new window, found through window discovery.
        ui.setWindowDiscoveryEnabled(true);
        QDialog dialog;
        dialog.setWindowTitle("Report");
        QTimer::singleShot(200, &dialog, [&]() { dialog.show(); });
        result = waitResult(ui, {{"condition", "window_opened"}});
        TEST_ASSERT(isOk(result));
        TEST_COMPARE(result.value("title").toString(), QString("Report"));
        const QString windowId = result.value("window").toString();
        TEST_ASSERT(ui.widgetForId(windowId) == &dialog);

        QTimer::singleShot(100, &dialog, [&]() { dialog.close(); });
        TEST_ASSERT(isOk(waitResult(ui, {{"condition", "window_closed"}, {"id", windowId}})));

        // Cancelled (the turn ended): no answer at all.
        bool answered = false;
        bool cancel = false;
        ui.waitFor({{"condition", "visible"}, {"id", f.id("hiddenButton")}},
                   [&](const QJsonObject&) { answered = true; }, [&]() { return cancel; });
        cancel = true;
        spinFor(300);
        TEST_ASSERT(!answered);
    }

    // ------------------------------------------------------------ windows

    TEST_FUNCTION(testWindowDiscovery)
    {
        TEST_START;
        if (!UnitTest::Gui::isAvailable()) { TEST_MESSAGE("Skipping: no GUI session"); return; }

        Fixture f;
        QtLLM::AppInterface ui(nullptr, &f.window);
        auto* chat = new QtLLM::ChatDockWidget(&f.window);
        f.window.addDockWidget(Qt::RightDockWidgetArea, chat);
        TEST_ASSERT(UnitTest::Gui::showAndWait(&f.window));
        chat->setFloating(true);
        chat->show();

        QDialog dialog;   // no parent, no object name
        dialog.setWindowTitle("Settings");
        auto* ok = new QPushButton("OK", &dialog);
        ok->setObjectName("ok");
        TEST_ASSERT(UnitTest::Gui::showAndWait(&dialog));
        QWidget excluded;
        excluded.setObjectName("excludedWindow");
        ui.excludeWidget(&excluded);
        TEST_ASSERT(UnitTest::Gui::showAndWait(&excluded));
        QMenu menu;
        menu.addAction("Item");
        menu.popup(f.window.mapToGlobal(QPoint(10, 10)));
        QtLLM::GuideOverlay::forWindow(&f.window);

        // Off (default): only the registered root.
        TEST_ASSERT(!ui.windowDiscoveryEnabled());
        TEST_COMPARE(windowIds(ui), QStringList({"win"}));
        TEST_ASSERT(ui.idForWidget(ok).isEmpty());

        ui.setWindowDiscoveryEnabled(true);
        const QStringList ids = windowIds(ui);
        TEST_ASSERT(ids.contains("win"));
        QString dialogId;
        for (const QJsonValue& value : ui.listWindows({}).value("windows").toArray()) {
            const QJsonObject window = value.toObject();
            const QString windowClass = window.value("class").toString();
            TEST_ASSERT(windowClass != "QMenu" && windowClass != "QtLLM::ChatDockWidget"
                        && windowClass != "QtLLM::GuideOverlay");
            TEST_ASSERT(window.value("id").toString() != "excludedWindow");
            if (window.value("title").toString() == "Settings")
                dialogId = window.value("id").toString();
        }
        TEST_ASSERT(dialogId.startsWith("QDialog@"));
        TEST_ASSERT(windowIds(ui).contains(dialogId));   // stable

        // Every tool sees it.
        TEST_COMPARE(ui.idForWidget(ok), dialogId + "/ok");
        TEST_ASSERT(ui.widgetForId(dialogId + "/ok") == ok);
        TEST_ASSERT(isOk(ui.click({{"id", dialogId + "/ok"}})));
        const QJsonArray found = ui.findWidgets({{"name", "ok"}}).value("widgets").toArray();
        TEST_ASSERT(found.size() == 1);
        TEST_COMPARE(found[0].toObject().value("id").toString(), dialogId + "/ok");

        // A second unnamed dialog gets an id of its own; the first keeps its.
        QDialog second;
        second.setWindowTitle("Other");
        TEST_ASSERT(UnitTest::Gui::showAndWait(&second));
        const QStringList withSecond = windowIds(ui);
        TEST_ASSERT(withSecond.contains(dialogId));
        TEST_ASSERT(withSecond.size() == ids.size() + 1);
        second.close();

        dialog.close();
        TEST_ASSERT(!windowIds(ui).contains(dialogId));
        menu.close();
    }

    // ------------------------------------------------------------ property undo

    TEST_FUNCTION(testPropertyUndo)
    {
        TEST_START;
        if (!UnitTest::Gui::isAvailable()) { TEST_MESSAGE("Skipping: no GUI session"); return; }

        Fixture f;
        QtLLM::AppInterface ui(nullptr, &f.window);

        TEST_ASSERT(!errorOf(ui.undoPropertyChange({})).isEmpty());
        TEST_ASSERT(isOk(ui.setWidgetProperty({{"id", f.id("edit")}, {"name", "text"}, {"value", "one"}})));
        TEST_ASSERT(isOk(ui.setWidgetProperty({{"id", f.id("edit")}, {"name", "text"}, {"value", "two"}})));
        TEST_ASSERT(isOk(ui.setWidgetProperty({{"id", f.id("check")}, {"name", "checked"}, {"value", true}})));

        QJsonObject result = ui.undoPropertyChange({});
        TEST_ASSERT(isOk(result));
        TEST_COMPARE(result.value("undoSteps").toInt(), 2);
        TEST_ASSERT(!f.check->isChecked());
        ui.undoPropertyChange({});
        TEST_COMPARE(f.edit->text(), QString("one"));

        ui.setWidgetProperty({{"id", f.id("spin")}, {"name", "value"}, {"value", 77}});
        ui.resetPropertyChanges();
        TEST_COMPARE(f.edit->text(), QString("abc"));
        TEST_COMPARE(f.spin->value(), 5);
        TEST_ASSERT(!errorOf(ui.undoPropertyChange({})).isEmpty());
    }

    // ------------------------------------------------------------ action log

    TEST_FUNCTION(testActionLog)
    {
        TEST_START;
        if (!UnitTest::Gui::isAvailable()) { TEST_MESSAGE("Skipping: no GUI session"); return; }

        QtLLM::Client client(QtLLM::Provider::Ollama, "http://127.0.0.1:9");
        Fixture f;
        QtLLM::AppInterface ui(&client, &f.window);
        ui.setCapabilities(Cap::ReadWidgets | Cap::WriteProperties);
        int signalled = 0;
        QObject::connect(&ui, &QtLLM::AppInterface::actionPerformed,
                         [&](const QtLLM::AppInterface::ActionRecord&) { ++signalled; });

        // The client reports every call; only the mutating ui_* ones are logged.
        const QJsonObject read{{"id", f.id("edit")}};
        emit client.toolInvoked("ui_get_widget", read);
        emit client.toolCompleted("ui_get_widget", QtLLM::toolOk());
        emit client.toolInvoked("other_tool", read);
        emit client.toolCompleted("other_tool", QtLLM::toolOk());

        const QJsonObject write{{"id", f.id("edit")}, {"name", "text"}, {"value", "x"}};
        emit client.toolInvoked("ui_set_property", write);
        emit client.toolCompleted("ui_set_property", QtLLM::toolOk());
        emit client.toolInvoked("ui_undo_property_change", QJsonObject());
        emit client.toolCompleted("ui_undo_property_change", QtLLM::toolError("nothing to undo"));

        const QList<QtLLM::AppInterface::ActionRecord> log = ui.actionLog();
        TEST_COMPARE(int(log.size()), 2);
        TEST_COMPARE(signalled, 2);
        TEST_COMPARE(log[0].tool, QString("ui_set_property"));
        TEST_ASSERT(log[0].input == write);
        TEST_ASSERT(log[0].ok);
        TEST_ASSERT(log[0].time.isValid());
        TEST_COMPARE(log[1].tool, QString("ui_undo_property_change"));
        TEST_ASSERT(!log[1].ok);
        TEST_COMPARE(log[1].message, QString("nothing to undo"));

        ui.clearActionLog();
        TEST_ASSERT(ui.actionLog().isEmpty());
    }
};

TEST_INSTANTIATE(TST_AppInput);

#endif // UNITTEST_GUI_ENABLED
