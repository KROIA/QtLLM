#include "ClaudeProtocol.h"
#include "Tool.h"
#include "ToolResult.h"
#include "Pricing.h"

#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QDebug>
#include <QPointer>
#include <QTimer>
#include <QUuid>

namespace QtLLM
{

ClaudeProtocol::ClaudeProtocol(const QString& apiKey,
                               const QUrl& url,
                               QObject* parent)
    : ProtocolBase(parent)
    , m_apiKey(apiKey)
    , m_url(url)
    , m_model("claude-sonnet-5-5")
    // Only a cap, not a target. 1024 truncated ordinary tool calls mid-JSON and
    // left thinking models no room to answer at all.
    , m_maxTokens(16000)
    , m_systemPrompt()
    , m_toolSchemas()
    , m_toolHandlers()
    , m_history()
    , m_transport(new HttpTransport(this))
    , m_modelsTransport(new HttpTransport(this))
{
    // The request is not streamed, so nothing arrives until the whole answer
    // is generated. The transport's 60 s default aborted long answers that the
    // API had already billed.
    m_transport->setTimeoutMs(600000);

    connect(m_transport, &HttpTransport::replyReceived,
            this, &ClaudeProtocol::onReplyReceived);
    connect(m_transport, &HttpTransport::errorOccurred,
            this, &ClaudeProtocol::onTransportError);
    connect(m_transport, &HttpTransport::chunkReceived,
            this, &ClaudeProtocol::onStreamChunk);
    connect(m_transport, &HttpTransport::streamFinished,
            this, &ClaudeProtocol::onStreamFinished);
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

void ClaudeProtocol::setAccessTokenProvider(AccessTokenProvider provider)
{
    m_accessTokenProvider = std::move(provider);
}

QPair<QByteArray, QByteArray> ClaudeProtocol::authHeader() const
{
    if (m_accessTokenProvider) {
        const QString token = m_accessTokenProvider();
        if (token.isEmpty())
            return {};
        return {QByteArray("Authorization"), "Bearer " + token.toUtf8()};
    }
    if (m_apiKey.isEmpty())
        return {};
    return {QByteArray("x-api-key"), m_apiKey.toUtf8()};
}

void ClaudeProtocol::setTools(const QList<QJsonObject>& toolSchemas,
                              const QMap<QString, ToolCallHandler>& handlers)
{
    m_toolSchemas  = toolSchemas;
    m_toolHandlers = handlers;
}

void ClaudeProtocol::beginTurn(const QString& userMessage)
{
    beginTurn(userMessage, QJsonArray());
}

void ClaudeProtocol::beginTurn(const QString& userMessage, const QJsonArray& images)
{
    QJsonObject msg;
    msg["role"] = "user";
    if (images.isEmpty()) {
        msg["content"] = userMessage;
    } else {
        // Pictures first, the question about them after.
        QJsonArray content;
        for (const QJsonValue& value : images) {
            const QJsonObject image = value.toObject();
            content.append(QJsonObject{
                {"type", "image"},
                {"source", QJsonObject{{"type", "base64"},
                                       {"media_type", image["media_type"]},
                                       {"data", image["data"]}}}});
        }
        if (!userMessage.isEmpty())
            content.append(QJsonObject{{"type", "text"}, {"text", userMessage}});
        msg["content"] = content;
    }

    if (m_turnInProgress) {
        m_pendingTurns.enqueue(msg);
        return;
    }
    startNewTurn(msg);
}

void ClaudeProtocol::startNewTurn(const QJsonObject& userMessage)
{
    m_turnInProgress              = true;
    m_turnInputTokens             = 0;
    m_turnOutputTokens            = 0;
    m_turnCacheCreationInputTokens = 0;
    m_turnCacheReadInputTokens    = 0;
    m_turnToolCalls               = 0;
    m_turnToolIterations          = 0;
    m_retryCount                  = 0;
    m_thinkingStripped            = false;
    m_turnTimerStarted            = false;
    m_historyLenBeforeTurn        = m_history.size();

    m_history.append(userMessage);
    sendRequest();
}

void ClaudeProtocol::drainQueue()
{
    m_turnInProgress = false;
    ++m_turnGeneration;
    if (!m_pendingTurns.isEmpty())
        startNewTurn(m_pendingTurns.dequeue());
}

void ClaudeProtocol::sendRequest()
{
    // Fail loudly and instantly instead of sending x-api-key: "" - Anthropic
    // does return a 401 with a parseable error body for that (so this isn't
    // the only path to a visible error), but skipping the round trip gives
    // immediate, unambiguous feedback instead of waiting on a network error.
    const QPair<QByteArray, QByteArray> auth = authHeader();
    if (auth.first.isEmpty()) {
        failTurn(m_accessTokenProvider
            ? QStringLiteral("Not signed in - sign in before sending messages "
                             "(the sign-in was not completed or has expired).")
            : QStringLiteral("No Anthropic API key set - pass a non-empty apiKey to the "
                             "Client constructor before sending messages."));
        return;
    }

    QJsonObject body = buildRequestBody();
    const bool stream = m_streamingEnabled && !m_streamUnsupported;
    if (stream)
        body["stream"] = true;
    QByteArray bytes = QJsonDocument(body).toJson(QJsonDocument::Compact);

    if (!m_turnTimerStarted) {
        m_turnTimer.start();
        m_turnTimerStarted = true;
    }

    emit requestStarted();
    QList<QPair<QByteArray, QByteArray>> headers;
    headers.append(auth);
    headers.append({ QByteArray("anthropic-version"), QByteArray("2023-06-01")});
    // Prompt caching with ephemeral cache_control works on the first-party API
    // without a beta header.  If a Foundry/Azure endpoint rejects cache_control,
    // add:  headers.append({"anthropic-beta", "prompt-caching-2024-07-31"});
    if (stream) {
        resetStreamState();
        m_transport->postStream(m_url, bytes, headers);
    } else {
        m_transport->post(m_url, bytes, headers);
    }
}

void ClaudeProtocol::resetStreamState()
{
    m_streamBuffer.clear();
    m_streamSniffed = false;
    m_streamIsPlainJson = false;
    m_streamBlocks.clear();
    m_streamPartialJson.clear();
    m_streamUsage = QJsonObject();
    m_streamStopReason.clear();
    m_streamStopDetails = QJsonObject();
    m_streamError = QJsonObject();
    m_streamTextEmitted = false;
}

void ClaudeProtocol::onStreamChunk(const QByteArray& data)
{
    m_streamBuffer += data;

    if (!m_streamSniffed) {
        // A gateway that ignores "stream" answers with one plain JSON object.
        const QByteArray head = m_streamBuffer.trimmed();
        if (head.isEmpty())
            return;
        m_streamSniffed = true;
        m_streamIsPlainJson = head.startsWith('{');
    }
    if (m_streamIsPlainJson)
        return;   // handled as a whole in onStreamFinished()

    // Events are separated by a blank line. Only complete events are taken out
    // of the buffer, so a chunk boundary inside an event (or inside a UTF-8
    // sequence) is harmless.
    m_streamBuffer.replace("\r\n", "\n");
    int end;
    while ((end = m_streamBuffer.indexOf("\n\n")) >= 0) {
        const QByteArray block = m_streamBuffer.left(end);
        m_streamBuffer.remove(0, end + 2);

        QString    event;
        QByteArray payload;
        for (const QByteArray& line : block.split('\n')) {
            if (line.startsWith("event:"))
                event = QString::fromUtf8(line.mid(6).trimmed());
            else if (line.startsWith("data:"))
                payload += line.mid(5).trimmed();
        }
        if (payload.isEmpty())
            continue;
        handleStreamEvent(event, QJsonDocument::fromJson(payload).object());
    }
}

void ClaudeProtocol::handleStreamEvent(const QString& event, const QJsonObject& data)
{
    const QString type  = event.isEmpty() ? data["type"].toString() : event;
    const int     index = data["index"].toInt();

    if (type == QLatin1String("message_start")) {
        m_streamUsage = data["message"].toObject()["usage"].toObject();
    } else if (type == QLatin1String("content_block_start")) {
        m_streamBlocks[index] = data["content_block"].toObject();
    } else if (type == QLatin1String("content_block_delta")) {
        const QJsonObject delta = data["delta"].toObject();
        const QString deltaType = delta["type"].toString();
        QJsonObject& block = m_streamBlocks[index];
        if (deltaType == QLatin1String("text_delta")) {
            const QString text = delta["text"].toString();
            block["text"] = block["text"].toString() + text;
            if (!text.isEmpty()) {
                m_streamTextEmitted = true;
                emit responseDelta(text);
            }
        } else if (deltaType == QLatin1String("input_json_delta")) {
            m_streamPartialJson[index] += delta["partial_json"].toString().toUtf8();
        } else if (deltaType == QLatin1String("thinking_delta")) {
            block["thinking"] = block["thinking"].toString() + delta["thinking"].toString();
        } else if (deltaType == QLatin1String("signature_delta")) {
            block["signature"] = block["signature"].toString() + delta["signature"].toString();
        }
    } else if (type == QLatin1String("message_delta")) {
        const QJsonObject delta = data["delta"].toObject();
        if (delta.contains("stop_reason"))
            m_streamStopReason = delta["stop_reason"].toString();
        if (delta["stop_details"].isObject())
            m_streamStopDetails = delta["stop_details"].toObject();
        // Cumulative counters; later values replace earlier ones.
        const QJsonObject usage = data["usage"].toObject();
        for (auto it = usage.begin(); it != usage.end(); ++it)
            m_streamUsage[it.key()] = it.value();
    } else if (type == QLatin1String("error")) {
        m_streamError = data;
    }
    // content_block_stop, message_stop, ping: nothing to do - the response is
    // assembled in onStreamFinished(), once the transport is free again.
}

void ClaudeProtocol::onStreamFinished()
{
    if (!m_turnInProgress)
        return;

    if (m_streamIsPlainJson) {
        onReplyReceived(m_streamBuffer);
        return;
    }

    if (!m_streamError.isEmpty()) {
        // Nothing shown yet: the normal error path may retry. Otherwise the
        // user has already read part of an answer; retrying would repeat it.
        if (!m_streamTextEmitted) {
            onReplyReceived(QJsonDocument(m_streamError).toJson(QJsonDocument::Compact));
        } else {
            failTurn(m_streamError["error"].toObject()["message"].toString(
                QStringLiteral("The response stream was interrupted")));
        }
        return;
    }

    if (m_streamStopReason.isEmpty()) {
        failTurn(QStringLiteral("The response stream ended before the answer was complete"));
        return;
    }

    QJsonArray content;
    m_invalidToolInputs.clear();
    for (auto it = m_streamBlocks.begin(); it != m_streamBlocks.end(); ++it) {
        QJsonObject block = it.value();
        if (block["type"].toString() == QLatin1String("tool_use")) {
            const QByteArray json = m_streamPartialJson.value(it.key());
            const QJsonDocument doc = QJsonDocument::fromJson(json);
            if (json.trimmed().isEmpty() || doc.isObject()) {
                block["input"] = doc.object();
            } else {
                block["input"] = QJsonObject();
                m_invalidToolInputs.append(block["id"].toString());
            }
        }
        content.append(block);
    }

    QJsonObject response{{"content", content},
                         {"stop_reason", m_streamStopReason},
                         {"usage", m_streamUsage}};
    if (!m_streamStopDetails.isEmpty())
        response["stop_details"] = m_streamStopDetails;

    m_retryCount = 0;
    processResponse(response);
}

bool ClaudeProtocol::cancelTurn()
{
    if (!m_turnInProgress)
        return false;
    m_transport->abort();
    m_pendingTurns.clear();
    rollbackFailedTurn();
    emit requestFinished();
    drainQueue();
    return true;
}

void ClaudeProtocol::clearHistory()
{
    m_history = QJsonArray();
}

void ClaudeProtocol::replaceHistory(const QJsonArray& messages)
{
    m_history = messages;
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

    if (m_effort != Effort::Default && !m_effortUnsupportedModels.contains(m_model))
        body["output_config"] = QJsonObject{{"effort", effortName(m_effort)}};

    if (!m_toolSchemas.isEmpty()) {
        QJsonArray tools;
        for (const QJsonObject& schema : m_toolSchemas)
            tools.append(schema);
        body["tools"] = tools;
        if (!m_parallelToolCalls)
            body["tool_choice"] = QJsonObject{{"type", "auto"}, {"disable_parallel_tool_use", true}};
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
    if (!doc.isObject()) {
        // Typically an HTML error page from a proxy/gateway in front of the API.
        failTurn(QStringLiteral("Failed to parse API response (HTTP %1): %2")
                     .arg(m_transport->lastStatusCode())
                     .arg(QString::fromUtf8(data.left(200))));
        return;
    }

    QJsonObject root = doc.object();

    // Real Anthropic errors are {"error":{"message":...}}. Gateways/proxies in
    // front of the real API (Azure/Foundry-style deployments, LiteLLM, etc.)
    // often return a flatter {"detail": "..."} shape instead for the same
    // HTTP-error case - recognize both, otherwise the error body gets fed to
    // processResponse() as if it were a real message.
    if (root.contains("error") || root.contains("detail")) {
        const QJsonObject error = root["error"].toObject();
        const QString     type  = error["type"].toString();
        const int         http  = m_transport->lastStatusCode();

        // Transient by definition: the same request succeeds a moment later.
        const bool transient = type == QLatin1String("overloaded_error")
                            || type == QLatin1String("rate_limit_error")
                            || type == QLatin1String("api_error")
                            || http == 429 || http == 529 || (http >= 500 && http < 600);
        if (transient && m_retryCount < kMaxRetries) {
            ++m_retryCount;
            const int generation = m_turnGeneration;
            const int delayMs    = 2000 * m_retryCount * m_retryCount;   // 2 s, 8 s
            qDebug() << "ClaudeProtocol: transient API error" << type << http
                     << "- retry" << m_retryCount << "in" << delayMs << "ms";
            QTimer::singleShot(delayMs, this, [this, generation]() {
                if (generation == m_turnGeneration && m_turnInProgress)
                    sendRequest();
            });
            return;
        }

        QString message = error["message"].toString();

        // "Invalid `signature` in `thinking` block. The block is bound to a
        // different conversation." - see stripThinkingBlocks(). Once per turn:
        // if it still fails, the cause is something else.
        if (!m_thinkingStripped && message.contains(QLatin1String("thinking"))
            && message.contains(QLatin1String("signature"))) {
            m_thinkingStripped = true;
            qDebug() << "ClaudeProtocol: thinking blocks no longer match the conversation -"
                        " resending without them";
            stripThinkingBlocks();
            sendRequest();
            return;
        }

        // An endpoint that cannot stream: ask again for the whole answer.
        if (m_streamingEnabled && !m_streamUnsupported && message.contains(QLatin1String("stream"))) {
            m_streamUnsupported = true;
            qDebug() << "ClaudeProtocol: endpoint rejects streaming - resending without it";
            sendRequest();
            return;
        }

        // An older model (or a gateway) that does not know the effort
        // parameter: send the same request without it.
        if (m_effort != Effort::Default && !m_effortUnsupportedModels.contains(m_model)
            && (message.contains(QLatin1String("effort")) || message.contains(QLatin1String("output_config")))) {
            m_effortUnsupportedModels.append(m_model);
            qDebug() << "ClaudeProtocol: model" << m_model << "rejects the effort parameter - resending without it";
            sendRequest();
            return;
        }

        if (message.isEmpty())
            message = root["error"].toString();   // {"error":"..."} gateways
        if (message.isEmpty())
            message = root["detail"].toString();
        if (message.isEmpty())
            message = QStringLiteral("Request failed (no error message in response)");
        failTurn(message);
        return;
    }

    m_retryCount = 0;
    processResponse(root);
}

void ClaudeProtocol::failTurn(const QString& message)
{
    rollbackFailedTurn();
    emit errorOccurred(message);
    emit requestFinished();
    drainQueue();
}

void ClaudeProtocol::finishTurn(const QString& text)
{
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
    stats.contextTokens            = m_lastContextTokens;
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
}

void ClaudeProtocol::processResponse(const QJsonObject& responseJson)
{
    // Accumulate token usage from this response (may be one of several in a tool loop)
    QJsonObject usage = responseJson["usage"].toObject();
    m_turnInputTokens              += usage["input_tokens"].toInt();
    m_turnOutputTokens             += usage["output_tokens"].toInt();
    m_turnCacheCreationInputTokens += usage["cache_creation_input_tokens"].toInt();
    m_turnCacheReadInputTokens     += usage["cache_read_input_tokens"].toInt();
    // What this one request read plus what it wrote is the conversation's
    // size now.
    m_lastContextTokens = usage["input_tokens"].toInt() + usage["cache_creation_input_tokens"].toInt()
                        + usage["cache_read_input_tokens"].toInt() + usage["output_tokens"].toInt();

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

        // Continues in finishToolRound() once every handler has answered.
        executeToolCalls(toolUseBlocks, content);
        return;
    }

    // A server tool hit its internal iteration limit; re-sending the
    // conversation as it stands lets the model carry on.
    if (stopReason == "pause_turn") {
        if (++m_turnToolIterations > kMaxToolIterations) {
            failTurn(QStringLiteral("Turn did not complete (pause_turn limit reached)"));
            return;
        }
        if (!content.isEmpty()) {
            QJsonObject assistantMsg;
            assistantMsg["role"]    = "assistant";
            assistantMsg["content"] = content;
            m_history.append(assistantMsg);
        }
        sendRequest();
        return;
    }

    if (stopReason == "refusal") {
        // Nothing usable came back; keeping the prompt in the history would
        // only make the next request be declined for the same reason.
        const QJsonObject details = responseJson["stop_details"].toObject();
        QString message = QStringLiteral("The model declined this request");
        if (!details["category"].toString().isEmpty())
            message += QStringLiteral(" (%1)").arg(details["category"].toString());
        if (!details["explanation"].toString().isEmpty())
            message += QStringLiteral(": ") + details["explanation"].toString();
        failTurn(message);
        return;
    }

    if (stopReason == "end_turn" || stopReason == "max_tokens"
        || stopReason == "stop_sequence" || stopReason == "model_context_window_exceeded") {
        QString text = assembleText(content);

        if (stopReason != "end_turn") {
            // The output was cut off. A tool_use block left in the history
            // without a matching tool_result makes the API reject every later
            // request, so it must not be stored.
            QJsonArray kept;
            for (const QJsonValue& val : content) {
                if (val.toObject()["type"].toString() != "tool_use")
                    kept.append(val);
            }
            content = kept;
            if (stopReason != "stop_sequence") {
                if (!text.isEmpty())
                    text += QStringLiteral("\n\n");
                text += (stopReason == "max_tokens")
                    ? QStringLiteral("[Response truncated: max_tokens (%1) reached]").arg(m_maxTokens)
                    : QStringLiteral("[Response truncated: context window is full - clear the conversation]");
            }
        }

        // An assistant message with empty content is rejected on the next
        // request; two consecutive user messages are not.
        if (!content.isEmpty()) {
            QJsonObject assistantMsg;
            assistantMsg["role"]    = "assistant";
            assistantMsg["content"] = content;
            m_history.append(assistantMsg);
        }

        finishTurn(text);
        return;
    }

    // Defense in depth: an unrecognized/empty stop_reason (a response shape
    // this client doesn't know about, from some future API change or a
    // nonstandard gateway) must still end the turn - silently doing nothing
    // here would leave m_turnInProgress stuck true forever, wedging every
    // subsequent sendPrompt() into the queue with no way out.
    failTurn(QStringLiteral("Unexpected API response (stop_reason=\"%1\"): %2")
        .arg(stopReason, QString::fromUtf8(QJsonDocument(responseJson).toJson(QJsonDocument::Compact)).left(500)));
}

void ClaudeProtocol::executeToolCalls(const QJsonArray& toolUseBlocks, const QJsonArray& content)
{
    auto round = QSharedPointer<ToolRound>::create();
    const int count = toolUseBlocks.size();
    round->results.resize(count);
    round->done = QVector<bool>(count, false);
    round->pending = count;
    round->generation = m_turnGeneration;
    round->content = content;
    if (count == 0) {
        finishToolRound(round);
        return;
    }

    // A handler may run a nested event loop (a dialog) in which the turn is
    // cancelled or this object destroyed; every step re-checks both.
    QPointer<ClaudeProtocol> self(this);

    for (int i = 0; i < count; ++i) {
        if (!self || round->generation != m_turnGeneration)
            return;

        QJsonObject block     = toolUseBlocks[i].toObject();
        QString     toolName  = block["name"].toString();
        QJsonObject toolInput = block["input"].toObject();
        QString     toolId    = block["id"].toString();

        // Stores one tool_result; the last one completes the round.
        auto complete = [this, self, round, i](const QJsonObject& toolResult) {
            if (!self || round->generation != m_turnGeneration || round->done[i])
                return;
            round->done[i] = true;
            round->results[i] = toolResult;
            if (--round->pending == 0)
                finishToolRound(round);
        };
        auto errorBlock = [toolId](const QString& text) {
            return QJsonObject{{"type", "tool_result"}, {"tool_use_id", toolId},
                               {"content", text}, {"is_error", true}};
        };

        if (!m_toolHandlers.contains(toolName)) {
            // Reported to the model only - it can recover by picking a real
            // tool. This is not a failure of the turn.
            qWarning() << "ClaudeProtocol: model called unknown tool" << toolName;
            QStringList available;
            for (const QJsonObject& schema : m_toolSchemas)
                available.append(schema["name"].toString());
            complete(errorBlock(QStringLiteral("Error: unknown tool \"%1\". Available tools: %2")
                                    .arg(toolName, available.join(", "))));
            continue;
        }

        if (m_invalidToolInputs.contains(toolId)) {
            complete(errorBlock(QStringLiteral(
                "Error: the input for this call did not arrive as valid JSON "
                "(the response was probably cut off). Call the tool again.")));
            continue;
        }

        ++m_turnToolCalls;
        emit toolInvoked(toolName, toolInput);

        auto done = [this, self, round, complete, toolName, toolId](const QJsonObject& handlerResult) {
            if (!self || round->generation != m_turnGeneration)
                return;
            QJsonObject result = handlerResult;
            // Pictures travel as image blocks, not as base64 inside the JSON text.
            const QJsonArray images = toolTakeImages(result);
            emit toolCompleted(toolName, result);

            QString resultStr = QString::fromUtf8(
                QJsonDocument(result).toJson(QJsonDocument::Compact));

            QJsonObject toolResult;
            toolResult["type"]        = "tool_result";
            toolResult["tool_use_id"] = toolId;
            if (images.isEmpty()) {
                toolResult["content"] = resultStr;
            } else {
                QJsonArray blocks{QJsonObject{{"type", "text"}, {"text", resultStr}}};
                for (const QJsonValue& image : images) {
                    const QJsonObject img = image.toObject();
                    blocks.append(QJsonObject{
                        {"type", "image"},
                        {"source", QJsonObject{{"type", "base64"},
                                               {"media_type", img["media_type"]},
                                               {"data", img["data"]}}}});
                }
                toolResult["content"] = blocks;
            }
            if (result["status"].toString() == "error")
                toolResult["is_error"] = true;
            complete(toolResult);
        };

        // A copy: setTools() may replace the map while the handler runs.
        const ToolCallHandler handler = m_toolHandlers.value(toolName);
        // A throwing handler must not unwind through the network callback
        // chain — convert to an is_error tool result and keep the turn alive.
        try {
            handler(toolInput, done);
        } catch (const std::exception& e) {
            done(QJsonObject{{"status", "error"},
                             {"message", QString("Internal error: %1").arg(e.what())}});
        } catch (...) {
            done(QJsonObject{{"status", "error"},
                             {"message", "Internal error: unknown exception in tool handler"}});
        }
    }
}

void ClaudeProtocol::finishToolRound(const QSharedPointer<ToolRound>& round)
{
    QJsonArray toolResults;
    for (const QJsonObject& result : round->results)
        toolResults.append(result);

    QJsonObject userMsg;
    userMsg["role"]    = "user";
    userMsg["content"] = toolResults;
    m_history.append(userMsg);

    if (++m_turnToolIterations > kMaxToolIterations) {
        // Not an error for the caller: the turn ends normally, with a note.
        QString capText = assembleText(round->content);
        if (!capText.isEmpty())
            capText += QStringLiteral("\n\n");
        capText += QStringLiteral("[Stopped: tool-use iteration limit (%1) reached]")
                       .arg(kMaxToolIterations);
        finishTurn(capText);
        return;
    }

    sendRequest();
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
    // Unique per call: ids repeated across turns are rejected by the API.
    block["id"]    = QStringLiteral("fallback_") + QUuid::createUuid().toString(QUuid::Id128);
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
    failTurn(message);
}

void ClaudeProtocol::stripThinkingBlocks()
{
    QJsonArray stripped;
    int removedBeforeTurn = 0;
    for (int i = 0; i < m_history.size(); ++i) {
        QJsonObject message = m_history[i].toObject();
        if (message["role"].toString() == QLatin1String("assistant") && message["content"].isArray()) {
            QJsonArray kept;
            for (const QJsonValue& block : message["content"].toArray()) {
                const QString type = block.toObject()["type"].toString();
                if (type != QLatin1String("thinking") && type != QLatin1String("redacted_thinking"))
                    kept.append(block);
            }
            // A message that held nothing but thinking cannot stay: empty
            // assistant content is rejected.
            if (kept.isEmpty()) {
                if (i < m_historyLenBeforeTurn)
                    ++removedBeforeTurn;
                continue;
            }
            message["content"] = kept;
        }
        stripped.append(message);
    }
    m_history = stripped;
    m_historyLenBeforeTurn -= removedBeforeTurn;   // keeps rollback aligned
}

void ClaudeProtocol::rollbackFailedTurn()
{
    while (m_history.size() > m_historyLenBeforeTurn)
        m_history.removeAt(m_history.size() - 1);
}

void ClaudeProtocol::fetchModels()
{
    // A second call while one is in flight (Client's startup validation racing
    // an app-triggered fetch) would be refused by the busy transport.
    if (m_modelsFetchInProgress)
        return;
    m_modelsFetchInProgress = true;

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
    const QPair<QByteArray, QByteArray> auth = authHeader();
    if (!auth.first.isEmpty())
        headers.append(auth);
    headers.append({QByteArray("anthropic-version"), QByteArray("2023-06-01")});
    m_modelsTransport->get(modelsUrl, headers);
}

// Listing models is a side channel, not a turn: failures are logged and
// reported as an empty list. Routing them through errorOccurred() made a
// models endpoint the deployment simply does not have (Foundry-style gateways)
// look like a failed chat request.
void ClaudeProtocol::onModelsReplyReceived(const QByteArray& data)
{
    m_modelsFetchInProgress = false;

    const QJsonObject root = QJsonDocument::fromJson(data).object();
    QStringList models;
    for (const QJsonValue& val : root["data"].toArray()) {
        QString id = val.toObject()["id"].toString();
        if (!id.isEmpty())
            models.append(id);
    }
    if (models.isEmpty())
        qWarning() << "ClaudeProtocol: model list unavailable:" << data.left(200);
    emit modelsFetched(models);
}

void ClaudeProtocol::onModelsTransportError(const QString& message)
{
    m_modelsFetchInProgress = false;
    qWarning() << "ClaudeProtocol: model list unavailable:" << message;
    emit modelsFetched({});
}

} // namespace QtLLM
