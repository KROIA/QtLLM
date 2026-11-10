#include "OpenAiProtocol.h"
#include "ToolResult.h"
#include "Pricing.h"
#include <QJsonDocument>
#include <QDebug>
#include <QPointer>
#include <QTimer>
#include <QUuid>

namespace QtLLM {

namespace {

// {"media_type","data"} (toolTakeImages() format) -> a Chat Completions image part.
QJsonObject imagePart(const QJsonObject& image)
{
    const QString url = QStringLiteral("data:%1;base64,%2")
                            .arg(image["media_type"].toString(), image["data"].toString());
    return QJsonObject{{"type", "image_url"}, {"image_url", QJsonObject{{"url", url}}}};
}

}

OpenAiProtocol::OpenAiProtocol(const QString& url, const QString& apiKey, QObject* parent)
    : ProtocolBase(parent)
    , m_apiKey(apiKey)
    , m_model("gpt-4o-mini")
    , m_maxTokens(4096)
    , m_transport(new HttpTransport(this))
    , m_modelsTransport(new HttpTransport(this))
{
    setUrl(url);
    // Local servers load the model on the first request and answer nothing
    // until then; with streaming this is an idle timeout.
    m_transport->setTimeoutMs(300000);

    connect(m_transport, &HttpTransport::replyReceived,
            this, &OpenAiProtocol::onReplyReceived);
    connect(m_transport, &HttpTransport::errorOccurred,
            this, &OpenAiProtocol::onTransportError);
    connect(m_transport, &HttpTransport::chunkReceived,
            this, &OpenAiProtocol::onStreamChunk);
    connect(m_transport, &HttpTransport::streamFinished,
            this, &OpenAiProtocol::onStreamFinished);
    connect(m_modelsTransport, &HttpTransport::replyReceived,
            this, &OpenAiProtocol::onModelsReplyReceived);
    connect(m_modelsTransport, &HttpTransport::errorOccurred,
            this, &OpenAiProtocol::onModelsTransportError);
}

OpenAiProtocol::~OpenAiProtocol() = default;

void OpenAiProtocol::setModel(const QString& model)
{
    m_model = model;
}

void OpenAiProtocol::setMaxTokens(int maxTokens)
{
    m_maxTokens = maxTokens;
}

void OpenAiProtocol::setSystemPrompt(const QString& systemPrompt)
{
    m_systemPrompt = systemPrompt;
}

void OpenAiProtocol::setApiKey(const QString& apiKey)
{
    m_apiKey = apiKey;
}

void OpenAiProtocol::setUrl(const QString& url)
{
    QString base = url.trimmed();
    while (base.endsWith('/'))
        base.chop(1);
    if (base.endsWith(QLatin1String("/chat/completions")))
        base.chop(17);
    // "http://localhost:1234" is how LM Studio and Ollama show their address.
    if (QUrl(base).path().isEmpty())
        base += QStringLiteral("/v1");
    m_baseUrl = base;
    m_rejectedFields.clear();
}

void OpenAiProtocol::setTools(const QList<QJsonObject>& toolSchemas,
                              const QMap<QString, ToolCallHandler>& handlers)
{
    m_toolSchemas  = toolSchemas;
    m_toolHandlers = handlers;
}

void OpenAiProtocol::beginTurn(const QString& userMessage)
{
    beginTurn(userMessage, QJsonArray());
}

void OpenAiProtocol::beginTurn(const QString& userMessage, const QJsonArray& images)
{
    QJsonObject msg{{"role", "user"}};
    if (images.isEmpty()) {
        msg["content"] = userMessage;
    } else {
        QJsonArray parts{QJsonObject{{"type", "text"}, {"text", userMessage}}};
        for (const QJsonValue& image : images)
            parts.append(imagePart(image.toObject()));
        msg["content"] = parts;
    }

    if (m_turnInProgress) {
        m_pendingTurns.enqueue(msg);
        return;
    }
    startNewTurn(msg);
}

void OpenAiProtocol::startNewTurn(const QJsonObject& userMessage)
{
    m_turnInProgress       = true;
    m_turnInputTokens      = 0;
    m_turnOutputTokens     = 0;
    m_turnToolCalls        = 0;
    m_turnToolIterations   = 0;
    m_retryCount           = 0;
    m_turnTimerStarted     = false;
    m_historyLenBeforeTurn = m_history.size();

    m_history.append(userMessage);
    sendRequest();
}

void OpenAiProtocol::drainQueue()
{
    m_turnInProgress = false;
    ++m_turnGeneration;
    if (!m_pendingTurns.isEmpty())
        startNewTurn(m_pendingTurns.dequeue());
}

QList<QPair<QByteArray, QByteArray>> OpenAiProtocol::requestHeaders() const
{
    QList<QPair<QByteArray, QByteArray>> list;
    if (!m_apiKey.isEmpty())
        list.append({QByteArray("Authorization"), "Bearer " + m_apiKey.toUtf8()});
    return list;
}

QJsonObject OpenAiProtocol::buildRequestBody() const
{
    QJsonArray messages;
    if (!m_systemPrompt.isEmpty())
        messages.append(QJsonObject{{"role", "system"}, {"content", m_systemPrompt}});
    for (const QJsonValue& val : m_history)
        messages.append(val);

    QJsonObject body;
    body["model"]    = m_model;
    body["messages"] = messages;
    // Newer OpenAI models reject max_tokens and ask for max_completion_tokens.
    if (m_rejectedFields.contains(QLatin1String("max_tokens")))
        body["max_completion_tokens"] = m_maxTokens;
    else
        body["max_tokens"] = m_maxTokens;

    if (!m_toolSchemas.isEmpty()) {
        QJsonArray tools;
        for (const QJsonObject& schema : m_toolSchemas)
            tools.append(schema);
        body["tools"] = tools;
        if (!m_parallelToolCalls && !m_rejectedFields.contains(QLatin1String("parallel_tool_calls")))
            body["parallel_tool_calls"] = false;
    }

    if (m_effort != Effort::Default && !m_rejectedFields.contains(QLatin1String("reasoning_effort"))) {
        const char* effort = m_effort == Effort::Low    ? "low"
                           : m_effort == Effort::Medium ? "medium"
                                                        : "high";
        body["reasoning_effort"] = QLatin1String(effort);
    }

    body["stream"] = m_streamingEnabled;
    if (m_streamingEnabled && !m_rejectedFields.contains(QLatin1String("stream_options")))
        body["stream_options"] = QJsonObject{{"include_usage", true}};
    return body;
}

void OpenAiProtocol::sendRequest()
{
    const QByteArray bytes = QJsonDocument(buildRequestBody()).toJson(QJsonDocument::Compact);

    if (!m_turnTimerStarted) {
        m_turnTimer.start();
        m_turnTimerStarted = true;
    }

    const QUrl url(m_baseUrl + QStringLiteral("/chat/completions"));
    emit requestStarted();
    if (m_streamingEnabled) {
        resetStreamState();
        m_transport->postStream(url, bytes, requestHeaders());
    } else {
        m_transport->post(url, bytes, requestHeaders());
    }
}

void OpenAiProtocol::resetStreamState()
{
    m_streamBuffer.clear();
    m_streamSniffed = false;
    m_streamIsPlainJson = false;
    m_streamContent.clear();
    m_streamCalls.clear();
    m_streamUsage = QJsonObject();
    m_streamFinishReason.clear();
    m_streamError.clear();
    m_streamDone = false;
}

void OpenAiProtocol::onStreamChunk(const QByteArray& data)
{
    m_streamBuffer += data;

    if (!m_streamSniffed) {
        // A server that ignores "stream" answers with one plain JSON object.
        const QByteArray head = m_streamBuffer.trimmed();
        if (head.isEmpty())
            return;
        m_streamSniffed = true;
        m_streamIsPlainJson = head.startsWith('{');
    }
    if (m_streamIsPlainJson)
        return;   // handled as a whole in onStreamFinished()

    // Only complete lines leave the buffer: a chunk may end mid-line.
    int end;
    while ((end = m_streamBuffer.indexOf('\n')) >= 0) {
        const QByteArray line = m_streamBuffer.left(end).trimmed();
        m_streamBuffer.remove(0, end + 1);
        handleStreamLine(line);
    }
}

void OpenAiProtocol::handleStreamLine(const QByteArray& line)
{
    if (!line.startsWith("data:"))
        return;   // blank separators, ": keep-alive" comments, "event:" lines
    const QByteArray payload = line.mid(5).trimmed();
    if (payload == "[DONE]") {
        m_streamDone = true;
        return;
    }

    const QJsonObject obj = QJsonDocument::fromJson(payload).object();
    if (obj.contains("error")) {
        const QJsonValue error = obj["error"];
        m_streamError = error.isObject() ? error.toObject()["message"].toString() : error.toString();
        if (m_streamError.isEmpty())
            m_streamError = QStringLiteral("The response stream reported an error");
        return;
    }
    if (obj["usage"].isObject())
        m_streamUsage = obj["usage"].toObject();

    const QJsonArray choices = obj["choices"].toArray();
    if (choices.isEmpty())
        return;
    const QJsonObject choice = choices[0].toObject();
    const QJsonObject delta  = choice["delta"].toObject();

    const QString text = delta["content"].toString();
    if (!text.isEmpty()) {
        m_streamContent += text;
        emit responseDelta(text);
    }

    for (const QJsonValue& val : delta["tool_calls"].toArray()) {
        const QJsonObject call = val.toObject();
        const QJsonObject fn   = call["function"].toObject();
        // Some servers leave out "index": a fragment with an id starts a new call.
        int index;
        if (call.contains("index"))
            index = call["index"].toInt();
        else if (call["id"].toString().isEmpty() && !m_streamCalls.isEmpty())
            index = m_streamCalls.lastKey();
        else
            index = m_streamCalls.size();

        StreamCall& target = m_streamCalls[index];
        if (!call["id"].toString().isEmpty())
            target.id = call["id"].toString();
        if (!fn["name"].toString().isEmpty())
            target.name = fn["name"].toString();
        const QJsonValue args = fn["arguments"];
        target.arguments += args.isObject()
            ? QString::fromUtf8(QJsonDocument(args.toObject()).toJson(QJsonDocument::Compact))
            : args.toString();
    }

    if (choice["finish_reason"].isString())
        m_streamFinishReason = choice["finish_reason"].toString();
}

void OpenAiProtocol::onStreamFinished()
{
    if (!m_turnInProgress)
        return;

    if (m_streamIsPlainJson) {
        onReplyReceived(m_streamBuffer);
        return;
    }

    // A last line without a trailing newline.
    const QByteArray rest = m_streamBuffer.trimmed();
    m_streamBuffer.clear();
    if (!rest.isEmpty())
        handleStreamLine(rest);

    if (!m_streamError.isEmpty()) {
        failTurn(m_streamError);
        return;
    }
    if (!m_streamDone && m_streamFinishReason.isEmpty()) {
        failTurn(QStringLiteral("The response stream ended before the answer was complete"));
        return;
    }

    QJsonArray toolCalls;
    for (const StreamCall& call : m_streamCalls) {
        toolCalls.append(QJsonObject{
            {"id", call.id},
            {"type", "function"},
            {"function", QJsonObject{{"name", call.name}, {"arguments", call.arguments}}}});
    }
    QJsonObject message{{"role", "assistant"}, {"content", m_streamContent}};
    if (!toolCalls.isEmpty())
        message["tool_calls"] = toolCalls;

    m_retryCount = 0;
    processResponse(QJsonObject{
        {"choices", QJsonArray{QJsonObject{{"message", message},
                                           {"finish_reason", m_streamFinishReason}}}},
        {"usage", m_streamUsage}});
}

void OpenAiProtocol::onReplyReceived(const QByteArray& data)
{
    const int           status = m_transport->lastStatusCode();
    const QJsonDocument doc    = QJsonDocument::fromJson(data);
    const QJsonObject   root   = doc.object();

    if (status >= 400 || root.contains("error")) {
        handleError(status, root, data);
        return;
    }
    if (!doc.isObject()) {
        failTurn(QStringLiteral("Failed to parse API response (HTTP %1): %2")
                     .arg(status)
                     .arg(QString::fromUtf8(data.left(200))));
        return;
    }

    m_retryCount = 0;
    processResponse(root);
}

void OpenAiProtocol::handleError(int status, const QJsonObject& root, const QByteArray& data)
{
    const QJsonValue error = root["error"];
    QString message = error.isObject() ? error.toObject()["message"].toString() : error.toString();
    if (message.isEmpty())
        message = root["message"].toString();
    if (message.isEmpty())
        message = root["detail"].toString();
    if (message.isEmpty())
        message = QString::fromUtf8(data.left(200)).trimmed();

    if ((status == 429 || (status >= 500 && status < 600)) && m_retryCount < kMaxRetries) {
        ++m_retryCount;
        const int generation = m_turnGeneration;
        const int delayMs    = 2000 * m_retryCount * m_retryCount;   // 2 s, 8 s
        qDebug() << "OpenAiProtocol: transient error" << status << "- retry"
                 << m_retryCount << "in" << delayMs << "ms";
        QTimer::singleShot(delayMs, this, [this, generation]() {
            if (generation == m_turnGeneration && m_turnInProgress)
                sendRequest();
        });
        return;
    }

    // An optional field this server does not know: leave it out from now on.
    if (status == 400) {
        const QJsonObject sent = buildRequestBody();
        for (const char* field : {"stream_options", "reasoning_effort", "parallel_tool_calls"}) {
            if (sent.contains(QLatin1String(field)) && message.contains(QLatin1String(field))) {
                m_rejectedFields.append(QLatin1String(field));
                qDebug() << "OpenAiProtocol: endpoint rejects" << field << "- resending without it";
                sendRequest();
                return;
            }
        }
        if (sent.contains(QLatin1String("max_tokens")) && message.contains(QLatin1String("max_completion_tokens"))) {
            m_rejectedFields.append(QStringLiteral("max_tokens"));
            sendRequest();
            return;
        }
    }

    if (message.isEmpty())
        message = QStringLiteral("Request failed (no error message in response)");
    failTurn(status > 0 ? QStringLiteral("HTTP %1: %2").arg(status).arg(message) : message);
}

void OpenAiProtocol::processResponse(const QJsonObject& responseJson)
{
    const QJsonObject usage = responseJson["usage"].toObject();
    const int promptTokens     = usage["prompt_tokens"].toInt();
    const int completionTokens = usage["completion_tokens"].toInt();
    m_turnInputTokens  += promptTokens;
    m_turnOutputTokens += completionTokens;
    m_lastContextTokens = promptTokens + completionTokens;

    const QJsonArray choices = responseJson["choices"].toArray();
    if (choices.isEmpty()) {
        failTurn(QStringLiteral("Unexpected API response: %1")
                     .arg(QString::fromUtf8(QJsonDocument(responseJson).toJson(QJsonDocument::Compact)).left(500)));
        return;
    }
    const QJsonObject choice  = choices[0].toObject();
    const QJsonObject message = choice["message"].toObject();
    const QString     finish  = choice["finish_reason"].toString();
    QString           text    = message["content"].toString();
    const QJsonArray  toolCalls = message["tool_calls"].toArray();

    if (finish == QLatin1String("content_filter")) {
        failTurn(QStringLiteral("The answer was blocked by the provider's content filter"));
        return;
    }

    // Decided by the calls themselves: some servers report "stop" with them.
    // A cut-off ("length") call is dropped - its arguments are incomplete.
    if (!toolCalls.isEmpty() && finish != QLatin1String("length")) {
        QJsonArray calls;
        for (const QJsonValue& val : toolCalls) {
            QJsonObject call = val.toObject();
            // A tool message must name its call; not every server sets an id.
            if (call["id"].toString().isEmpty())
                call["id"] = QStringLiteral("call_") + QUuid::createUuid().toString(QUuid::Id128).left(24);
            call["type"] = "function";
            QJsonObject fn = call["function"].toObject();
            if (fn["arguments"].isObject())
                fn["arguments"] = QString::fromUtf8(QJsonDocument(fn["arguments"].toObject()).toJson(QJsonDocument::Compact));
            call["function"] = fn;
            calls.append(call);
        }
        // null, not "": some servers reject an empty string next to tool_calls.
        m_history.append(QJsonObject{{"role", "assistant"},
                                     {"content", text.isEmpty() ? QJsonValue() : QJsonValue(text)},
                                     {"tool_calls", calls}});
        // Continues in finishToolRound() once every handler has answered.
        executeToolCalls(calls, text);
        return;
    }

    m_history.append(QJsonObject{{"role", "assistant"}, {"content", text}});
    if (finish == QLatin1String("length")) {
        if (!text.isEmpty())
            text += QStringLiteral("\n\n");
        text += QStringLiteral("[Response truncated: max_tokens (%1) reached]").arg(m_maxTokens);
    }
    finishTurn(text);
}

void OpenAiProtocol::executeToolCalls(const QJsonArray& toolCalls, const QString& text)
{
    auto round = QSharedPointer<ToolRound>::create();
    const int count = toolCalls.size();
    round->results.resize(count);
    round->images.resize(count);
    round->done = QVector<bool>(count, false);
    round->pending = count;
    round->generation = m_turnGeneration;
    round->text = text;

    // A handler may run a nested event loop in which the turn is cancelled or
    // this object destroyed; every step re-checks both.
    QPointer<OpenAiProtocol> self(this);

    for (int i = 0; i < count; ++i) {
        if (!self || round->generation != m_turnGeneration)
            return;

        const QJsonObject call     = toolCalls[i].toObject();
        const QJsonObject fn       = call["function"].toObject();
        const QString     toolName = fn["name"].toString();
        const QString     toolId   = call["id"].toString();

        auto complete = [this, self, round, i, toolId](const QString& content, const QJsonArray& images) {
            if (!self || round->generation != m_turnGeneration || round->done[i])
                return;
            round->done[i] = true;
            round->results[i] = QJsonObject{{"role", "tool"}, {"tool_call_id", toolId}, {"content", content}};
            round->images[i] = images;
            if (--round->pending == 0)
                finishToolRound(round);
        };

        if (!m_toolHandlers.contains(toolName)) {
            // Reported to the model only - it can recover by picking a real tool.
            qWarning() << "OpenAiProtocol: model called unknown tool" << toolName;
            QStringList available;
            for (const QJsonObject& schema : m_toolSchemas)
                available.append(schema["function"].toObject()["name"].toString());
            complete(QStringLiteral("Error: unknown tool \"%1\". Available tools: %2")
                         .arg(toolName, available.join(", ")), QJsonArray());
            continue;
        }

        QJsonObject toolInput;
        const QByteArray args = fn["arguments"].toString().toUtf8();
        if (!args.trimmed().isEmpty()) {
            const QJsonDocument argDoc = QJsonDocument::fromJson(args);
            if (!argDoc.isObject()) {
                complete(QStringLiteral("Error: the arguments of this call were not valid JSON. "
                                        "Call the tool again with a valid JSON object as arguments."),
                         QJsonArray());
                continue;
            }
            toolInput = argDoc.object();
        }

        ++m_turnToolCalls;
        emit toolInvoked(toolName, toolInput);

        auto done = [this, self, round, complete, toolName](const QJsonObject& handlerResult) {
            if (!self || round->generation != m_turnGeneration)
                return;
            QJsonObject result = handlerResult;
            const QJsonArray images = toolTakeImages(result);
            emit toolCompleted(toolName, result);
            complete(QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact)), images);
        };

        // A copy: setTools() may replace the map while the handler runs.
        const ToolCallHandler handler = m_toolHandlers.value(toolName);
        // A throwing handler must not unwind through the network callback chain.
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

void OpenAiProtocol::finishToolRound(const QSharedPointer<ToolRound>& round)
{
    QJsonArray imageParts;
    for (int i = 0; i < round->results.size(); ++i) {
        m_history.append(round->results[i]);
        for (const QJsonValue& image : round->images[i])
            imageParts.append(imagePart(image.toObject()));
    }
    // Tool messages carry text only; pictures follow as a user message.
    if (!imageParts.isEmpty()) {
        imageParts.prepend(QJsonObject{{"type", "text"},
                                       {"text", "Pictures returned by the tool calls above."}});
        m_history.append(QJsonObject{{"role", "user"}, {"content", imageParts}});
    }

    if (++m_turnToolIterations > kMaxToolIterations) {
        QString text = round->text;
        if (!text.isEmpty())
            text += QStringLiteral("\n\n");
        text += QStringLiteral("[Stopped: tool-use iteration limit (%1) reached]")
                    .arg(kMaxToolIterations);
        finishTurn(text);
        return;
    }

    sendRequest();
}

bool OpenAiProtocol::cancelTurn()
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

void OpenAiProtocol::failTurn(const QString& message)
{
    rollbackFailedTurn();
    emit errorOccurred(message);
    emit requestFinished();
    drainQueue();
}

void OpenAiProtocol::finishTurn(const QString& text)
{
    m_sessionInputTokens  += m_turnInputTokens;
    m_sessionOutputTokens += m_turnOutputTokens;
    m_sessionToolCalls    += m_turnToolCalls;
    ++m_sessionTurnCount;
    m_sessionCostUsd += PricingRegistry::instance().estimateCostUsd(
        m_model, m_turnInputTokens, m_turnOutputTokens);

    UsageStats stats;
    stats.inputTokens         = m_turnInputTokens;
    stats.outputTokens        = m_turnOutputTokens;
    stats.toolCalls           = m_turnToolCalls;
    stats.durationMs          = m_turnTimer.elapsed();
    stats.contextTokens       = m_lastContextTokens;
    stats.sessionInputTokens  = m_sessionInputTokens;
    stats.sessionOutputTokens = m_sessionOutputTokens;
    stats.sessionToolCalls    = m_sessionToolCalls;
    stats.sessionTurnCount    = m_sessionTurnCount;
    stats.sessionCostUsd      = m_sessionCostUsd;

    emit requestFinished();
    emit responseReady(text);
    emit statsReady(stats);
    drainQueue();
}

void OpenAiProtocol::rollbackFailedTurn()
{
    while (m_history.size() > m_historyLenBeforeTurn)
        m_history.removeAt(m_history.size() - 1);
}

void OpenAiProtocol::onTransportError(const QString& message)
{
    failTurn(message);
}

void OpenAiProtocol::clearHistory()
{
    m_history = QJsonArray();
}

void OpenAiProtocol::replaceHistory(const QJsonArray& messages)
{
    m_history = messages;
}

void OpenAiProtocol::clearStats()
{
    m_sessionInputTokens  = 0;
    m_sessionOutputTokens = 0;
    m_sessionToolCalls    = 0;
    m_sessionTurnCount    = 0;
    m_sessionCostUsd      = 0.0;
}

QJsonArray OpenAiProtocol::conversationMessages() const
{
    return m_history;
}

void OpenAiProtocol::fetchModels()
{
    // A second call while one is in flight would be refused by the busy transport.
    if (m_modelsFetchInProgress)
        return;
    m_modelsFetchInProgress = true;
    m_modelsTransport->get(QUrl(m_baseUrl + QStringLiteral("/models")), requestHeaders());
}

// A side channel, not a turn: failures are logged and reported as an empty list.
void OpenAiProtocol::onModelsReplyReceived(const QByteArray& data)
{
    m_modelsFetchInProgress = false;
    QStringList models;
    for (const QJsonValue& val : QJsonDocument::fromJson(data).object()["data"].toArray()) {
        const QString id = val.toObject()["id"].toString();
        if (!id.isEmpty())
            models.append(id);
    }
    if (models.isEmpty())
        qWarning() << "OpenAiProtocol: model list unavailable:" << data.left(200);
    emit modelsFetched(models);
}

void OpenAiProtocol::onModelsTransportError(const QString& message)
{
    m_modelsFetchInProgress = false;
    qWarning() << "OpenAiProtocol: model list unavailable:" << message;
    emit modelsFetched({});
}

} // namespace QtLLM
