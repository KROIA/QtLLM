#pragma once

// AppInterface against a small fixture window: tool registration per
// capability, widget ids, actions, tree/find, properties, password redaction,
// layout changes with undo, and annotated screenshots.
#ifdef UNITTEST_GUI_ENABLED

#include "UnitTest.h"
#include "UnitTest_Gui.h"
#include "QtLLM.h"
#include "AppInterface.h"
#include <QAction>
#include <QComboBox>
#include <QHBoxLayout>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QPushButton>
#include <QVBoxLayout>

class TST_AppInterface : public UnitTest::Test
{
    TEST_CLASS(TST_AppInterface)
public:
    TST_AppInterface()
        : Test("TST_AppInterface")
    {
        ADD_TEST(TST_AppInterface::testCapabilitiesRegisterTools);
        ADD_TEST(TST_AppInterface::testIdRoundTrip);
        ADD_TEST(TST_AppInterface::testStaleIds);
        ADD_TEST(TST_AppInterface::testActions);
        ADD_TEST(TST_AppInterface::testTreeDepthAndExclusion);
        ADD_TEST(TST_AppInterface::testFindWidgets);
        ADD_TEST(TST_AppInterface::testProperties);
        ADD_TEST(TST_AppInterface::testPasswordRedaction);
        ADD_TEST(TST_AppInterface::testLayouts);
        ADD_TEST(TST_AppInterface::testScreenshot);
    }

private:
    using Cap = QtLLM::AppInterface::Capability;

    // Main window with a File menu, named and unnamed buttons, two line edits
    // (one a password field), a combo box, a nested row layout and an
    // excluded panel.
    struct Fixture
    {
        QMainWindow  window;
        QAction*     openAction = nullptr;
        QAction*     saveAction = nullptr;
        QAction*     wrapAction = nullptr;
        QWidget*     central = nullptr;
        QVBoxLayout* mainLayout = nullptr;
        QHBoxLayout* row = nullptr;
        QPushButton* okButton = nullptr;
        QPushButton* firstButton = nullptr;
        QPushButton* secondButton = nullptr;
        QLineEdit*   nameEdit = nullptr;
        QLineEdit*   passwordEdit = nullptr;
        QComboBox*   combo = nullptr;
        QLabel*      leftLabel = nullptr;
        QLabel*      rightLabel = nullptr;
        QWidget*     secretPanel = nullptr;
        int          openCount = 0;

        Fixture()
        {
            window.setObjectName("mainWindow");
            window.resize(400, 360);
            QMenu* file = window.menuBar()->addMenu("&File");
            openAction = file->addAction("&Open");
            openAction->setObjectName("actionOpen");
            openAction->setShortcut(QKeySequence("Ctrl+O"));
            QObject::connect(openAction, &QAction::triggered, [this]() { ++openCount; });
            saveAction = file->addAction("Save");
            saveAction->setEnabled(false);
            wrapAction = file->addAction("Word wrap");
            wrapAction->setCheckable(true);

            central = new QWidget;
            central->setObjectName("central");
            window.setCentralWidget(central);
            mainLayout = new QVBoxLayout(central);

            okButton = new QPushButton("OK");
            okButton->setObjectName("okButton");
            firstButton = new QPushButton("First");
            secondButton = new QPushButton("Second");
            nameEdit = new QLineEdit("Alice");
            nameEdit->setObjectName("nameEdit");
            passwordEdit = new QLineEdit("hunter2");
            passwordEdit->setObjectName("passwordEdit");
            passwordEdit->setEchoMode(QLineEdit::Password);
            combo = new QComboBox;
            combo->setObjectName("colorCombo");
            combo->addItems({"red", "green", "blue"});

            row = new QHBoxLayout;
            leftLabel = new QLabel("Left");
            rightLabel = new QLabel("Right");
            row->addWidget(leftLabel);
            row->addWidget(rightLabel);

            secretPanel = new QWidget;
            secretPanel->setObjectName("secretPanel");
            auto* secretLayout = new QVBoxLayout(secretPanel);
            auto* secretButton = new QPushButton("Secret");
            secretButton->setObjectName("secretButton");
            secretLayout->addWidget(secretButton);

            mainLayout->addWidget(okButton);       // 0
            mainLayout->addWidget(firstButton);    // 1
            mainLayout->addWidget(secondButton);   // 2
            mainLayout->addWidget(nameEdit);       // 3
            mainLayout->addWidget(passwordEdit);   // 4
            mainLayout->addWidget(combo);          // 5
            mainLayout->addLayout(row);            // 6
            mainLayout->addWidget(secretPanel);    // 7
        }
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

    static QString dump(const QJsonObject& object)
    {
        return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
    }

    static QStringList idsOf(const QJsonArray& list)
    {
        QStringList ids;
        for (const QJsonValue& value : list)
            ids.append(value.toObject().value("id").toString());
        return ids;
    }

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

    // ------------------------------------------------------------ registration

    TEST_FUNCTION(testCapabilitiesRegisterTools)
    {
        TEST_START;
        if (!UnitTest::Gui::isAvailable()) { TEST_MESSAGE("Skipping: no GUI session"); return; }

        QtLLM::Client client(QtLLM::Provider::Ollama, "http://127.0.0.1:9");
        QWidget window;
        {
            QtLLM::AppInterface ui(&client, &window);
            TEST_ASSERT(uiTools(client).isEmpty());
            TEST_ASSERT(!ui.capabilities());

            ui.setCapabilityEnabled(Cap::ReadWidgets, true);
            TEST_COMPARE(uiTools(client),
                         QStringList({"ui_find_widgets", "ui_get_widget", "ui_get_widget_tree"}));

            ui.setCapabilityEnabled(Cap::ModifyLayouts, true);
            ui.setCapabilityEnabled(Cap::ReadWidgets, false);
            TEST_COMPARE(uiTools(client), QStringList({"ui_modify_layout", "ui_undo_layout_change"}));

            // setCapabilities() replaces the whole set. (The tools of the
            // guidance flags are covered by TST_AppGuidance.)
            ui.setCapabilities(Cap::ReadActions | Cap::Screenshots);
            TEST_COMPARE(uiTools(client), QStringList({"ui_list_actions", "ui_screenshot"}));
            TEST_ASSERT(!ui.isCapabilityEnabled(Cap::Highlights));
            TEST_ASSERT(!ui.isCapabilityEnabled(Cap::ModifyLayouts));

            ui.setCapabilities(Cap::ReadActions | Cap::TriggerActions | Cap::ReadWidgets
                               | Cap::ReadProperties | Cap::WriteProperties | Cap::ReadLayouts
                               | Cap::ModifyLayouts | Cap::Screenshots);
            TEST_COMPARE(uiTools(client),
                         QStringList({"ui_find_widgets", "ui_get_layout", "ui_get_properties",
                                      "ui_get_widget", "ui_get_widget_tree", "ui_list_actions",
                                      "ui_modify_layout", "ui_screenshot", "ui_set_property",
                                      "ui_trigger_action", "ui_undo_layout_change"}));

            for (const QtLLM::Tool& tool : client.registeredTools()) {
                if (!tool.name().startsWith("ui_"))
                    continue;
                TEST_COMPARE(tool.group(), QString("App UI"));
                const bool writes = tool.name() == "ui_trigger_action" || tool.name() == "ui_set_property"
                                 || tool.name() == "ui_modify_layout" || tool.name() == "ui_undo_layout_change";
                TEST_ASSERT(tool.isReadOnly() == !writes);
            }
        }
        // The destructor takes its tools back.
        TEST_ASSERT(uiTools(client).isEmpty());
    }

    // ------------------------------------------------------------ ids

    TEST_FUNCTION(testIdRoundTrip)
    {
        TEST_START;
        if (!UnitTest::Gui::isAvailable()) { TEST_MESSAGE("Skipping: no GUI session"); return; }

        Fixture f;
        QtLLM::AppInterface ui(nullptr, &f.window);
        f.window.addDockWidget(Qt::RightDockWidgetArea, new QtLLM::ChatDockWidget(&f.window));
        ui.excludeWidget(f.secretPanel);
        TEST_ASSERT(UnitTest::Gui::showAndWait(&f.window));

        TEST_COMPARE(ui.idForWidget(&f.window), QString("mainWindow"));
        TEST_COMPARE(ui.idForWidget(f.okButton), QString("mainWindow/central/okButton"));
        TEST_COMPARE(ui.idForWidget(f.firstButton), QString("mainWindow/central/QPushButton#0"));
        TEST_COMPARE(ui.idForWidget(f.secondButton), QString("mainWindow/central/QPushButton#1"));
        TEST_COMPARE(ui.idForWidget(f.rightLabel), QString("mainWindow/central/QLabel#1"));

        for (QWidget* widget : {static_cast<QWidget*>(f.okButton), static_cast<QWidget*>(f.secondButton),
                                static_cast<QWidget*>(f.rightLabel), static_cast<QWidget*>(f.combo)})
            TEST_ASSERT(ui.widgetForId(ui.idForWidget(widget)) == widget);

        // Excluded subtrees, the chat dock and foreign widgets have no id.
        TEST_ASSERT(ui.idForWidget(f.secretPanel).isEmpty());
        TEST_ASSERT(ui.widgetForId("mainWindow/central/secretPanel/secretButton") == nullptr);
        TEST_ASSERT(ui.idForWidget(f.window.findChild<QtLLM::ChatDockWidget*>()).isEmpty());
        QPushButton foreign("Elsewhere");
        TEST_ASSERT(ui.idForWidget(&foreign).isEmpty());
        TEST_ASSERT(ui.widgetForId("Elsewhere") == nullptr);
    }

    TEST_FUNCTION(testStaleIds)
    {
        TEST_START;
        if (!UnitTest::Gui::isAvailable()) { TEST_MESSAGE("Skipping: no GUI session"); return; }

        Fixture f;
        QtLLM::AppInterface ui(nullptr, &f.window);
        TEST_ASSERT(UnitTest::Gui::showAndWait(&f.window));

        const QString firstId = ui.idForWidget(f.firstButton);
        const QString secondId = ui.idForWidget(f.secondButton);
        delete f.firstButton;

        // The second button now sits at the first one's path; neither old id
        // may silently reach it.
        TEST_ASSERT(ui.widgetForId(firstId) == nullptr);
        TEST_ASSERT(ui.widgetForId(secondId) == nullptr);
        const QJsonObject stale = ui.getWidget({{"id", firstId}});
        TEST_ASSERT(errorOf(stale).contains("no longer exists"));

        const QJsonObject unknown = ui.getWidget({{"id", "mainWindow/nothingHere"}});
        TEST_ASSERT(errorOf(unknown).contains("no widget with id"));

        // A fresh id works again.
        TEST_ASSERT(ui.widgetForId(ui.idForWidget(f.secondButton)) == f.secondButton);
    }

    // ------------------------------------------------------------ actions

    TEST_FUNCTION(testActions)
    {
        TEST_START;
        if (!UnitTest::Gui::isAvailable()) { TEST_MESSAGE("Skipping: no GUI session"); return; }

        Fixture f;
        QtLLM::AppInterface ui(nullptr, &f.window);
        TEST_ASSERT(UnitTest::Gui::showAndWait(&f.window));

        const QJsonObject list = ui.listActions({});
        TEST_ASSERT(isOk(list));
        QHash<QString, QJsonObject> byId;
        for (const QJsonValue& value : list.value("actions").toArray())
            byId.insert(value.toObject().value("id").toString(), value.toObject());
        TEST_COMPARE(byId.size(), 3);
        TEST_COMPARE(byId["actionOpen"].value("text").toString(), QString("Open"));
        TEST_COMPARE(byId["actionOpen"].value("menu").toString(), QString("File"));
        TEST_COMPARE(byId["actionOpen"].value("shortcut").toString(), QString("Ctrl+O"));
        TEST_ASSERT(!byId["actionOpen"].contains("enabled"));
        TEST_ASSERT(!byId["actionOpen"].contains("checked"));
        TEST_ASSERT(byId["File/Save"].value("enabled") == QJsonValue(false));
        TEST_ASSERT(byId["File/Word wrap"].value("checked") == QJsonValue(false));

        TEST_COMPARE(ui.listActions({{"filter", "WRAP"}}).value("actions").toArray().size(), 1);

        TEST_ASSERT(isOk(ui.triggerAction({{"id", "actionOpen"}})));
        TEST_ASSERT(UnitTest::Gui::waitFor([&]() { return f.openCount == 1; }));
        TEST_ASSERT(isOk(ui.triggerAction({{"id", "File/Word wrap"}})));
        TEST_ASSERT(UnitTest::Gui::waitFor([&]() { return f.wrapAction->isChecked(); }));

        TEST_ASSERT(errorOf(ui.triggerAction({{"id", "File/Save"}})).contains("disabled"));
        TEST_ASSERT(!errorOf(ui.triggerAction({{"id", "File/Quit"}})).isEmpty());
    }

    // ------------------------------------------------------------ tree and find

    TEST_FUNCTION(testTreeDepthAndExclusion)
    {
        TEST_START;
        if (!UnitTest::Gui::isAvailable()) { TEST_MESSAGE("Skipping: no GUI session"); return; }

        Fixture f;
        QtLLM::AppInterface ui(nullptr, &f.window);
        ui.excludeWidget(f.secretPanel);
        TEST_ASSERT(UnitTest::Gui::showAndWait(&f.window));

        const QJsonObject shallow = ui.getWidgetTree({{"depth", 0}});
        TEST_ASSERT(isOk(shallow));
        const QJsonArray roots = shallow.value("tree").toArray();
        TEST_COMPARE(roots.size(), 1);
        const QJsonObject root = roots.at(0).toObject();
        TEST_COMPARE(root.value("id").toString(), QString("mainWindow"));
        TEST_ASSERT(!root.contains("children"));
        TEST_ASSERT(root.value("more").toInt() > 0);

        const QJsonObject central = ui.getWidgetTree({{"root", "mainWindow/central"}, {"depth", 1}});
        const QJsonArray children = central.value("tree").toArray().at(0).toObject()
                                       .value("children").toArray();
        const QStringList ids = idsOf(children);
        TEST_ASSERT(ids.contains("mainWindow/central/okButton"));
        TEST_ASSERT(ids.contains("mainWindow/central/QPushButton#1"));
        TEST_ASSERT(ids.contains("mainWindow/central/QLabel#0"));
        TEST_ASSERT(!ids.contains("mainWindow/central/secretPanel"));
        for (const QJsonValue& child : children)
            TEST_ASSERT(!child.toObject().contains("children"));

        // The whole tree: nothing from the excluded panel, no password text.
        const QString full = dump(ui.getWidgetTree({{"depth", 10}}));
        TEST_ASSERT(!full.contains("secret", Qt::CaseInsensitive));
        TEST_ASSERT(!full.contains("hunter2"));
        TEST_ASSERT(full.contains("[redacted]"));

        TEST_ASSERT(!errorOf(ui.getWidgetTree({{"root", "mainWindow/central/secretPanel"}})).isEmpty());
    }

    TEST_FUNCTION(testFindWidgets)
    {
        TEST_START;
        if (!UnitTest::Gui::isAvailable()) { TEST_MESSAGE("Skipping: no GUI session"); return; }

        Fixture f;
        QtLLM::AppInterface ui(nullptr, &f.window);
        ui.excludeWidget(f.secretPanel);
        TEST_ASSERT(UnitTest::Gui::showAndWait(&f.window));

        auto find = [&](const QJsonObject& args) { return idsOf(ui.findWidgets(args).value("widgets").toArray()); };

        TEST_COMPARE(find({{"text", "second"}}), QStringList({"mainWindow/central/QPushButton#1"}));
        TEST_COMPARE(find({{"class", "QLineEdit"}}),
                     QStringList({"mainWindow/central/nameEdit", "mainWindow/central/passwordEdit"}));
        TEST_COMPARE(find({{"name", "color"}}), QStringList({"mainWindow/central/colorCombo"}));
        TEST_COMPARE(find({{"class", "QPushButton"}, {"text", "ok"}}),
                     QStringList({"mainWindow/central/okButton"}));
        TEST_ASSERT(find({{"text", "Secret"}}).isEmpty());
        TEST_ASSERT(find({{"text", "hunter"}}).isEmpty());
        TEST_ASSERT(!errorOf(ui.findWidgets({})).isEmpty());

        for (const QString& id : find({{"class", "Button"}}))
            TEST_ASSERT(qobject_cast<QAbstractButton*>(ui.widgetForId(id)) != nullptr);
    }

    // ------------------------------------------------------------ properties

    TEST_FUNCTION(testProperties)
    {
        TEST_START;
        if (!UnitTest::Gui::isAvailable()) { TEST_MESSAGE("Skipping: no GUI session"); return; }

        Fixture f;
        QtLLM::AppInterface ui(nullptr, &f.window);
        TEST_ASSERT(UnitTest::Gui::showAndWait(&f.window));
        const QString nameId = ui.idForWidget(f.nameEdit);

        const QJsonObject read = ui.getProperties({{"id", nameId},
                                                   {"names", QJsonArray{"text", "maxLength", "bogus"}}});
        TEST_ASSERT(isOk(read));
        TEST_COMPARE(read.value("properties").toObject().value("text").toString(), QString("Alice"));
        TEST_COMPARE(read.value("properties").toObject().value("maxLength").toInt(), 32767);
        TEST_COMPARE(read.value("unknown").toArray(), QJsonArray({"bogus"}));

        // Default listing: the class's own properties, empty ones left out.
        const QJsonObject defaults = ui.getProperties({{"id", nameId}}).value("properties").toObject();
        TEST_COMPARE(defaults.value("text").toString(), QString("Alice"));
        TEST_COMPARE(defaults.value("echoMode").toString(), QString("Normal"));
        TEST_ASSERT(!defaults.contains("placeholderText"));

        const QJsonObject written = ui.setWidgetProperty({{"id", nameId}, {"name", "text"}, {"value", "Bob"}});
        TEST_ASSERT(isOk(written));
        TEST_COMPARE(f.nameEdit->text(), QString("Bob"));
        TEST_COMPARE(written.value("value").toString(), QString("Bob"));

        TEST_ASSERT(isOk(ui.setWidgetProperty({{"id", ui.idForWidget(f.combo)},
                                               {"name", "currentIndex"}, {"value", 2}})));
        TEST_COMPARE(f.combo->currentIndex(), 2);

        // Not on the allow-list.
        const QJsonObject refused = ui.setWidgetProperty({{"id", nameId}, {"name", "styleSheet"},
                                                          {"value", "color: red"}});
        TEST_ASSERT(!errorOf(refused).isEmpty());
        TEST_ASSERT(f.nameEdit->styleSheet().isEmpty());

        // A disabled control refuses value changes.
        f.okButton->setEnabled(false);
        TEST_ASSERT(!errorOf(ui.setWidgetProperty({{"id", ui.idForWidget(f.okButton)},
                                                   {"name", "text"}, {"value", "Go"}})).isEmpty());
        TEST_COMPARE(f.okButton->text(), QString("OK"));

        ui.setWritableProperties({"styleSheet"});
        TEST_ASSERT(isOk(ui.setWidgetProperty({{"id", nameId}, {"name", "styleSheet"},
                                               {"value", "color: red"}})));
        TEST_ASSERT(!errorOf(ui.setWidgetProperty({{"id", nameId}, {"name", "text"},
                                                   {"value", "Eve"}})).isEmpty());
    }

    TEST_FUNCTION(testPasswordRedaction)
    {
        TEST_START;
        if (!UnitTest::Gui::isAvailable()) { TEST_MESSAGE("Skipping: no GUI session"); return; }

        Fixture f;
        QtLLM::AppInterface ui(nullptr, &f.window);
        TEST_ASSERT(UnitTest::Gui::showAndWait(&f.window));
        const QString id = ui.idForWidget(f.passwordEdit);

        const QJsonObject details = ui.getWidget({{"id", id}});
        TEST_ASSERT(isOk(details));
        TEST_COMPARE(details.value("text").toString(), QString("[redacted]"));
        TEST_ASSERT(details.value("password").toBool());
        TEST_ASSERT(!dump(details).contains("hunter2"));

        const QJsonObject named = ui.getProperties({{"id", id},
                                                    {"names", QJsonArray{"text", "displayText"}}});
        TEST_COMPARE(named.value("properties").toObject().value("text").toString(), QString("[redacted]"));
        TEST_ASSERT(!dump(named).contains("hunter2"));
        TEST_ASSERT(!dump(ui.getProperties({{"id", id}})).contains("hunter2"));
        TEST_ASSERT(!dump(ui.getWidgetTree({{"depth", 10}})).contains("hunter2"));

        // Writing is allowed; the echoed value is not.
        const QJsonObject written = ui.setWidgetProperty({{"id", id}, {"name", "text"}, {"value", "s3cret"}});
        TEST_ASSERT(isOk(written));
        TEST_COMPARE(f.passwordEdit->text(), QString("s3cret"));
        TEST_ASSERT(!dump(written).contains("s3cret"));
    }

    // ------------------------------------------------------------ layouts

    TEST_FUNCTION(testLayouts)
    {
        TEST_START;
        if (!UnitTest::Gui::isAvailable()) { TEST_MESSAGE("Skipping: no GUI session"); return; }

        Fixture f;
        QtLLM::AppInterface ui(nullptr, &f.window);
        ui.excludeWidget(f.secretPanel);
        TEST_ASSERT(UnitTest::Gui::showAndWait(&f.window));
        const QString centralId = ui.idForWidget(f.central);
        const int spacing = f.mainLayout->spacing();
        const QMargins margins = f.mainLayout->contentsMargins();

        const QJsonObject read = ui.getLayout({{"id", centralId}});
        TEST_ASSERT(isOk(read));
        const QJsonObject layout = read.value("layout").toObject();
        TEST_COMPARE(layout.value("type").toString(), QString("QVBoxLayout"));
        TEST_COMPARE(layout.value("direction").toString(), QString("vertical"));
        const QJsonArray items = layout.value("items").toArray();
        TEST_COMPARE(items.size(), 7);   // the excluded panel is left out
        TEST_COMPARE(items.at(0).toObject().value("widget").toString(), QString("mainWindow/central/okButton"));
        const QJsonObject nested = items.at(6).toObject().value("layout").toObject();
        TEST_COMPARE(nested.value("sublayout").toString(), QString("6"));
        TEST_COMPARE(nested.value("items").toArray().at(0).toObject().value("widget").toString(),
                     QString("mainWindow/central/QLabel#0"));

        TEST_ASSERT(errorOf(ui.getLayout({{"id", ui.idForWidget(f.okButton)}})).contains("mainWindow/central"));

        TEST_ASSERT(isOk(ui.modifyLayout({{"id", centralId}, {"operation", "set_spacing"}, {"value", 17}})));
        TEST_COMPARE(f.mainLayout->spacing(), 17);
        TEST_ASSERT(isOk(ui.modifyLayout({{"id", centralId}, {"operation", "set_margins"}, {"value", 3}})));
        TEST_ASSERT(f.mainLayout->contentsMargins() == QMargins(3, 3, 3, 3));
        TEST_ASSERT(isOk(ui.modifyLayout({{"id", centralId}, {"operation", "set_visible"},
                                          {"item", 1}, {"visible", false}})));
        TEST_ASSERT(f.firstButton->isHidden());
        TEST_ASSERT(isOk(ui.modifyLayout({{"id", centralId}, {"sublayout", "6"}, {"operation", "move_item"},
                                          {"item", 0}, {"to", 1}})));
        TEST_ASSERT(f.row->itemAt(0)->widget() == f.rightLabel);
        TEST_ASSERT(isOk(ui.modifyLayout({{"id", centralId}, {"operation", "set_stretch"},
                                          {"item", 5}, {"value", 2}})));
        TEST_COMPARE(f.mainLayout->stretch(5), 2);
        TEST_ASSERT(!errorOf(ui.modifyLayout({{"id", centralId}, {"operation", "move_item"},
                                              {"item", 0}, {"to", 99}})).isEmpty());

        // Undo reverts only the last change.
        TEST_ASSERT(isOk(ui.undoLayoutChange({})));
        TEST_COMPARE(f.mainLayout->stretch(5), 0);
        TEST_ASSERT(f.row->itemAt(0)->widget() == f.rightLabel);
        TEST_ASSERT(isOk(ui.undoLayoutChange({})));
        TEST_ASSERT(f.row->itemAt(0)->widget() == f.leftLabel);
        TEST_COMPARE(f.mainLayout->spacing(), 17);

        // Reset reverts everything that is left.
        ui.resetLayoutChanges();
        TEST_COMPARE(f.mainLayout->spacing(), spacing);
        TEST_ASSERT(f.mainLayout->contentsMargins() == margins);
        TEST_ASSERT(!f.firstButton->isHidden());
        TEST_ASSERT(!errorOf(ui.undoLayoutChange({})).isEmpty());
    }

    // ------------------------------------------------------------ screenshot

    TEST_FUNCTION(testScreenshot)
    {
        TEST_START;
        if (!UnitTest::Gui::isAvailable()) { TEST_MESSAGE("Skipping: no GUI session"); return; }

        Fixture f;
        QtLLM::AppInterface ui(nullptr, &f.window);
        ui.excludeWidget(f.secretPanel);
        TEST_ASSERT(UnitTest::Gui::showAndWait(&f.window));
        const QString imagesKey = QString::fromLatin1(QtLLM::kToolImagesKey);

        const QJsonObject shot = ui.screenshot({{"annotate", true}, {"maxEdge", 300}});
        TEST_ASSERT(isOk(shot));
        const QJsonArray images = shot.value(imagesKey).toArray();
        TEST_COMPARE(images.size(), 1);
        const QJsonObject image = images.at(0).toObject();
        TEST_ASSERT(qMax(image.value("width").toInt(), image.value("height").toInt()) <= 300);
        TEST_COMPARE(shot.value("target").toString(), QString("mainWindow"));

        // Every marker names a real, interactive widget; all controls are marked.
        const QJsonObject markers = shot.value("markers").toObject();
        QStringList marked;
        for (const QString& number : markers.keys()) {
            const QString id = markers.value(number).toString();
            TEST_ASSERT(ui.widgetForId(id) != nullptr);
            marked.append(id);
        }
        for (QWidget* control : {static_cast<QWidget*>(f.okButton), static_cast<QWidget*>(f.firstButton),
                                 static_cast<QWidget*>(f.secondButton), static_cast<QWidget*>(f.nameEdit),
                                 static_cast<QWidget*>(f.passwordEdit), static_cast<QWidget*>(f.combo)})
            TEST_ASSERT(marked.contains(ui.idForWidget(control)));
        TEST_ASSERT(!marked.contains(ui.idForWidget(f.leftLabel)));
        TEST_COMPARE(marked.size(), 6);
        TEST_ASSERT(!dump(shot).contains("secret"));

        // A single widget, here the password field: blacked out.
        const QJsonObject field = ui.screenshot({{"target", ui.idForWidget(f.passwordEdit)}});
        TEST_ASSERT(isOk(field));
        TEST_ASSERT(field.value("size").toArray() == QJsonArray({f.passwordEdit->width(), f.passwordEdit->height()}));
        const QJsonObject fieldImage = field.value(imagesKey).toArray().at(0).toObject();
        const QImage decoded = QImage::fromData(QByteArray::fromBase64(fieldImage.value("data").toString().toLatin1()));
        TEST_ASSERT(!decoded.isNull());
        if (!decoded.isNull())
            TEST_ASSERT(decoded.pixelColor(decoded.width() / 2, decoded.height() / 2) == QColor(Qt::black));
    }
};

TEST_INSTANTIATE(TST_AppInterface);

#endif // UNITTEST_GUI_ENABLED
