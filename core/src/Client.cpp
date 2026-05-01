#include "Client.h"
#include "ClaudeProtocol.h"
#include <QJsonObject>
#include <QJsonDocument>

namespace QtLLM {

Client::Client(const QString& apiKey, const QString& url, QObject* parent)
    : QObject(parent)
    , m_protocol(new ClaudeProtocol(apiKey, QUrl(url), this))
{
    connect(m_protocol, &ClaudeProtocol::responseReady,    this, &Client::responseReady);
    connect(m_protocol, &ClaudeProtocol::toolInvoked,      this, &Client::toolInvoked);
    connect(m_protocol, &ClaudeProtocol::toolCompleted,    this, &Client::toolCompleted);
    connect(m_protocol, &ClaudeProtocol::errorOccurred,    this, &Client::errorOccurred);
    connect(m_protocol, &ClaudeProtocol::requestStarted,   this, &Client::requestStarted);
    connect(m_protocol, &ClaudeProtocol::requestFinished,  this, &Client::requestFinished);
}

Client::~Client() = default;

void Client::setModel(const QString& model)
{
    m_protocol->setModel(model);
}

void Client::setMaxTokens(int maxTokens)
{
    m_protocol->setMaxTokens(maxTokens);
}

void Client::setSystemPrompt(const QString& systemPrompt)
{
    m_protocol->setSystemPrompt(systemPrompt);
}

void Client::registerTool(const Tool& tool, ToolHandler handler)
{
    m_tools[tool.name()] = RegisteredTool{ tool.toApiObject(), handler };
    syncToolsToProtocol();
}

void Client::registerTool(const QString& name,
                           const QString& description,
                           const QJsonObject& parameterSchema,
                           ToolHandler handler)
{
    QJsonObject schema;
    schema["name"]         = name;
    schema["description"]  = description;
    schema["input_schema"] = parameterSchema;

    m_tools[name] = RegisteredTool{ schema, handler };
    syncToolsToProtocol();
}

void Client::unregisterTool(const QString& toolName)
{
    m_tools.remove(toolName);
    syncToolsToProtocol();
}

void Client::syncToolsToProtocol()
{
    QList<QJsonObject> schemas;
    QMap<QString, ToolHandler> handlers;

    for (auto it = m_tools.constBegin(); it != m_tools.constEnd(); ++it) {
        schemas.append(it.value().schema);
        handlers[it.key()] = it.value().handler;
    }

    m_protocol->setTools(schemas, handlers);
}

void Client::sendPrompt(const QString& userMessage)
{
    QJsonObject msg;
    msg["role"]    = QStringLiteral("user");
    msg["content"] = userMessage;
    m_history.append(msg);

    m_protocol->sendTurn(m_history);
}

void Client::clearConversation()
{
    m_history = QJsonArray();
    m_protocol->clearHistory();
}

QJsonArray Client::conversationHistory() const
{
    return m_history;
}

} // namespace QtLLM
