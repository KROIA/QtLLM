#include "BuiltinTools.h"
#include "Client.h"
#include "ChatDockWidget.h"
#include "InterviewTool.h"
#include "Tool.h"
#include "ToolResult.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QColorDialog>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QJsonArray>
#include <QLocale>
#include <QMessageBox>
#include <QPointer>
#include <QScreen>
#include <QSysInfo>
#include <QTimeZone>
#include <QTimer>
#include <QUrl>

namespace QtLLM
{

namespace
{

// ---------------------------------------------------------------- FileDialog

void registerFileDialog(Client* client, QWidget* dialogParent)
{
    Tool tool;
    tool.setName("file_dialog")
        .setDescription(
            "Show the user a native file dialog and return the chosen path(s). "
            "Use this whenever a file or folder on the user's machine is needed — "
            "never guess paths. 'filter' uses Qt syntax, e.g. "
            "\"Images (*.png *.jpg);;All files (*.*)\".")
        .setTitle("File dialog")
        .setGroup("Dialogs")
        .setStatusText("Waiting for file selection…")
        .addEnumParameter("mode",
                          {"openFile", "openFiles", "saveFile", "selectFolder"},
                          "Dialog type: open one file, open several, choose a "
                          "save target, or pick a folder.", true)
        .addParameter("title", "string", "Dialog window title.", false)
        .addParameter("startPath", "string",
                      "Directory (or preselected file for saveFile) the dialog "
                      "starts in. Defaults to the user's home directory.", false)
        .addParameter("filter", "string",
                      "File type filter in Qt syntax; ignored for selectFolder.",
                      false);

    QPointer<QWidget> parent(dialogParent);
    client->registerTool(tool, [parent](const QJsonObject& args) -> QJsonObject {
        const QString mode   = args["mode"].toString();
        const QString title  = args.value("title").toString(
                                   QStringLiteral("Select"));
        const QString start  = args.value("startPath").toString(QDir::homePath());
        const QString filter = args.value("filter").toString();

        if (mode == "openFile") {
            QString path = QFileDialog::getOpenFileName(parent, title, start, filter);
            if (path.isEmpty())
                return QJsonObject{{"status", "cancelled"},
                                   {"message", "user cancelled the dialog"}};
            return toolOk({{"path", path}});
        }
        if (mode == "openFiles") {
            QStringList paths = QFileDialog::getOpenFileNames(parent, title, start, filter);
            if (paths.isEmpty())
                return QJsonObject{{"status", "cancelled"},
                                   {"message", "user cancelled the dialog"}};
            return toolOk({{"paths", QJsonArray::fromStringList(paths)}});
        }
        if (mode == "saveFile") {
            QString path = QFileDialog::getSaveFileName(parent, title, start, filter);
            if (path.isEmpty())
                return QJsonObject{{"status", "cancelled"},
                                   {"message", "user cancelled the dialog"}};
            return toolOk({{"path", path}});
        }
        if (mode == "selectFolder") {
            QString path = QFileDialog::getExistingDirectory(parent, title, start);
            if (path.isEmpty())
                return QJsonObject{{"status", "cancelled"},
                                   {"message", "user cancelled the dialog"}};
            return toolOk({{"path", path}});
        }
        return toolError("invalid value for 'mode'",
                         {{"allowed_values", QJsonArray{"openFile", "openFiles",
                                                        "saveFile", "selectFolder"}}});
    });
}

// ---------------------------------------------------------------- MessageBox

void registerMessageBox(Client* client, QWidget* dialogParent)
{
    Tool tool;
    tool.setName("show_message")
        .setDescription(
            "Show the user a message box. type 'info' and 'warning' just display "
            "the message; type 'question' asks a yes/no question and returns the "
            "answer. For decisions with more than two options use "
            "ask_user_question instead (if available).")
        .setTitle("Message box")
        .setGroup("Dialogs")
        .setStatusText("Waiting for user confirmation…")
        .addEnumParameter("type", {"info", "warning", "question"},
                          "Kind of message box.", true)
        .addParameter("message", "string", "The text to show.", true)
        .addParameter("title", "string", "Window title.", false);

    QPointer<QWidget> parent(dialogParent);
    client->registerTool(tool, [parent](const QJsonObject& args) -> QJsonObject {
        const QString type    = args["type"].toString().toLower();
        const QString message = args["message"].toString();
        const QString title   = args.value("title").toString(
                                    QStringLiteral("Assistant"));

        if (type == "question") {
            auto answer = QMessageBox::question(parent, title, message,
                                                QMessageBox::Yes | QMessageBox::No);
            return toolOk({{"answer", answer == QMessageBox::Yes ? "yes" : "no"}});
        }
        if (type == "warning")
            QMessageBox::warning(parent, title, message);
        else
            QMessageBox::information(parent, title, message);
        return toolOk();
    });
}

// ---------------------------------------------------------------- ColorPicker

void registerColorPicker(Client* client, QWidget* dialogParent)
{
    Tool tool;
    tool.setName("pick_color")
        .setDescription(
            "Let the user pick a color with the system color dialog. Returns the "
            "chosen color as hex string plus RGB components. When you mention the "
            "result in your reply, write the hex code (e.g. #ff8800) — the chat "
            "renders a colored rectangle next to it automatically, so do not "
            "describe the color in words or list RGB numbers.")
        .setTitle("Color picker")
        .setGroup("Dialogs")
        .setStatusText("Waiting for color selection…")
        .addParameter("initialColor", "string",
                      "Preselected color, e.g. \"#ff8800\" or \"red\".", false)
        .addParameter("title", "string", "Dialog window title.", false);

    QPointer<QWidget> parent(dialogParent);
    client->registerTool(tool, [parent](const QJsonObject& args) -> QJsonObject {
        QColor initial(args.value("initialColor").toString());
        if (!initial.isValid())
            initial = Qt::white;
        const QString title = args.value("title").toString(
                                  QStringLiteral("Select color"));

        QColor color = QColorDialog::getColor(initial, parent, title);
        if (!color.isValid())
            return QJsonObject{{"status", "cancelled"},
                               {"message", "user cancelled the dialog"}};
        return toolOk({{"hex", color.name()},
                       {"r", color.red()},
                       {"g", color.green()},
                       {"b", color.blue()}});
    });
}

// ------------------------------------------------------------------ Clipboard

void registerClipboardRead(Client* client)
{
    Tool tool;
    tool.setName("clipboard_read")
        .setDescription(
            "Read the current text content of the system clipboard. Returns an "
            "empty string when the clipboard is empty or holds no text.")
        .setTitle("Read clipboard")
        .setGroup("Clipboard")
        .setStatusText("Reading clipboard…");

    client->registerTool(tool, [](const QJsonObject&) -> QJsonObject {
        QClipboard* clipboard = QApplication::clipboard();
        if (!clipboard)
            return toolError("no clipboard available");
        return toolOk({{"text", clipboard->text()}});
    });
}

void registerClipboardWrite(Client* client)
{
    Tool tool;
    tool.setName("clipboard_write")
        .setDescription("Place text on the system clipboard so the user can "
                        "paste it elsewhere.")
        .setTitle("Write clipboard")
        .setGroup("Clipboard")
        .setStatusText("Writing clipboard…")
        .addParameter("text", "string", "The text to place on the clipboard.", true);

    client->registerTool(tool, [](const QJsonObject& args) -> QJsonObject {
        QClipboard* clipboard = QApplication::clipboard();
        if (!clipboard)
            return toolError("no clipboard available");
        clipboard->setText(args["text"].toString());
        return toolOk();
    });
}

// --------------------------------------------------------------- System info

void registerCurrentDateTime(Client* client)
{
    Tool tool;
    tool.setName("current_datetime")
        .setDescription(
            "Get the user's current local date and time, UTC time and timezone. "
            "Use this instead of guessing — you have no internal clock.")
        .setTitle("Current date/time")
        .setGroup("System");

    client->registerTool(tool, [](const QJsonObject&) -> QJsonObject {
        const QDateTime now = QDateTime::currentDateTime();
        return toolOk({{"local", now.toString(Qt::ISODate)},
                       {"utc", now.toUTC().toString(Qt::ISODate)},
                       {"timezone", QString::fromUtf8(QTimeZone::systemTimeZoneId())},
                       {"weekday", QLocale::c().dayName(now.date().dayOfWeek())}});
    });
}

void registerSystemInfo(Client* client)
{
    Tool tool;
    tool.setName("system_info")
        .setDescription(
            "Get information about the user's environment: operating system, "
            "Qt version, UI language/locale, application name and screen size.")
        .setTitle("System info")
        .setGroup("System");

    client->registerTool(tool, [](const QJsonObject&) -> QJsonObject {
        QJsonObject info{{"os", QSysInfo::prettyProductName()},
                         {"architecture", QSysInfo::currentCpuArchitecture()},
                         {"qtVersion", QString::fromLatin1(qVersion())},
                         {"locale", QLocale::system().name()}};
        if (qApp) {
            info["applicationName"]    = QApplication::applicationName();
            info["applicationVersion"] = QApplication::applicationVersion();
            if (QScreen* screen = QApplication::primaryScreen()) {
                info["screenWidth"]  = screen->geometry().width();
                info["screenHeight"] = screen->geometry().height();
            }
        }
        return toolOk(info);
    });
}

void registerOpenUrl(Client* client)
{
    Tool tool;
    tool.setName("open_url")
        .setDescription(
            "Open a URL in the user's default browser (or default handler for "
            "the scheme). Use for documentation links or web pages the user "
            "should see.")
        .setTitle("Open URL")
        .setGroup("System")
        .setStatusText("Opening URL…")
        .addParameter("url", "string",
                      "Absolute URL including scheme, e.g. \"https://…\".", true);

    client->registerTool(tool, [](const QJsonObject& args) -> QJsonObject {
        const QUrl url(args["url"].toString());
        if (!url.isValid() || url.scheme().isEmpty())
            return toolError("invalid url");
        if (!QDesktopServices::openUrl(url))
            return toolError("failed to open url");
        return toolOk();
    });
}

// ---------------------------------------------------------------- Filesystem

enum class FsAccess { Read, Write };

// "Allow for the rest of this session" flags — per access kind, process-wide.
bool s_sessionAllowRead  = false;
bool s_sessionAllowWrite = false;

// Modal consent dialog shown before every filesystem tool call (unless the
// user already allowed this access kind for the session). Runs on the GUI
// thread — handlers are guaranteed synchronous there.
bool filesystemAccessAllowed(QWidget* parent, FsAccess access,
                             const QString& description)
{
    bool& sessionFlag = (access == FsAccess::Write) ? s_sessionAllowWrite
                                                    : s_sessionAllowRead;
    if (sessionFlag)
        return true;

    QMessageBox box(parent);
    box.setIcon(QMessageBox::Question);
    box.setWindowTitle(QObject::tr("Allow filesystem access?"));
    box.setText(QObject::tr("The assistant requests filesystem access:"));
    box.setInformativeText(description);
    QCheckBox* remember = new QCheckBox(
        access == FsAccess::Write
            ? QObject::tr("Allow file writes for the rest of this session")
            : QObject::tr("Allow file reads for the rest of this session"),
        &box);
    box.setCheckBox(remember);
    box.setStandardButtons(QMessageBox::Yes | QMessageBox::No);
    box.setDefaultButton(QMessageBox::No);

    const bool allowed = (box.exec() == QMessageBox::Yes);
    if (allowed && remember->isChecked())
        sessionFlag = true;
    return allowed;
}

void registerListDirectory(Client* client, QWidget* dialogParent, bool confirm)
{
    Tool tool;
    tool.setName("list_directory")
        .setDescription(
            "List the files and subfolders of a directory. Returns name, type "
            "('file'/'dir') and size in bytes per entry; at most 500 entries "
            "('truncated' is true when more exist).")
        .setTitle("List directory")
        .setGroup("Filesystem")
        .setStatusText("Listing directory…")
        .addParameter("path", "string", "Absolute directory path.", true)
        .addParameter("nameFilter", "string",
                      "Optional wildcard filter for file names, e.g. \"*.txt\".",
                      false);

    QPointer<QWidget> parent(dialogParent);
    client->registerTool(tool, [parent, confirm](const QJsonObject& args) -> QJsonObject {
        if (confirm && !filesystemAccessAllowed(parent, FsAccess::Read,
                QObject::tr("List directory:\n%1").arg(args["path"].toString())))
            return toolError("user declined");
        QDir dir(args["path"].toString());
        if (!dir.exists())
            return toolError("directory does not exist",
                             {{"path", args["path"].toString()}});

        QStringList nameFilters;
        const QString filter = args.value("nameFilter").toString();
        if (!filter.isEmpty())
            nameFilters << filter;

        const QFileInfoList infos = dir.entryInfoList(
            nameFilters, QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot,
            QDir::DirsFirst | QDir::Name);

        const int maxEntries = 500;
        QJsonArray entries;
        for (const QFileInfo& info : infos) {
            if (entries.size() >= maxEntries)
                break;
            entries.append(QJsonObject{
                {"name", info.fileName()},
                {"type", info.isDir() ? "dir" : "file"},
                {"size", static_cast<double>(info.size())}});
        }
        return toolOk({{"entries", entries},
                       {"truncated", infos.size() > maxEntries}});
    });
}

void registerReadTextFile(Client* client, QWidget* dialogParent, bool confirm)
{
    Tool tool;
    tool.setName("read_text_file")
        .setDescription(
            "Read a text file (UTF-8) and return its content. Content is capped "
            "at maxBytes (default 65536, maximum 262144); 'truncated' is true "
            "when the file is larger.")
        .setTitle("Read text file")
        .setGroup("Filesystem")
        .setStatusText("Reading file…")
        .addParameter("path", "string", "Absolute file path.", true)
        .addParameter("maxBytes", "integer",
                      "Maximum number of bytes to return.", false);

    QPointer<QWidget> parent(dialogParent);
    client->registerTool(tool, [parent, confirm](const QJsonObject& args) -> QJsonObject {
        const QString path = args["path"].toString();
        if (confirm && !filesystemAccessAllowed(parent, FsAccess::Read,
                QObject::tr("Read file:\n%1").arg(path)))
            return toolError("user declined");
        QFile file(path);
        if (!file.exists())
            return toolError("file does not exist", {{"path", path}});
        if (!file.open(QIODevice::ReadOnly))
            return toolError(QString("cannot open file: %1").arg(file.errorString()),
                             {{"path", path}});

        qint64 cap = args.value("maxBytes").toInt(65536);
        cap = qBound<qint64>(1, cap, 262144);

        const QByteArray data = file.read(cap);
        return toolOk({{"content", QString::fromUtf8(data)},
                       {"size", static_cast<double>(file.size())},
                       {"truncated", file.size() > cap}});
    });
}

void registerWriteTextFile(Client* client, QWidget* dialogParent, bool confirm)
{
    Tool tool;
    tool.setName("write_text_file")
        .setDescription(
            "Write text (UTF-8) to a file, creating it if needed. Set append to "
            "true to add to the end instead of overwriting. Parent directories "
            "must exist.")
        .setTitle("Write text file")
        .setGroup("Filesystem")
        .setStatusText("Writing file…")
        .addParameter("path", "string", "Absolute file path.", true)
        .addParameter("content", "string", "The text to write.", true)
        .addParameter("append", "boolean",
                      "true appends, false (default) overwrites.", false);

    QPointer<QWidget> parent(dialogParent);
    client->registerTool(tool, [parent, confirm](const QJsonObject& args) -> QJsonObject {
        const QString path = args["path"].toString();
        if (confirm && !filesystemAccessAllowed(parent, FsAccess::Write,
                QObject::tr("%1 file:\n%2")
                    .arg(args.value("append").toBool(false) ? QObject::tr("Append to")
                                                            : QObject::tr("Write"))
                    .arg(path)))
            return toolError("user declined");
        QIODevice::OpenMode mode = QIODevice::WriteOnly | QIODevice::Text;
        if (args.value("append").toBool(false))
            mode |= QIODevice::Append;
        else
            mode |= QIODevice::Truncate;

        QFile file(path);
        if (!file.open(mode))
            return toolError(QString("cannot open file: %1").arg(file.errorString()),
                             {{"path", path}});

        const qint64 written = file.write(args["content"].toString().toUtf8());
        if (written < 0)
            return toolError(QString("write failed: %1").arg(file.errorString()),
                             {{"path", path}});
        return toolOk({{"bytesWritten", static_cast<double>(written)}});
    });
}

// ------------------------------------------------------------ RepeatingTask

// Owns the timers of the repeating-task tools. Child of the Client, so its
// lifetime matches the registered handlers. Output goes to the bound
// ChatDockWidget only — no API request, no token cost.
class RepeatingTaskManager : public QObject
{
public:
    RepeatingTaskManager(Client* client, ChatDockWidget* chat)
        : QObject(client)
        , m_client(client)
        , m_chat(chat)
    {}

    QJsonObject start(const QJsonObject& args)
    {
        if (!m_chat)
            return toolError("chat widget no longer available");

        QString id = args.value("id").toString();
        if (id.isEmpty())
            id = QString("task-%1").arg(++m_autoId);
        if (m_tasks.contains(id))
            return toolError("a task with this id already exists", {{"id", id}});

        Task task;
        task.intervalMs = qMax(100, args["intervalMs"].toInt());
        task.count      = qMax(0, args.value("count").toInt(0));
        task.message    = args["message"].toString();
        task.target     = args.value("target").toString(QStringLiteral("status"));
        task.notifyWhenDone = args.value("notifyWhenDone").toBool(false);
        task.iteration  = 0;
        if (task.message.isEmpty())
            return toolError("missing required parameter: message");

        task.timer = new QTimer(this);
        task.timer->setInterval(task.intervalMs);
        connect(task.timer, &QTimer::timeout, this, [this, id]() { tick(id); });
        m_tasks.insert(id, task);
        task.timer->start();

        return toolOk({{"id", id},
                       {"intervalMs", task.intervalMs},
                       {"count", task.count}});
    }

    QJsonObject cancel(const QString& id)
    {
        auto it = m_tasks.find(id);
        if (it == m_tasks.end())
            return toolError("no such task", {{"id", id}});
        const int done = it->iteration;
        remove(it);
        return toolOk({{"id", id}, {"iterationsDone", done}});
    }

    QJsonObject list() const
    {
        QJsonArray tasks;
        for (auto it = m_tasks.constBegin(); it != m_tasks.constEnd(); ++it)
            tasks.append(QJsonObject{{"id", it.key()},
                                     {"intervalMs", it->intervalMs},
                                     {"count", it->count},
                                     {"iteration", it->iteration},
                                     {"target", it->target}});
        return toolOk({{"tasks", tasks}});
    }

private:
    struct Task {
        QTimer* timer = nullptr;
        QString message;
        QString target;          // "status" | "chat"
        int     intervalMs = 0;
        int     count = 0;       // 0 = until canceled
        int     iteration = 0;
        bool    notifyWhenDone = false;
    };

    void tick(const QString& id)
    {
        auto it = m_tasks.find(id);
        if (it == m_tasks.end())
            return;
        if (!m_chat) {           // chat gone -> stop everything silently
            remove(it);
            return;
        }

        Task& task = *it;
        ++task.iteration;

        QString text = task.message;
        text.replace(QStringLiteral("{i}"), QString::number(task.iteration));
        text.replace(QStringLiteral("{count}"), QString::number(task.count));
        text.replace(QStringLiteral("{id}"), id);

        if (task.target == QLatin1String("chat"))
            m_chat->addAssistantMessage(text);
        else
            m_chat->setStatusText(text);

        if (task.count > 0 && task.iteration >= task.count) {
            const bool notify = task.notifyWhenDone;
            const int  done   = task.iteration;
            remove(it);
            // Optional new model turn — the only path that costs tokens.
            if (notify && m_client)
                m_client->sendToolMessage(
                    QStringLiteral("start_repeating_task"),
                    QJsonObject{{"id", id},
                                {"status", "finished"},
                                {"iterationsDone", done}});
        }
    }

    void remove(QMap<QString, Task>::iterator it)
    {
        it->timer->stop();
        it->timer->deleteLater();
        m_tasks.erase(it);
    }

    QPointer<Client>         m_client;
    QPointer<ChatDockWidget> m_chat;
    QMap<QString, Task>      m_tasks;
    int                      m_autoId = 0;
};

void registerRepeatingTask(Client* client, ChatDockWidget* chat)
{
    auto* manager = new RepeatingTaskManager(client, chat);

    Tool startTool;
    startTool.setName("start_repeating_task")
        .setDescription(
            "Start a repeating task that periodically writes a message into the "
            "chat window — purely local UI output, it costs no tokens and does "
            "not call you. The message may contain the placeholders {i} "
            "(iteration number, starts at 1), {count} and {id}, e.g. "
            "\"Counter: {i}/{count}\". Runs 'count' times, or until "
            "cancel_repeating_task if count is 0. target 'status' overwrites "
            "the status line each tick (good for counters); target 'chat' "
            "appends a chat message each tick (avoid for fast intervals). Set "
            "notifyWhenDone to true only if you need to be called back when "
            "the task finishes — that starts a new model turn and costs tokens.")
        .setTitle("Start repeating task")
        .setGroup("Automation")
        .addParameter("intervalMs", "integer",
                      "Interval between ticks in milliseconds (minimum 100).", true)
        .addParameter("message", "string",
                      "Text shown each tick; supports {i}, {count}, {id}.", true)
        .addParameter("count", "integer",
                      "Number of ticks; 0 (default) repeats until canceled.", false)
        .addEnumParameter("target", {"status", "chat"},
                          "Where the message goes; default 'status'.", false)
        .addParameter("id", "string",
                      "Task id for cancelling; auto-generated when omitted.", false)
        .addParameter("notifyWhenDone", "boolean",
                      "true sends you a tool message when the task finished "
                      "(costs tokens). Default false.", false);
    client->registerTool(startTool, [manager](const QJsonObject& args) {
        return manager->start(args);
    });

    Tool cancelTool;
    cancelTool.setName("cancel_repeating_task")
        .setDescription("Cancel a repeating task started with "
                        "start_repeating_task.")
        .setTitle("Cancel repeating task")
        .setGroup("Automation")
        .addParameter("id", "string", "Id of the task to cancel.", true);
    client->registerTool(cancelTool, [manager](const QJsonObject& args) {
        return manager->cancel(args["id"].toString());
    });

    Tool listTool;
    listTool.setName("list_repeating_tasks")
        .setDescription("List all currently running repeating tasks with their "
                        "id, interval and progress.")
        .setTitle("List repeating tasks")
        .setGroup("Automation");
    client->registerTool(listTool, [manager](const QJsonObject&) {
        return manager->list();
    });
}

} // namespace

// ------------------------------------------------------------- BuiltinTools

void BuiltinTools::registerTool(Client* client, BuiltinTool tool,
                                const Context& context)
{
    if (!client)
        return;

    switch (tool) {
    case BuiltinTool::AskUserQuestion:
        InterviewTool::registerOn(client, context.chat);
        break;
    case BuiltinTool::FileDialog:
        registerFileDialog(client, context.dialogParent);
        break;
    case BuiltinTool::MessageBox:
        registerMessageBox(client, context.dialogParent);
        break;
    case BuiltinTool::ColorPicker:
        registerColorPicker(client, context.dialogParent);
        break;
    case BuiltinTool::ClipboardRead:
        registerClipboardRead(client);
        break;
    case BuiltinTool::ClipboardWrite:
        registerClipboardWrite(client);
        break;
    case BuiltinTool::CurrentDateTime:
        registerCurrentDateTime(client);
        break;
    case BuiltinTool::SystemInfo:
        registerSystemInfo(client);
        break;
    case BuiltinTool::OpenUrl:
        registerOpenUrl(client);
        break;
    case BuiltinTool::ListDirectory:
        registerListDirectory(client, context.dialogParent,
                              context.confirmFilesystemAccess);
        break;
    case BuiltinTool::ReadTextFile:
        registerReadTextFile(client, context.dialogParent,
                             context.confirmFilesystemAccess);
        break;
    case BuiltinTool::WriteTextFile:
        registerWriteTextFile(client, context.dialogParent,
                              context.confirmFilesystemAccess);
        break;
    case BuiltinTool::RepeatingTask:
        registerRepeatingTask(client, context.chat);
        break;
    }
}

void BuiltinTools::registerTools(Client* client, const QList<BuiltinTool>& tools,
                                 const Context& context)
{
    for (BuiltinTool tool : tools)
        registerTool(client, tool, context);
}

QString BuiltinTools::toolName(BuiltinTool tool)
{
    switch (tool) {
    case BuiltinTool::AskUserQuestion: return QStringLiteral("ask_user_question");
    case BuiltinTool::FileDialog:      return QStringLiteral("file_dialog");
    case BuiltinTool::MessageBox:      return QStringLiteral("show_message");
    case BuiltinTool::ColorPicker:     return QStringLiteral("pick_color");
    case BuiltinTool::ClipboardRead:   return QStringLiteral("clipboard_read");
    case BuiltinTool::ClipboardWrite:  return QStringLiteral("clipboard_write");
    case BuiltinTool::CurrentDateTime: return QStringLiteral("current_datetime");
    case BuiltinTool::SystemInfo:      return QStringLiteral("system_info");
    case BuiltinTool::OpenUrl:         return QStringLiteral("open_url");
    case BuiltinTool::ListDirectory:   return QStringLiteral("list_directory");
    case BuiltinTool::ReadTextFile:    return QStringLiteral("read_text_file");
    case BuiltinTool::WriteTextFile:   return QStringLiteral("write_text_file");
    // Registers cancel_repeating_task and list_repeating_tasks as well.
    case BuiltinTool::RepeatingTask:   return QStringLiteral("start_repeating_task");
    }
    return QString();
}

QList<BuiltinTool> BuiltinTools::allTools()
{
    return {BuiltinTool::AskUserQuestion,
            BuiltinTool::FileDialog,
            BuiltinTool::MessageBox,
            BuiltinTool::ColorPicker,
            BuiltinTool::ClipboardRead,
            BuiltinTool::ClipboardWrite,
            BuiltinTool::CurrentDateTime,
            BuiltinTool::SystemInfo,
            BuiltinTool::OpenUrl,
            BuiltinTool::ListDirectory,
            BuiltinTool::ReadTextFile,
            BuiltinTool::WriteTextFile,
            BuiltinTool::RepeatingTask};
}

} // namespace QtLLM
