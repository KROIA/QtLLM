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
    connect(m_protocol, &ProtocolBase::errorOccurred,   this, &Client::errorOccurred);
    connect(m_protocol, &ProtocolBase::requestStarted,  this, &Client::requestStarted);
    connect(m_protocol, &ProtocolBase::requestFinished, this, &Client::requestFinished);
}

void Client::onProtocolResponseReady(const QString& text)
{
    QJsonObject msg;
    msg["role"]    = "assistant";
    msg["content"] = text;
    m_history.append(msg);
    emit responseReady(text);
}

void Client::onProtocolStatsReady(const QtLLM::UsageStats& stats)
{
    m_lastStats = stats;
    emit statsUpdated(stats);
}

void Client::setModel(const QString& model)       { m_protocol->setModel(model); }
void Client::setMaxTokens(int maxTokens)           { m_protocol->setMaxTokens(maxTokens); }
void Client::setSystemPrompt(const QString& p)     { m_protocol->setSystemPrompt(p); }

void Client::registerTool(const Tool& tool, ToolHandler handler)
{
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
    QJsonObject msg;
    msg["role"]    = "user";
    msg["content"] = userMessage;
    m_history.append(msg);
    m_protocol->beginTurn(userMessage);
}

void Client::sendToolMessage(const QString& toolName, const QJsonObject& input)
{
    if (!m_tools.contains(toolName)) {
        qWarning() << "Attempted to send message for unregistered tool:" << toolName;
        return;
    }

	
    QJsonObject msg;
    msg["role"]    = "tool";
    msg["tool"]    = toolName;
    msg["content"] = input;

    QJsonDocument doc(msg);
    m_history.append(msg);
	QString serializedMsg = QString::fromUtf8(doc.toJson(QJsonDocument::Compact));
	m_protocol->beginTurn(serializedMsg);
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
