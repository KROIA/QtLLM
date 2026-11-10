#pragma once

// Styling tools of AppInterface: recolouring single widgets (with restore) and
// switching between the themes the application registered.
#ifdef UNITTEST_GUI_ENABLED

#include "UnitTest.h"
#include "UnitTest_Gui.h"
#include "QtLLM.h"
#include "AppInterface.h"
#include <QApplication>
#include <QFrame>
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QPixmap>
#include <QPushButton>
#include <QStyle>
#include <QVBoxLayout>

class TST_AppStyling : public UnitTest::Test
{
    TEST_CLASS(TST_AppStyling)
public:
    TST_AppStyling()
        : Test("TST_AppStyling")
    {
        ADD_TEST(TST_AppStyling::testCapabilitiesRegisterTools);
        ADD_TEST(TST_AppStyling::testColorsApplyToTheWidgetOnly);
        ADD_TEST(TST_AppStyling::testColorsMergeAndRestore);
        ADD_TEST(TST_AppStyling::testInvalidInputIsRejected);
        ADD_TEST(TST_AppStyling::testThemesAreLimitedToRegisteredOnes);
        ADD_TEST(TST_AppStyling::testStandardThemesSwitchThePalette);
        ADD_TEST(TST_AppStyling::testThemeSwitchReachesInsideColouredWidgets);
    }

private:
    using Cap = QtLLM::AppInterface::Capability;

    // A window with a framed panel (label inside, free space below it) and a
    // title label that already has a style sheet of its own.
    struct Fixture
    {
        QWidget window;
        QFrame* panel = nullptr;
        QLabel* inner = nullptr;
        QLabel* title = nullptr;

        Fixture()
        {
            window.setObjectName("styleWindow");
            auto* layout = new QVBoxLayout(&window);
            title = new QLabel("Title", &window);
            title->setObjectName("titleLabel");
            title->setStyleSheet("color: #888888;");
            layout->addWidget(title);

            panel = new QFrame(&window);
            panel->setObjectName("panel");
            auto* panelLayout = new QVBoxLayout(panel);
            panelLayout->setContentsMargins(20, 20, 20, 20);
            inner = new QLabel("Inner", panel);
            inner->setObjectName("innerLabel");
            panelLayout->addWidget(inner);
            panelLayout->addStretch();
            layout->addWidget(panel, 1);
            window.resize(320, 260);
        }
    };

    static bool isOk(const QJsonObject& result) { return result["status"].toString() == "ok"; }

    static QStringList styleTools(const QtLLM::Client& client)
    {
        QStringList names;
        for (const QString& name : client.toolNames()) {
            if (name.contains("color") || name.contains("theme"))
                names.append(name);
        }
        names.sort();
        return names;
    }

    TEST_FUNCTION(testCapabilitiesRegisterTools)
    {
        TEST_START;

        QtLLM::Client client(QtLLM::Provider::Ollama, "http://127.0.0.1:9");
        QWidget window;
        {
            QtLLM::AppInterface ui(&client, &window);
            ui.setCapabilityEnabled(Cap::WidgetColors, true);
            TEST_COMPARE(styleTools(client),
                         QStringList({"ui_restore_widget_colors", "ui_set_widget_colors"}));

            // setCapabilities() covers the new flags like the old ones.
            ui.setCapabilities(Cap::Themes | Cap::Highlights);
            TEST_COMPARE(styleTools(client), QStringList({"ui_list_themes", "ui_set_theme"}));
            TEST_ASSERT(client.toolNames().contains("ui_highlight_widget"));

            // The theme names are part of the schema, also when a theme is
            // registered after the capability was switched on.
            ui.registerTheme("ocean", "Blue tones", []() {});
            QJsonArray allowed;
            for (const QtLLM::Tool& tool : client.registeredTools()) {
                if (tool.name() == "ui_set_theme")
                    allowed = tool.parameterSchema()["properties"].toObject()["theme"].toObject()["enum"].toArray();
            }
            TEST_ASSERT(allowed == QJsonArray({"ocean"}));
        }
        // Destroying the interface takes its tools along.
        TEST_ASSERT(styleTools(client).isEmpty());
    }

    // The colours must land on the addressed widget and not on what is inside
    // it - a style sheet without selector would colour every child as well.
    TEST_FUNCTION(testColorsApplyToTheWidgetOnly)
    {
        TEST_START;
        if (!UnitTest::Gui::isAvailable()) { TEST_MESSAGE("Skipping: no GUI session"); return; }

        QtLLM::Client client(QtLLM::Provider::Ollama, "http://127.0.0.1:9");
        Fixture f;
        QtLLM::AppInterface ui(&client, &f.window);
        TEST_ASSERT(UnitTest::Gui::showAndWait(&f.window));

        const QJsonObject result = ui.setWidgetColors(
            {{"id", ui.idForWidget(f.panel)}, {"background", "#ff0000"}, {"border", "blue"}});
        TEST_ASSERT(isOk(result));
        TEST_COMPARE(result["colors"].toObject()["border"].toString(), QString("#0000ff"));
        QApplication::processEvents();

        const QImage panel = f.panel->grab().toImage();
        TEST_ASSERT(QColor(panel.pixel(panel.width() / 2, panel.height() - 10)) == QColor("#ff0000"));
        TEST_ASSERT(QColor(panel.pixel(0, panel.height() / 2)) == QColor("#0000ff"));

        const QImage inner = f.inner->grab().toImage();
        TEST_ASSERT(QColor(inner.pixel(0, inner.height() / 2)) != QColor("#0000ff"));   // no border of its own
        TEST_ASSERT(f.inner->styleSheet().isEmpty());

        TEST_COMPARE(ui.restoreWidgetColors({})["restored"].toInt(), 1);
        QApplication::processEvents();
        const QImage restored = f.panel->grab().toImage();
        TEST_ASSERT(QColor(restored.pixel(restored.width() / 2, restored.height() - 10)) != QColor("#ff0000"));
    }

    TEST_FUNCTION(testColorsMergeAndRestore)
    {
        TEST_START;

        QtLLM::Client client(QtLLM::Provider::Ollama, "http://127.0.0.1:9");
        Fixture f;
        QtLLM::AppInterface ui(&client, &f.window);
        const QString titleId = ui.idForWidget(f.title);
        const QString original = f.title->styleSheet();

        TEST_ASSERT(isOk(ui.setWidgetColors({{"id", titleId}, {"background", "gold"}})));
        // The widget's own style survives next to the new rule.
        TEST_ASSERT(f.title->styleSheet().contains("color: #888888;"));
        TEST_ASSERT(f.title->styleSheet().contains("background-color: #ffd700;"));

        // A second call changes what it names and keeps the rest.
        const QJsonObject second = ui.setWidgetColors({{"id", titleId}, {"text", "white"}});
        TEST_COMPARE(second["colors"].toObject()["background"].toString(), QString("#ffd700"));
        TEST_ASSERT(f.title->styleSheet().contains("background-color: #ffd700;"));
        TEST_ASSERT(f.title->styleSheet().contains("color: #ffffff;"));

        TEST_COMPARE(ui.restoreWidgetColors({{"id", titleId}})["restored"].toInt(), 1);
        TEST_COMPARE(f.title->styleSheet(), original);
        TEST_COMPARE(ui.restoreWidgetColors({{"id", titleId}})["restored"].toInt(), 0);

        // Without an id everything goes back - and a widget that was deleted
        // in the meantime is skipped.
        auto* temporary = new QLabel("gone soon", &f.window);
        temporary->setObjectName("temporary");
        ui.setWidgetColors({{"id", titleId}, {"border", "red"}});
        ui.setWidgetColors({{"id", ui.idForWidget(f.panel)}, {"background", "#102030"}});
        ui.setWidgetColors({{"id", ui.idForWidget(temporary)}, {"background", "green"}});
        delete temporary;
        TEST_COMPARE(ui.restoreWidgetColors({})["restored"].toInt(), 2);
        TEST_COMPARE(f.title->styleSheet(), original);
        TEST_ASSERT(f.panel->styleSheet().isEmpty());

        // The C++ side of the same thing.
        ui.setWidgetColors({{"id", titleId}, {"border", "red"}});
        ui.resetWidgetColors();
        TEST_COMPARE(f.title->styleSheet(), original);
    }

    TEST_FUNCTION(testInvalidInputIsRejected)
    {
        TEST_START;

        QtLLM::Client client(QtLLM::Provider::Ollama, "http://127.0.0.1:9");
        Fixture f;
        QtLLM::AppInterface ui(&client, &f.window);
        const QString panelId = ui.idForWidget(f.panel);

        // Only colours reach the style sheet - nothing else can be injected.
        TEST_ASSERT(!isOk(ui.setWidgetColors({{"id", panelId}, {"background", "red; font-size: 40px"}})));
        TEST_ASSERT(!isOk(ui.setWidgetColors({{"id", panelId}, {"text", "notacolour"}})));
        TEST_ASSERT(!isOk(ui.setWidgetColors({{"id", panelId}})));
        TEST_ASSERT(!isOk(ui.setWidgetColors({{"id", "styleWindow/nothing"}, {"text", "red"}})));
        TEST_ASSERT(f.panel->styleSheet().isEmpty());
    }

    TEST_FUNCTION(testThemesAreLimitedToRegisteredOnes)
    {
        TEST_START;

        QtLLM::Client client(QtLLM::Provider::Ollama, "http://127.0.0.1:9");
        QWidget window;
        QtLLM::AppInterface ui(&client, &window);
        int ocean = 0, sand = 0;
        ui.registerTheme("ocean", "Blue tones", [&]() { ++ocean; });
        ui.registerTheme("sand", "Warm tones", [&]() { ++sand; });
        QStringList changes;
        QObject::connect(&ui, &QtLLM::AppInterface::themeChanged,
                         [&](const QString& name) { changes.append(name); });

        const QJsonObject list = ui.listThemes({});
        TEST_COMPARE(list["themes"].toArray().size(), 2);
        TEST_COMPARE(list["themes"].toArray()[1].toObject()["description"].toString(), QString("Warm tones"));
        TEST_COMPARE(list["current"].toString(), QString());

        TEST_ASSERT(isOk(ui.applyTheme({{"theme", "sand"}})));
        TEST_COMPARE(sand, 1);
        TEST_COMPARE(ui.currentTheme(), QString("sand"));

        // An invented theme changes nothing and names what exists.
        const QJsonObject refused = ui.applyTheme({{"theme", "neon"}});
        TEST_ASSERT(!isOk(refused));
        TEST_ASSERT(refused["message"].toString().contains("ocean, sand"));
        TEST_COMPARE(ui.currentTheme(), QString("sand"));

        TEST_ASSERT(ui.setTheme("OCEAN"));   // forgiving about case
        TEST_COMPARE(ocean, 1);
        TEST_COMPARE(changes, QStringList({"sand", "ocean"}));

        // Registering a name again replaces the theme instead of adding one.
        ui.registerTheme("sand", "Warmer tones", [&]() { sand += 10; });
        TEST_COMPARE(ui.themeNames(), QStringList({"ocean", "sand"}));
        ui.setTheme("sand");
        TEST_COMPARE(sand, 11);
    }

    TEST_FUNCTION(testStandardThemesSwitchThePalette)
    {
        TEST_START;
        if (!UnitTest::Gui::isAvailable()) { TEST_MESSAGE("Skipping: no GUI session"); return; }

        const QColor windowBefore = QApplication::palette().color(QPalette::Window);
        const QString styleBefore = QApplication::style()->objectName();

        QtLLM::Client client(QtLLM::Provider::Ollama, "http://127.0.0.1:9");
        QWidget window;
        QtLLM::AppInterface ui(&client, &window);
        ui.registerStandardThemes();
        TEST_COMPARE(ui.themeNames(), QStringList({"default", "dark"}));
        TEST_COMPARE(ui.currentTheme(), QString("default"));

        TEST_ASSERT(ui.setTheme("dark"));
        const QColor dark = QtLLM::AppInterface::darkPalette().color(QPalette::Window);
        TEST_ASSERT(QApplication::palette().color(QPalette::Window) == dark);
        TEST_ASSERT(dark.lightness() < 80);
        TEST_COMPARE(QApplication::style()->objectName().toLower(), QString("fusion"));

        // Back to exactly what the application looked like before.
        TEST_ASSERT(ui.setTheme("default"));
        TEST_ASSERT(QApplication::palette().color(QPalette::Window) == windowBefore);
        TEST_COMPARE(QApplication::style()->objectName().toLower(), styleBefore.toLower());
    }

    // A recoloured widget carries a style sheet, and Qt does not hand a new
    // application palette on to the widgets below one by itself: after
    // dark -> default the buttons inside kept their light text.
    TEST_FUNCTION(testThemeSwitchReachesInsideColouredWidgets)
    {
        TEST_START;
        if (!UnitTest::Gui::isAvailable()) { TEST_MESSAGE("Skipping: no GUI session"); return; }

        QtLLM::Client client(QtLLM::Provider::Ollama, "http://127.0.0.1:9");
        Fixture f;
        auto* button = new QPushButton("Inside", f.panel);
        button->setObjectName("insideButton");
        f.panel->layout()->addWidget(button);
        QtLLM::AppInterface ui(&client, &f.window);
        ui.registerStandardThemes();
        TEST_ASSERT(UnitTest::Gui::showAndWait(&f.window));
        const QColor lightText = QApplication::palette().color(QPalette::ButtonText);
        const QColor lightWindow = QApplication::palette().color(QPalette::Window);
        const QColor darkText = QtLLM::AppInterface::darkPalette().color(QPalette::ButtonText);

        ui.setTheme("dark");
        ui.setWidgetColors({{"id", ui.idForWidget(f.panel)}, {"background", "#ffffe0"}});
        QApplication::processEvents();
        TEST_ASSERT(button->palette().color(QPalette::ButtonText) == darkText);

        ui.setTheme("default");
        QApplication::processEvents();
        TEST_ASSERT(button->palette().color(QPalette::ButtonText) == lightText);
        TEST_ASSERT(f.inner->palette().color(QPalette::WindowText)
                    == QApplication::palette().color(QPalette::WindowText));
        TEST_ASSERT(f.title->palette().color(QPalette::Window) == lightWindow);   // outside the panel

        // The colours survive the theme switch, and restoring afterwards
        // leaves nothing of the dark theme behind.
        TEST_ASSERT(f.panel->styleSheet().contains("#ffffe0"));
        ui.resetWidgetColors();
        QApplication::processEvents();
        TEST_ASSERT(f.panel->palette().color(QPalette::Window) == lightWindow);
        TEST_ASSERT(button->palette().color(QPalette::ButtonText) == lightText);

        // And the other way round: coloured in the default theme, then dark.
        ui.setWidgetColors({{"id", ui.idForWidget(f.panel)}, {"background", "#ffffe0"}});
        ui.setTheme("dark");
        QApplication::processEvents();
        TEST_ASSERT(button->palette().color(QPalette::ButtonText) == darkText);
        ui.resetWidgetColors();
        ui.setTheme("default");
        QApplication::processEvents();
        TEST_ASSERT(button->palette().color(QPalette::ButtonText) == lightText);
        TEST_ASSERT(f.panel->palette().color(QPalette::Window) == lightWindow);
    }
};

TEST_INSTANTIATE(TST_AppStyling);

#endif // UNITTEST_GUI_ENABLED
