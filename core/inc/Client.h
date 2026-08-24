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

class ContextInfoProvider;

// Selects which LLM provider protocol Client uses internally.
enum class Provider {
    Claude,  // Anthropic Claude Messages API (default)
    Ollama   // Ollama local inference server
};

// Called before a tool handler runs. Return false to deny execution — the
// model receives {"status":"error","message":"user declined"} instead.
// Invoked synchronously on the GUI thread, so a modal dialog is possible.
using ToolConsentHandler = std::function<bool(const QString& toolName, const QJsonObject& input)>;

// App-supplied source for a model's max context window size, consulted by
// contextBreakdown(). Return <= 0 to fall through to the built-in static
// table. Exists so that once a provider exposes this via API instead of only
// docs, the app can inject a live lookup without Client changing at all —
// same shape as PricingRegistry::PricingResolver.
using ContextWindowResolver = std::function<int(const QString& provider, const QString& model)>;

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

    // Change the API key / endpoint after construction (e.g. applying a
    // Settings dialog edit) - no-ops on the field the current provider
    // doesn't use (setApiKey() for Ollama, since it needs no auth).
    void setApiKey(const QString& apiKey);
    void setEndpointUrl(const QString& url);

    // Switch providers at runtime (e.g. applying a Settings dialog change).
    // Tears down and replaces the internal protocol + context-info provider
    // and clears conversation history, since message formats aren't
    // compatible across providers. apiKey is ignored for Ollama.
    void setProvider(Provider provider, const QString& url, const QString& apiKey = QString());

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

    // Breakdown of the current context: system prompt, enabled tool schemas,
    // and conversation history (segments are always a chars/4 estimate -
    // no tokenizer API breaks its count down by section). contextWindowTokens
    // and exactUsedTokens become real, provider-reported values once the
    // background ContextInfoProvider calls triggered by contextChanged()
    // complete; until then they hold estimates/-1. Cheap to call from a UI
    // update slot - no I/O happens here, it just reads cached state.
    ContextBreakdown contextBreakdown() const;

    // Real per-provider tokenizer / context-window source backing the
    // fields above (OllamaContextInfoProvider or ClaudeContextInfoProvider).
    // Exposed for apps that want to call it directly or swap it out.
    ContextInfoProvider* contextInfoProvider() const { return m_contextInfoProvider; }

    // Install a custom resolver for the model's context window size, taking
    // priority over both the live ContextInfoProvider result and the
    // built-in static table. Pass nullptr to reset. GUI-thread only.
    void setContextWindowResolver(ContextWindowResolver resolver);

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
    // Emitted whenever anything feeding contextBreakdown() changes (system
    // prompt, tool set, or conversation history) — hook for live context UI.
    void contextChanged();

private slots:
    void onProtocolResponseReady(const QString& text);
    void onProtocolStatsReady(const QtLLM::UsageStats& stats);
    void onProtocolError(const QString& errorMessage);
    void onContextWindowTokensReady(int tokens);
    void onExactTokenCountReady(int tokens);
    void onContextInfoError(const QString& message);
    // Self-corrects the current model if it isn't actually one of the models
    // this provider offers - connected to modelsAvailable() and triggered
    // once automatically right after construction (see fetchAvailableModels()
    // call in each constructor), so a hardcoded/stale default model doesn't
    // silently fail every send until the app happens to fetch a model list
    // some other way (e.g. opening a Settings dialog). Never overrides a
    // model that genuinely is offered, even if it isn't first in the list.
    void validateCurrentModel(const QStringList& models);

private:
    void connectProtocol();
    void syncToolsToProtocol();
    void refreshContextInfo();  // debounced; called after every contextChanged()

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
    ContextWindowResolver         m_contextWindowResolver;
    bool                          m_validateToolInput = false;
    int                           m_maxToolCallsPerTurn = 0;   // 0 = unlimited
    int                           m_toolCallsThisTurn = 0;
    bool                          m_limitSignalEmitted = false;

    ContextInfoProvider*          m_contextInfoProvider = nullptr;
    QMap<QString, int>            m_contextWindowCache;             // model -> real max tokens
    bool                          m_contextWindowFetchInFlight = false;
    bool                          m_tokenCountFetchInFlight = false;
    bool                          m_contextInfoRefreshScheduled = false;
    uint                          m_lastCountedHash = 0;
    int                           m_exactUsedTokens = -1;

#if LOGGER_LIBRARY_AVAILABLE == 1
    Log::LogObject m_logger{Logger::getID(), "QtLLM::Client"};
#endif
};

} // namespace QtLLM
