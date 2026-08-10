#pragma once
#include "QtLLM_base.h"
#include <QString>
#include <QList>

class QWidget;

namespace QtLLM
{

class Client;
class ChatDockWidget;

// Predefined tools the library offers out of the box. Each value can be
// registered individually — the host app decides exactly which capabilities
// the model gets. Nothing is registered automatically.
enum class BuiltinTool
{
    AskUserQuestion,  // interactive question form in the chat (InterviewTool)
    FileDialog,       // native open/save/folder dialog, filter + start path
    MessageBox,       // info/warning box or yes-no confirmation dialog
    ColorPicker,      // QColorDialog, returns the chosen color
    ClipboardRead,    // read text from the system clipboard
    ClipboardWrite,   // place text on the system clipboard
    CurrentDateTime,  // local + UTC time, timezone (models have no clock)
    SystemInfo,       // OS, Qt version, locale, app name, screen geometry
    OpenUrl,          // open a URL in the default browser/handler
    ListDirectory,    // list files/folders of a directory
    ReadTextFile,     // read a text file (size-capped)
    WriteTextFile,    // write/append a text file — gate with the consent hook
    RepeatingTask,    // registers 3 tools: start_repeating_task /
                      // cancel_repeating_task / list_repeating_tasks —
                      // periodic messages into the chat/status line, purely
                      // local (no tokens); needs Context::chat
};

// Registrar for the built-in tools:
//
//   QtLLM::BuiltinTools::registerTools(&client,
//       { QtLLM::BuiltinTool::AskUserQuestion,
//         QtLLM::BuiltinTool::FileDialog,
//         QtLLM::BuiltinTool::CurrentDateTime },
//       { /*chat*/ chatDock, /*dialogParent*/ this });
//
// Every tool arrives with UI metadata (title/group/statusText) set, so it
// shows up nicely in settings dialogs and the ChatDockWidget status line.
// After registration they behave like any user tool: setToolEnabled,
// consent hook, validation and the call cap all apply.
class QT_LLM_API BuiltinTools
{
public:
    struct Context
    {
        // Required by AskUserQuestion (where the interview card is shown).
        ChatDockWidget* chat = nullptr;
        // Parent for modal dialogs (FileDialog, MessageBox, ColorPicker,
        // filesystem consent). May stay nullptr — dialogs are then unparented.
        QWidget* dialogParent = nullptr;

        // When true (default), ListDirectory/ReadTextFile ask the user for
        // read consent and WriteTextFile for write consent before every call.
        // The consent dialog offers "Allow for the rest of this session";
        // that choice persists per access kind (read/write) for the process
        // lifetime. Declining returns {"status":"error","message":"user
        // declined"} to the model. Set false if the app gates filesystem
        // access itself (e.g. via Client::setToolConsentHandler).
        bool confirmFilesystemAccess = true;
    };

    // Register one built-in tool on the client. Registering a tool whose
    // context requirement is missing (e.g. AskUserQuestion without chat)
    // still works — the tool then returns an error result when called.
    static void registerTool(Client* client, BuiltinTool tool,
                             const Context& context = {});

    // Register several built-in tools at once (one schema sync per tool —
    // do this before the first sendPrompt to keep the prompt cache warm).
    static void registerTools(Client* client, const QList<BuiltinTool>& tools,
                              const Context& context = {});

    // Model-facing tool name for an enum value, e.g. "file_dialog".
    static QString toolName(BuiltinTool tool);

    // All enum values, in declaration order. Basis for settings UIs that
    // offer every built-in tool as a checkbox.
    static QList<BuiltinTool> allTools();

private:
    BuiltinTools() = delete;
};

} // namespace QtLLM
