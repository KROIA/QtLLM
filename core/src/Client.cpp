#include "Client.h"
#include "ClaudeProtocol.h"
#include "OllamaProtocol.h"
#include <QJsonObject>
#include <QJsonDocument>

namespace QtLLM {

Client::Client(const QString& apiKey, const QString& url, QObject* parent)
    : QObject(parent)
    , m_protocol(new ClaudeProtocol(apiKey, QUrl(url), this))
{
    connectProtocol();
}

Client::Client(Provider provider, const QString& url, const QString& apiKey, QObject* parent)
    : QObject(parent)
    , m_protocol(nullptr)
{
    switch (provider) {
    case Provider::Ollama:
        m_protocol = new OllamaProtocol(QUrl(url), this);
        break;
    case Provider::Claude:
    default:
        m_protocol = new ClaudeProtocol(apiKey, QUrl(url), this);
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
    emit statsUpdated(stats);
}

void Client::onProtocolError(const QString& errorMessage)
{
#if LOGGER_LIBRARY_AVAILABLE == 1
    m_logger.logError(errorMessage.toStdString());
#endif
    emit errorOccurred(errorMessage);
}

void Client::setModel(const QString& model)       { m_protocol->setModel(model); }
void Client::setMaxTokens(int maxTokens)           { m_protocol->setMaxTokens(maxTokens); }
void Client::setSystemPrompt(const QString& p)     { m_protocol->setSystemPrompt(p); }

void Client::registerTool(const Tool& tool, ToolHandler handler)
{
#if LOGGER_LIBRARY_AVAILABLE == 1
    m_logger.logDebug("Register tool: " + tool.name().toStdString());
#endif
    RegisteredTool rt;
    rt.claudeSchema = tool.toApiObject();
    rt.openAiSchema = tool.toOpenAiApiObject();
    rt.handler      = std::move(handler);
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
    m_tools[name]   = rt;
    syncToolsToProtocol();
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
        schemas.append(isOllama ? it.value().openAiSchema : it.value().claudeSchema);
        handlers.insert(it.key(), it.value().handler);
    }
    m_protocol->setTools(schemas, handlers);
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
    m_protocol->beginTurn(notification);
}

void Client::clearConversation()
{
    m_history = QJsonArray();
    m_protocol->clearHistory();
    m_protocol->clearStats();
    m_lastStats = UsageStats{};
}

QJsonArray Client::conversationHistory() const
{
    return m_history;
}

UsageStats Client::usageStats() const
{
    return m_lastStats;
}

} // namespace QtLLM
