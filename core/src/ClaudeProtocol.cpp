#include "ClaudeProtocol.h"
#include "Tool.h"

#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>

namespace QtLLM
{

ClaudeProtocol::ClaudeProtocol(const QString& apiKey,
                               const QUrl& url,
                               QObject* parent)
    : QObject(parent)
    , m_apiKey(apiKey)
    , m_url(url)
    , m_model("claude-opus-4-5")
    , m_maxTokens(1024)
    , m_systemPrompt()
    , m_toolSchemas()
    , m_toolHandlers()
    , m_history()
    , m_transport(new HttpTransport(this))
{
    connect(m_transport, &HttpTransport::replyReceived,
            this, &ClaudeProtocol::onReplyReceived);
    connect(m_transport, &HttpTransport::errorOccurred,
            this, &ClaudeProtocol::onTransportError);
}

ClaudeProtocol::~ClaudeProtocol() = default;

void ClaudeProtocol::setModel(const QString& model)
{
    m_model = model;
}

void ClaudeProtocol::setMaxTokens(int maxTokens)
{
    m_maxTokens = maxTokens;
}

void ClaudeProtocol::setSystemPrompt(const QString& systemPrompt)
{
    m_systemPrompt = systemPrompt;
}

void ClaudeProtocol::setTools(const QList<QJsonObject>& toolSchemas,
                              const QMap<QString, ToolHandler>& handlers)
{
    m_toolSchemas  = toolSchemas;
    m_toolHandlers = handlers;
}

void ClaudeProtocol::sendTurn(const QJsonArray& history)
{
    m_history = history;
    QJsonObject body = buildRequestBody();
    QByteArray bytes = QJsonDocument(body).toJson(QJsonDocument::Compact);
    emit requestStarted();
    m_transport->post(m_url, bytes, m_apiKey);
}

void ClaudeProtocol::clearHistory()
{
    m_history = QJsonArray();
}

QJsonArray ClaudeProtocol::history() const
{
    return m_history;
}

QJsonObject ClaudeProtocol::buildRequestBody() const
{
    QJsonObject body;
    body["model"]      = m_model;
    body["max_tokens"] = m_maxTokens;
    body["messages"]   = m_history;

    if (!m_systemPrompt.isEmpty()) {
        body["system"] = m_systemPrompt;
    }

    if (!m_toolSchemas.isEmpty()) {
        QJsonArray tools;
        for (const QJsonObject& schema : m_toolSchemas) {
            tools.append(schema);
        }
        body["tools"] = tools;
    }

    return body;
}

void ClaudeProtocol::onReplyReceived(const QByteArray& data)
{
    QJsonDocument doc = QJsonDocument::fromJson(data);
    if (doc.isNull()) {
        emit errorOccurred("Failed to parse API response");
        return;
    }

    QJsonObject root = doc.object();

    if (root.contains("error")) {
        QString message = root["error"].toObject()["message"].toString();
        emit errorOccurred(message);
        emit requestFinished();
        return;
    }

    processResponse(root);
}

void ClaudeProtocol::processResponse(const QJsonObject& responseJson)
{
    QString    stopReason = responseJson["stop_reason"].toString();
    QJsonArray content    = responseJson["content"].toArray();

    if (stopReason == "end_turn" || stopReason == "max_tokens") {
        QJsonObject assistantMsg;
        assistantMsg["role"]    = "assistant";
        assistantMsg["content"] = content;
        m_history.append(assistantMsg);

        QString text = assembleText(content);
        emit requestFinished();
        emit responseReady(text);
        return;
    }

    if (stopReason == "tool_use") {
        QJsonObject assistantMsg;
        assistantMsg["role"]    = "assistant";
        assistantMsg["content"] = content;
        m_history.append(assistantMsg);

        QJsonArray toolUseBlocks;
        for (const QJsonValue& val : content) {
            QJsonObject block = val.toObject();
            if (block["type"].toString() == "tool_use") {
                toolUseBlocks.append(block);
            }
        }

        executeToolCalls(toolUseBlocks);
        sendTurn(m_history);
    }
}

void ClaudeProtocol::executeToolCalls(const QJsonArray& toolUseBlocks)
{
    QJsonArray toolResults;

    for (const QJsonValue& val : toolUseBlocks) {
        QJsonObject block     = val.toObject();
        QString     toolName  = block["name"].toString();
        QJsonObject toolInput = block["input"].toObject();
        QString     toolId    = block["id"].toString();

        if (!m_toolHandlers.contains(toolName)) {
            emit errorOccurred("No handler registered for tool: " + toolName);

            QJsonObject errorResult;
            errorResult["type"]        = "tool_result";
            errorResult["tool_use_id"] = toolId;
            errorResult["content"]     = "Error: unknown tool";
            errorResult["is_error"]    = true;
            toolResults.append(errorResult);
            continue;
        }

        emit toolInvoked(toolName, toolInput);
        QJsonObject result = m_toolHandlers[toolName](toolInput);
        emit toolCompleted(toolName, result);

        QString resultStr = QString::fromUtf8(
            QJsonDocument(result).toJson(QJsonDocument::Compact));

        QJsonObject toolResult;
        toolResult["type"]        = "tool_result";
        toolResult["tool_use_id"] = toolId;
        toolResult["content"]     = resultStr;
        toolResults.append(toolResult);
    }

    QJsonObject userMsg;
    userMsg["role"]    = "user";
    userMsg["content"] = toolResults;
    m_history.append(userMsg);
}

QString ClaudeProtocol::assembleText(const QJsonArray& content) const
{
    QString assembled;
    for (const QJsonValue& val : content) {
        QJsonObject block = val.toObject();
        if (block["type"].toString() == "text") {
            assembled += block["text"].toString();
        }
    }
    return assembled;
}

void ClaudeProtocol::onTransportError(const QString& message)
{
    emit errorOccurred(message);
    emit requestFinished();
}

} // namespace QtLLM
