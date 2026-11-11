// Scripting in the example: what an application does to let the assistant
// (and the user) run scripts against its data.
//
//   1. Define the methods a script may call - here the inventory table as
//      the object `inventory`.
//   2. Say which of them are available, and what the user may change.
//   3. Say where saved scripts live.
//
// Everything else - the model's tools, checking a script before it runs,
// limits, the approval dialog, the Scripts tab with its editor and debugger -
// comes from the library.

#include "ChatWindow.h"
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QStandardPaths>
#include <QTableWidget>
#include <QVBoxLayout>

#if QTLLM_HAS_SCRIPTING

static const char* kScriptSettingsKey = "llm/userScriptSettings";

namespace
{

int rowOf(const QTableWidget* table, const QString& item)
{
    for (int row = 0; row < table->rowCount(); ++row) {
        if (table->item(row, 0) && table->item(row, 0)->text().compare(item, Qt::CaseInsensitive) == 0)
            return row;
    }
    return -1;
}

// A method handler fails by throwing: the script gets an exception.
int existingRow(const QTableWidget* table, const QString& item)
{
    const int row = rowOf(table, item);
    if (row < 0)
        throw QtLLM::ScriptError(QString("there is no item '%1'").arg(item));
    return row;
}

QString cell(const QTableWidget* table, int row, int column)
{
    return table->item(row, column) ? table->item(row, column)->text() : QString();
}

QJsonObject rowObject(const QTableWidget* table, int row)
{
    return {{"item", cell(table, row, 0)},
            {"quantity", cell(table, row, 1).toInt()},
            {"location", cell(table, row, 2)}};
}

void setCell(QTableWidget* table, int row, int column, const QString& text)
{
    if (!table->item(row, column))
        table->setItem(row, column, new QTableWidgetItem);
    table->item(row, column)->setText(text);
}

void writeFile(const QString& path, const char* text)
{
    QFile file(path);
    if (file.open(QIODevice::WriteOnly))
        file.write(text);
}

// Scripts that come with the application. Plain files with a header of
// comment lines; see QtLLM::ScriptLibrary.
const char* const kRestockScript =
    "// @name Restock low items\n"
    "// @description Adds `amount` to the quantity of every item that has less than `threshold`.\n"
    "// @param threshold number Items below this quantity are restocked (default 50)\n"
    "// @param amount number How much to add (default 100)\n"
    "\n"
    "const threshold = args.threshold === undefined ? 50 : args.threshold;\n"
    "const amount = args.amount === undefined ? 100 : args.amount;\n"
    "const restocked = [];\n"
    "const items = inventory.list();\n"
    "for (let i = 0; i < items.length; i++) {\n"
    "    const entry = items[i];\n"
    "    progress(i + 1, items.length, entry.item);\n"
    "    if (entry.quantity < threshold) {\n"
    "        inventory.setQuantity(entry.item, entry.quantity + amount);\n"
    "        restocked.push(entry.item);\n"
    "    }\n"
    "}\n"
    "return {restocked: restocked, checked: items.length};\n";

const char* const kReportScript =
    "// @name Inventory report\n"
    "// @description Totals per location. Changes nothing.\n"
    "\n"
    "const perLocation = {};\n"
    "let total = 0;\n"
    "for (const entry of inventory.list()) {\n"
    "    perLocation[entry.location] = (perLocation[entry.location] || 0) + entry.quantity;\n"
    "    total += entry.quantity;\n"
    "}\n"
    "log('locations: ' + Object.keys(perLocation).length);\n"
    "return {total: total, perLocation: perLocation};\n";

} // namespace

void ChatWindow::registerScripting()
{
    // QtLLM::Scripting is a ScriptHost::ScriptEngine with the model's side
    // added: the tools run_script and script_api, tools as methods, llm.ask.
    m_scripting = new QtLLM::Scripting(&m_client, this);
    QtLLM::useApprovalDialog(m_scripting, this);
    // The window's keys and clicks for scripts: on("key", fn), on("mouse", fn).
    // Keys come while a control that ignores them has the focus - not what
    // is typed into the chat or a text field.
    QtLLM::forwardInput(m_scripting, this);
    // Scripts draw over the window with gfx (ScriptHost's canvas): outlines,
    // arrows, animations - the model has no draw tools of its own.
    m_scripting->attachCanvas(this);
    // Scripts read and change this window's widgets, and build dialogs, with
    // ui.*. The chat stays out of their reach, as it does for AppInterface.
    QtLLM::exposeWidgets(m_scripting, this);
    QtLLM::ScriptUi::hideWidget(m_chatDock);

    auto* table = findChild<QTableWidget*>("inventoryTable");
    const QString group = "Inventory";
    const QJsonObject exampleRow{{"item", "Screws"}, {"quantity", 120}, {"location", "Shelf A"}};

    // ---- 1. The methods ----------------------------------------------------
    // Name, description, parameters, return value: one definition, read by
    // the model (script_api), shown to the user (the editor's reference) and
    // used to check every call. Handlers run on the GUI thread.
    QtLLM::ScriptMethod list("inventory.list");
    list.setDescription("All inventory items.").setGroup(group).setReadOnly(true)
        .setReturns("array", "One object per item", QJsonArray{exampleRow});
    m_scripting->registerMethod(list, [table](const QJsonObject&) -> QJsonValue {
        QJsonArray rows;
        for (int row = 0; row < table->rowCount(); ++row)
            rows.append(rowObject(table, row));
        return rows;
    });

    QtLLM::ScriptMethod get("inventory.get");
    get.setDescription("One inventory item, by name.").setGroup(group).setReadOnly(true)
       .addParameter("item", "string", "Name of the item")
       .setReturns("object", "The item", exampleRow);
    m_scripting->registerMethod(get, [table](const QJsonObject& args) -> QJsonValue {
        return rowObject(table, existingRow(table, args["item"].toString()));
    });

    QtLLM::ScriptMethod setQuantity("inventory.setQuantity");
    setQuantity.setDescription("Sets the quantity of an item.").setGroup(group)
               .addParameter("item", "string", "Name of the item")
               .addParameter("quantity", "integer", "The new quantity")
               .setRange("quantity", 0, 1000000)
               .setReturns("object", "The item as it is now", exampleRow);
    m_scripting->registerMethod(setQuantity, [table](const QJsonObject& args) -> QJsonValue {
        const int row = existingRow(table, args["item"].toString());
        setCell(table, row, 1, QString::number(args["quantity"].toInt()));
        return rowObject(table, row);
    });

    QtLLM::ScriptMethod setLocation("inventory.setLocation");
    setLocation.setDescription("Moves an item to another location.").setGroup(group)
               .addParameter("item", "string", "Name of the item")
               .addParameter("location", "string", "Where it is kept now")
               .setReturns("object", "The item as it is now", exampleRow);
    m_scripting->registerMethod(setLocation, [table](const QJsonObject& args) -> QJsonValue {
        const int row = existingRow(table, args["item"].toString());
        setCell(table, row, 2, args["location"].toString());
        return rowObject(table, row);
    });

    QtLLM::ScriptMethod add("inventory.add");
    add.setDescription("Adds a new item.").setGroup(group)
       .addParameter("item", "string", "Name of the item")
       .addParameter("quantity", "integer", "How many there are")
       .addParameter("location", "string", "Where it is kept", false)
       .setDefault("location", "Unsorted")
       .setReturns("object", "The new item", exampleRow);
    m_scripting->registerMethod(add, [table](const QJsonObject& args) -> QJsonValue {
        const QString item = args["item"].toString().trimmed();
        if (item.isEmpty())
            throw QtLLM::ScriptError("the item needs a name");
        if (rowOf(table, item) >= 0)
            throw QtLLM::ScriptError(QString("there is an item '%1' already").arg(item));
        const int row = table->rowCount();
        table->insertRow(row);
        setCell(table, row, 0, item);
        setCell(table, row, 1, QString::number(args["quantity"].toInt()));
        setCell(table, row, 2, args["location"].toString());
        return rowObject(table, row);
    });

    QtLLM::ScriptMethod remove("inventory.remove");
    remove.setDescription("Removes an item.").setGroup(group)
          .addParameter("item", "string", "Name of the item");
    m_scripting->registerMethod(remove, [table](const QJsonObject& args) -> QJsonValue {
        table->removeRow(existingRow(table, args["item"].toString()));
        return QJsonValue();
    });

    // A tool the assistant has can be made a method as well: tools.<name>({...}).
    // Nothing is, until the application says so. exposeToolGroup("App UI")
    // would hand scripts all the tools that operate the user interface.
    m_scripting->exposeTool("setWindowTitle");

    // llm.ask() / llm.choose(): a script can put a question to a model, with
    // a fresh conversation every time - for decisions inside a loop. The
    // helper runs on the chat's provider.
    m_scripting->setLlmConfigProvider([this]() {
        QtLLM::AgentConfig config = agentConfig("script-llm");
        config.maxTokens = 200;
        config.effort = QtLLM::Effort::Low;
        return config;
    });

    // ---- 2. What is available, and who decides -----------------------------
    // Removing items is not for scripts, and not the user's to switch on in
    // Settings > Scripts: the application decides that alone.
    m_scripting->setMethodEnabled("inventory.remove", false);
    m_scripting->setMethodUserConfigurable("inventory.remove", false);
    // A script of the assistant that changes something is shown to the user
    // first. setApprovalRequired() decides per kind of script instead (new or
    // saved, reading or changing, dry run). The user may change each kind in
    // Settings > Scripts; setApprovalUserConfigurable(false) would make it
    // mandatory.
    m_scripting->setApproval(QtLLM::Scripting::Approval::ChangingScripts);
    // Whether the assistant may run scripts at all is the user's choice too;
    // setEnabled(false) + setEnabledUserConfigurable(false) would switch
    // scripting off for good.

    // ---- 3. Saved scripts ---------------------------------------------------
    // The user's (and the assistant's) scripts go to a folder of the
    // application's choice; scripts shipped with the application are read
    // from a second one and cannot be changed.
    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    // ponytail: the two shipped scripts are written out at every start instead
    // of being installed with the application; a real application ships a folder.
    const QString shipped = base + "/scripts-shipped";
    QDir().mkpath(shipped);
    writeFile(shipped + "/restock-low-items.js", kRestockScript);
    writeFile(shipped + "/inventory-report.js", kReportScript);

    m_scriptLibrary = new QtLLM::ScriptLibrary(this);
    m_scriptLibrary->setDirectory(base + "/scripts");
    m_scriptLibrary->addReadOnlyDirectory(shipped);
    m_scripting->setLibrary(m_scriptLibrary);

    // The user's choices from earlier sessions (what is locked is skipped).
    m_scripting->applyUserSettings(
        QJsonDocument::fromJson(QSettings().value(kScriptSettingsKey).toByteArray()).object());

    // What scripts do is shown in the agent log.
    connect(m_scripting, &QtLLM::Scripting::runStarted, this, [this](QtLLM::Scripting::RunId, const QString& name, bool byModel) {
        logAgent(QString("[script] %1 started by the %2")
                     .arg(name.isEmpty() ? QString("a script") : "'" + name + "'",
                          byModel ? "assistant" : "user"));
    });
    connect(m_scripting, &QtLLM::Scripting::runLog, this, [this](QtLLM::Scripting::RunId, const QString& text) {
        logAgent("[script] " + text);
    });
    connect(m_scripting, &QtLLM::Scripting::runFinished, this, [this](QtLLM::Scripting::RunId, const QJsonObject& result) {
        logAgent("[script] finished: "
                 + QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact)).left(400));
    });
}

void ChatWindow::saveScriptSettings()
{
    if (m_scripting) {
        QSettings().setValue(kScriptSettingsKey,
            QJsonDocument(m_scripting->userSettings()).toJson(QJsonDocument::Compact));
    }
}

// The Scripts tab of the Settings dialog is a widget an application can put
// into a window of its own: here one that opens from the toolbar. Switches
// take effect at once instead of waiting for an Apply button.
void ChatWindow::openScripts()
{
    if (!m_scriptsWindow) {
        m_scriptsWindow = new QWidget(this, Qt::Window);
        m_scriptsWindow->setObjectName("scriptsWindow");
        m_scriptsWindow->setWindowTitle("Scripts");
        m_scriptsWindow->resize(760, 520);
        auto* layout = new QVBoxLayout(m_scriptsWindow);
        auto* scripts = new QtLLM::ScriptsWidget(m_scriptsWindow);
        scripts->setApplyImmediately(true);
        scripts->setScripting(m_scripting);
        connect(scripts, &QtLLM::ScriptsWidget::changed, this, &ChatWindow::saveScriptSettings);
        layout->addWidget(scripts);
    }
    m_scriptsWindow->show();
    m_scriptsWindow->raise();
    m_scriptsWindow->activateWindow();
}

#else // built without scripting: the window has no script features

#include <QMessageBox>

void ChatWindow::registerScripting()
{
}

void ChatWindow::saveScriptSettings()
{
}

void ChatWindow::openScripts()
{
    QMessageBox::information(this, "Scripts",
        "This build has no scripting (CMake option QTLLM_SCRIPTING=OFF).");
}

#endif
