#pragma once
#include "QtLLM_base.h"
#include <QString>
#include <QJsonObject>

namespace QtLLM
{

class Client;
class ChatDockWidget;

// LLM tool that lets the model interview the user: it opens an interactive
// card inside the chat (single-select, multi-select, or free-text answers)
// and blocks the tool call until the user submits — the same interaction
// pattern as Claude Code's AskUserQuestion tool in its CLI.
//
// Usage:
//   QtLLM::InterviewTool::registerOn(&client, chatDock);
//
// The handler runs on the GUI thread and spins a local event loop
// (like QDialog::exec) while waiting, so the UI stays responsive.
class QT_LLM_API InterviewTool
{
public:
    // Tool name sent to the API: "ask_user_question"
    static QString name();
    static QString description();
    // Raw JSON Schema for the tool input (nested questions/options structure).
    static QJsonObject parameterSchema();

    // Register the tool on client, wired to show interview cards in chat.
    // chat must outlive the client's use of the tool; if chat is destroyed
    // while a question is pending, the tool returns an error result.
    static void registerOn(Client* client, ChatDockWidget* chat);

private:
    InterviewTool() = delete;
};

} // namespace QtLLM
