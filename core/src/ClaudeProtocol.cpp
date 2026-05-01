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
    : ProtocolBase(parent)
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

void ClaudeProtocol::beginTurn(const QString& userMessage)
{
    // Reset per-turn accumulators
    m_turnInputTokens  = 0;
    m_turnOutputTokens = 0;
    m_turnToolCalls    = 0;
    m_turnTimerStarted = false;

    QJsonObject msg;
    msg["role"]    = "user";
    msg["content"] = userMessage;
    m_history.append(msg);
    sendRequest();
}

void ClaudeProtocol::sendRequest()
{
    QJsonObject body = buildRequestBody();
    QByteArray bytes = QJsonDocument(body).toJson(QJsonDocument::Compact);

    if (!m_turnTimerStarted) {
        m_turnTimer.start();
        m_turnTimerStarted = true;
    }

    emit requestStarted();
    QList<QPair<QByteArray, QByteArray>> headers;
    headers.append({"x-api-key",        m_apiKey.toUtf8()});
    headers.append({ QByteArray("anthropic-version"), QByteArray("2023-06-01")});
    m_transport->post(m_url, bytes, headers);
}

void ClaudeProtocol::clearHistory()
{
    m_history = QJsonArray();
}

void ClaudeProtocol::clearStats()
{
    m_sessionInputTokens  = 0;
    m_sessionOutputTokens = 0;
    m_sessionToolCalls    = 0;
    m_sessionTurnCount    = 0;
    m_sessionCostUsd      = 0.0;
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
    // Accumulate token usage from this response (may be one of several in a tool loop)
    QJsonObject usage = responseJson["usage"].toObject();
    m_turnInputTokens  += usage["input_tokens"].toInt();
    m_turnOutputTokens += usage["output_tokens"].toInt();

    QString    stopReason = responseJson["stop_reason"].toString();
    QJsonArray content    = responseJson["content"].toArray();

    if (stopReason == "end_turn" || stopReason == "max_tokens") {
        QJsonObject assistantMsg;
        assistantMsg["role"]    = "assistant";
        assistantMsg["content"] = content;
        m_history.append(assistantMsg);

        QString text = assembleText(content);

        // Finalize session stats
        m_sessionInputTokens  += m_turnInputTokens;
        m_sessionOutputTokens += m_turnOutputTokens;
        m_sessionToolCalls    += m_turnToolCalls;
        ++m_sessionTurnCount;

        auto [inPrice, outPrice] = modelPricing();
        double turnCost = (m_turnInputTokens  / 1'000'000.0) * inPrice
                        + (m_turnOutputTokens / 1'000'000.0) * outPrice;
        m_sessionCostUsd += turnCost;

        UsageStats stats;
        stats.inputTokens         = m_turnInputTokens;
        stats.outputTokens        = m_turnOutputTokens;
        stats.toolCalls           = m_turnToolCalls;
        stats.durationMs          = m_turnTimer.elapsed();
        stats.sessionInputTokens  = m_sessionInputTokens;
        stats.sessionOutputTokens = m_sessionOutputTokens;
        stats.sessionToolCalls    = m_sessionToolCalls;
        stats.sessionTurnCount    = m_sessionTurnCount;
        stats.sessionCostUsd      = m_sessionCostUsd;

        emit requestFinished();
        emit responseReady(text);
        emit statsReady(stats);
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
        sendRequest();
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

        ++m_turnToolCalls;
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

QPair<double, double> ClaudeProtocol::modelPricing() const
{
    // Prices in USD per 1M tokens {input, output}. Approximate as of mid-2025.
    const QString m = m_model.toLower();
    if (m.contains("opus-4"))    return {15.0,  75.0};
    if (m.contains("sonnet-4"))  return { 3.0,  15.0};
    if (m.contains("haiku-4"))   return { 0.80,  4.0};
    if (m.contains("opus-3"))    return {15.0,  75.0};
    if (m.contains("sonnet-3"))  return { 3.0,  15.0};
    if (m.contains("haiku-3"))   return { 0.25,  1.25};
    return {0.0, 0.0};
}

void ClaudeProtocol::onTransportError(const QString& message)
{
    emit errorOccurred(message);
    emit requestFinished();
}

} // namespace QtLLM
