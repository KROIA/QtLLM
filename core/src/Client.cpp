#include "Client.h"
#include "ClaudeProtocol.h"
#include "OllamaProtocol.h"
#include "ToolResult.h"
#include "UsageSample.h"
#include <QJsonObject>
#include <QJsonDocument>
#include <QDateTime>
#include <QCoreApplication>

namespace QtLLM {

Client::Client(const QString& apiKey, const QString& url, QObject* parent)
    : QObject(parent)
    , m_protocol(new ClaudeProtocol(apiKey, QUrl(url), this))
    , m_usageHistory(new UsageHistory(this))
    , m_currentModel("claude-opus-4-5")
    , m_currentProvider("claude")
{
    connectProtocol();
}

Client::Client(Provider provider, const QString& url, const QString& apiKey, QObject* parent)
    : QObject(parent)
    , m_protocol(nullptr)
    , m_usageHistory(new UsageHistory(this))
{
    switch (provider) {
    case Provider::Ollama:
        m_protocol = new OllamaProtocol(QUrl(url), this);
        m_currentProvider = "ollama";
        m_currentModel    = "llama3.2";
        break;
    case Provider::Claude:
    default:
        m_protocol = new ClaudeProtocol(apiKey, QUrl(url), this);
        m_currentProvider = "claude";
        m_currentModel    = "claude-opus-4-5";
        break;
    }
    connectProtocol();
}

Client::~Client() = default;

void Client::connectProtocol()
{
    // responseReady goes through a slot so we can update m_history first
    connect(m_protocol, &ProtocolBase::responseReady,   this, &Client::onProtocolResponseReady);
    connect(m_protocol, &ProtocolBase::statsReady,      this, &Client::onProtocolStatsReady);
    connect(m_protocol, &ProtocolBase::toolInvoked,     this, &Client::toolInvoked);
    connect(m_protocol, &ProtocolBase::toolCompleted,   this, &Client::toolCompleted);
    connect(m_protocol, &ProtocolBase::errorOccurred,   this, &Client::onProtocolError);
    connect(m_protocol, &ProtocolBase::requestStarted,  this, &Client::requestStarted);
    connect(m_protocol, &ProtocolBase::requestFinished, this, &Client::requestFinished);
    connect(m_protocol, &ProtocolBase::modelsFetched,   this, &Client::modelsAvailable);
}

void Client::onProtocolResponseReady(const QString& text)
{
    QLLM_GENERAL_PROFILING_FUNCTION(QLLM_COLOR_STAGE_2)
#if LOGGER_LIBRARY_AVAILABLE == 1
    m_logger.logInfo("Response received (" + std::to_string(text.size()) + " chars)");
#endif
    QJsonObject msg;
    msg["role"]    = "assistant";
    msg["content"] = text;
    m_history.append(msg);
    emit responseReady(text);
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
    emit statsUpdated(stats);
}

void Client::onProtocolError(const QString& errorMessage)
{
#if LOGGER_LIBRARY_AVAILABLE == 1
    m_logger.logError(errorMessage.toStdString());
#endif
    emit errorOccurred(errorMessage);
}

void Client::setModel(const QString& model)       { m_currentModel = model; m_protocol->setModel(model); }
void Client::setMaxTokens(int maxTokens)           { m_protocol->setMaxTokens(maxTokens); }
void Client::setSystemPrompt(const QString& p)     { m_systemPrompt = p; m_protocol->setSystemPrompt(p); }

void Client::registerTool(const Tool& tool, ToolHandler handler)
{
#if LOGGER_LIBRARY_AVAILABLE == 1
    m_logger.logDebug("Register tool: " + tool.name().toStdString());
#endif
    RegisteredTool rt;
    rt.claudeSchema = tool.toApiObject();
    rt.openAiSchema = tool.toOpenAiApiObject();
    rt.handler      = std::move(handler);
    rt.tool         = tool;
    m_tools[tool.name()] = rt;
    syncToolsToProtocol();
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

    RegisteredTool rt;
    rt.claudeSchema = claudeSchema;
    rt.openAiSchema = openAiSchema;
    rt.handler      = std::move(handler);
    rt.tool         = Tool().setName(name).setDescription(description);
    m_tools[name]   = rt;
    syncToolsToProtocol();
}

void Client::setToolEnabled(const QString& toolName, bool enabled)
{
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
}

void Client::syncToolsToProtocol()
{
    bool isOllama = (qobject_cast<OllamaProtocol*>(m_protocol) != nullptr);

    QList<QJsonObject> schemas;
    QMap<QString, ToolHandler> handlers;

    for (auto it = m_tools.cbegin(); it != m_tools.cend(); ++it) {
        if (!it.value().enabled) {
            // Not advertised, but still answerable: the model may call it from
            // a stale/cached tool list. "disabled" guides better than "unknown".
            handlers.insert(it.key(), [](const QJsonObject&) {
                return toolError(QStringLiteral("tool is disabled"));
            });
            continue;
        }
        schemas.append(isOllama ? it.value().openAiSchema : it.value().claudeSchema);
        handlers.insert(it.key(), wrapHandler(it.key(), it.value()));
    }
    m_protocol->setTools(schemas, handlers);
}

// Single choke point in front of every tool execution:
// call cap -> input validation -> consent -> registered handler.
// All checks are opt-in; with defaults this forwards to the handler unchanged.
ToolHandler Client::wrapHandler(const QString& toolName, const RegisteredTool& rt)
{
    ToolHandler inner = rt.handler;
    QJsonObject parameterSchema = rt.claudeSchema["input_schema"].toObject();

    return [this, toolName, inner, parameterSchema](const QJsonObject& input) -> QJsonObject {
        if (m_maxToolCallsPerTurn > 0 && m_toolCallsThisTurn >= m_maxToolCallsPerTurn) {
            if (!m_limitSignalEmitted) {
                m_limitSignalEmitted = true;
                emit toolCallLimitReached(m_maxToolCallsPerTurn);
            }
            return toolError(QStringLiteral("tool call limit reached"));
        }

        if (m_validateToolInput) {
            QJsonObject error = Tool::validateAgainstSchema(parameterSchema, input);
            if (!error.isEmpty())
                return error;
        }

        if (m_consentHandler && !m_consentHandler(toolName, input)) {
#if LOGGER_LIBRARY_AVAILABLE == 1
            m_logger.logInfo("Tool declined by consent handler: " + toolName.toStdString());
#endif
            return toolError(QStringLiteral("user declined"));
        }

        ++m_toolCallsThisTurn;
        return inner(input);
    };
}

void Client::sendPrompt(const QString& userMessage)
{
    QLLM_GENERAL_PROFILING_FUNCTION(QLLM_COLOR_STAGE_1)
#if LOGGER_LIBRARY_AVAILABLE == 1
    m_logger.logInfo("sendPrompt (" + std::to_string(userMessage.size()) + " chars)");
#endif
    QJsonObject msg;
    msg["role"]    = "user";
    msg["content"] = userMessage;
    m_history.append(msg);
    m_toolCallsThisTurn = 0;
    m_limitSignalEmitted = false;
    m_protocol->beginTurn(userMessage);
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
    m_protocol->beginTurn(notification);
}

void Client::clearConversation()
{
    m_history = QJsonArray();
    m_protocol->clearHistory();
    m_protocol->clearStats();
    m_lastStats = UsageStats{};
    m_toolCallsThisTurn = 0;
    m_limitSignalEmitted = false;
}

QJsonArray Client::conversationHistory() const
{
    return m_history;
}

UsageStats Client::usageStats() const
{
    return m_lastStats;
}

UsageHistory* Client::usageHistory()
{
    return m_usageHistory;
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
    QString appName = QCoreApplication::applicationName();
    sample.app                      = appName.isEmpty() ? QStringLiteral("unknown") : appName;
    sample.inputTokens              = stats.inputTokens;
    sample.outputTokens             = stats.outputTokens;
    sample.cacheReadInputTokens     = stats.cacheReadInputTokens;
    sample.cacheCreationInputTokens = stats.cacheCreationInputTokens;
    sample.toolCalls                = stats.toolCalls;
    sample.durationMs               = stats.durationMs;

    // Est. per-turn cost reflecting prompt-caching pricing.
    // Cache reads cost ~10% of input price; cache writes cost ~125% (5-min ephemeral premium).
    // Ollama stays 0.
    sample.costUsd = 0.0;
    if (m_currentProvider == "claude") {
        double inPrice = 0, outPrice = 0;
        const QString m = m_currentModel.toLower();
        if      (m.contains("opus-4"))   { inPrice = 15.0;  outPrice = 75.0; }
        else if (m.contains("sonnet-4")) { inPrice = 3.0;   outPrice = 15.0; }
        else if (m.contains("haiku-4"))  { inPrice = 0.80;  outPrice = 4.0;  }
        else if (m.contains("opus-3"))   { inPrice = 15.0;  outPrice = 75.0; }
        else if (m.contains("sonnet-3")) { inPrice = 3.0;   outPrice = 15.0; }
        else if (m.contains("haiku-3"))  { inPrice = 0.25;  outPrice = 1.25; }
        sample.costUsd = (stats.inputTokens              * inPrice
                        + stats.cacheReadInputTokens     * 0.1  * inPrice
                        + stats.cacheCreationInputTokens * 1.25 * inPrice)
                         / 1'000'000.0
                       + (stats.outputTokens * outPrice) / 1'000'000.0;
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
