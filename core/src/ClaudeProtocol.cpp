#include "ClaudeProtocol.h"
#include "Tool.h"
#include "Pricing.h"

#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QDebug>

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
    , m_modelsTransport(new HttpTransport(this))
{
    connect(m_transport, &HttpTransport::replyReceived,
            this, &ClaudeProtocol::onReplyReceived);
    connect(m_transport, &HttpTransport::errorOccurred,
            this, &ClaudeProtocol::onTransportError);
    connect(m_modelsTransport, &HttpTransport::replyReceived,
            this, &ClaudeProtocol::onModelsReplyReceived);
    connect(m_modelsTransport, &HttpTransport::errorOccurred,
            this, &ClaudeProtocol::onModelsTransportError);
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

void ClaudeProtocol::setApiKey(const QString& apiKey)
{
    m_apiKey = apiKey;
}

void ClaudeProtocol::setUrl(const QUrl& url)
{
    m_url = url;
}

void ClaudeProtocol::setTools(const QList<QJsonObject>& toolSchemas,
                              const QMap<QString, ToolHandler>& handlers)
{
    m_toolSchemas  = toolSchemas;
    m_toolHandlers = handlers;
}

void ClaudeProtocol::beginTurn(const QString& userMessage)
{
    if (m_turnInProgress) {
        m_pendingTurns.enqueue(userMessage);
        return;
    }
    startNewTurn(userMessage);
}

void ClaudeProtocol::startNewTurn(const QString& userMessage)
{
    m_turnInProgress              = true;
    m_turnInputTokens             = 0;
    m_turnOutputTokens            = 0;
    m_turnCacheCreationInputTokens = 0;
    m_turnCacheReadInputTokens    = 0;
    m_turnToolCalls               = 0;
    m_turnToolIterations          = 0;
    m_turnTimerStarted            = false;
    m_historyLenBeforeTurn        = m_history.size();

    QJsonObject msg;
    msg["role"]    = "user";
    msg["content"] = userMessage;
    m_history.append(msg);
    sendRequest();
}

void ClaudeProtocol::drainQueue()
{
    m_turnInProgress = false;
    if (!m_pendingTurns.isEmpty())
        startNewTurn(m_pendingTurns.dequeue());
}

void ClaudeProtocol::sendRequest()
{
    // Fail loudly and instantly instead of sending x-api-key: "" - Anthropic
    // does return a 401 with a parseable error body for that (so this isn't
    // the only path to a visible error), but skipping the round trip gives
    // immediate, unambiguous feedback instead of waiting on a network error.
    if (m_apiKey.isEmpty()) {
        rollbackFailedTurn();
        emit errorOccurred(QStringLiteral(
            "No Anthropic API key set - pass a non-empty apiKey to the "
            "Client constructor before sending messages."));
        emit requestFinished();
        drainQueue();
        return;
    }

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
    // Prompt caching with ephemeral cache_control works on the first-party API
    // without a beta header.  If a Foundry/Azure endpoint rejects cache_control,
    // add:  headers.append({"anthropic-beta", "prompt-caching-2024-07-31"});
    m_transport->post(m_url, bytes, headers);
}

void ClaudeProtocol::clearHistory()
{
    m_history = QJsonArray();
}

void ClaudeProtocol::clearStats()
{
    m_sessionInputTokens              = 0;
    m_sessionOutputTokens             = 0;
    m_sessionCacheCreationInputTokens = 0;
    m_sessionCacheReadInputTokens     = 0;
    m_sessionToolCalls                = 0;
    m_sessionTurnCount                = 0;
    m_sessionCostUsd                  = 0.0;
}

QJsonArray ClaudeProtocol::conversationMessages() const
{
    return m_history;
}

QJsonObject ClaudeProtocol::buildRequestBody() const
{
    QJsonObject body;
    body["model"]      = m_model;
    body["max_tokens"] = m_maxTokens;

    if (!m_toolSchemas.isEmpty()) {
        QJsonArray tools;
        for (const QJsonObject& schema : m_toolSchemas)
            tools.append(schema);
        body["tools"] = tools;
    }

    // System prompt as a cached content-block array (caches tools + system together
    // because Anthropic render order is tools -> system -> messages).
    if (!m_systemPrompt.isEmpty()) {
        QJsonObject cacheControl;
        cacheControl["type"] = "ephemeral";

        QJsonObject sysBlock;
        sysBlock["type"]          = "text";
        sysBlock["text"]          = m_systemPrompt;
        sysBlock["cache_control"] = cacheControl;

        QJsonArray systemArr;
        systemArr.append(sysBlock);
        body["system"] = systemArr;
    }

    // Build a transient messages copy with a rolling cache breakpoint on the
    // last message's last content block.  We never mutate m_history itself —
    // persisting cache_control into history would scatter >4 breakpoints across
    // repeated requests and drift the cached prefix.
    QJsonArray messages = m_history;
    if (!messages.isEmpty()) {
        QJsonObject cacheControl;
        cacheControl["type"] = "ephemeral";

        QJsonObject lastMsg = messages.last().toObject();
        QJsonValue  content = lastMsg["content"];

        if (content.isArray()) {
            QJsonArray blocks = content.toArray();
            if (!blocks.isEmpty()) {
                QJsonObject lastBlock = blocks.last().toObject();
                lastBlock["cache_control"] = cacheControl;
                blocks[blocks.size() - 1] = lastBlock;
                lastMsg["content"] = blocks;
            }
        } else {
            // Plain string content — convert to a single cached text block
            QJsonObject textBlock;
            textBlock["type"]          = "text";
            textBlock["text"]          = content.toString();
            textBlock["cache_control"] = cacheControl;

            QJsonArray blocks;
            blocks.append(textBlock);
            lastMsg["content"] = blocks;
        }

        messages[messages.size() - 1] = lastMsg;
    }
    body["messages"] = messages;

    return body;
}

void ClaudeProtocol::onReplyReceived(const QByteArray& data)
{
    QJsonDocument doc = QJsonDocument::fromJson(data);
    if (doc.isNull()) {
        rollbackFailedTurn();
        emit errorOccurred("Failed to parse API response");
        emit requestFinished();
        drainQueue();
        return;
    }

    QJsonObject root = doc.object();

    // Real Anthropic errors are {"error":{"message":...}}. Gateways/proxies in
    // front of the real API (Azure/Foundry-style deployments, LiteLLM, etc.)
    // often return a flatter {"detail": "..."} shape instead for the same
    // HTTP-error case - recognize both, otherwise the error body gets fed to
    // processResponse() as if it were a real message (empty stop_reason,
    // empty content) which silently does nothing: no error, no response,
    // and the turn-in-progress flag never clears, wedging every future send.
    if (root.contains("error") || root.contains("detail")) {
        QString message = root["error"].toObject()["message"].toString();
        if (message.isEmpty())
            message = root["detail"].toString();
        if (message.isEmpty())
            message = QStringLiteral("Request failed (no error message in response)");
        rollbackFailedTurn();
        emit errorOccurred(message);
        emit requestFinished();
        drainQueue();
        return;
    }

    processResponse(root);
}

void ClaudeProtocol::processResponse(const QJsonObject& responseJson)
{
    // Accumulate token usage from this response (may be one of several in a tool loop)
    QJsonObject usage = responseJson["usage"].toObject();
    m_turnInputTokens              += usage["input_tokens"].toInt();
    m_turnOutputTokens             += usage["output_tokens"].toInt();
    m_turnCacheCreationInputTokens += usage["cache_creation_input_tokens"].toInt();
    m_turnCacheReadInputTokens     += usage["cache_read_input_tokens"].toInt();

    QString    stopReason = responseJson["stop_reason"].toString();
    QJsonArray content    = responseJson["content"].toArray();

    // Some models routed through a non-Anthropic gateway/proxy don't
    // reliably emit a real tool_use block - they just end the turn with the
    // call serialized as plain JSON text. Detect and treat it exactly like a
    // real tool_use response instead of showing the raw JSON to the user.
    if (stopReason == "end_turn") {
        QJsonArray fallback = extractFallbackToolUse(assembleText(content));
        if (!fallback.isEmpty()) {
            content    = fallback;
            stopReason = "tool_use";
        }
    }

    if (stopReason == "end_turn" || stopReason == "max_tokens") {
        QJsonObject assistantMsg;
        assistantMsg["role"]    = "assistant";
        assistantMsg["content"] = content;
        m_history.append(assistantMsg);

        QString text = assembleText(content);

        // Finalize session stats
        m_sessionInputTokens              += m_turnInputTokens;
        m_sessionOutputTokens             += m_turnOutputTokens;
        m_sessionCacheCreationInputTokens += m_turnCacheCreationInputTokens;
        m_sessionCacheReadInputTokens     += m_turnCacheReadInputTokens;
        m_sessionToolCalls                += m_turnToolCalls;
        ++m_sessionTurnCount;

        m_sessionCostUsd += PricingRegistry::instance().estimateCostUsd(
            m_model, m_turnInputTokens, m_turnOutputTokens,
            m_turnCacheReadInputTokens, m_turnCacheCreationInputTokens);

        qDebug() << "ClaudeProtocol turn stats:"
                 << "in=" << m_turnInputTokens
                 << "out=" << m_turnOutputTokens
                 << "cache_read=" << m_turnCacheReadInputTokens
                 << "cache_creation=" << m_turnCacheCreationInputTokens;

        UsageStats stats;
        stats.inputTokens              = m_turnInputTokens;
        stats.outputTokens             = m_turnOutputTokens;
        stats.cacheCreationInputTokens = m_turnCacheCreationInputTokens;
        stats.cacheReadInputTokens     = m_turnCacheReadInputTokens;
        stats.toolCalls                = m_turnToolCalls;
        stats.durationMs               = m_turnTimer.elapsed();
        stats.sessionInputTokens              = m_sessionInputTokens;
        stats.sessionOutputTokens             = m_sessionOutputTokens;
        stats.sessionCacheCreationInputTokens = m_sessionCacheCreationInputTokens;
        stats.sessionCacheReadInputTokens     = m_sessionCacheReadInputTokens;
        stats.sessionToolCalls                = m_sessionToolCalls;
        stats.sessionTurnCount                = m_sessionTurnCount;
        stats.sessionCostUsd                  = m_sessionCostUsd;

        emit requestFinished();
        emit responseReady(text);
        emit statsReady(stats);
        drainQueue();
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

        if (++m_turnToolIterations > kMaxToolIterations) {
            QString capText = assembleText(content);
            if (capText.isEmpty())
                capText = "[Tool-use iteration limit reached]";

            m_sessionInputTokens              += m_turnInputTokens;
            m_sessionOutputTokens             += m_turnOutputTokens;
            m_sessionCacheCreationInputTokens += m_turnCacheCreationInputTokens;
            m_sessionCacheReadInputTokens     += m_turnCacheReadInputTokens;
            m_sessionToolCalls                += m_turnToolCalls;
            ++m_sessionTurnCount;

            m_sessionCostUsd += PricingRegistry::instance().estimateCostUsd(
                m_model, m_turnInputTokens, m_turnOutputTokens,
                m_turnCacheReadInputTokens, m_turnCacheCreationInputTokens);

            qDebug() << "ClaudeProtocol turn stats (cap hit):"
                     << "in=" << m_turnInputTokens
                     << "out=" << m_turnOutputTokens
                     << "cache_read=" << m_turnCacheReadInputTokens
                     << "cache_creation=" << m_turnCacheCreationInputTokens;

            UsageStats stats;
            stats.inputTokens              = m_turnInputTokens;
            stats.outputTokens             = m_turnOutputTokens;
            stats.cacheCreationInputTokens = m_turnCacheCreationInputTokens;
            stats.cacheReadInputTokens     = m_turnCacheReadInputTokens;
            stats.toolCalls                = m_turnToolCalls;
            stats.durationMs               = m_turnTimer.elapsed();
            stats.sessionInputTokens              = m_sessionInputTokens;
            stats.sessionOutputTokens             = m_sessionOutputTokens;
            stats.sessionCacheCreationInputTokens = m_sessionCacheCreationInputTokens;
            stats.sessionCacheReadInputTokens     = m_sessionCacheReadInputTokens;
            stats.sessionToolCalls                = m_sessionToolCalls;
            stats.sessionTurnCount                = m_sessionTurnCount;
            stats.sessionCostUsd                  = m_sessionCostUsd;

            emit errorOccurred("Tool-use iteration limit reached ("
                               + QString::number(kMaxToolIterations) + ")");
            emit requestFinished();
            emit responseReady(capText);
            emit statsReady(stats);
            drainQueue();
            return;
        }

        sendRequest();
        return;
    }

    // Defense in depth: an unrecognized/empty stop_reason (a response shape
    // this client doesn't know about, from some future API change or a
    // nonstandard gateway) must still end the turn - silently doing nothing
    // here would leave m_turnInProgress stuck true forever, wedging every
    // subsequent sendPrompt() into the queue with no way out.
    rollbackFailedTurn();
    emit errorOccurred(QStringLiteral("Unexpected API response (stop_reason=\"%1\"): %2")
        .arg(stopReason, QString::fromUtf8(QJsonDocument(responseJson).toJson(QJsonDocument::Compact))));
    emit requestFinished();
    drainQueue();
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
        // A throwing handler must not unwind through the network callback
        // chain — convert to an is_error tool result and keep the turn alive.
        QJsonObject result;
        try {
            result = m_toolHandlers[toolName](toolInput);
        } catch (const std::exception& e) {
            result = QJsonObject{{"status", "error"},
                                 {"message", QString("Internal error: %1").arg(e.what())}};
        } catch (...) {
            result = QJsonObject{{"status", "error"},
                                 {"message", "Internal error: unknown exception in tool handler"}};
        }
        emit toolCompleted(toolName, result);

        QString resultStr = QString::fromUtf8(
            QJsonDocument(result).toJson(QJsonDocument::Compact));

        QJsonObject toolResult;
        toolResult["type"]        = "tool_result";
        toolResult["tool_use_id"] = toolId;
        toolResult["content"]     = resultStr;
        if (result["status"].toString() == "error")
            toolResult["is_error"] = true;
        toolResults.append(toolResult);
    }

    QJsonObject userMsg;
    userMsg["role"]    = "user";
    userMsg["content"] = toolResults;
    m_history.append(userMsg);
}

QJsonArray ClaudeProtocol::extractFallbackToolUse(const QString& text) const
{
    QJsonDocument doc = QJsonDocument::fromJson(text.trimmed().toUtf8());
    if (!doc.isObject())
        return {};

    QJsonObject obj = doc.object();
    QString name = obj["name"].toString();
    if (name.isEmpty() || !m_toolHandlers.contains(name))
        return {};

    QJsonObject input = obj.contains("parameters") ? obj["parameters"].toObject()
                       : obj.contains("input")      ? obj["input"].toObject()
                       : obj.contains("arguments")   ? obj["arguments"].toObject()
                       : QJsonObject();

    QJsonObject block;
    block["type"]  = "tool_use";
    block["id"]    = QStringLiteral("fallback_tool_use");
    block["name"]  = name;
    block["input"] = input;

    return QJsonArray{ block };
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
    rollbackFailedTurn();
    emit errorOccurred(message);
    emit requestFinished();
    drainQueue();
}

void ClaudeProtocol::rollbackFailedTurn()
{
    while (m_history.size() > m_historyLenBeforeTurn)
        m_history.removeAt(m_history.size() - 1);
}

void ClaudeProtocol::fetchModels()
{
    // Derive models endpoint from the messages endpoint URL.
    // e.g. https://api.anthropic.com/v1/messages -> https://api.anthropic.com/v1/models
    QUrl modelsUrl = m_url;
    QString path = modelsUrl.path();
    if (path.endsWith("/messages"))
        path.replace(path.length() - 9, 9, "/models");
    else {
        // Drop last segment and append "models"
        int lastSlash = path.lastIndexOf('/');
        if (lastSlash >= 0)
            path = path.left(lastSlash + 1) + "models";
        else
            path += "/models";
    }
    modelsUrl.setPath(path);

    QList<QPair<QByteArray, QByteArray>> headers;
    headers.append({"x-api-key", m_apiKey.toUtf8()});
    headers.append({QByteArray("anthropic-version"), QByteArray("2023-06-01")});
    m_modelsTransport->get(modelsUrl, headers);
}

void ClaudeProtocol::onModelsReplyReceived(const QByteArray& data)
{
    QJsonDocument doc = QJsonDocument::fromJson(data);
    if (doc.isNull()) {
        emit errorOccurred("Failed to parse models response");
        emit modelsFetched({});
        return;
    }

    QJsonObject root = doc.object();
    if (root.contains("error")) {
        emit errorOccurred(root["error"].toObject()["message"].toString());
        emit modelsFetched({});
        return;
    }

    QStringList models;
    QJsonArray dataArr = root["data"].toArray();
    for (const QJsonValue& val : dataArr) {
        QString id = val.toObject()["id"].toString();
        if (!id.isEmpty())
            models.append(id);
    }
    emit modelsFetched(models);
}

void ClaudeProtocol::onModelsTransportError(const QString& message)
{
    emit errorOccurred(message);
    emit modelsFetched({});
}

} // namespace QtLLM
