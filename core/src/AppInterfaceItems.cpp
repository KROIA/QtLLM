// Item tools of AppInterface: reading, selecting and editing the items of
// lists, tables, trees, combo boxes and tabs. Views and combo boxes are read
// through their model, so any model the application uses works; tabs through
// QTabBar.
#include "AppInterface.h"
#include "Client.h"
#include "Tool.h"
#include "ToolResult.h"

#include <QAbstractItemModel>
#include <QAbstractItemView>
#include <QComboBox>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QJsonArray>
#include <QListView>
#include <QTabBar>
#include <QTabWidget>
#include <QTableView>
#include <QTreeView>
#include <functional>

namespace QtLLM
{

namespace
{

const int kDefaultRows = 50;
const int kRowCap      = 200;
const int kColumnCap   = 20;
const int kCellChars   = 200;
const int kDepthCap    = 5;
const int kListedMatches = 10;

const char* const kIdParam = "Widget id from ui_get_widget_tree or ui_find_widgets.";
const char* const kParentParam =
    "Row numbers leading to a tree node, e.g. [0, 2] = the third child of the first top-level "
    "row. Omit for the top level.";

// The addressed widget seen as rows: a model level (views, combo boxes) or tabs.
struct Items
{
    QAbstractItemView*  view = nullptr;
    QComboBox*          combo = nullptr;
    QTabBar*            tabs = nullptr;
    QAbstractItemModel* model = nullptr;
    QModelIndex         root;
    int                 itemColumn = 0;   // the column a list or combo box shows
};

QJsonObject itemsOf(QWidget* widget, const QString& id, Items& items)
{
    items.tabs = qobject_cast<QTabBar*>(widget);
    if (auto* tabWidget = qobject_cast<QTabWidget*>(widget))
        items.tabs = tabWidget->tabBar();
    if (items.tabs)
        return QJsonObject();

    items.combo = qobject_cast<QComboBox*>(widget);
    items.view = qobject_cast<QAbstractItemView*>(widget);
    if (items.combo) {
        items.model = items.combo->model();
        items.root = items.combo->rootModelIndex();
        items.itemColumn = items.combo->modelColumn();
    } else if (items.view) {
        items.model = items.view->model();
        items.root = items.view->rootIndex();
        if (auto* list = qobject_cast<QListView*>(items.view))
            items.itemColumn = list->modelColumn();
    } else {
        return toolError(QStringLiteral("'%1' is a %2; the item tools work on lists, tables and "
                                        "trees (any QAbstractItemView), combo boxes, tab widgets "
                                        "and tab bars")
                             .arg(id, QString::fromLatin1(widget->metaObject()->className())));
    }
    if (!items.model)
        return toolError(QStringLiteral("'%1' has no model, so it has no items").arg(id));
    return QJsonObject();
}

QString cut(const QString& text)
{
    return text.size() > kCellChars ? text.left(kCellChars) + QStringLiteral("...") : text;
}

// "&File" -> "File", "&&" -> "&".
QString tabTitle(QTabBar* bar, int tab)
{
    const QString text = bar->tabText(tab);
    QString plain;
    for (int i = 0; i < text.size(); ++i) {
        if (text[i] == QLatin1Char('&') && i + 1 < text.size())
            ++i;
        plain += text[i];
    }
    return plain;
}

QString rangeText(int count, const char* what)
{
    if (count == 0)
        return QStringLiteral("there are no %1 at that level").arg(QLatin1String(what));
    return QStringLiteral("valid %1 are 0..%2").arg(QLatin1String(what)).arg(count - 1);
}

int rowsAt(QAbstractItemModel* model, const QModelIndex& parent)
{
    // ponytail: one fetchMore per call; lazy models (SQL, file system) may
    // report fewer rows than they will eventually have.
    if (model->canFetchMore(parent))
        model->fetchMore(parent);
    return model->rowCount(parent);
}

// The tree node the 'parent' row path leads to. Combo boxes have one level.
QModelIndex nodeForPath(const Items& items, const QJsonValue& path, QString& error)
{
    QModelIndex node = items.root;
    if (items.combo)
        return node;
    const QJsonArray steps = path.toArray();
    for (int i = 0; i < steps.size(); ++i) {
        const int step = steps[i].toInt(-1);
        const int count = rowsAt(items.model, node);
        if (step < 0 || step >= count) {
            error = QStringLiteral("parent[%1] = %2 does not exist: %3")
                        .arg(i).arg(steps[i].toVariant().toString(), rangeText(count, "rows"));
            return QModelIndex();
        }
        node = items.model->index(step, 0, node);
    }
    return node;
}

// The columns a user sees: the shown column of lists and combo boxes, the
// columns that are not hidden otherwise.
QList<int> visibleColumns(const Items& items, const QModelIndex& parent)
{
    if (items.combo || qobject_cast<QListView*>(items.view))
        return {items.itemColumn};
    auto* table = qobject_cast<QTableView*>(items.view);
    auto* tree = qobject_cast<QTreeView*>(items.view);
    QList<int> columns;
    const int count = items.model->columnCount(parent);
    for (int column = 0; column < count && columns.size() < kColumnCap; ++column) {
        if ((table && table->isColumnHidden(column)) || (tree && tree->isColumnHidden(column)))
            continue;
        columns.append(column);
    }
    return columns;
}

QStringList rowTexts(const Items& items, const QModelIndex& parent, int row,
                     const QList<int>& columns)
{
    QStringList texts;
    for (int column : columns)
        texts.append(items.model->index(row, column, parent).data(Qt::DisplayRole).toString());
    return texts;
}

bool isCurrent(const Items& items, const QModelIndex& parent, int row)
{
    if (items.combo)
        return parent == items.root && items.combo->currentIndex() == row;
    const QModelIndex current = items.view->currentIndex();
    return current.isValid() && current.row() == row && current.parent() == parent;
}

QJsonValue checkJson(const QVariant& state)
{
    if (state.toInt() == Qt::PartiallyChecked)
        return QJsonValue(QStringLiteral("partial"));
    return QJsonValue(state.toInt() == Qt::Checked);
}

struct Page
{
    QJsonArray rows;
    int        next = -1;   // first row not returned, -1 when nothing is left
};

// Rows from firstRow on that match the filter, until the shared budget is used
// up. The budget covers inline children too, so depth cannot blow up a result.
Page pageRows(int count, int firstRow, const QString& filter, int& budget,
              const std::function<QStringList(int)>& texts,
              const std::function<QJsonObject(int)>& rowJson)
{
    Page page;
    for (int row = firstRow; row < count; ++row) {
        if (!filter.isEmpty()
            && !texts(row).join(QLatin1Char('\n')).contains(filter, Qt::CaseInsensitive))
            continue;
        if (budget <= 0) {
            page.next = row;
            break;
        }
        --budget;
        page.rows.append(rowJson(row));
    }
    return page;
}

QJsonObject itemRow(const Items& items, const QModelIndex& parent, int row,
                    const QList<int>& columns, int depth, int& budget)
{
    QAbstractItemModel* model = items.model;
    QItemSelectionModel* selection = items.view ? items.view->selectionModel() : nullptr;
    QJsonObject result{{"row", row}};
    QJsonArray cells;
    bool selected = false;
    for (int column : columns) {
        const QModelIndex index = model->index(row, column, parent);
        cells.append(cut(index.data(Qt::DisplayRole).toString()));
        if (selection && selection->isSelected(index))
            selected = true;
        const QVariant state = index.data(Qt::CheckStateRole);
        if (state.isValid() && !result.contains("checked")) {
            result["checked"] = checkJson(state);
            if (column != items.itemColumn)
                result["checkColumn"] = column;
        }
    }
    result["cells"] = cells;
    if (selected)
        result["selected"] = true;
    if (isCurrent(items, parent, row))
        result["current"] = true;
    if (!(model->flags(model->index(row, items.itemColumn, parent)) & Qt::ItemIsEnabled))
        result["enabled"] = false;

    // Only trees show children; table models often ignore the parent argument.
    auto* tree = qobject_cast<QTreeView*>(items.view);
    if (!tree)
        return result;
    const QModelIndex node = model->index(row, 0, parent);
    const int childCount = rowsAt(model, node);
    if (childCount == 0)
        return result;
    result["childCount"] = childCount;
    result["expanded"] = tree->isExpanded(node);
    if (depth > 0 && budget > 0) {
        result["children"] = pageRows(
            childCount, 0, QString(), budget,
            [&](int child) { return rowTexts(items, node, child, columns); },
            [&](int child) { return itemRow(items, node, child, columns, depth - 1, budget); }).rows;
    }
    return result;
}

QJsonObject tabRow(QTabBar* bar, int tab)
{
    QJsonObject result{{"row", tab}, {"cells", QJsonArray{tabTitle(bar, tab)}}};
    if (bar->currentIndex() == tab)
        result["current"] = true;
    if (!bar->isTabEnabled(tab))
        result["enabled"] = false;
    return result;
}

// The one row whose cell equals `wanted`, or failing that contains it
// (case-insensitive); -1 and an error when none or several match. `column`
// receives the column of the matching cell.
int pickRow(int count, const QList<int>& columns, const std::function<QStringList(int)>& texts,
            const QString& wanted, int& column, QString& error)
{
    struct Match { int at; int cellColumn; QString cell; };
    QList<Match> exact;
    QList<Match> partial;
    for (int row = 0; row < count; ++row) {
        const QStringList cells = texts(row);
        int containing = -1;
        int equal = -1;
        for (int i = 0; i < cells.size() && equal < 0; ++i) {
            if (cells[i] == wanted)
                equal = i;
            else if (containing < 0 && cells[i].contains(wanted, Qt::CaseInsensitive))
                containing = i;
        }
        if (equal >= 0)
            exact.append({row, columns.value(equal), cells[equal]});
        else if (containing >= 0)
            partial.append({row, columns.value(containing), cells[containing]});
    }
    const QList<Match>& matches = exact.isEmpty() ? partial : exact;
    if (matches.size() == 1) {
        column = matches.first().cellColumn;
        return matches.first().at;
    }
    if (matches.isEmpty()) {
        error = QStringLiteral("no item at this level contains '%1'; read the items with "
                               "ui_get_items (children of a tree node need 'parent')").arg(wanted);
        return -1;
    }
    QStringList listed;
    for (int i = 0; i < matches.size() && i < kListedMatches; ++i)
        listed.append(QStringLiteral("row %1 '%2'").arg(matches[i].at).arg(cut(matches[i].cell)));
    if (matches.size() > kListedMatches)
        listed.append(QStringLiteral("..."));
    error = QStringLiteral("'%1' matches %2 items: %3; pass 'row' instead")
                .arg(wanted).arg(matches.size()).arg(listed.join(QStringLiteral(", ")));
    return -1;
}

} // namespace

void AppInterface::syncItemTools()
{
    if (!m_client) {
        m_itemTools.clear();
        return;
    }

    for (const QString& name : m_itemTools)
        m_client->unregisterTool(name);
    m_itemTools.clear();

    auto add = [this](Tool& tool, bool readOnly, ToolHandler handler) {
        tool.setGroup(QStringLiteral("App UI")).setReadOnly(readOnly);
        m_client->registerTool(tool, std::move(handler));
        m_itemTools.append(tool.name());
    };

    if (isCapabilityEnabled(Capability::ReadItems)) {
        Tool tool;
        tool.setName("ui_get_items")
            .setTitle("Read items")
            .setStatusText("Reading list items...")
            .setDescription(
                "Read the items of a list, table, tree, combo box or tab widget. Each row has its "
                "'row' number and 'cells' (display text, cut at 200 characters); only where they "
                "apply: 'checked', 'selected', 'current', 'enabled': false, and for tree nodes "
                "'childCount' and 'expanded'. Also returns 'headers' when the view shows them and "
                "'rowCount' of the level; when more rows exist, 'truncated' is true and "
                "'nextFirstRow' is where to continue. Use 'filter' to find a row in a long list "
                "instead of paging. The row numbers are what ui_select_item and ui_set_item_text "
                "take.")
            .addParameter("id", "string", QString::fromLatin1(kIdParam), true)
            .addArrayParameter("parent", "integer", QString::fromLatin1(kParentParam))
            .addParameter("firstRow", "integer", "First row to return (default 0).")
            .addParameter("maxRows", "integer",
                          "Rows to return, inline children included (default 50, max 200).")
            .addArrayParameter("columns", "integer",
                               "Column numbers to return. Omit for all visible columns (max 20).")
            .addParameter("filter", "string",
                          "Case-insensitive text; only rows with a cell containing it, searched "
                          "over the whole level. Each keeps its real row number.")
            .addParameter("depth", "integer",
                          "Trees: levels of children to include inline (default 0 = only "
                          "childCount, max 5).");
        add(tool, true, [this](const QJsonObject& args) { return getItems(args); });
    }

    if (isCapabilityEnabled(Capability::SelectItems)) {
        Tool tool;
        tool.setName("ui_select_item")
            .setTitle("Select item")
            .setStatusText("Selecting an item...")
            .setDescription(
                "Select an item of a list, table, tree, combo box or tab widget as the user would "
                "by clicking it: it becomes the current item, is selected according to the view's "
                "selection rules and scrolled into view; a combo box or tab widget switches to it. "
                "Address it by 'row' (from ui_get_items) or by 'text' (exact cell text first, then "
                "case-insensitive part of it; several matches are an error that lists them). "
                "Disabled items are refused. Use 'activate' to open the item the way a double "
                "click or Enter does.")
            .addParameter("id", "string", QString::fromLatin1(kIdParam), true)
            .addParameter("row", "integer", "Row number at the level given by 'parent'.")
            .addParameter("column", "integer", "Column of the cell (default: the item column).")
            .addArrayParameter("parent", "integer", QString::fromLatin1(kParentParam))
            .addParameter("text", "string", "Text of the item, instead of 'row'.")
            .addParameter("extend", "boolean",
                          "Add to the current selection instead of replacing it (multi-selection "
                          "views only).")
            .addParameter("expanded", "boolean", "Trees: expand (true) or collapse (false) the node.")
            .addParameter("activate", "boolean",
                          "Also activate the item, as a double click or Enter would.");
        add(tool, false, [this](const QJsonObject& args) { return selectItem(args); });
    }

    if (isCapabilityEnabled(Capability::EditItems)) {
        Tool tool;
        tool.setName("ui_set_item_text")
            .setTitle("Edit item")
            .setStatusText("Editing an item...")
            .setDescription(
                "Change the text and/or the check box of one cell in a list, table or tree, as "
                "the user could by editing it in place. Only items the application lets the user "
                "edit or check are accepted. Returns the text and check state the item holds "
                "afterwards; the application may adjust or refuse a value.")
            .addParameter("id", "string", QString::fromLatin1(kIdParam), true)
            .addParameter("row", "integer", "Row number from ui_get_items.", true)
            .addParameter("column", "integer", "Column of the cell (default: the item column).")
            .addArrayParameter("parent", "integer", QString::fromLatin1(kParentParam))
            .addParameter("text", "string", "New text.")
            .addParameter("checked", "boolean", "New check state.");
        add(tool, false, [this](const QJsonObject& args) { return setItemText(args); });
    }
}

QJsonObject AppInterface::getItems(const QJsonObject& args)
{
    const QString id = args["id"].toString();
    QWidget* widget = nullptr;
    QJsonObject error = resolve(id, widget);
    if (!error.isEmpty())
        return error;
    Items items;
    error = itemsOf(widget, id, items);
    if (!error.isEmpty())
        return error;

    int budget = qBound(1, args["maxRows"].toInt(kDefaultRows), kRowCap);
    const int firstRow = qMax(0, args["firstRow"].toInt());
    const QString filter = args["filter"].toString();
    QJsonObject result;
    Page page;

    if (items.tabs) {
        QTabBar* bar = items.tabs;
        result["rowCount"] = bar->count();
        page = pageRows(bar->count(), firstRow, filter, budget,
                        [bar](int tab) { return QStringList{tabTitle(bar, tab)}; },
                        [bar](int tab) { return tabRow(bar, tab); });
    } else {
        QString message;
        const QModelIndex parent = nodeForPath(items, args["parent"], message);
        if (!message.isEmpty())
            return toolError(message);

        const int columnCount = items.model->columnCount(parent);
        QList<int> columns = visibleColumns(items, parent);
        if (args.contains("columns")) {
            columns.clear();
            const QJsonArray wanted = args["columns"].toArray();
            for (const QJsonValue& value : wanted) {
                const int column = value.toInt(-1);
                if (column < 0 || column >= columnCount)
                    return toolError(QStringLiteral("column %1 does not exist; %2")
                                         .arg(value.toVariant().toString(),
                                              rangeText(columnCount, "columns")));
                if (columns.size() < kColumnCap)
                    columns.append(column);
            }
        }

        bool allColumns = columns.size() == columnCount;
        QJsonArray columnList;
        for (int i = 0; i < columns.size(); ++i) {
            allColumns = allColumns && columns[i] == i;
            columnList.append(columns[i]);
        }
        if (!allColumns)
            result["columns"] = columnList;

        auto* table = qobject_cast<QTableView*>(items.view);
        auto* tree = qobject_cast<QTreeView*>(items.view);
        if ((table && !table->horizontalHeader()->isHidden())
            || (tree && !tree->header()->isHidden())) {
            QJsonArray headers;
            for (int column : columns)
                headers.append(items.model->headerData(column, Qt::Horizontal).toString());
            result["headers"] = headers;
        }

        const int depth = qBound(0, args["depth"].toInt(), kDepthCap);
        const int rowCount = rowsAt(items.model, parent);
        result["rowCount"] = rowCount;
        page = pageRows(rowCount, firstRow, filter, budget,
                        [&](int r) { return rowTexts(items, parent, r, columns); },
                        [&](int r) { return itemRow(items, parent, r, columns, depth, budget); });
    }

    result["rows"] = page.rows;
    if (page.next >= 0) {
        result["truncated"] = true;
        result["nextFirstRow"] = page.next;
    }
    return toolOk(result);
}

QJsonObject AppInterface::selectItem(const QJsonObject& args)
{
    const QString id = args["id"].toString();
    QWidget* widget = nullptr;
    QJsonObject error = resolve(id, widget);
    if (!error.isEmpty())
        return error;
    Items items;
    error = itemsOf(widget, id, items);
    if (!error.isEmpty())
        return error;
    if (!widget->isEnabled())
        return toolError(QStringLiteral("'%1' is disabled").arg(id));
    const bool byText = args.contains("text");
    if (!byText && !args.contains("row"))
        return toolError(QStringLiteral("give the item's 'row' or its 'text'"));
    const QString wanted = args["text"].toString();
    QString message;
    int row = args["row"].toInt(-1);

    if (items.tabs) {
        QTabBar* bar = items.tabs;
        int column = 0;
        if (byText)
            row = pickRow(bar->count(), {0}, [bar](int tab) { return QStringList{tabTitle(bar, tab)}; },
                          wanted, column, message);
        if (!message.isEmpty())
            return toolError(message);
        if (row < 0 || row >= bar->count())
            return toolError(QStringLiteral("tab %1 does not exist; %2")
                                 .arg(row).arg(rangeText(bar->count(), "rows")));
        if (!bar->isTabEnabled(row))
            return toolError(QStringLiteral("tab %1 '%2' is disabled").arg(row).arg(tabTitle(bar, row)));
        bar->setCurrentIndex(row);
        return toolOk({{"row", row}, {"text", tabTitle(bar, row)}});
    }

    const QModelIndex parent = nodeForPath(items, args["parent"], message);
    if (!message.isEmpty())
        return toolError(message);
    QAbstractItemModel* model = items.model;
    const int rowCount = rowsAt(model, parent);
    int column = args["column"].toInt(items.itemColumn);
    if (byText) {
        const QList<int> columns = visibleColumns(items, parent);
        row = pickRow(rowCount, columns,
                      [&](int r) { return rowTexts(items, parent, r, columns); },
                      wanted, column, message);
        if (!message.isEmpty())
            return toolError(message);
    }
    if (row < 0 || row >= rowCount)
        return toolError(QStringLiteral("row %1 does not exist; %2")
                             .arg(row).arg(rangeText(rowCount, "rows")));
    const int columnCount = model->columnCount(parent);
    if (column < 0 || column >= columnCount)
        return toolError(QStringLiteral("column %1 does not exist; %2")
                             .arg(column).arg(rangeText(columnCount, "columns")));
    const QModelIndex index = model->index(row, column, parent);
    const Qt::ItemFlags flags = model->flags(index);
    if (!(flags & Qt::ItemIsEnabled))
        return toolError(QStringLiteral("row %1 is disabled").arg(row));
    const QString text = cut(index.data(Qt::DisplayRole).toString());

    if (QComboBox* combo = items.combo) {
        combo->setCurrentIndex(row);
        // A choice made by the user also emits activated; applications often
        // react only to that.
        emit combo->activated(row);
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
        emit combo->textActivated(combo->itemText(row));
#endif
        return toolOk({{"row", row}, {"text", text}});
    }

    QAbstractItemView* view = items.view;
    auto* tree = qobject_cast<QTreeView*>(view);
    if (args.contains("expanded") && !tree)
        return toolError(QStringLiteral("'expanded' only applies to tree views"));

    QItemSelectionModel* selection = view->selectionModel();
    const QAbstractItemView::SelectionMode mode = view->selectionMode();
    QItemSelectionModel::SelectionFlags command = QItemSelectionModel::NoUpdate;
    if (mode != QAbstractItemView::NoSelection && (flags & Qt::ItemIsSelectable)) {
        // ponytail: 'extend' adds just this item, also in ContiguousSelection
        // views, where a user would select the whole range.
        const bool extend = args["extend"].toBool() && mode != QAbstractItemView::SingleSelection;
        command = extend ? QItemSelectionModel::Select : QItemSelectionModel::ClearAndSelect;
        if (view->selectionBehavior() == QAbstractItemView::SelectRows)
            command |= QItemSelectionModel::Rows;
        else if (view->selectionBehavior() == QAbstractItemView::SelectColumns)
            command |= QItemSelectionModel::Columns;
    }
    if (selection)
        selection->setCurrentIndex(index, command);
    view->scrollTo(index);

    QJsonObject result{{"row", row}, {"text", text},
                       {"selected", selection && selection->isSelected(index)}};
    if (column != items.itemColumn)
        result["column"] = column;
    if (tree) {
        const QModelIndex node = model->index(row, 0, parent);
        if (args.contains("expanded"))
            tree->setExpanded(node, args["expanded"].toBool());
        if (model->hasChildren(node))
            result["expanded"] = tree->isExpanded(node);
    }
    if (args["activate"].toBool()) {
        // The signals a double click / Enter produce; the convenience views
        // (QListWidget, ...) turn them into their itemActivated etc.
        emit view->activated(index);
        emit view->doubleClicked(index);
    }
    return toolOk(result);
}

QJsonObject AppInterface::setItemText(const QJsonObject& args)
{
    const QString id = args["id"].toString();
    QWidget* widget = nullptr;
    QJsonObject error = resolve(id, widget);
    if (!error.isEmpty())
        return error;
    Items items;
    error = itemsOf(widget, id, items);
    if (!error.isEmpty())
        return error;
    // Combo box entries and tab titles are the application's choices, not
    // user data; the user cannot rename them either.
    if (!items.view)
        return toolError(QStringLiteral("only the items of lists, tables and trees can be edited; "
                                        "use ui_select_item to choose a combo box item or tab"));
    if (!widget->isEnabled())
        return toolError(QStringLiteral("'%1' is disabled").arg(id));
    const bool setText = args.contains("text");
    const bool setCheck = args.contains("checked");
    if (!setText && !setCheck)
        return toolError(QStringLiteral("give 'text' and/or 'checked'"));

    QString message;
    const QModelIndex parent = nodeForPath(items, args["parent"], message);
    if (!message.isEmpty())
        return toolError(message);
    QAbstractItemModel* model = items.model;
    const int row = args["row"].toInt(-1);
    const int rowCount = rowsAt(model, parent);
    if (row < 0 || row >= rowCount)
        return toolError(QStringLiteral("row %1 does not exist; %2")
                             .arg(row).arg(rangeText(rowCount, "rows")));
    const int column = args["column"].toInt(items.itemColumn);
    const int columnCount = model->columnCount(parent);
    if (column < 0 || column >= columnCount)
        return toolError(QStringLiteral("column %1 does not exist; %2")
                             .arg(column).arg(rangeText(columnCount, "columns")));

    const QModelIndex index = model->index(row, column, parent);
    const Qt::ItemFlags flags = model->flags(index);
    const QString where = QStringLiteral("the item at row %1, column %2").arg(row).arg(column);
    if (!(flags & Qt::ItemIsEnabled))
        return toolError(where + QStringLiteral(" is disabled"));
    // Item flags say editable by default (QTableWidgetItem, QStandardItem);
    // a view without edit triggers is how applications make them read-only.
    if (setText && (!(flags & Qt::ItemIsEditable)
                    || items.view->editTriggers() == QAbstractItemView::NoEditTriggers))
        return toolError(where + QStringLiteral(" is not editable"));
    // The flag is on by default too; a check box is only drawn when the item
    // has a check state.
    if (setCheck && (!(flags & Qt::ItemIsUserCheckable) || !index.data(Qt::CheckStateRole).isValid()))
        return toolError(where + QStringLiteral(" is not editable: it has no check box the user can change"));

    if (setText && !model->setData(index, args["text"].toString(), Qt::EditRole))
        return toolError(QStringLiteral("the application refused the text for ") + where);
    const int state = args["checked"].toBool() ? Qt::Checked : Qt::Unchecked;
    if (setCheck && !model->setData(index, state, Qt::CheckStateRole))
        return toolError(QStringLiteral("the application refused the check state for ") + where
                         + (setText ? QStringLiteral(" (the text was written)") : QString()));

    QJsonObject result{{"row", row}, {"column", column},
                       {"text", cut(index.data(Qt::EditRole).toString())}};
    const QVariant check = index.data(Qt::CheckStateRole);
    if (check.isValid())
        result["checked"] = checkJson(check);
    return toolOk(result);
}

} // namespace QtLLM
