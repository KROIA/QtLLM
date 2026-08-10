#pragma once
#include "QtLLM_base.h"
#include "Tool.h"
#include "ProtocolBase.h"
#include "UsageStats.h"
#include "UsageHistory.h"
#include <QObject>
#include <QString>
#include <QStringList>
#include <QList>
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

// Called before a tool handler runs. Return false to deny execution — the
// model receives {"status":"error","message":"user declined"} instead.
// Invoked synchronously on the GUI thread, so a modal dialog is possible.
using ToolConsentHandler = std::function<bool(const QString& toolName, const QJsonObject& input)>;

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

    // Enable/disable a registered tool without losing its definition or
    // handler. Disabled tools are omitted from the schema list sent to the
    // model; if the model calls one anyway (stale/cached tool list) it
    // receives {"status":"error","message":"tool is disabled"}.
    void setToolEnabled(const QString& toolName, bool enabled);
    bool isToolEnabled(const QString& toolName) const;   // false if unknown
    QStringList toolNames() const;                       // all registered
    QStringList enabledToolNames() const;                // currently advertised to the model

    // Tool definitions incl. UI metadata (title/group/statusText) — basis for
    // a generic tool-settings widget. Tools registered via the raw-schema
    // overload appear with name/description only.
    QList<Tool> registeredTools() const;

    // Validate tool input against the registered schema before invoking the
    // handler (required fields, types, enum values case-insensitively).
    // On violation the handler is NOT called; the model receives the error
    // result directly. Default: false (behaviour identical to before).
    void setValidateToolInput(bool enabled);
    bool validateToolInput() const;

    // Cap on executed tool calls per user turn; 0 = unlimited (default).
    // When exceeded, handlers are no longer invoked and the model receives
    // {"status":"error","message":"tool call limit reached"};
    // toolCallLimitReached() is emitted once per turn.
    void setMaxToolCallsPerTurn(int maxCalls);
    int  maxToolCallsPerTurn() const;

    // Consent gate before every tool execution (accept/decline pattern).
    // No handler set = everything allowed (default). Pass nullptr to reset.
    void setToolConsentHandler(ToolConsentHandler handler);

    // Appends userMessage to history and sends the full conversation to the API.
    void sendPrompt(const QString& userMessage);

	void sendToolMessage(const QString& toolName, const QJsonObject& input);

    // Clears conversation history and resets session statistics. Preserves registered tools and settings.
    void clearConversation();

    QJsonArray conversationHistory() const;

    // Returns the statistics from the most recently completed turn.
    UsageStats usageStats() const;

    // Persistent per-turn usage history (JSONL-backed).
    UsageHistory* usageHistory();

    // Asynchronously fetch available models from the current provider.
    // Results arrive via modelsAvailable().
    void fetchAvailableModels();

    // Build a rich JSON export of the full conversation including raw messages
    // (with tool_use / tool_result blocks), usage statistics, and per-turn timeline.
    QJsonObject exportConversation() const;

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
    // Emitted when fetchAvailableModels() completes.
    void modelsAvailable(const QStringList& models);
    // Emitted once per turn when setMaxToolCallsPerTurn() is exceeded.
    void toolCallLimitReached(int limit);

private slots:
    void onProtocolResponseReady(const QString& text);
    void onProtocolStatsReady(const QtLLM::UsageStats& stats);
    void onProtocolError(const QString& errorMessage);

private:
    void connectProtocol();
    void syncToolsToProtocol();

    struct RegisteredTool {
        QJsonObject claudeSchema;   // toApiObject() format
        QJsonObject openAiSchema;   // toOpenAiApiObject() format
        ToolHandler handler;
        Tool        tool;           // definition incl. UI metadata (name/description only for raw registrations)
        bool        enabled = true;
    };

    void recordSample(const UsageStats& stats);
    ToolHandler wrapHandler(const QString& toolName, const RegisteredTool& rt);

    QMap<QString, RegisteredTool> m_tools;
    QJsonArray                    m_history;
    ProtocolBase*                 m_protocol;
    UsageStats                    m_lastStats;
    UsageHistory*                 m_usageHistory;
    QString                       m_currentModel;
    QString                       m_currentProvider;
    QString                       m_systemPrompt;
    ToolConsentHandler            m_consentHandler;
    bool                          m_validateToolInput = false;
    int                           m_maxToolCallsPerTurn = 0;   // 0 = unlimited
    int                           m_toolCallsThisTurn = 0;
    bool                          m_limitSignalEmitted = false;

#if LOGGER_LIBRARY_AVAILABLE == 1
    Log::LogObject m_logger{Logger::getID(), "QtLLM::Client"};
#endif
};

} // namespace QtLLM
