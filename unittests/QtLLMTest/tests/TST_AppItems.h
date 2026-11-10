#pragma once

// Item tools of AppInterface: reading, selecting and editing the items of
// tables, trees, lists, combo boxes and tab widgets. Nothing is shown; the
// views work on their models without a window.
#ifdef UNITTEST_GUI_ENABLED

#include "UnitTest.h"
#include "UnitTest_Gui.h"
#include "QtLLM.h"
#include "AppInterface.h"
#include <QComboBox>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QListWidget>
#include <QTabWidget>
#include <QTableWidget>
#include <QTreeWidget>

class TST_AppItems : public UnitTest::Test
{
    TEST_CLASS(TST_AppItems)
public:
    TST_AppItems()
        : Test("TST_AppItems")
    {
        ADD_TEST(TST_AppItems::testCapabilitiesRegisterTools);
        ADD_TEST(TST_AppItems::testTableRead);
        ADD_TEST(TST_AppItems::testTreeRead);
        ADD_TEST(TST_AppItems::testListComboAndTabsRead);
        ADD_TEST(TST_AppItems::testSelectInViews);
        ADD_TEST(TST_AppItems::testSelectComboAndTabs);
        ADD_TEST(TST_AppItems::testEdit);
        ADD_TEST(TST_AppItems::testErrors);
    }

private:
    using Cap = QtLLM::AppInterface::Capability;

    // One window holding every supported kind of item widget plus a label.
    struct Fixture
    {
        QWidget window;
        QTableWidget* table = nullptr;
        QTreeWidget* tree = nullptr;
        QListWidget* list = nullptr;
        QComboBox* combo = nullptr;
        QTabWidget* tabs = nullptr;
        QLabel* label = nullptr;

        Fixture()
        {
            window.setObjectName("itemWindow");

            // 120 rows "file0".."file119" | size | kind
            table = new QTableWidget(120, 3, &window);
            table->setObjectName("table");
            table->setHorizontalHeaderLabels({"Name", "Size", "Kind"});
            for (int r = 0; r < 120; ++r) {
                table->setItem(r, 0, new QTableWidgetItem(QString("file%1").arg(r)));
                table->setItem(r, 1, new QTableWidgetItem(QString::number(r * 10)));
                table->setItem(r, 2, new QTableWidgetItem(r % 2 ? "odd" : "even"));
            }
            table->item(5, 1)->setText(QString(500, 'x'));
            table->item(6, 0)->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);

            // A (A0, A1 (A1a)), B
            tree = new QTreeWidget(&window);
            tree->setObjectName("tree");
            tree->setHeaderLabels({"Node"});
            auto* a = new QTreeWidgetItem(tree, QStringList{"A"});
            new QTreeWidgetItem(a, QStringList{"A0"});
            auto* a1 = new QTreeWidgetItem(a, QStringList{"A1"});
            new QTreeWidgetItem(a1, QStringList{"A1a"});
            new QTreeWidgetItem(tree, QStringList{"B"});

            list = new QListWidget(&window);
            list->setObjectName("list");
            list->setSelectionMode(QAbstractItemView::ExtendedSelection);
            for (const QString& text : {"one", "two", "three", "plain"}) {
                auto* item = new QListWidgetItem(text, list);
                if (text != "plain") {
                    item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
                    item->setCheckState(text == "one" ? Qt::Checked : Qt::Unchecked);
                }
            }
            list->item(2)->setFlags(list->item(2)->flags() & ~Qt::ItemIsEnabled);

            combo = new QComboBox(&window);
            combo->setObjectName("combo");
            combo->addItems({"Red", "Green", "Blue"});
            combo->setCurrentIndex(1);

            tabs = new QTabWidget(&window);
            tabs->setObjectName("tabs");
            tabs->addTab(new QWidget, "&First");
            tabs->addTab(new QWidget, "Second");
            tabs->addTab(new QWidget, "Third");
            tabs->setTabEnabled(2, false);

            label = new QLabel("Label", &window);
            label->setObjectName("label");
        }
    };

    static bool isOk(const QJsonObject& result) { return result["status"].toString() == "ok"; }
    static QString errorOf(const QJsonObject& result)
    {
        return result["status"].toString() == "error" ? result["message"].toString() : QString();
    }
    static QStringList itemTools(const QtLLM::Client& client)
    {
        QStringList names;
        for (const QString& name : client.toolNames()) {
            if (name.contains("item"))
                names.append(name);
        }
        names.sort();
        return names;
    }
    static QString cell(const QJsonValue& row, int index = 0)
    {
        return row.toObject()["cells"].toArray()[index].toString();
    }

    TEST_FUNCTION(testCapabilitiesRegisterTools)
    {
        TEST_START;

        QtLLM::Client client(QtLLM::Provider::Ollama, "http://127.0.0.1:9");
        QWidget window;
        {
            QtLLM::AppInterface ui(&client, &window);
            TEST_ASSERT(itemTools(client).isEmpty());
            ui.setCapabilityEnabled(Cap::ReadItems, true);
            TEST_COMPARE(itemTools(client), QStringList({"ui_get_items"}));
            ui.setCapabilityEnabled(Cap::SelectItems, true);
            ui.setCapabilityEnabled(Cap::EditItems, true);
            TEST_COMPARE(itemTools(client),
                         QStringList({"ui_get_items", "ui_select_item", "ui_set_item_text"}));
            for (const QtLLM::Tool& tool : client.registeredTools()) {
                if (tool.name() == "ui_get_items")
                    TEST_ASSERT(tool.isReadOnly());
                if (tool.name() == "ui_select_item" || tool.name() == "ui_set_item_text")
                    TEST_ASSERT(!tool.isReadOnly());
            }
            ui.setCapabilityEnabled(Cap::ReadItems, false);
            TEST_COMPARE(itemTools(client), QStringList({"ui_select_item", "ui_set_item_text"}));
        }
        TEST_ASSERT(itemTools(client).isEmpty());
    }

    TEST_FUNCTION(testTableRead)
    {
        TEST_START;

        QtLLM::Client client(QtLLM::Provider::Ollama, "http://127.0.0.1:9");
        Fixture f;
        QtLLM::AppInterface ui(&client, &f.window);
        const QString id = ui.idForWidget(f.table);

        const QJsonObject first = ui.getItems({{"id", id}});
        TEST_ASSERT(isOk(first));
        TEST_ASSERT(first["headers"].toArray() == QJsonArray({"Name", "Size", "Kind"}));
        TEST_ASSERT(!first.contains("columns"));
        TEST_COMPARE(first["rowCount"].toInt(), 120);
        TEST_COMPARE(first["rows"].toArray().size(), 50);
        TEST_ASSERT(first["truncated"].toBool());
        TEST_COMPARE(first["nextFirstRow"].toInt(), 50);
        TEST_COMPARE(cell(first["rows"].toArray()[3], 1), QString("30"));
        // Long cells are cut, flags appear only where they apply.
        TEST_ASSERT(cell(first["rows"].toArray()[5], 1).size() <= 203);
        TEST_ASSERT(!first["rows"].toArray()[0].toObject().contains("checked"));

        const QJsonObject last = ui.getItems({{"id", id}, {"firstRow", 100}});
        TEST_COMPARE(last["rows"].toArray().size(), 20);
        TEST_COMPARE(last["rows"].toArray()[0].toObject()["row"].toInt(), 100);
        TEST_ASSERT(!last.contains("truncated"));

        // maxRows is capped at 200.
        TEST_COMPARE(ui.getItems({{"id", id}, {"maxRows", 1000}})["rows"].toArray().size(), 120);

        const QJsonObject subset = ui.getItems({{"id", id}, {"columns", QJsonArray{2, 0}}, {"maxRows", 1}});
        TEST_ASSERT(subset["headers"].toArray() == QJsonArray({"Kind", "Name"}));
        TEST_ASSERT(subset["columns"].toArray() == QJsonArray({2, 0}));
        TEST_ASSERT(subset["rows"].toArray()[0].toObject()["cells"].toArray() == QJsonArray({"even", "file0"}));

        // The filter searches the whole level and keeps the real row numbers.
        const QJsonObject filtered = ui.getItems({{"id", id}, {"filter", "FILE11"}});
        const QJsonArray rows = filtered["rows"].toArray();
        TEST_COMPARE(rows.size(), 11);
        TEST_COMPARE(rows[0].toObject()["row"].toInt(), 11);
        TEST_COMPARE(rows[1].toObject()["row"].toInt(), 110);
        TEST_COMPARE(filtered["rowCount"].toInt(), 120);

        TEST_ASSERT(errorOf(ui.getItems({{"id", id}, {"columns", QJsonArray{3}}})).contains("0..2"));
    }

    TEST_FUNCTION(testTreeRead)
    {
        TEST_START;

        QtLLM::Client client(QtLLM::Provider::Ollama, "http://127.0.0.1:9");
        Fixture f;
        QtLLM::AppInterface ui(&client, &f.window);
        const QString id = ui.idForWidget(f.tree);

        const QJsonObject top = ui.getItems({{"id", id}});
        TEST_COMPARE(top["rowCount"].toInt(), 2);
        const QJsonObject a = top["rows"].toArray()[0].toObject();
        TEST_COMPARE(a["childCount"].toInt(), 2);
        TEST_ASSERT(a["expanded"] == QJsonValue(false));
        TEST_ASSERT(!a.contains("children"));
        TEST_ASSERT(!top["rows"].toArray()[1].toObject().contains("childCount"));
        TEST_ASSERT(top["headers"].toArray() == QJsonArray({"Node"}));

        const QJsonObject level = ui.getItems({{"id", id}, {"parent", QJsonArray{0}}});
        TEST_COMPARE(level["rowCount"].toInt(), 2);
        TEST_COMPARE(cell(level["rows"].toArray()[1]), QString("A1"));
        TEST_COMPARE(level["rows"].toArray()[1].toObject()["childCount"].toInt(), 1);

        const QJsonObject deep = ui.getItems({{"id", id}, {"depth", 2}});
        const QJsonArray children = deep["rows"].toArray()[0].toObject()["children"].toArray();
        TEST_COMPARE(children.size(), 2);
        TEST_COMPARE(cell(children[1].toObject()["children"].toArray()[0]), QString("A1a"));

        TEST_ASSERT(errorOf(ui.getItems({{"id", id}, {"parent", QJsonArray{5}}})).contains("0..1"));
    }

    TEST_FUNCTION(testListComboAndTabsRead)
    {
        TEST_START;

        QtLLM::Client client(QtLLM::Provider::Ollama, "http://127.0.0.1:9");
        Fixture f;
        QtLLM::AppInterface ui(&client, &f.window);

        const QJsonArray list = ui.getItems({{"id", ui.idForWidget(f.list)}})["rows"].toArray();
        TEST_COMPARE(list.size(), 4);
        TEST_ASSERT(list[0].toObject()["checked"] == QJsonValue(true));
        TEST_ASSERT(list[1].toObject()["checked"] == QJsonValue(false));
        TEST_ASSERT(list[2].toObject()["enabled"] == QJsonValue(false));
        TEST_ASSERT(!list[3].toObject().contains("checked"));

        const QJsonObject combo = ui.getItems({{"id", ui.idForWidget(f.combo)}});
        TEST_COMPARE(combo["rowCount"].toInt(), 3);
        TEST_ASSERT(!combo.contains("headers"));
        TEST_ASSERT(combo["rows"].toArray()[1].toObject()["current"].toBool());
        TEST_ASSERT(!combo["rows"].toArray()[0].toObject().contains("current"));
        TEST_COMPARE(cell(combo["rows"].toArray()[2]), QString("Blue"));

        const QJsonArray tabs = ui.getItems({{"id", ui.idForWidget(f.tabs)}})["rows"].toArray();
        TEST_COMPARE(tabs.size(), 3);
        TEST_COMPARE(cell(tabs[0]), QString("First"));
        TEST_ASSERT(tabs[0].toObject()["current"].toBool());
        TEST_ASSERT(tabs[2].toObject()["enabled"] == QJsonValue(false));
    }

    TEST_FUNCTION(testSelectInViews)
    {
        TEST_START;

        QtLLM::Client client(QtLLM::Provider::Ollama, "http://127.0.0.1:9");
        Fixture f;
        QtLLM::AppInterface ui(&client, &f.window);
        const QString tableId = ui.idForWidget(f.table);

        TEST_ASSERT(isOk(ui.selectItem({{"id", tableId}, {"row", 3}})));
        TEST_COMPARE(f.table->currentRow(), 3);
        TEST_ASSERT(f.table->item(3, 0)->isSelected());

        // Exact text wins over partial matches ("file7" is part of "file70").
        TEST_ASSERT(isOk(ui.selectItem({{"id", tableId}, {"text", "file7"}})));
        TEST_COMPARE(f.table->currentRow(), 7);
        TEST_ASSERT(!f.table->item(3, 0)->isSelected());
        TEST_ASSERT(errorOf(ui.selectItem({{"id", tableId}, {"text", "ile1"}})).contains("matches"));
        TEST_ASSERT(errorOf(ui.selectItem({{"id", tableId}, {"row", 500}})).contains("0..119"));
        // The selection is visible to ui_get_items.
        const QJsonObject row7 = ui.getItems({{"id", tableId}, {"firstRow", 7}, {"maxRows", 1}})["rows"]
                                     .toArray()[0].toObject();
        TEST_ASSERT(row7["current"].toBool() && row7["selected"].toBool());

        const QString listId = ui.idForWidget(f.list);
        TEST_ASSERT(isOk(ui.selectItem({{"id", listId}, {"row", 0}})));
        TEST_ASSERT(isOk(ui.selectItem({{"id", listId}, {"row", 1}, {"extend", true}})));
        TEST_COMPARE(f.list->selectedItems().size(), 2);
        TEST_ASSERT(isOk(ui.selectItem({{"id", listId}, {"row", 3}})));
        TEST_COMPARE(f.list->selectedItems().size(), 1);
        TEST_ASSERT(errorOf(ui.selectItem({{"id", listId}, {"row", 2}})).contains("disabled"));

        int activated = 0;
        int doubleClicked = 0;
        int itemActivated = 0;
        QObject::connect(f.list, &QAbstractItemView::activated, [&]() { ++activated; });
        QObject::connect(f.list, &QAbstractItemView::doubleClicked, [&]() { ++doubleClicked; });
        QObject::connect(f.list, &QListWidget::itemActivated, [&]() { ++itemActivated; });
        TEST_ASSERT(isOk(ui.selectItem({{"id", listId}, {"text", "two"}, {"activate", true}})));
        TEST_COMPARE(activated, 1);
        TEST_COMPARE(doubleClicked, 1);
        TEST_COMPARE(itemActivated, 1);
        TEST_COMPARE(f.list->currentRow(), 1);

        const QString treeId = ui.idForWidget(f.tree);
        const QJsonObject expanded = ui.selectItem({{"id", treeId}, {"row", 0}, {"expanded", true}});
        TEST_ASSERT(expanded["expanded"] == QJsonValue(true));
        TEST_ASSERT(f.tree->topLevelItem(0)->isExpanded());
        TEST_ASSERT(isOk(ui.selectItem({{"id", treeId}, {"parent", QJsonArray{0}}, {"text", "A1"}})));
        TEST_COMPARE(f.tree->currentItem()->text(0), QString("A1"));
        TEST_ASSERT(!errorOf(ui.selectItem({{"id", listId}, {"row", 0}, {"expanded", true}})).isEmpty());
    }

    TEST_FUNCTION(testSelectComboAndTabs)
    {
        TEST_START;

        QtLLM::Client client(QtLLM::Provider::Ollama, "http://127.0.0.1:9");
        Fixture f;
        QtLLM::AppInterface ui(&client, &f.window);

        int changed = 0;
        int activated = 0;
        QObject::connect(f.combo, QOverload<int>::of(&QComboBox::currentIndexChanged), [&]() { ++changed; });
        QObject::connect(f.combo, QOverload<int>::of(&QComboBox::activated), [&]() { ++activated; });
        const QJsonObject blue = ui.selectItem({{"id", ui.idForWidget(f.combo)}, {"text", "blu"}});
        TEST_ASSERT(isOk(blue));
        TEST_COMPARE(blue["text"].toString(), QString("Blue"));
        TEST_COMPARE(f.combo->currentIndex(), 2);
        TEST_COMPARE(changed, 1);
        TEST_COMPARE(activated, 1);

        const QString tabsId = ui.idForWidget(f.tabs);
        TEST_ASSERT(isOk(ui.selectItem({{"id", tabsId}, {"text", "second"}})));
        TEST_COMPARE(f.tabs->currentIndex(), 1);
        TEST_ASSERT(errorOf(ui.selectItem({{"id", tabsId}, {"row", 2}})).contains("disabled"));
        TEST_ASSERT(errorOf(ui.selectItem({{"id", tabsId}, {"row", 9}})).contains("0..2"));
        TEST_COMPARE(f.tabs->currentIndex(), 1);
    }

    TEST_FUNCTION(testEdit)
    {
        TEST_START;

        QtLLM::Client client(QtLLM::Provider::Ollama, "http://127.0.0.1:9");
        Fixture f;
        QtLLM::AppInterface ui(&client, &f.window);
        const QString tableId = ui.idForWidget(f.table);

        const QJsonObject edited = ui.setItemText({{"id", tableId}, {"row", 0}, {"column", 1}, {"text", "42"}});
        TEST_ASSERT(isOk(edited));
        TEST_COMPARE(edited["text"].toString(), QString("42"));
        TEST_COMPARE(f.table->item(0, 1)->text(), QString("42"));

        TEST_ASSERT(errorOf(ui.setItemText({{"id", tableId}, {"row", 6}, {"text", "x"}})).contains("not editable"));
        TEST_COMPARE(f.table->item(6, 0)->text(), QString("file6"));
        // A view without edit triggers is read-only even though its items say editable.
        f.table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        TEST_ASSERT(errorOf(ui.setItemText({{"id", tableId}, {"row", 0}, {"text", "x"}})).contains("not editable"));

        const QString listId = ui.idForWidget(f.list);
        const QJsonObject checked = ui.setItemText({{"id", listId}, {"row", 1}, {"checked", true}});
        TEST_ASSERT(checked["checked"] == QJsonValue(true));
        TEST_ASSERT(f.list->item(1)->checkState() == Qt::Checked);
        TEST_ASSERT(errorOf(ui.setItemText({{"id", listId}, {"row", 3}, {"checked", true}})).contains("not editable"));
        TEST_ASSERT(!errorOf(ui.setItemText({{"id", listId}, {"row", 0}})).isEmpty());

        TEST_ASSERT(!errorOf(ui.setItemText({{"id", ui.idForWidget(f.combo)}, {"row", 0}, {"text", "x"}})).isEmpty());
        TEST_COMPARE(f.combo->itemText(0), QString("Red"));
    }

    TEST_FUNCTION(testErrors)
    {
        TEST_START;

        QtLLM::Client client(QtLLM::Provider::Ollama, "http://127.0.0.1:9");
        Fixture f;
        QtLLM::AppInterface ui(&client, &f.window);

        const QString labelError = errorOf(ui.getItems({{"id", ui.idForWidget(f.label)}}));
        TEST_ASSERT(labelError.contains("QLabel") && labelError.contains("combo boxes"));
        TEST_ASSERT(!errorOf(ui.selectItem({{"id", ui.idForWidget(f.label)}, {"row", 0}})).isEmpty());
        TEST_ASSERT(errorOf(ui.getItems({{"id", "itemWindow/nothing"}})).contains("no widget with id"));
    }
};

TEST_INSTANTIATE(TST_AppItems);

#endif // UNITTEST_GUI_ENABLED
