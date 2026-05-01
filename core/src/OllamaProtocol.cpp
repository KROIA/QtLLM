#include "OllamaProtocol.h"
#include <QJsonDocument>

namespace QtLLM {

OllamaProtocol::OllamaProtocol(const QUrl& url, QObject* parent)
    : ProtocolBase(parent)
    , m_url(url)
    , m_model("llama3.2")
    , m_maxTokens(2048)
    , m_systemPrompt()
    , m_toolSchemas()
    , m_toolHandlers()
    , m_history()
    , m_transport(new HttpTransport(this))
{
    connect(m_transport, &HttpTransport::replyReceived,
            this, &OllamaProtocol::onReplyReceived);
    connect(m_transport, &HttpTransport::errorOccurred,
            this, &OllamaProtocol::onTransportError);
}

OllamaProtocol::~OllamaProtocol() = default;

void OllamaProtocol::setModel(const QString& model)
{
    m_model = model;
}

void OllamaProtocol::setMaxTokens(int maxTokens)
{
    m_maxTokens = maxTokens;
}

void OllamaProtocol::setSystemPrompt(const QString& systemPrompt)
{
    m_systemPrompt = systemPrompt;
}

void OllamaProtocol::setTools(const QList<QJsonObject>& toolSchemas,
                               const QMap<QString, ToolHandler>& handlers)
{
    m_toolSchemas  = toolSchemas;
    m_toolHandlers = handlers;
}

void OllamaProtocol::beginTurn(const QString& userMessage)
{
    QJsonObject msg;
    msg["role"]    = "user";
    msg["content"] = userMessage;
    m_history.append(msg);
    sendRequest();
}

void OllamaProtocol::sendRequest()
{
    QJsonObject body  = buildRequestBody();
    QByteArray  bytes = QJsonDocument(body).toJson(QJsonDocument::Compact);
    emit requestStarted();
    m_transport->post(m_url, bytes, {});
}

void OllamaProtocol::clearHistory()
{
    m_history = QJsonArray();
}

QJsonObject OllamaProtocol::buildRequestBody() const
{
    QJsonArray messages;

    if (!m_systemPrompt.isEmpty()) {
        QJsonObject sysMsg;
        sysMsg["role"]    = "system";
        sysMsg["content"] = m_systemPrompt;
        messages.append(sysMsg);
    }

    for (const QJsonValue& val : m_history) {
        messages.append(val);
    }

    QJsonObject options;
    options["num_predict"] = m_maxTokens;

    QJsonObject body;
    body["model"]   = m_model;
    body["messages"] = messages;
    body["stream"]  = false;
    body["options"] = options;

    if (!m_toolSchemas.isEmpty()) {
        QJsonArray tools;
        for (const QJsonObject& schema : m_toolSchemas) {
            tools.append(schema);
        }
        body["tools"] = tools;
    }

    return body;
}

void OllamaProtocol::onReplyReceived(const QByteArray& data)
{
    QJsonDocument doc = QJsonDocument::fromJson(data);
    if (doc.isNull()) {
        emit errorOccurred("Failed to parse Ollama response");
        return;
    }

    QJsonObject root = doc.object();

    if (root.contains("error")) {
        emit errorOccurred(root["error"].toString());
        emit requestFinished();
        return;
    }

    processResponse(root);
}

void OllamaProtocol::processResponse(const QJsonObject& responseJson)
{
    QJsonObject message    = responseJson["message"].toObject();
    QJsonArray  toolCalls  = message["tool_calls"].toArray();

    if (!toolCalls.isEmpty()) {
        QJsonObject assistantMsg;
        assistantMsg["role"]       = "assistant";
        assistantMsg["content"]    = message["content"].toString();
        assistantMsg["tool_calls"] = toolCalls;
        m_history.append(assistantMsg);

        executeToolCalls(toolCalls);
        sendRequest();
        return;
    }

    QString text = message["content"].toString();

    QJsonObject assistantMsg;
    assistantMsg["role"]    = "assistant";
    assistantMsg["content"] = text;
    m_history.append(assistantMsg);

    emit requestFinished();
    emit responseReady(text);
}

void OllamaProtocol::executeToolCalls(const QJsonArray& toolCalls)
{
    QJsonArray toolResults;

    for (const QJsonValue& val : toolCalls) {
        QJsonObject toolCall = val.toObject();
        QJsonObject fn       = toolCall["function"].toObject();
        QString     toolName = fn["name"].toString();

        QJsonObject toolInput;
        QJsonValue  argsVal = fn["arguments"];
        if (argsVal.isObject()) {
            toolInput = argsVal.toObject();
        } else if (argsVal.isString()) {
            // Some models serialize arguments as a JSON string
            QJsonDocument argDoc = QJsonDocument::fromJson(argsVal.toString().toUtf8());
            if (!argDoc.isNull())
                toolInput = argDoc.object();
        }

        if (!m_toolHandlers.contains(toolName)) {
            emit errorOccurred("No handler registered for tool: " + toolName);

            QJsonObject errorResult;
            errorResult["role"]    = "tool";
            errorResult["content"] = "Error: unknown tool";
            toolResults.append(errorResult);
            continue;
        }

        emit toolInvoked(toolName, toolInput);
        QJsonObject result = m_toolHandlers[toolName](toolInput);
        emit toolCompleted(toolName, result);

        QString resultStr = QString::fromUtf8(
            QJsonDocument(result).toJson(QJsonDocument::Compact));

        QJsonObject toolResult;
        toolResult["role"]    = "tool";
        toolResult["content"] = resultStr;
        toolResults.append(toolResult);
    }

    for (const QJsonValue& val : toolResults) {
        m_history.append(val);
    }
}

void OllamaProtocol::onTransportError(const QString& message)
{
    emit errorOccurred(message);
    emit requestFinished();
}

} // namespace QtLLM
