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
#include <QHash>
#include <QImage>
#include <QMap>
#include <QSharedPointer>
#include <functional>

namespace QtLLM {

class ContextInfoProvider;

// Selects which LLM provider protocol Client uses internally.
enum class Provider {
    Claude,     // Anthropic Messages API with an API key: api.anthropic.com,
                // Microsoft Foundry, or any gateway speaking the same protocol
    Ollama,     // Ollama local inference server
    ClaudeCode, // the user's claude.ai subscription, through the locally
                // installed Claude Code CLI (see ClaudeCodeProtocol); url and
                // apiKey are unused
    OpenAI      // any server speaking the OpenAI Chat Completions API: LM
                // Studio, llama.cpp, vLLM, Ollama's /v1, api.openai.com. url is
                // the base ("http://localhost:1234/v1"); apiKey may be empty
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

class Client;

// How Client::callTool() runs a tool. The defaults are what a call by the
// model gets; a caller that makes many calls on its own account (a script)
// switches off what is meant for single calls in a conversation.
struct ToolCallOptions
{
    bool announce    = true;   // emit toolInvoked() / toolCompleted()
    bool countInTurn = true;   // counts against setMaxToolCallsPerTurn()
    bool capResult   = true;   // setMaxToolResultChars() applies
    bool askConsent  = true;   // the consent handler is asked
};

// Handed to an asynchronous tool handler: how it reports back. Copyable, and
// usable from any thread - the result is delivered on the GUI thread.
class QT_LLM_API ToolCompletion
{
public:
    ToolCompletion() = default;

    // Reports the tool's result to the model. Only the first call counts; a
    // call after the timeout or after the turn was cancelled does nothing.
    void finish(const QJsonObject& result) const;
    // Shown as the chat's status line while the tool is working
    // (Client::toolProgress).
    void progress(const QString& statusText) const;
    // True once finish() was called or the tool timed out - a long-running
    // tool can check it and stop early.
    bool isFinished() const;

private:
    friend class Client;
    struct State;
    QSharedPointer<State> d;
};

// A tool that answers later: the handler returns immediately and calls
// done.finish(result) when the work is done.
using AsyncToolHandler = std::function<void(const QJsonObject& input, ToolCompletion done)>;

// How large the conversation may get, and what happens when it gets there.
struct QT_LLM_API ContextPolicy
{
    enum class Strategy {
        Summarize,   // the model writes a summary that replaces the conversation
        DropOldest   // oldest turns are removed; no request, nothing remembered of them
    };

    // Compact by itself when the limit is reached. Off: only on request
    // (Client::compactConversation(), the chat's compact button).
    bool     autoCompact  = false;
    // The context to work with, in tokens; 0 = the model's own window. Lets a
    // 1M-token model be held at e.g. 300k: cheaper turns, and the usage bar
    // fills up against this figure.
    int      limitTokens  = 0;
    // Compaction starts at this share of the limit, because the next turn
    // still adds to the conversation.
    double   triggerRatio = 0.9;
    Strategy strategy     = Strategy::Summarize;
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

    // Model used until the application sets one: the best value for tool work
    // on each backend (Claude: the current Sonnet), not the most capable.
    static QString defaultModel(Provider provider);
    // What to run on when the configured model is not offered: the default
    // model, else the next cheaper current one, and only then whatever the
    // provider lists first - which on the Anthropic API is the newest and most
    // expensive model.
    static QString pickFallbackModel(Provider provider, const QStringList& offered);

    // Model ID string; provider-specific (e.g. "claude-sonnet-5-5" or "llama3.2").
    void setModel(const QString& model);
    // The model requests actually go out with. Not necessarily the one that was
    // set: validateCurrentModel() replaces a model the provider does not offer.
    QString model() const;

    // Which models may be used at all. Patterns are model names with * and ?
    // wildcards, case-insensitive: a model must match `allowed` (when that is
    // not empty) and must not match `blocked`. Enforced everywhere: setModel()
    // ignores a model that is not allowed, modelsAvailable() lists only
    // allowed ones, the automatic fallback picks among those, and sendPrompt()
    // refuses to send with a model that is not allowed.
    void setModelFilter(const QStringList& allowed, const QStringList& blocked = QStringList());
    QStringList allowedModels() const { return m_allowedModels; }
    QStringList blockedModels() const { return m_blockedModels; }
    bool isModelAllowed(const QString& model) const;
    // True when `model` matches one of the wildcard patterns.
    static bool modelMatches(const QString& model, const QStringList& patterns);
    // Caps response length; maps to num_predict for Ollama.
    void setMaxTokens(int maxTokens);
    // Injected as first message each request; empty = omitted.
    void setSystemPrompt(const QString& systemPrompt);

    // Change the API key / endpoint after construction (e.g. applying a
    // Settings dialog edit) - no-ops on the field the current provider
    // doesn't use (setApiKey() for Ollama, since it needs no auth).
    void setApiKey(const QString& apiKey);
    void setEndpointUrl(const QString& url);

    // Sign requests with a user's bearer token instead of an API key, for a
    // gateway behind an OAuth2 proxy that swaps the token for the real key
    // (see OAuth2Session::accessTokenProvider()). Asked before every request;
    // an empty token fails the request with "Not signed in". Applies to the
    // Claude and OpenAI providers and survives setProvider(). Pass an empty
    // provider to go back to the API key.
    void setAccessTokenProvider(AccessTokenProvider provider);

    // Switch providers at runtime (e.g. applying a Settings dialog change).
    // Tears down and replaces the internal protocol + context-info provider
    // and clears conversation history, since message formats aren't
    // compatible across providers. apiKey is ignored for Ollama.
    void setProvider(Provider provider, const QString& url, const QString& apiKey = QString());
    Provider provider() const { return m_provider; }

    // Abort the turn in flight, if any: the request is cancelled, queued
    // prompts are dropped and the unanswered prompt is removed from the
    // history. requestFinished() is emitted; responseReady() and
    // errorOccurred() are not. A tool handler that is already running is not
    // interrupted - its result is discarded.
    void cancelRequest();

    // Register a tool the LLM can call. The handler is called synchronously when the LLM invokes the tool.
    void registerTool(const Tool& tool, ToolHandler handler);

    // Overload: register using the low-level raw JSON schema directly
    void registerTool(const QString& name,
                      const QString& description,
                      const QJsonObject& parameterSchema,
                      ToolHandler handler);

    // Register a tool that answers later, without blocking the application
    // while it works. The handler runs on the GUI thread and must return
    // quickly; it hands the result to `done` whenever it has it - from a
    // signal, a timer, a network reply. The turn waits for it. A tool that
    // has not answered within Tool::setTimeoutMs() (default 5 minutes) is
    // reported to the model as timed out.
    void registerAsyncTool(const Tool& tool, AsyncToolHandler handler);

    // Register a tool whose (ordinary, blocking) handler runs on a worker
    // thread, for work that takes a while: computation, file processing. The
    // handler must not touch widgets.
    void registerBackgroundTool(const Tool& tool, ToolHandler handler);

    void unregisterTool(const QString& toolName);

    // Runs a registered tool as the model would: input repaired and checked
    // against its definition, consent, timeout for asynchronous tools. `done`
    // is called with the result - at once for an ordinary tool, later for an
    // asynchronous one. An unknown or disabled tool, or input that does not
    // fit, yields an error result ({"status":"error",...}) without the
    // handler having run.
    void callTool(const QString& toolName, const QJsonObject& arguments,
                  ToolResultCallback done, const ToolCallOptions& options = ToolCallOptions());

    // Registers this client's tools on `other` as well: same definitions,
    // same handlers, same enabled state - e.g. to give a background agent what
    // the chat has. The handlers are shared, not copied: a tool that reports
    // into the chat still does so when the other client calls it. Tools named
    // in `except` are left out.
    void shareToolsWith(Client* other, const QStringList& except = QStringList()) const;

    // Enable/disable a registered tool without losing its definition or
    // handler. Disabled tools are omitted from the schema list sent to the
    // model; if the model calls one anyway (stale/cached tool list) it
    // receives {"status":"error","message":"tool is disabled"}.
    void setToolEnabled(const QString& toolName, bool enabled);
    bool isToolEnabled(const QString& toolName) const;   // false if unknown
    QStringList toolNames() const;                       // all registered

    // ---- Which tools the model has, and who decides -----------------------
    // Three independent switches per tool. Each can be set for one tool or for
    // a whole Tool::group(); a tool's own setting wins over its group's. All
    // of them may be set before the tool is registered and survive
    // unregistering and re-registering it, so an application states its
    // policy once, in one place.
    //
    //   enabled            the model can use the tool (setToolEnabled above)
    //   user configurable  the user may switch it in a settings window
    //                      (ToolSettingsWidget, the Settings dialog's Tools
    //                      tab). false = the application alone decides: it is
    //                      shown locked and setToolEnabledByUser() refuses.
    //   deferred           not listed in every request, see setToolDeferred()
    void setToolGroupEnabled(const QString& group, bool enabled);
    void setToolUserConfigurable(const QString& toolName, bool configurable);
    void setToolGroupUserConfigurable(const QString& group, bool configurable);
    bool isToolUserConfigurable(const QString& toolName) const;   // true if unknown

    // What a settings window calls for the user's choice. Returns false, and
    // changes nothing, for a tool the user may not switch.
    bool setToolEnabledByUser(const QString& toolName, bool enabled);
    // The user's choices, for storing between sessions: {"<tool>": true|false}
    // for every registered tool the user may switch. applyUserToolSettings()
    // skips tools that are locked or not registered.
    QJsonObject userToolSettings() const;
    void applyUserToolSettings(const QJsonObject& settings);

    // Deferred tools cost no tokens until they are needed. Instead of their
    // definitions the model gets two small tools: find_tools(query) returns
    // name, description and parameters of the matching tools, and
    // call_tool(name, arguments) runs one - through the same checks (input
    // validation, consent, result cap) as a direct call. The tool list sent to
    // the provider stays the same for the whole conversation, so the prompt
    // cache keeps working. Worth it for large tool sets used now and then; a
    // tool the model needs in most conversations should stay listed.
    void setToolDeferred(const QString& toolName, bool deferred);
    void setToolGroupDeferred(const QString& group, bool deferred);
    bool isToolDeferred(const QString& toolName) const;
    QStringList enabledToolNames() const;                // currently advertised to the model

    // Tool definitions incl. UI metadata (title/group/statusText) — basis for
    // a generic tool-settings widget. Tools registered via the raw-schema
    // overload appear with name/description only.
    QList<Tool> registeredTools() const;

    // Check and repair tool input before invoking the handler
    // (Tool::prepareInput): "5" for an integer and "true" for a boolean are
    // converted, enum spelling is normalised, defaults are applied, then
    // required fields, types, ranges, array items and nested objects are
    // validated, and a parameter name the tool does not have is rejected with
    // the valid names. On violation the handler is NOT called; the model
    // receives the error result directly and can correct itself.
    // Default: false - only enum spelling is normalised.
    void setValidateToolInput(bool enabled);
    bool validateToolInput() const;

    // Cap on executed tool calls per user turn; 0 = unlimited (default).
    // When exceeded, handlers are no longer invoked and the model receives
    // {"status":"error","message":"tool call limit reached"};
    // toolCallLimitReached() is emitted once per turn.
    void setMaxToolCallsPerTurn(int maxCalls);
    int  maxToolCallsPerTurn() const;

    // Upper bound for the serialized size of one tool result. A result stays
    // in the conversation and is paid for on every later request, so one huge
    // result is expensive for the rest of the session. A larger result is
    // replaced by {"truncated":true,"original_chars":N,"content_head":"...",
    // "hint":...} - valid JSON that tells the model to ask for less. Images
    // attached with toolImage() do not count. Default 20000; 0 = unlimited.
    // Tool::setMaxResultChars() overrides it per tool.
    void setMaxToolResultChars(int chars);
    int  maxToolResultChars() const;

    // How much the model thinks before answering (see Effort). Lower is faster
    // and cheaper. Default: the model's own default.
    void setEffort(Effort effort);
    Effort effort() const;

    // Deliver the answer while it is generated: responseDelta() carries the
    // text piece by piece, responseReady() still fires once with all of it.
    // On by default. An endpoint that cannot stream is detected and used
    // without streaming automatically.
    void setStreamingEnabled(bool enabled);
    bool streamingEnabled() const;

    // Consent gate before every tool execution (accept/decline pattern).
    // No handler set = everything allowed (default). Pass nullptr to reset.
    void setToolConsentHandler(ToolConsentHandler handler);

    // Appends userMessage to history and sends the full conversation to the API.
    void sendPrompt(const QString& userMessage);
    // Same, with pictures the model should look at (scaled to at most 1568 px
    // on the long edge). On a provider that cannot take pictures
    // (supportsImageInput()) nothing is sent and errorOccurred() says so.
    void sendPrompt(const QString& userMessage, const QList<QImage>& images);
    bool supportsImageInput() const;
    // Pictures for the next sendPrompt(), whoever calls it. This is how the
    // chat widget hands over what the user pasted or dropped, without the
    // application changing its messageSent -> sendPrompt connection.
    void attachImages(const QList<QImage>& images);

    // Takes the last exchange - the last prompt and everything that followed
    // it - out of the conversation and returns the prompt's text. Empty, and
    // nothing changed, when there is none or a turn is still running. The
    // building block for "regenerate" (send it again) and "edit" (let the
    // user change it first).
    QString removeLastExchange();

    // Continue a conversation saved with exportConversation(). The provider
    // family must be the one it was saved with (message formats differ); the
    // system prompt and the model are not touched. Returns false, changing
    // nothing, for a foreign format or while a turn is running.
    bool importConversation(const QJsonObject& exported);

    // Stop sending once this conversation has cost that much: sendPrompt()
    // then reports through errorOccurred() and costLimitReached() instead.
    // clearConversation() starts counting again. 0 = no limit (default).
    void setCostLimitUsd(double usd);
    double costLimitUsd() const { return m_costLimitUsd; }

    // false: the model is asked to call one tool at a time, for applications
    // whose tools must not run side by side. Default true. Providers without
    // such a control ignore it.
    void setParallelToolCalls(bool enabled);
    bool parallelToolCalls() const { return m_parallelToolCalls; }

	void sendToolMessage(const QString& toolName, const QJsonObject& input);

    // Clears conversation history and resets session statistics. Preserves registered tools and settings.
    void clearConversation();

    void setContextPolicy(const ContextPolicy& policy);
    ContextPolicy contextPolicy() const;

    // Shrink the conversation now, by the policy's strategy. Summarize sends
    // one request and replaces the whole history with the summary it returns;
    // a prompt sent meanwhile waits for it. Returns false when there is
    // nothing to compact or a turn is still running. Result arrives through
    // compactionFinished() / compactionFailed().
    bool compactConversation();
    bool isCompacting() const { return m_compacting; }

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

    // Identifies the producer of recorded usage samples, i.e. UsageSample::app.
    // Defaults to QCoreApplication::applicationName(). Agent sets it to
    // "agent:<name>" so background spend is separable from chat spend in the
    // usage statistics, which already filter by this field.
    void setUsageAppTag(const QString& tag);
    QString usageAppTag() const;

    // Asynchronously fetch available models from the current provider.
    // Results arrive via modelsAvailable().
    void fetchAvailableModels();

    // Build a rich JSON export of the full conversation including raw messages
    // (with tool_use / tool_result blocks), usage statistics, and per-turn timeline.
    QJsonObject exportConversation() const;

signals:
    // Emitted after all tool calls in a turn are resolved; text is the final LLM reply.
    void responseReady(const QString& text);
    // A piece of answer text as it arrives (streaming). Includes text the
    // model writes before calling a tool.
    void responseDelta(const QString& text);
    // Emitted just before the registered handler is called.
    void toolInvoked(const QString& toolName, const QJsonObject& input);
    // Emitted after the handler returns with its result.
    void toolCompleted(const QString& toolName, const QJsonObject& result);
    // An asynchronous tool reporting what it is doing (ToolCompletion::progress).
    void toolProgress(const QString& toolName, const QString& statusText);
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
    // Emitted whenever the active model actually changes, including the
    // automatic correction applied when the provider does not offer the
    // configured one. Lets an app keep its own UI in step with reality.
    void modelChanged(const QString& model);
    // Emitted once per turn when setMaxToolCallsPerTurn() is exceeded.
    void toolCallLimitReached(int limit);
    void compactionStarted();
    // Context size before and after, in tokens (the "after" is an estimate
    // until the next turn reports the real figure).
    void compactionFinished(int tokensBefore, int tokensAfter);
    // The conversation is unchanged.
    void compactionFailed(const QString& message);
    // Emitted when a tool result exceeded the size cap and was shortened.
    void toolResultTruncated(const QString& toolName, int originalChars);
    // Emitted whenever anything feeding contextBreakdown() changes (system
    // prompt, tool set, or conversation history) — hook for live context UI.
    void contextChanged();
    // A tool was added or removed, or one of its switches changed.
    void toolsChanged();
    void costLimitReached(double spentUsd);

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
    void createProtocol(Provider provider, const QString& url, const QString& apiKey);
    void applyAccessTokenProvider();   // m_accessTokenProvider to the current protocol
    void connectProtocol();
    void syncToolsToProtocol();
    bool usesOpenAiToolFormat() const;   // Ollama and OpenAI-compatible servers
    void refreshContextInfo();  // debounced; called after every contextChanged()

    struct RegisteredTool {
        QJsonObject claudeSchema;   // toApiObject() format
        QJsonObject openAiSchema;   // toOpenAiApiObject() format
        // What input is checked against. Not the schema sent to the model:
        // in strict mode that one has lost ranges and defaults.
        QJsonObject validationSchema;
        ToolHandler handler;
        AsyncToolHandler asyncHandler;   // set instead of handler for asynchronous tools
        Tool        tool;           // definition incl. UI metadata (name/description only for raw registrations)
        // Copies of the policy below, refreshed by applyToolPolicies().
        bool        enabled = true;
        bool        userConfigurable = true;
        bool        deferred = false;
    };

    void applyToolPolicy(RegisteredTool& rt) const;
    void applyToolPolicies();
    void addDeferredToolAccess(QList<QJsonObject>& schemas,
                               QMap<QString, ToolCallHandler>& handlers, bool openAiFormat);
    QJsonObject findDeferredTools(const QJsonObject& input) const;
    void callDeferredTool(const QJsonObject& input, ToolResultCallback done);

    void recordSample(const UsageStats& stats);
    ToolCallHandler wrapHandler(const QString& toolName, const RegisteredTool& rt,
                                const ToolCallOptions& options = ToolCallOptions());
    QJsonObject capResult(const QString& toolName, int toolLimit, QJsonObject result);
    void finishCompaction(const QString& summary);
    void dropOldestTurns();
    void maybeAutoCompact();

    QMap<QString, RegisteredTool> m_tools;
    // Policy by tool name and by group; outlives the registration of a tool.
    QHash<QString, bool>          m_toolEnabledPolicy, m_groupEnabledPolicy;
    QHash<QString, bool>          m_toolConfigurablePolicy, m_groupConfigurablePolicy;
    QHash<QString, bool>          m_toolDeferredPolicy, m_groupDeferredPolicy;
    QList<QImage>                 m_attachedImages;
    QStringList                   m_allowedModels, m_blockedModels;
    double                        m_costLimitUsd = 0.0;
    bool                          m_parallelToolCalls = true;
    QJsonArray                    m_history;
    ProtocolBase*                 m_protocol;
    UsageStats                    m_lastStats;
    UsageHistory*                 m_usageHistory;
    QString                       m_currentModel;
    QString                       m_currentProvider;
    Provider                      m_provider = Provider::Claude;
    QString                       m_systemPrompt;
    QString                       m_usageAppTag;   // empty = fall back to the application name
    ToolConsentHandler            m_consentHandler;
    ContextWindowResolver         m_contextWindowResolver;
    AccessTokenProvider           m_accessTokenProvider;
    bool                          m_validateToolInput = false;
    int                           m_maxToolCallsPerTurn = 0;   // 0 = unlimited
    int                           m_maxToolResultChars = 20000; // 0 = unlimited
    Effort                        m_effort = Effort::Default;
    bool                          m_streamingEnabled = true;

    ContextPolicy                 m_contextPolicy;
    bool                          m_compacting = false;
    int                           m_turnsInFlight = 0;          // prompts sent, not yet answered
    int                           m_tokensBeforeCompaction = 0;
    QJsonArray                    m_historyBeforeCompaction;    // restored if the summary fails
    QStringList                   m_promptsDuringCompaction;    // sent while summarizing; re-attached after
    bool                          m_ignoreNextContextTokens = false;
    // History length right after the last compaction. Automatic compaction
    // waits for something new: a summary that is itself over the limit would
    // otherwise be summarized again and again.
    int                           m_messagesAfterCompaction = 0;
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
