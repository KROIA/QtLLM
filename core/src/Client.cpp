#include "Client.h"
#include "ClaudeProtocol.h"
#include "OllamaProtocol.h"
#include "ClaudeCodeProtocol.h"
#include "OpenAiProtocol.h"
#include "ClaudeContextInfoProvider.h"
#include "OllamaContextInfoProvider.h"
#include "ToolResult.h"
#include "UsageSample.h"
#include "Pricing.h"
#include <QJsonObject>
#include <QJsonDocument>
#include <QDateTime>
#include <QCoreApplication>
#include <QTimer>
#include <QRegularExpression>
#include <QThreadPool>
#include <algorithm>
#include <atomic>

namespace QtLLM {

namespace {

// What the model is told to write when the conversation is compacted. Naming
// what to keep matters: the summary is all that remains of the conversation.
const char* const kSummaryInstruction =
    "The conversation is about to be compacted: everything above will be replaced by "
    "your summary, and the work continues from the summary alone. Write that summary now.\n\n"
    "Include:\n"
    "- what the user is trying to achieve;\n"
    "- decisions that were made, and why;\n"
    "- the current state of the application and of any work in progress, as far as the "
    "conversation and the tool results show it;\n"
    "- facts learned from tool results that will be needed again (names, paths, ids, values, settings);\n"
    "- open tasks and the next step;\n"
    "- preferences the user stated.\n\n"
    "Leave out pleasantries and anything that is settled and no longer matters. "
    "Do not call any tools. Answer with the summary only.";

QJsonArray summaryAsHistory(const QString& summary)
{
    return QJsonArray{QJsonObject{
        {"role", "user"},
        {"content", QStringLiteral("<conversation-summary>\n%1\n</conversation-summary>\n\n"
                                   "(The earlier conversation was compacted into the summary above. "
                                   "Continue from it.)").arg(summary.trimmed())}}};
}

// A user message that opens a turn - as opposed to one that only carries tool
// results back (Claude: array content; Ollama: role "tool").
bool startsTurn(const QJsonValue& message)
{
    const QJsonObject obj = message.toObject();
    if (obj["role"].toString() != QLatin1String("user"))
        return false;
    if (obj["content"].isString())
        return true;
    // A prompt with pictures is an array as well.
    for (const QJsonValue& block : obj["content"].toArray()) {
        if (block.toObject()["type"].toString() == QLatin1String("tool_result"))
            return false;
    }
    return true;
}

const char* const kFindTools = "find_tools";
const char* const kCallTool  = "call_tool";

// Tool setting first, then its group's, then the default.
bool policyValue(const QHash<QString, bool>& byTool, const QHash<QString, bool>& byGroup,
                 const Tool& tool, bool fallback)
{
    const auto toolIt = byTool.constFind(tool.name());
    if (toolIt != byTool.constEnd())
        return toolIt.value();
    const auto groupIt = byGroup.constFind(tool.group());
    if (groupIt != byGroup.constEnd())
        return groupIt.value();
    return fallback;
}

// The {role, text} view Client::conversationHistory() offers, from a
// provider-format history.
QJsonArray simplifiedHistory(const QJsonArray& messages)
{
    QJsonArray simple;
    for (const QJsonValue& value : messages) {
        const QJsonObject message = value.toObject();
        const QString role = message["role"].toString();
        if (role != QLatin1String("user") && role != QLatin1String("assistant"))
            continue;
        QString text;
        if (message["content"].isString()) {
            text = message["content"].toString();
        } else {
            for (const QJsonValue& block : message["content"].toArray()) {
                if (block.toObject()["type"].toString() == QLatin1String("text"))
                    text += block.toObject()["text"].toString();
            }
        }
        if (!text.isEmpty())
            simple.append(QJsonObject{{"role", role}, {"content", text}});
    }
    return simple;
}

} // namespace

// ------------------------------------------------------------ ToolCompletion

struct ToolCompletion::State
{
    QPointer<Client>                        client;
    QString                                 toolName;
    std::function<void(const QJsonObject&)> deliver;   // runs on the GUI thread
    std::atomic<bool>                       finished{false};
};

void ToolCompletion::finish(const QJsonObject& result) const
{
    if (!d || d->finished.exchange(true))
        return;
    Client* client = d->client.data();
    if (!client)
        return;
    // Direct when already on the client's thread, queued from a worker.
    const QSharedPointer<State> state = d;
    QMetaObject::invokeMethod(client, [state, result]() {
        if (state->client)
            state->deliver(result);
    });
}

void ToolCompletion::progress(const QString& statusText) const
{
    if (!d || d->finished.load())
        return;
    Client* client = d->client.data();
    if (!client)
        return;
    const QSharedPointer<State> state = d;
    QMetaObject::invokeMethod(client, [state, statusText]() {
        if (state->client)
            emit state->client->toolProgress(state->toolName, statusText);
    });
}

bool ToolCompletion::isFinished() const
{
    return !d || d->finished.load();
}

// ------------------------------------------------------------ Client

Client::Client(const QString& apiKey, const QString& url, QObject* parent)
    : Client(Provider::Claude, url, apiKey, parent)
{
}

Client::Client(Provider provider, const QString& url, const QString& apiKey, QObject* parent)
    : QObject(parent)
    , m_protocol(nullptr)
    , m_usageHistory(new UsageHistory(this))
{
    m_currentModel = defaultModel(provider);
    createProtocol(provider, url, apiKey);
    m_protocol->setModel(m_currentModel);
    connectProtocol();
    fetchAvailableModels();
}

Client::~Client() = default;

QString Client::defaultModel(Provider provider)
{
    switch (provider) {
    case Provider::Ollama:     return QStringLiteral("llama3.2");
    case Provider::ClaudeCode: return QStringLiteral("sonnet");
    // Only a placeholder: a local server offers what is loaded there, and
    // validateCurrentModel() switches to that as soon as the list is in.
    case Provider::OpenAI:     return QStringLiteral("gpt-4o-mini");
    case Provider::Claude:     break;
    }
    return QStringLiteral("claude-sonnet-5-5");
}

QString Client::pickFallbackModel(Provider provider, const QStringList& offered)
{
    if (provider == Provider::Claude) {
        static const QStringList preferred = {
            QStringLiteral("claude-sonnet-5-5"), QStringLiteral("claude-haiku-5-5"),
            QStringLiteral("claude-sonnet-5"),   QStringLiteral("claude-sonnet-4-6")};
        for (const QString& candidate : preferred) {
            if (offered.contains(candidate))
                return candidate;
        }
    } else {
        // Ollama lists tagged names ("llama3.2:latest") for an untagged default.
        const QString wanted = defaultModel(provider);
        for (const QString& candidate : {wanted, wanted + QStringLiteral(":latest")}) {
            if (offered.contains(candidate))
                return candidate;
        }
    }
    return offered.value(0);
}

void Client::createProtocol(Provider provider, const QString& url, const QString& apiKey)
{
    m_provider = provider;
    switch (provider) {
    case Provider::Ollama:
        m_protocol = new OllamaProtocol(QUrl(url), this);
        m_contextInfoProvider = new OllamaContextInfoProvider(QUrl(url), this);
        m_currentProvider = "ollama";
        break;
    case Provider::ClaudeCode:
        m_protocol = new ClaudeCodeProtocol(this);
        // The CLI exposes no tokenizer; contextBreakdown() stays on estimates.
        m_contextInfoProvider = nullptr;
        m_currentProvider = "claude-code";
        break;
    case Provider::OpenAI:
        m_protocol = new OpenAiProtocol(url, apiKey, this);
        // No tokenizer endpoint in this API; contextBreakdown() stays on estimates.
        m_contextInfoProvider = nullptr;
        m_currentProvider = "openai";
        break;
    case Provider::Claude:
        m_protocol = new ClaudeProtocol(apiKey, QUrl(url), this);
        m_contextInfoProvider = new ClaudeContextInfoProvider(apiKey, QUrl(url), this);
        m_currentProvider = "claude";
        break;
    }
}

void Client::connectProtocol()
{
    // Keep cost estimates current without the app having to ask. Cheap: the
    // disk cache is reused and the network is only hit once a week.
    static bool pricingFetchStarted = false;
    if (!pricingFetchStarted) {
        pricingFetchStarted = true;
        PricingRegistry::instance().fetchOnlinePricing();
    }

    // responseReady goes through a slot so we can update m_history first
    connect(m_protocol, &ProtocolBase::responseReady,   this, &Client::onProtocolResponseReady);
    // The summary written during a compaction is not an answer to the user:
    // neither its text nor its (refused) tool calls are passed on.
    connect(m_protocol, &ProtocolBase::responseDelta, this, [this](const QString& text) {
        if (!m_compacting)
            emit responseDelta(text);
    });
    connect(m_protocol, &ProtocolBase::statsReady,      this, &Client::onProtocolStatsReady);
    connect(m_protocol, &ProtocolBase::toolInvoked, this, [this](const QString& name, const QJsonObject& input) {
        if (!m_compacting)
            emit toolInvoked(name, input);
    });
    connect(m_protocol, &ProtocolBase::toolCompleted, this, [this](const QString& name, const QJsonObject& result) {
        if (!m_compacting)
            emit toolCompleted(name, result);
    });
    connect(m_protocol, &ProtocolBase::errorOccurred,   this, &Client::onProtocolError);
    connect(m_protocol, &ProtocolBase::requestStarted,  this, &Client::requestStarted);
    connect(m_protocol, &ProtocolBase::requestFinished, this, &Client::requestFinished);
    // Only what the application allows is passed on - to the settings
    // dialog, to the automatic fallback, to an agent's model resolution.
    connect(m_protocol, &ProtocolBase::modelsFetched, this, [this](const QStringList& models) {
        QStringList allowed;
        for (const QString& name : models) {
            if (isModelAllowed(name))
                allowed.append(name);
        }
        emit modelsAvailable(allowed);
    });

    if (m_contextInfoProvider) {
        connect(m_contextInfoProvider, &ContextInfoProvider::contextWindowTokensReady, this, &Client::onContextWindowTokensReady);
        connect(m_contextInfoProvider, &ContextInfoProvider::tokenCountReady,          this, &Client::onExactTokenCountReady);
        connect(m_contextInfoProvider, &ContextInfoProvider::contextInfoErrorOccurred, this, &Client::onContextInfoError);
    }
    // UniqueConnection: setProvider() runs this again on the same Client.
    connect(this, &Client::contextChanged, this, &Client::refreshContextInfo, Qt::UniqueConnection);
    connect(this, &Client::modelsAvailable, this, &Client::validateCurrentModel, Qt::UniqueConnection);
}

void Client::validateCurrentModel(const QStringList& models)
{
    if (!models.isEmpty() && !models.contains(m_currentModel))
        setModel(pickFallbackModel(m_provider, models));
}

void Client::onProtocolResponseReady(const QString& text)
{
    QLLM_GENERAL_PROFILING_FUNCTION(QLLM_COLOR_STAGE_2)
#if LOGGER_LIBRARY_AVAILABLE == 1
    m_logger.logInfo("Response received (" + std::to_string(text.size()) + " chars)");
#endif
    if (m_compacting) {
        finishCompaction(text);
        return;
    }
    m_turnsInFlight = qMax(0, m_turnsInFlight - 1);
    QJsonObject msg;
    msg["role"]    = "assistant";
    msg["content"] = text;
    m_history.append(msg);
    emit responseReady(text);
    emit contextChanged();
}

void Client::onProtocolStatsReady(const QtLLM::UsageStats& stats)
{
    QLLM_GENERAL_PROFILING_FUNCTION(QLLM_COLOR_STAGE_2)
#if LOGGER_LIBRARY_AVAILABLE == 1
    m_logger.logDebug("Turn stats: in=" + std::to_string(stats.inputTokens)
                    + " out=" + std::to_string(stats.outputTokens)
                    + " tools=" + std::to_string(stats.toolCalls)
                    + " ms=" + std::to_string(stats.durationMs));
#endif
    m_lastStats = stats;
    recordSample(stats);

    // The provider's own count of the conversation it just processed - unless
    // that conversation was replaced by a summary a moment ago.
    if (m_ignoreNextContextTokens)
        m_ignoreNextContextTokens = false;
    else if (stats.contextTokens > 0)
        m_exactUsedTokens = stats.contextTokens;

    emit statsUpdated(stats);
    emit contextChanged();

    // Not from inside the protocol's own signal: it is still finishing the turn.
    QTimer::singleShot(0, this, [this]() { maybeAutoCompact(); });
}

void Client::onProtocolError(const QString& errorMessage)
{
#if LOGGER_LIBRARY_AVAILABLE == 1
    m_logger.logError(errorMessage.toStdString());
#endif
    if (m_compacting) {
        // The protocol rolled the summary request back; nothing changed.
        m_compacting = false;
        m_promptsDuringCompaction.clear();
        emit compactionFailed(errorMessage);
        return;
    }
    m_turnsInFlight = qMax(0, m_turnsInFlight - 1);

    // The turn that triggered this error never got an assistant reply - drop
    // the dangling user message so this (separate, simplified) history copy
    // stays consistent with the protocol's own rollback.
    if (!m_history.isEmpty() && m_history.last().toObject()["role"].toString() == "user")
        m_history.removeAt(m_history.size() - 1);

    emit errorOccurred(errorMessage);
    emit contextChanged();
}

bool Client::modelMatches(const QString& model, const QStringList& patterns)
{
    for (const QString& pattern : patterns) {
        // Not QRegularExpression::wildcardToRegularExpression(): that one is
        // for file paths, where * stops at a '/', and model names have those
        // ("hf.co/user/model:tag").
        QString expression = QRegularExpression::escape(pattern);
        expression.replace(QStringLiteral("\\*"), QStringLiteral(".*"));
        expression.replace(QStringLiteral("\\?"), QStringLiteral("."));
        const QRegularExpression regex(QRegularExpression::anchoredPattern(expression),
                                       QRegularExpression::CaseInsensitiveOption);
        if (regex.match(model).hasMatch())
            return true;
    }
    return false;
}

bool Client::isModelAllowed(const QString& model) const
{
    return (m_allowedModels.isEmpty() || modelMatches(model, m_allowedModels))
        && !modelMatches(model, m_blockedModels);
}

void Client::setModelFilter(const QStringList& allowed, const QStringList& blocked)
{
    if (m_allowedModels == allowed && m_blockedModels == blocked)
        return;
    m_allowedModels = allowed;
    m_blockedModels = blocked;
    // The model in use may have just become one that is not allowed; the
    // provider's list (filtered) picks a replacement.
    if (!isModelAllowed(m_currentModel))
        fetchAvailableModels();
}

void Client::setModel(const QString& model)
{
    if (!isModelAllowed(model)) {
        qWarning() << "QtLLM: model" << model << "is not allowed by the model filter - ignored";
        return;
    }
    const bool changed = (m_currentModel != model);
    m_currentModel = model;
    m_protocol->setModel(model);
    // Emitted for every change, including the silent self-correction in
    // validateCurrentModel() — without this an application has no way to learn
    // which model its requests are actually going out with.
    if (changed)
        emit modelChanged(m_currentModel);
}

QString Client::model() const                     { return m_currentModel; }
void Client::setMaxTokens(int maxTokens)           { m_protocol->setMaxTokens(maxTokens); }
void Client::setSystemPrompt(const QString& p)     { m_systemPrompt = p; m_protocol->setSystemPrompt(p); emit contextChanged(); }

void Client::setApiKey(const QString& apiKey)
{
    if (auto* claude = qobject_cast<ClaudeProtocol*>(m_protocol)) {
        claude->setApiKey(apiKey);
        if (auto* claudeInfo = qobject_cast<ClaudeContextInfoProvider*>(m_contextInfoProvider))
            claudeInfo->setApiKey(apiKey);
    } else if (auto* openAi = qobject_cast<OpenAiProtocol*>(m_protocol)) {
        openAi->setApiKey(apiKey);
    }
    // Ollama needs no API key - no-op there.
}

void Client::setEndpointUrl(const QString& url)
{
    const QUrl u(url);
    if (auto* claude = qobject_cast<ClaudeProtocol*>(m_protocol)) {
        claude->setUrl(u);
        if (auto* claudeInfo = qobject_cast<ClaudeContextInfoProvider*>(m_contextInfoProvider))
            claudeInfo->setMessagesUrl(u);
    } else if (auto* ollama = qobject_cast<OllamaProtocol*>(m_protocol)) {
        ollama->setUrl(u);
        if (auto* ollamaInfo = qobject_cast<OllamaContextInfoProvider*>(m_contextInfoProvider))
            ollamaInfo->setBaseUrl(u);
    } else if (auto* openAi = qobject_cast<OpenAiProtocol*>(m_protocol)) {
        openAi->setUrl(url);
    }
}

bool Client::usesOpenAiToolFormat() const
{
    return qobject_cast<OllamaProtocol*>(m_protocol) || qobject_cast<OpenAiProtocol*>(m_protocol);
}

void Client::setProvider(Provider provider, const QString& url, const QString& apiKey)
{
    if (m_protocol) {
        // Ends a turn in flight properly (requestFinished reaches the UI)
        // instead of leaving a spinner waiting for a reply that never comes.
        m_protocol->cancelTurn();
        m_protocol->disconnect(this);
        m_protocol->deleteLater();
    }
    if (m_contextInfoProvider) {
        m_contextInfoProvider->disconnect(this);
        m_contextInfoProvider->deleteLater();
    }

    createProtocol(provider, url, apiKey);
    connectProtocol();

    m_protocol->setModel(m_currentModel);
    m_protocol->setSystemPrompt(m_systemPrompt);
    m_protocol->setEffort(m_effort);
    m_protocol->setStreamingEnabled(m_streamingEnabled);
    m_protocol->setParallelToolCalls(m_parallelToolCalls);
    m_protocol->setContextLimit(m_contextPolicy.limitTokens);
    syncToolsToProtocol();
    m_compacting = false;
    m_turnsInFlight = 0;
    m_messagesAfterCompaction = 0;
    m_promptsDuringCompaction.clear();

    // A conversation with one provider's message format can't continue with
    // another - start fresh, same as clearConversation().
    m_history = QJsonArray();
    m_protocol->clearHistory();
    m_lastStats = UsageStats{};
    m_contextWindowCache.clear();
    m_exactUsedTokens = -1;
    m_lastCountedHash = 0;
    // Replies from the provider that was just torn down will never arrive.
    m_contextWindowFetchInFlight = false;
    m_tokenCountFetchInFlight = false;

    emit contextChanged();
    fetchAvailableModels();
}

void Client::registerTool(const Tool& tool, ToolHandler handler)
{
#if LOGGER_LIBRARY_AVAILABLE == 1
    m_logger.logDebug("Register tool: " + tool.name().toStdString());
#endif
    // An invalid name is rejected by the API on every request of the
    // conversation; say so where the mistake is made.
    if (!Tool::isValidName(tool.name()))
        qWarning() << "QtLLM: tool name" << tool.name()
                   << "is not valid (letters, digits, '_' and '-', at most 64 characters) - requests will fail";

    RegisteredTool rt;
    rt.claudeSchema = tool.toApiObject();
    rt.openAiSchema = tool.toOpenAiApiObject();
    rt.validationSchema = tool.parameterSchema();
    rt.handler      = std::move(handler);
    rt.tool         = tool;
    applyToolPolicy(rt);
    m_tools[tool.name()] = rt;
    syncToolsToProtocol();
    emit contextChanged();
    emit toolsChanged();
}

void Client::registerTool(const QString& name,
                           const QString& description,
                           const QJsonObject& parameterSchema,
                           ToolHandler handler)
{
    // Claude format
    QJsonObject claudeSchema;
    claudeSchema["name"]         = name;
    claudeSchema["description"]  = description;
    claudeSchema["input_schema"] = parameterSchema;

    // OpenAI format — wrap parameterSchema as "parameters"
    QJsonObject fnObj;
    fnObj["name"]        = name;
    fnObj["description"] = description;
    fnObj["parameters"]  = parameterSchema;
    QJsonObject openAiSchema;
    openAiSchema["type"]     = QStringLiteral("function");
    openAiSchema["function"] = fnObj;

    if (!Tool::isValidName(name))
        qWarning() << "QtLLM: tool name" << name
                   << "is not valid (letters, digits, '_' and '-', at most 64 characters) - requests will fail";

    RegisteredTool rt;
    rt.claudeSchema = claudeSchema;
    rt.openAiSchema = openAiSchema;
    rt.validationSchema = parameterSchema;
    rt.handler      = std::move(handler);
    rt.tool         = Tool().setName(name).setDescription(description);
    applyToolPolicy(rt);
    m_tools[name]   = rt;
    syncToolsToProtocol();
    emit contextChanged();
    emit toolsChanged();
}

void Client::registerAsyncTool(const Tool& tool, AsyncToolHandler handler)
{
    // Same registration as a synchronous tool; only how it is run differs.
    registerTool(tool, ToolHandler());
    m_tools[tool.name()].asyncHandler = std::move(handler);
    syncToolsToProtocol();
}

void Client::shareToolsWith(Client* other, const QStringList& except) const
{
    if (!other || other == this)
        return;
    for (auto it = m_tools.constBegin(); it != m_tools.constEnd(); ++it) {
        if (except.contains(it.key()))
            continue;
        other->m_tools[it.key()] = it.value();
        // As policy too, so that a later policy change on `other` keeps them.
        other->m_toolEnabledPolicy[it.key()]      = it.value().enabled;
        other->m_toolConfigurablePolicy[it.key()] = it.value().userConfigurable;
        other->m_toolDeferredPolicy[it.key()]     = it.value().deferred;
    }
    other->syncToolsToProtocol();
    emit other->contextChanged();
    emit other->toolsChanged();
}

void Client::registerBackgroundTool(const Tool& tool, ToolHandler handler)
{
    registerAsyncTool(tool, [handler](const QJsonObject& input, ToolCompletion done) {
        QThreadPool::globalInstance()->start([handler, input, done]() {
            QJsonObject result;
            try {
                result = handler(input);
            } catch (const std::exception& e) {
                result = toolError(QString("Internal error: %1").arg(e.what()));
            } catch (...) {
                result = toolError(QStringLiteral("Internal error: unknown exception in tool handler"));
            }
            done.finish(result);
        });
    });
}

// ------------------------------------------------------------ tool policy

void Client::applyToolPolicy(RegisteredTool& rt) const
{
    rt.enabled          = policyValue(m_toolEnabledPolicy, m_groupEnabledPolicy, rt.tool, true);
    rt.userConfigurable = policyValue(m_toolConfigurablePolicy, m_groupConfigurablePolicy, rt.tool, true);
    rt.deferred         = policyValue(m_toolDeferredPolicy, m_groupDeferredPolicy, rt.tool, false);
}

void Client::applyToolPolicies()
{
    for (auto it = m_tools.begin(); it != m_tools.end(); ++it)
        applyToolPolicy(it.value());
    syncToolsToProtocol();
    emit contextChanged();
    emit toolsChanged();
}

void Client::setToolGroupEnabled(const QString& group, bool enabled)
{
    m_groupEnabledPolicy[group] = enabled;
    applyToolPolicies();
}

void Client::setToolUserConfigurable(const QString& toolName, bool configurable)
{
    m_toolConfigurablePolicy[toolName] = configurable;
    applyToolPolicies();
}

void Client::setToolGroupUserConfigurable(const QString& group, bool configurable)
{
    m_groupConfigurablePolicy[group] = configurable;
    applyToolPolicies();
}

bool Client::isToolUserConfigurable(const QString& toolName) const
{
    const auto it = m_tools.constFind(toolName);
    return it == m_tools.cend() || it.value().userConfigurable;
}

bool Client::setToolEnabledByUser(const QString& toolName, bool enabled)
{
    const auto it = m_tools.constFind(toolName);
    if (it == m_tools.cend() || !it.value().userConfigurable)
        return false;
    setToolEnabled(toolName, enabled);
    return true;
}

QJsonObject Client::userToolSettings() const
{
    QJsonObject settings;
    for (auto it = m_tools.cbegin(); it != m_tools.cend(); ++it) {
        if (it.value().userConfigurable)
            settings[it.key()] = it.value().enabled;
    }
    return settings;
}

void Client::applyUserToolSettings(const QJsonObject& settings)
{
    for (auto it = settings.constBegin(); it != settings.constEnd(); ++it) {
        if (it.value().isBool())
            setToolEnabledByUser(it.key(), it.value().toBool());
    }
}

void Client::setToolDeferred(const QString& toolName, bool deferred)
{
    m_toolDeferredPolicy[toolName] = deferred;
    applyToolPolicies();
}

void Client::setToolGroupDeferred(const QString& group, bool deferred)
{
    m_groupDeferredPolicy[group] = deferred;
    applyToolPolicies();
}

bool Client::isToolDeferred(const QString& toolName) const
{
    const auto it = m_tools.constFind(toolName);
    return it != m_tools.cend() && it.value().deferred;
}

void Client::setToolEnabled(const QString& toolName, bool enabled)
{
    // Remembered even for a tool that is not registered (yet, or any more).
    m_toolEnabledPolicy[toolName] = enabled;
    auto it = m_tools.find(toolName);
    if (it == m_tools.end() || it.value().enabled == enabled)
        return;
    it.value().enabled = enabled;
#if LOGGER_LIBRARY_AVAILABLE == 1
    m_logger.logDebug(std::string(enabled ? "Enable" : "Disable")
                      + " tool: " + toolName.toStdString());
#endif
    // Mandatory: keep the advertised schema list and the handler map in sync,
    // otherwise the protocol reports "No handler registered for tool".
    syncToolsToProtocol();
    emit contextChanged();
    emit toolsChanged();
}

bool Client::isToolEnabled(const QString& toolName) const
{
    auto it = m_tools.constFind(toolName);
    return it != m_tools.cend() && it.value().enabled;
}

QStringList Client::toolNames() const
{
    return m_tools.keys();
}

QStringList Client::enabledToolNames() const
{
    QStringList names;
    for (auto it = m_tools.cbegin(); it != m_tools.cend(); ++it) {
        if (it.value().enabled)
            names.append(it.key());
    }
    return names;
}

QList<Tool> Client::registeredTools() const
{
    QList<Tool> tools;
    for (const RegisteredTool& rt : m_tools)
        tools.append(rt.tool);
    return tools;
}

void Client::setValidateToolInput(bool enabled)
{
    m_validateToolInput = enabled;
}

bool Client::validateToolInput() const
{
    return m_validateToolInput;
}

void Client::setMaxToolCallsPerTurn(int maxCalls)
{
    m_maxToolCallsPerTurn = maxCalls;
}

int Client::maxToolCallsPerTurn() const
{
    return m_maxToolCallsPerTurn;
}

void Client::setMaxToolResultChars(int chars)
{
    m_maxToolResultChars = chars;
}

int Client::maxToolResultChars() const
{
    return m_maxToolResultChars;
}

void Client::setStreamingEnabled(bool enabled)
{
    m_streamingEnabled = enabled;
    m_protocol->setStreamingEnabled(enabled);
}

bool Client::streamingEnabled() const
{
    return m_streamingEnabled;
}

void Client::setEffort(Effort effort)
{
    m_effort = effort;
    m_protocol->setEffort(effort);
}

Effort Client::effort() const
{
    return m_effort;
}

QJsonObject Client::capResult(const QString& toolName, int toolLimit, QJsonObject result)
{
    const int limit = toolLimit >= 0 ? toolLimit : m_maxToolResultChars;
    if (limit <= 0)
        return result;

    // Images are sent as image blocks, not as text; they are not what the cap
    // is about and must survive it.
    const QJsonValue images = result.take(kToolImagesKey);
    const QByteArray serialized = QJsonDocument(result).toJson(QJsonDocument::Compact);
    if (serialized.size() > limit) {
        emit toolResultTruncated(toolName, int(serialized.size()));
        // Cutting the JSON text itself would hand the model broken JSON.
        result = QJsonObject{
            {"status", result.value("status").toString(QStringLiteral("ok"))},
            {"truncated", true},
            {"original_chars", int(serialized.size())},
            {"content_head", QString::fromUtf8(serialized.left(limit))},
            {"hint", "Result too large and cut off. Ask for a smaller range, "
                     "fewer items or a narrower filter."}};
    }
    if (!images.isUndefined())
        result[kToolImagesKey] = images;
    return result;
}

void Client::setToolConsentHandler(ToolConsentHandler handler)
{
    m_consentHandler = std::move(handler);
}

void Client::unregisterTool(const QString& toolName)
{
#if LOGGER_LIBRARY_AVAILABLE == 1
    m_logger.logDebug("Unregister tool: " + toolName.toStdString());
#endif
    m_tools.remove(toolName);
    syncToolsToProtocol();
    emit contextChanged();
    emit toolsChanged();
}

void Client::syncToolsToProtocol()
{
    const bool isOllama = usesOpenAiToolFormat();   // name kept: the OpenAI tool format

    QList<QJsonObject> schemas;
    QMap<QString, ToolCallHandler> handlers;

    for (auto it = m_tools.cbegin(); it != m_tools.cend(); ++it) {
        if (!it.value().enabled) {
            // Not advertised, but still answerable: the model may call it from
            // a stale/cached tool list. "disabled" guides better than "unknown".
            handlers.insert(it.key(), [](const QJsonObject&, ToolResultCallback done) {
                done(toolError(QStringLiteral("tool is disabled")));
            });
            continue;
        }
        // A deferred tool is reached through call_tool; a model that calls it
        // by name anyway is served all the same.
        if (!it.value().deferred)
            schemas.append(isOllama ? it.value().openAiSchema : it.value().claudeSchema);
        handlers.insert(it.key(), wrapHandler(it.key(), it.value()));
    }
    addDeferredToolAccess(schemas, handlers, isOllama);
    m_protocol->setTools(schemas, handlers);
}

void Client::addDeferredToolAccess(QList<QJsonObject>& schemas,
                                   QMap<QString, ToolCallHandler>& handlers, bool openAiFormat)
{
    QMap<QString, int> groups;
    for (const RegisteredTool& rt : m_tools) {
        if (rt.enabled && rt.deferred)
            ++groups[rt.tool.group().isEmpty() ? QStringLiteral("Other") : rt.tool.group()];
    }
    if (groups.isEmpty())
        return;
    QStringList overview;
    for (auto it = groups.cbegin(); it != groups.cend(); ++it)
        overview.append(QStringLiteral("%1 (%2)").arg(it.key()).arg(it.value()));

    Tool find;
    find.setName(kFindTools)
        .setTitle("Find tools")
        .setDescription(QStringLiteral(
            "More tools exist than are listed in this request: %1. Search them by what you want "
            "to do; each match comes with its name, description and parameters. Then run it "
            "with call_tool. Without a query every such tool is listed by name. Look here "
            "before telling the user that something cannot be done.")
                            .arg(overview.join(QStringLiteral(", "))))
        .addParameter("query", "string",
                      "Words for the task or the tool, e.g. \"read table cells\".");
    Tool call;
    call.setName(kCallTool)
        .setTitle("Call tool")
        .setDescription("Run a tool that find_tools reported. 'arguments' must match the "
                        "parameters find_tools gave for it.")
        .addParameter("name", "string", "Tool name, exactly as find_tools reported it.", true)
        .addRawParameter("arguments",
                         QJsonObject{{"type", "object"},
                                     {"description", "The tool's parameters as an object."}});

    schemas.append(openAiFormat ? find.toOpenAiApiObject() : find.toApiObject());
    schemas.append(openAiFormat ? call.toOpenAiApiObject() : call.toApiObject());
    handlers.insert(kFindTools, [this](const QJsonObject& input, ToolResultCallback done) {
        done(findDeferredTools(input));
    });
    handlers.insert(kCallTool, [this](const QJsonObject& input, ToolResultCallback done) {
        callDeferredTool(input, std::move(done));
    });
}

QJsonObject Client::findDeferredTools(const QJsonObject& input) const
{
    const QStringList words = input.value("query").toString().toLower()
        .split(QRegularExpression(QStringLiteral("[^a-z0-9_]+")), Qt::SkipEmptyParts);

    struct Match { int score; const RegisteredTool* tool; };
    QList<Match> matches;
    QJsonArray catalog;
    for (const RegisteredTool& rt : m_tools) {
        if (!rt.enabled || !rt.deferred)
            continue;
        // One line per tool: the first sentence of its description.
        QString summary = rt.tool.description();
        const int sentenceEnd = summary.indexOf(QStringLiteral(". "));
        if (sentenceEnd > 0)
            summary.truncate(sentenceEnd + 1);
        catalog.append(QJsonObject{{"name", rt.tool.name()}, {"summary", summary.left(160)}});

        const QString name = rt.tool.name().toLower();
        const QString text = (rt.tool.title() + QLatin1Char(' ') + rt.tool.group() + QLatin1Char(' ')
                              + rt.tool.description()).toLower();
        int score = 0;
        for (const QString& word : words)
            score += name.contains(word) ? 3 : (text.contains(word) ? 1 : 0);
        if (score > 0)
            matches.append({score, &rt});
    }
    if (words.isEmpty())
        return toolOk({{"tools", catalog}});
    if (matches.isEmpty())
        return toolOk({{"matches", QJsonArray()},
                       {"note", "nothing matched; these are all the tools there are"},
                       {"tools", catalog}});

    std::stable_sort(matches.begin(), matches.end(),
                     [](const Match& a, const Match& b) { return a.score > b.score; });
    QJsonArray found;
    // ponytail: keyword scoring, best 6. Enough for a few hundred tools with
    // decent descriptions; an embedding search is the upgrade if it is not.
    for (int i = 0; i < matches.size() && i < 6; ++i) {
        const RegisteredTool& rt = *matches[i].tool;
        found.append(QJsonObject{{"name", rt.tool.name()},
                                 {"description", rt.tool.description()},
                                 {"parameters", rt.validationSchema}});
    }
    return toolOk({{"matches", found}, {"note", "run one with call_tool(name, arguments)"}});
}

void Client::callDeferredTool(const QJsonObject& input, ToolResultCallback done)
{
    const QString name = input.value("name").toString();
    const auto it = m_tools.constFind(name);
    if (it == m_tools.cend()) {
        done(toolError(QStringLiteral("no tool '%1'; search with find_tools").arg(name)));
        return;
    }
    if (!it.value().enabled) {
        done(toolError(QStringLiteral("tool is disabled")));
        return;
    }

    // Some models send the arguments as JSON text.
    QJsonObject args = input.value("arguments").toObject();
    if (input.value("arguments").isString())
        args = QJsonDocument::fromJson(input.value("arguments").toString().toUtf8()).object();

    // Shown and logged as the tool it is, not as "call_tool".
    callTool(name, args, std::move(done));
}

void Client::callTool(const QString& toolName, const QJsonObject& arguments,
                      ToolResultCallback done, const ToolCallOptions& options)
{
    const auto it = m_tools.constFind(toolName);
    if (it == m_tools.cend()) {
        done(toolError(QStringLiteral("no tool '%1'").arg(toolName)));
        return;
    }
    if (!it.value().enabled) {
        done(toolError(QStringLiteral("tool is disabled")));
        return;
    }

    // The caller may know this schema only as text, so check against it
    // whether or not validation is switched on - and hand it back on a
    // mistake, which saves a round of looking it up.
    QJsonObject args = arguments;
    QJsonObject error;
    if (!Tool::prepareInput(it.value().validationSchema, args, &error)) {
        error["parameters"] = it.value().validationSchema;
        done(error);
        return;
    }

    const bool announce = options.announce;
    if (announce)
        emit toolInvoked(toolName, args);
    wrapHandler(toolName, it.value(), options)(args, [this, toolName, done, announce](const QJsonObject& result) {
        if (announce)
            emit toolCompleted(toolName, result);
        done(result);
    });
}

// Single choke point in front of every tool execution:
// call cap -> input validation -> consent -> registered handler.
// All checks are opt-in; with defaults this forwards to the handler unchanged.
ToolCallHandler Client::wrapHandler(const QString& toolName, const RegisteredTool& rt,
                                    const ToolCallOptions& options)
{
    ToolHandler inner = rt.handler;
    AsyncToolHandler asyncInner = rt.asyncHandler;
    const int timeoutMs = rt.tool.timeoutMs() > 0 ? rt.tool.timeoutMs() : 300000;
    QJsonObject parameterSchema = rt.validationSchema;

    // Not capping = unlimited for this call, whatever client and tool say.
    const int toolLimit = options.capResult ? rt.tool.maxResultChars() : 0;
    const bool countInTurn = options.countInTurn;
    const bool askConsent = options.askConsent;

    return [this, toolName, inner, asyncInner, timeoutMs, parameterSchema, toolLimit, countInTurn, askConsent]
           (const QJsonObject& input, ToolResultCallback done) {
        // The summary request carries the tool list (so the cached prefix is
        // reused), but a summary must not have side effects on the application.
        if (m_compacting) {
            done(toolError(QStringLiteral(
                "Tools are not available while the conversation is being summarized. Write the summary.")));
            return;
        }

        if (countInTurn && m_maxToolCallsPerTurn > 0 && m_toolCallsThisTurn >= m_maxToolCallsPerTurn) {
            if (!m_limitSignalEmitted) {
                m_limitSignalEmitted = true;
                emit toolCallLimitReached(m_maxToolCallsPerTurn);
            }
            done(toolError(QStringLiteral("tool call limit reached")));
            return;
        }

        // Models often return enum values in a different case, numbers as
        // text, or invent a parameter. The handler must receive what it
        // expects - or not be called at all.
        QJsonObject args = input;
        if (m_validateToolInput) {
            QJsonObject error;
            if (!Tool::prepareInput(parameterSchema, args, &error)) {
                done(error);
                return;
            }
        } else {
            args = Tool::normalizeEnumCase(parameterSchema, input);
        }

        if (askConsent && m_consentHandler && !m_consentHandler(toolName, args)) {
#if LOGGER_LIBRARY_AVAILABLE == 1
            m_logger.logInfo("Tool declined by consent handler: " + toolName.toStdString());
#endif
            done(toolError(QStringLiteral("user declined")));
            return;
        }

        if (countInTurn)
            ++m_toolCallsThisTurn;

        if (!asyncInner) {
            done(capResult(toolName, toolLimit, inner(args)));
            return;
        }

        // Asynchronous: the handler returns now and reports through the
        // completion - or the timeout reports for it.
        ToolCompletion completion;
        completion.d = QSharedPointer<ToolCompletion::State>::create();
        completion.d->client = this;
        completion.d->toolName = toolName;
        completion.d->deliver = [this, toolName, toolLimit, done](const QJsonObject& result) {
            done(capResult(toolName, toolLimit, result));
        };
        QTimer::singleShot(timeoutMs, this, [completion, timeoutMs]() {
            completion.finish(toolError(QStringLiteral(
                "The tool did not answer within %1 seconds.").arg(timeoutMs / 1000)));
        });
        asyncInner(args, completion);
    };
}

void Client::sendPrompt(const QString& userMessage)
{
    sendPrompt(userMessage, QList<QImage>());
}

bool Client::supportsImageInput() const
{
    return m_protocol->supportsImageInput();
}

void Client::attachImages(const QList<QImage>& images)
{
    m_attachedImages += images;
}

void Client::setCostLimitUsd(double usd)
{
    m_costLimitUsd = usd;
}

void Client::setParallelToolCalls(bool enabled)
{
    m_parallelToolCalls = enabled;
    m_protocol->setParallelToolCalls(enabled);
}

void Client::sendPrompt(const QString& userMessage, const QList<QImage>& promptImages)
{
    QLLM_GENERAL_PROFILING_FUNCTION(QLLM_COLOR_STAGE_1)
    const QList<QImage> pictures = m_attachedImages + promptImages;
    m_attachedImages.clear();

    if (m_costLimitUsd > 0.0 && m_lastStats.sessionCostUsd >= m_costLimitUsd) {
        emit costLimitReached(m_lastStats.sessionCostUsd);
        emit errorOccurred(QStringLiteral("The cost limit of $%1 for this conversation is reached "
                                          "($%2 spent). Start a new conversation to continue.")
                               .arg(m_costLimitUsd, 0, 'f', 2)
                               .arg(m_lastStats.sessionCostUsd, 0, 'f', 2));
        return;
    }
    if (!isModelAllowed(m_currentModel)) {
        emit errorOccurred(QStringLiteral("The model \"%1\" may not be used in this application. "
                                          "Choose another one in the settings.").arg(m_currentModel));
        return;
    }
    if (!pictures.isEmpty() && !m_protocol->supportsImageInput()) {
        emit errorOccurred(QStringLiteral("This provider cannot take pictures. The message was not sent."));
        return;
    }
    QJsonObject encoded;
    for (const QImage& picture : pictures)
        toolAddImage(encoded, picture);
    const QJsonArray images = toolTakeImages(encoded);
#if LOGGER_LIBRARY_AVAILABLE == 1
    m_logger.logInfo("sendPrompt (" + std::to_string(userMessage.size()) + " chars)");
#endif
    QJsonObject msg;
    msg["role"]    = "user";
    msg["content"] = userMessage;
    m_history.append(msg);
    m_toolCallsThisTurn = 0;
    m_limitSignalEmitted = false;
    ++m_turnsInFlight;
    if (m_compacting)
        m_promptsDuringCompaction.append(userMessage);   // re-attached once the summary is in
    // ponytail: pictures of a prompt sent while a summary is being written
    // are not re-attached with it.
    if (images.isEmpty())
        m_protocol->beginTurn(userMessage);
    else
        m_protocol->beginTurn(userMessage, images);
    emit contextChanged();
}

QString Client::removeLastExchange()
{
    if (m_turnsInFlight > 0 || m_compacting)
        return QString();
    QJsonArray messages = m_protocol->conversationMessages();
    int start = -1;
    for (int i = messages.size() - 1; i >= 0 && start < 0; --i) {
        if (startsTurn(messages[i]))
            start = i;
    }
    if (start < 0)
        return QString();

    const QJsonArray last = simplifiedHistory(QJsonArray{messages[start]});
    const QString prompt = last.isEmpty() ? QString() : last[0].toObject()["content"].toString();
    while (messages.size() > start)
        messages.removeLast();
    // Cutting the end off leaves everything before it as it was sent, so the
    // provider's cache and any thinking blocks stay valid.
    m_protocol->replaceHistory(messages);
    m_history = simplifiedHistory(messages);
    emit contextChanged();
    return prompt;
}

bool Client::importConversation(const QJsonObject& exported)
{
    if (m_turnsInFlight > 0 || m_compacting)
        return false;
    if (!exported["messages"].isArray() || exported["provider"].toString() != m_currentProvider)
        return false;

    QJsonArray messages;
    for (const QJsonValue& value : exported["messages"].toArray()) {
        QJsonObject message = value.toObject();
        // Thinking blocks are bound to the conversation that produced them;
        // sent again from a new one, the API rejects the request.
        if (message["content"].isArray()) {
            QJsonArray kept;
            for (const QJsonValue& block : message["content"].toArray()) {
                const QString type = block.toObject()["type"].toString();
                if (type != QLatin1String("thinking") && type != QLatin1String("redacted_thinking"))
                    kept.append(block);
            }
            if (kept.isEmpty())
                continue;
            message["content"] = kept;
        }
        messages.append(message);
    }
    m_protocol->replaceHistory(messages);
    m_history = simplifiedHistory(messages);
    m_messagesAfterCompaction = 0;
    emit contextChanged();
    return true;
}

void Client::sendToolMessage(const QString& toolName, const QJsonObject& input)
{
    QLLM_GENERAL_PROFILING_FUNCTION(QLLM_COLOR_STAGE_1)
    if (!m_tools.contains(toolName)) {
#if LOGGER_LIBRARY_AVAILABLE == 1
        m_logger.logWarning("sendToolMessage: unregistered tool \"" + toolName.toStdString() + "\"");
#endif
        qWarning() << "Attempted to send message for unregistered tool:" << toolName;
        return;
    }
#if LOGGER_LIBRARY_AVAILABLE == 1
    m_logger.logInfo("Async tool callback: " + toolName.toStdString());
#endif
    // Async tool completion arrives as a new user turn since its tool_use/tool_result
    // cycle already closed synchronously. Format it as a readable notification.
    QString payload = QString::fromUtf8(
        QJsonDocument(input).toJson(QJsonDocument::Compact));
    QString notification = QString("[%1 completed] %2").arg(toolName, payload);
    m_toolCallsThisTurn = 0;
    m_limitSignalEmitted = false;
    ++m_turnsInFlight;
    m_protocol->beginTurn(notification);
}

void Client::cancelRequest()
{
    if (!m_protocol->cancelTurn())
        return;
    m_turnsInFlight = 0;
    if (m_compacting) {
        m_compacting = false;
        m_promptsDuringCompaction.clear();
        emit compactionFailed(QStringLiteral("Compaction was cancelled"));
    }
    // Same bookkeeping as a failed turn: the prompt never got an answer.
    if (!m_history.isEmpty() && m_history.last().toObject()["role"].toString() == "user")
        m_history.removeAt(m_history.size() - 1);
    emit contextChanged();
}

void Client::clearConversation()
{
    // A turn still in flight would append its answer (or tool results) to the
    // history that is being emptied here.
    cancelRequest();
    m_turnsInFlight = 0;
    m_messagesAfterCompaction = 0;
    m_history = QJsonArray();
    m_protocol->clearHistory();
    m_protocol->clearStats();
    m_lastStats = UsageStats{};
    m_toolCallsThisTurn = 0;
    m_limitSignalEmitted = false;
    emit contextChanged();
}

QJsonArray Client::conversationHistory() const
{
    return m_history;
}

UsageStats Client::usageStats() const
{
    return m_lastStats;
}

// Last-resort fallback when neither an explicit resolver nor a live
// ContextInfoProvider result (m_contextWindowCache) is available yet -
// e.g. the very first paint before the background fetch lands.
static int builtinContextWindowTokensFor(const QString& provider, const QString&)
{
    if (provider == QStringLiteral("claude") || provider == QStringLiteral("claude-code"))
        return 200000; // every current Claude model publishes 200k (1M in beta, not assumed here)
    return 8192; // typical Ollama default num_ctx when unknown
}

void Client::setContextPolicy(const ContextPolicy& policy)
{
    m_contextPolicy = policy;
    m_protocol->setContextLimit(policy.limitTokens);
    emit contextChanged();
    maybeAutoCompact();
}

ContextPolicy Client::contextPolicy() const
{
    return m_contextPolicy;
}

void Client::maybeAutoCompact()
{
    if (!m_contextPolicy.autoCompact || m_compacting || m_turnsInFlight > 0)
        return;
    if (m_protocol->conversationMessages().size() <= m_messagesAfterCompaction)
        return;   // nothing new since the last compaction
    const ContextBreakdown b = contextBreakdown();
    if (b.bestUsedTokens() >= b.contextWindowTokens * m_contextPolicy.triggerRatio)
        compactConversation();
}

bool Client::compactConversation()
{
    if (m_compacting || m_turnsInFlight > 0)
        return false;
    const QJsonArray messages = m_protocol->conversationMessages();
    if (messages.isEmpty())
        return false;

    m_tokensBeforeCompaction = contextBreakdown().bestUsedTokens();

    // The subscription provider keeps its conversation inside the CLI; there
    // is nothing here to drop turns from.
    if (m_contextPolicy.strategy == ContextPolicy::Strategy::DropOldest
        && m_provider != Provider::ClaudeCode) {
        dropOldestTurns();
        return true;
    }

    m_compacting = true;
    m_historyBeforeCompaction = messages;
    emit compactionStarted();
    m_protocol->beginTurn(QString::fromLatin1(kSummaryInstruction));
    return true;
}

void Client::finishCompaction(const QString& summary)
{
    m_compacting = false;

    if (summary.trimmed().isEmpty()) {
        m_protocol->replaceHistory(m_historyBeforeCompaction);
        m_historyBeforeCompaction = QJsonArray();
        m_promptsDuringCompaction.clear();
        emit compactionFailed(QStringLiteral("The model returned an empty summary"));
        return;
    }

    // Simple compaction: the summary is the whole conversation from here on.
    // Nothing else is carried over - retained turns would bring thinking
    // blocks that are bound to the history that no longer exists.
    const QJsonArray compacted = summaryAsHistory(summary);
    m_protocol->replaceHistory(compacted);
    m_history = compacted;
    for (const QString& prompt : m_promptsDuringCompaction)
        m_history.append(QJsonObject{{"role", "user"}, {"content", prompt}});
    m_promptsDuringCompaction.clear();
    m_historyBeforeCompaction = QJsonArray();
    m_messagesAfterCompaction = compacted.size();

    // The summary request's own token report describes the old conversation.
    m_ignoreNextContextTokens = true;
    m_exactUsedTokens = -1;
    m_lastCountedHash = 0;

    emit compactionFinished(m_tokensBeforeCompaction, contextBreakdown().bestUsedTokens());
    emit contextChanged();
}

void Client::dropOldestTurns()
{
    emit compactionStarted();

    QJsonArray messages = m_protocol->conversationMessages();
    auto estimate = [](const QJsonArray& array) {
        return int(QJsonDocument(array).toJson(QJsonDocument::Compact).size() / 4);
    };
    // Well below the limit, so the next few turns do not trigger again.
    const int target = contextBreakdown().contextWindowTokens / 2;

    while (estimate(messages) > target) {
        // Remove one whole turn: up to the next message that opens a turn.
        // A tool call is never separated from its result this way.
        int next = -1;
        for (int i = 1; i < messages.size(); ++i) {
            if (startsTurn(messages[i])) {
                next = i;
                break;
            }
        }
        if (next < 0)
            break;   // one turn left; nothing more to drop without cutting into it
        for (int i = 0; i < next; ++i)
            messages.removeFirst();
    }

    m_protocol->replaceHistory(messages);
    m_history = simplifiedHistory(messages);
    m_messagesAfterCompaction = messages.size();
    m_exactUsedTokens = -1;
    m_lastCountedHash = 0;

    emit compactionFinished(m_tokensBeforeCompaction, contextBreakdown().bestUsedTokens());
    emit contextChanged();
}

void Client::setContextWindowResolver(ContextWindowResolver resolver)
{
    m_contextWindowResolver = std::move(resolver);
}

ContextBreakdown Client::contextBreakdown() const
{
    ContextBreakdown b;
    int resolved = m_contextWindowResolver ? m_contextWindowResolver(m_currentProvider, m_currentModel) : 0;
    if (resolved > 0)
        b.contextWindowTokens = resolved;
    else if (m_contextWindowCache.contains(m_currentModel))
        b.contextWindowTokens = m_contextWindowCache.value(m_currentModel);
    else
        b.contextWindowTokens = builtinContextWindowTokensFor(m_currentProvider, m_currentModel);

    // The user's limit is the window to work with, never more than the model has.
    if (m_contextPolicy.limitTokens > 0)
        b.contextWindowTokens = qMin(b.contextWindowTokens, m_contextPolicy.limitTokens);

    b.exactUsedTokens = m_exactUsedTokens;

    auto estimateTokens = [](qsizetype chars) { return static_cast<int>(chars / 4); };

    b.systemPromptTokens = estimateTokens(m_systemPrompt.toUtf8().size());

    qsizetype toolsChars = 0;
    const bool isOllama = usesOpenAiToolFormat();   // name kept: the OpenAI tool format
    for (auto it = m_tools.cbegin(); it != m_tools.cend(); ++it) {
        if (!it.value().enabled)
            continue;
        const QJsonObject& schema = isOllama ? it.value().openAiSchema : it.value().claudeSchema;
        toolsChars += QJsonDocument(schema).toJson(QJsonDocument::Compact).size();
    }
    b.toolsTokens = estimateTokens(toolsChars);

    qsizetype messagesChars = QJsonDocument(m_protocol->conversationMessages())
                                  .toJson(QJsonDocument::Compact).size();
    b.messagesTokens = estimateTokens(messagesChars);

    return b;
}

// Debounced: coalesces a burst of contextChanged() emissions (e.g. several
// registerTool() calls during app startup) into a single background refresh.
void Client::refreshContextInfo()
{
    if (m_contextInfoRefreshScheduled)
        return;
    m_contextInfoRefreshScheduled = true;

    QTimer::singleShot(0, this, [this]() {
        m_contextInfoRefreshScheduled = false;
        if (!m_contextInfoProvider)
            return;

        if (!m_contextWindowFetchInFlight && !m_contextWindowCache.contains(m_currentModel)) {
            m_contextWindowFetchInFlight = true;
            m_contextInfoProvider->fetchContextWindowTokens(m_currentModel);
        }

        const bool isOllama = usesOpenAiToolFormat();
        QJsonArray toolSchemas;
        for (auto it = m_tools.cbegin(); it != m_tools.cend(); ++it) {
            if (it.value().enabled)
                toolSchemas.append(isOllama ? it.value().openAiSchema : it.value().claudeSchema);
        }
        QJsonArray messages = m_protocol->conversationMessages();

        QByteArray sigBytes = m_systemPrompt.toUtf8()
            + QJsonDocument(toolSchemas).toJson(QJsonDocument::Compact)
            + QJsonDocument(messages).toJson(QJsonDocument::Compact);
        uint sig = qHash(sigBytes);
        if (m_tokenCountFetchInFlight || sig == m_lastCountedHash)
            return;

        m_tokenCountFetchInFlight = true;
        m_lastCountedHash = sig;
        m_contextInfoProvider->countTokens(m_currentModel, m_systemPrompt, toolSchemas, messages);
    });
}

void Client::onContextWindowTokensReady(int tokens)
{
    m_contextWindowFetchInFlight = false;
    if (tokens > 0)
        m_contextWindowCache[m_currentModel] = tokens;
    emit contextChanged();
}

void Client::onExactTokenCountReady(int tokens)
{
    m_tokenCountFetchInFlight = false;
    m_exactUsedTokens = tokens;
    emit contextChanged();
}

void Client::onContextInfoError(const QString&)
{
    // Best-effort feature: silently fall back to the chars/4 estimate and
    // the static context-window table already used before either fetch
    // completes. Just clear the in-flight flags so the next contextChanged()
    // retries instead of getting stuck forever.
    m_contextWindowFetchInFlight = false;
    m_tokenCountFetchInFlight = false;
}

UsageHistory* Client::usageHistory()
{
    return m_usageHistory;
}

void Client::setUsageAppTag(const QString& tag)
{
    m_usageAppTag = tag;
}

QString Client::usageAppTag() const
{
    if (!m_usageAppTag.isEmpty())
        return m_usageAppTag;
    const QString appName = QCoreApplication::applicationName();
    return appName.isEmpty() ? QStringLiteral("unknown") : appName;
}

void Client::fetchAvailableModels()
{
    m_protocol->fetchModels();
}

void Client::recordSample(const UsageStats& stats)
{
    UsageSample sample;
    sample.timestampMsEpoch         = QDateTime::currentMSecsSinceEpoch();
    sample.model                    = m_currentModel;
    sample.provider                 = m_currentProvider;
    sample.app                      = usageAppTag();
    sample.inputTokens              = stats.inputTokens;
    sample.outputTokens             = stats.outputTokens;
    sample.cacheReadInputTokens     = stats.cacheReadInputTokens;
    sample.cacheCreationInputTokens = stats.cacheCreationInputTokens;
    sample.toolCalls                = stats.toolCalls;
    sample.durationMs               = stats.durationMs;

    // Est. per-turn cost across all token categories, resolved through the
    // PricingRegistry (custom resolver / injected / fetched / built-in).
    // Ollama stays 0.
    sample.costUsd = 0.0;
    if (m_currentProvider == "claude") {
        sample.costUsd = PricingRegistry::instance().estimateCostUsd(
            m_currentModel, stats.inputTokens, stats.outputTokens,
            stats.cacheReadInputTokens, stats.cacheCreationInputTokens);
    }

    m_usageHistory->append(sample);
}

QJsonObject Client::exportConversation() const
{
    QJsonObject root;
    root["schemaVersion"] = 1;

    QDateTime now = QDateTime::currentDateTime();
    root["exportedAt"]        = now.toString(Qt::ISODateWithMs);
    root["exportedAtEpochMs"] = QDateTime::currentMSecsSinceEpoch();
    root["provider"]          = m_currentProvider;
    root["model"]             = m_currentModel;
    root["systemPrompt"]      = m_systemPrompt;

    // Full raw message history from the protocol (includes tool_use / tool_result blocks)
    root["messages"] = m_protocol->conversationMessages();

    // Session-level usage statistics
    QJsonObject usage;
    usage["sessionInputTokens"]              = m_lastStats.sessionInputTokens;
    usage["sessionOutputTokens"]             = m_lastStats.sessionOutputTokens;
    usage["sessionCacheReadInputTokens"]     = m_lastStats.sessionCacheReadInputTokens;
    usage["sessionCacheCreationInputTokens"] = m_lastStats.sessionCacheCreationInputTokens;
    usage["sessionToolCalls"]                = m_lastStats.sessionToolCalls;
    usage["sessionTurnCount"]                = m_lastStats.sessionTurnCount;
    usage["sessionCostUsd"]                  = m_lastStats.sessionCostUsd;
    root["usage"] = usage;

    // Per-turn timeline for this process session
    QJsonArray turns;
    qint64 sessionStart = m_usageHistory->sessionStartMs();
    for (const UsageSample& s : m_usageHistory->samples()) {
        if (s.timestampMsEpoch < sessionStart)
            continue;
        // The history is shared process-wide; background agents and other
        // clients record into it too. This export is about this conversation.
        if (s.app != usageAppTag())
            continue;
        QJsonObject turn;
        turn["timestamp"]                = QDateTime::fromMSecsSinceEpoch(s.timestampMsEpoch).toString(Qt::ISODateWithMs);
        turn["epochMs"]                  = s.timestampMsEpoch;
        turn["model"]                    = s.model;
        turn["provider"]                 = s.provider;
        turn["app"]                      = s.app;
        turn["inputTokens"]              = s.inputTokens;
        turn["outputTokens"]             = s.outputTokens;
        turn["cacheReadInputTokens"]     = s.cacheReadInputTokens;
        turn["cacheCreationInputTokens"] = s.cacheCreationInputTokens;
        turn["toolCalls"]                = s.toolCalls;
        turn["durationMs"]               = s.durationMs;
        turn["costUsd"]                  = s.costUsd;
        turns.append(turn);
    }
    root["turns"] = turns;

    return root;
}

} // namespace QtLLM
