#pragma once
#include "QtLLM_base.h"
#include "Tool.h"
#include "ProtocolBase.h"
#include "UsageStats.h"
#include <QObject>
#include <QString>
#include <QJsonArray>
#include <QJsonObject>
#include <QUrl>
#include <QMap>
#include <functional>

namespace QtLLM {

// Selects which LLM provider protocol Client uses internally.
enum class Provider {
    Claude,  // Anthropic Claude Messages API (default)
    Ollama   // Ollama local inference server
};

class QT_LLM_API Client : public QObject
{
    Q_OBJECT
public:
    explicit Client(const QString& apiKey,
                    const QString& url = QStringLiteral("https://api.anthropic.com/v1/messages"),
                    QObject* parent = nullptr);

    // Construct with explicit provider selection.
    // For Ollama, apiKey may be empty (local server needs no auth).
    // Default Ollama URL: http://localhost:11434/api/chat
    explicit Client(Provider provider,
                    const QString& url,
                    const QString& apiKey = QString(),
                    QObject* parent = nullptr);

    ~Client() override;

    // Model ID string; provider-specific (e.g. "claude-sonnet-4-5" or "llama3.2").
    void setModel(const QString& model);
    // Caps response length; maps to num_predict for Ollama.
    void setMaxTokens(int maxTokens);
    // Injected as first message each request; empty = omitted.
    void setSystemPrompt(const QString& systemPrompt);

    // Register a tool the LLM can call. The handler is called synchronously when the LLM invokes the tool.
    void registerTool(const Tool& tool, ToolHandler handler);

    // Overload: register using the low-level raw JSON schema directly
    void registerTool(const QString& name,
                      const QString& description,
                      const QJsonObject& parameterSchema,
                      ToolHandler handler);

    void unregisterTool(const QString& toolName);

    // Appends userMessage to history and sends the full conversation to the API.
    void sendPrompt(const QString& userMessage);

    // Clears conversation history and resets session statistics. Preserves registered tools and settings.
    void clearConversation();

    QJsonArray conversationHistory() const;

    // Returns the statistics from the most recently completed turn.
    UsageStats usageStats() const;

signals:
    // Emitted after all tool calls in a turn are resolved; text is the final LLM reply.
    void responseReady(const QString& text);
    // Emitted just before the registered handler is called.
    void toolInvoked(const QString& toolName, const QJsonObject& input);
    // Emitted after the handler returns with its result.
    void toolCompleted(const QString& toolName, const QJsonObject& result);
    // Covers network errors, HTTP errors, and parse failures; requestFinished is also emitted.
    void errorOccurred(const QString& errorMessage);
    // Emitted once per HTTP send, including tool-loop re-sends.
    void requestStarted();
    // Emitted when the turn is fully resolved (after final response or error).
    void requestFinished();
    // Emitted once per completed turn with token counts, timing, and estimated cost.
    void statsUpdated(const QtLLM::UsageStats& stats);

private slots:
    void onProtocolResponseReady(const QString& text);
    void onProtocolStatsReady(const QtLLM::UsageStats& stats);

private:
    void connectProtocol();
    void syncToolsToProtocol();

    struct RegisteredTool {
        QJsonObject claudeSchema;   // toApiObject() format
        QJsonObject openAiSchema;   // toOpenAiApiObject() format
        ToolHandler handler;
    };

    QMap<QString, RegisteredTool> m_tools;
    QJsonArray                    m_history;
    ProtocolBase*                 m_protocol;
    UsageStats                    m_lastStats;
};

} // namespace QtLLM
