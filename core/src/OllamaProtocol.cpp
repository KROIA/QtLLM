#include "OllamaProtocol.h"
#include "ToolResult.h"
#include <QJsonDocument>
#include <QDebug>
#include <QPointer>

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
    , m_modelsTransport(new HttpTransport(this))
    , m_capabilityTransport(new HttpTransport(this))
{
    // Local generation (esp. loading a large model into memory/VRAM on the
    // first request, or a long prompt with many tool schemas) can easily
    // exceed the 60s default that's tuned for cloud APIs - Ollama's
    // non-streaming /api/chat doesn't reply until generation is complete.
    m_transport->setTimeoutMs(300000);

    connect(m_transport, &HttpTransport::replyReceived,
            this, &OllamaProtocol::onReplyReceived);
    connect(m_transport, &HttpTransport::errorOccurred,
            this, &OllamaProtocol::onTransportError);
    connect(m_transport, &HttpTransport::chunkReceived,
            this, &OllamaProtocol::onStreamChunk);
    connect(m_transport, &HttpTransport::streamFinished,
            this, &OllamaProtocol::onStreamFinished);
    connect(m_capabilityTransport, &HttpTransport::replyReceived,
            this, &OllamaProtocol::onCapabilityReplyReceived);
    connect(m_capabilityTransport, &HttpTransport::errorOccurred,
            this, &OllamaProtocol::onCapabilityTransportError);
    connect(m_modelsTransport, &HttpTransport::replyReceived,
            this, &OllamaProtocol::onModelsReplyReceived);
    connect(m_modelsTransport, &HttpTransport::errorOccurred,
            this, &OllamaProtocol::onModelsTransportError);
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

void OllamaProtocol::setUrl(const QUrl& url)
{
    m_url = url;
}

void OllamaProtocol::setTools(const QList<QJsonObject>& toolSchemas,
                               const QMap<QString, ToolCallHandler>& handlers)
{
    m_toolSchemas  = toolSchemas;
    m_toolHandlers = handlers;
}

void OllamaProtocol::beginTurn(const QString& userMessage)
{
    beginTurn(userMessage, QJsonArray());
}

void OllamaProtocol::beginTurn(const QString& userMessage, const QJsonArray& images)
{
    QJsonObject msg;
    msg["role"]    = "user";
    msg["content"] = userMessage;
    if (!images.isEmpty()) {
        // ponytail: sent to whatever model is selected. One without vision
        // ignores or rejects the pictures; checking /api/show for the
        // "vision" capability first is the upgrade.
        QJsonArray data;
        for (const QJsonValue& value : images)
            data.append(value.toObject()["data"]);
        msg["images"] = data;
    }

    if (m_turnInProgress) {
        m_pendingTurns.enqueue(msg);
        return;
    }
    startNewTurn(msg);
}

void OllamaProtocol::startNewTurn(const QJsonObject& userMessage)
{
    m_turnInProgress   = true;
    m_turnInputTokens  = 0;
    m_turnOutputTokens = 0;
    m_turnToolCalls    = 0;
    m_turnTimerStarted = false;
    m_turnToolIterations = 0;
    m_historyLenBeforeTurn = m_history.size();

    m_history.append(userMessage);
    sendRequest();
}

void OllamaProtocol::drainQueue()
{
    m_turnInProgress = false;
    ++m_turnGeneration;
    if (!m_pendingTurns.isEmpty())
        startNewTurn(m_pendingTurns.dequeue());
}

void OllamaProtocol::sendRequest()
{
    QJsonObject body  = buildRequestBody();
    body["stream"] = m_streamingEnabled;
    QByteArray  bytes = QJsonDocument(body).toJson(QJsonDocument::Compact);

    if (!m_turnTimerStarted) {
        m_turnTimer.start();
        m_turnTimerStarted = true;
    }

    // m_url is treated as the Ollama server's base URL everywhere else
    // (fetchModels() rewrites it to /api/tags) - do the same here instead of
    // trusting the caller to have appended /api/chat themselves, since e.g.
    // SettingsDialog's Ollama URL field is just the base address.
    QUrl chatUrl = m_url;
    chatUrl.setPath("/api/chat");

    emit requestStarted();
    if (m_streamingEnabled) {
        m_streamBuffer.clear();
        m_streamContent.clear();
        m_streamToolCalls = QJsonArray();
        m_streamFinal = QJsonObject();
        m_streamError.clear();
        m_transport->postStream(chatUrl, bytes, {});
    } else {
        m_transport->post(chatUrl, bytes, {});
    }
}

void OllamaProtocol::onStreamChunk(const QByteArray& data)
{
    m_streamBuffer += data;
    int end;
    while ((end = m_streamBuffer.indexOf('\n')) >= 0) {
        const QByteArray line = m_streamBuffer.left(end).trimmed();
        m_streamBuffer.remove(0, end + 1);
        if (!line.isEmpty())
            handleStreamLine(line);
    }
}

void OllamaProtocol::handleStreamLine(const QByteArray& line)
{
    const QJsonObject obj = QJsonDocument::fromJson(line).object();
    if (obj.contains("error")) {
        m_streamError = obj["error"].toString();
        return;
    }
    const QJsonObject message = obj["message"].toObject();
    const QString text = message["content"].toString();
    if (!text.isEmpty()) {
        m_streamContent += text;
        emit responseDelta(text);
    }
    for (const QJsonValue& call : message["tool_calls"].toArray())
        m_streamToolCalls.append(call);
    if (obj["done"].toBool())
        m_streamFinal = obj;
}

void OllamaProtocol::onStreamFinished()
{
    if (!m_turnInProgress)
        return;
    // A last line without a trailing newline.
    const QByteArray rest = m_streamBuffer.trimmed();
    m_streamBuffer.clear();
    if (!rest.isEmpty())
        handleStreamLine(rest);

    if (!m_streamError.isEmpty()) {
        failTurn(m_streamError);
        return;
    }
    if (m_streamFinal.isEmpty()) {
        failTurn(QStringLiteral("The response stream ended before the answer was complete"));
        return;
    }

    QJsonObject message{{"role", "assistant"}, {"content", m_streamContent}};
    if (!m_streamToolCalls.isEmpty())
        message["tool_calls"] = m_streamToolCalls;
    QJsonObject response = m_streamFinal;
    response["message"] = message;
    processResponse(response);
}

bool OllamaProtocol::cancelTurn()
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

void OllamaProtocol::failTurn(const QString& message)
{
    rollbackFailedTurn();
    emit errorOccurred(message);
    emit requestFinished();
    drainQueue();
}

void OllamaProtocol::finishTurn(const QString& text)
{
    // Finalize session stats (no cost for local models)
    m_sessionInputTokens  += m_turnInputTokens;
    m_sessionOutputTokens += m_turnOutputTokens;
    m_sessionToolCalls    += m_turnToolCalls;
    ++m_sessionTurnCount;

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
    stats.sessionCostUsd      = 0.0;

    emit requestFinished();
    emit responseReady(text);
    emit statsReady(stats);
    drainQueue();
}

void OllamaProtocol::clearHistory()
{
    m_history = QJsonArray();
}

void OllamaProtocol::replaceHistory(const QJsonArray& messages)
{
    m_history = messages;
}

void OllamaProtocol::setContextLimit(int tokens)
{
    m_contextLimit = tokens;
}

void OllamaProtocol::clearStats()
{
    m_sessionInputTokens  = 0;
    m_sessionOutputTokens = 0;
    m_sessionToolCalls    = 0;
    m_sessionTurnCount    = 0;
}

QJsonArray OllamaProtocol::conversationMessages() const
{
    return m_history;
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
    if (m_contextLimit > 0)
        options["num_ctx"] = m_contextLimit;

    QJsonObject body;
    body["model"]   = m_model;
    body["messages"] = messages;
    body["stream"]  = false;
    body["options"] = options;
    // ponytail: only "low" is mapped (thinking off - accepted by every model);
    // thinking levels for the models that have them need a capability check first.
    if (m_effort == Effort::Low)
        body["think"] = false;

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
    if (!doc.isObject()) {
        failTurn(QStringLiteral("Failed to parse Ollama response (HTTP %1): %2")
                     .arg(m_transport->lastStatusCode())
                     .arg(QString::fromUtf8(data.left(200))));
        return;
    }

    QJsonObject root = doc.object();

    if (root.contains("error")) {
        failTurn(root["error"].toString());
        return;
    }

    processResponse(root);
}

void OllamaProtocol::processResponse(const QJsonObject& responseJson)
{
    // Ollama reports token counts in the top-level response object
    m_turnInputTokens  += responseJson["prompt_eval_count"].toInt();
    m_turnOutputTokens += responseJson["eval_count"].toInt();
    m_lastContextTokens = responseJson["prompt_eval_count"].toInt() + responseJson["eval_count"].toInt();

    QJsonObject message    = responseJson["message"].toObject();
    QJsonArray  toolCalls  = message["tool_calls"].toArray();
    QString     text       = message["content"].toString();

    if (toolCalls.isEmpty())
        toolCalls = extractFallbackToolCall(text);

    if (!toolCalls.isEmpty()) {
        QJsonObject assistantMsg;
        assistantMsg["role"]       = "assistant";
        assistantMsg["content"]    = text;
        assistantMsg["tool_calls"] = toolCalls;
        m_history.append(assistantMsg);

        // Continues in finishToolRound() once every handler has answered.
        executeToolCalls(toolCalls, text);
        return;
    }

    QJsonObject assistantMsg;
    assistantMsg["role"]    = "assistant";
    assistantMsg["content"] = text;
    m_history.append(assistantMsg);

    finishTurn(text);
}

void OllamaProtocol::executeToolCalls(const QJsonArray& toolCalls, const QString& text)
{
    auto round = QSharedPointer<ToolRound>::create();
    const int count = toolCalls.size();
    round->results.resize(count);
    round->done = QVector<bool>(count, false);
    round->pending = count;
    round->generation = m_turnGeneration;
    round->text = text;

    QPointer<OllamaProtocol> self(this);

    for (int i = 0; i < count; ++i) {
        if (!self || round->generation != m_turnGeneration)
            return;

        QJsonObject toolCall = toolCalls[i].toObject();
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

        auto complete = [this, self, round, i](const QJsonObject& toolMessage) {
            if (!self || round->generation != m_turnGeneration || round->done[i])
                return;
            round->done[i] = true;
            round->results[i] = toolMessage;
            if (--round->pending == 0)
                finishToolRound(round);
        };

        if (!m_toolHandlers.contains(toolName)) {
            // Reported to the model only - it can recover by picking a real
            // tool. This is not a failure of the turn.
            qWarning() << "OllamaProtocol: model called unknown tool" << toolName;

            QStringList available;
            for (const QJsonObject& schema : m_toolSchemas)
                available.append(schema["function"].toObject()["name"].toString());

            QJsonObject errorResult;
            errorResult["role"]      = "tool";
            errorResult["tool_name"] = toolName;
            errorResult["content"]   = QStringLiteral("Error: unknown tool \"%1\". Available tools: %2")
                                           .arg(toolName, available.join(", "));
            complete(errorResult);
            continue;
        }

        ++m_turnToolCalls;
        emit toolInvoked(toolName, toolInput);

        auto done = [this, self, round, complete, toolName](const QJsonObject& handlerResult) {
            if (!self || round->generation != m_turnGeneration)
                return;
            QJsonObject result = handlerResult;
            // ponytail: images are dropped (the JSON keeps an "images": N note).
            // Passing them on needs a per-model vision capability check first.
            toolTakeImages(result);
            emit toolCompleted(toolName, result);

            QJsonObject toolResult;
            toolResult["role"]      = "tool";
            // Lets the model match results to calls when it made several at once.
            toolResult["tool_name"] = toolName;
            toolResult["content"]   = QString::fromUtf8(
                QJsonDocument(result).toJson(QJsonDocument::Compact));
            complete(toolResult);
        };

        // A copy: setTools() may replace the map while the handler runs.
        const ToolCallHandler handler = m_toolHandlers.value(toolName);
        // A throwing handler must not unwind through the network callback
        // chain — convert to an error result and keep the turn alive.
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

void OllamaProtocol::finishToolRound(const QSharedPointer<ToolRound>& round)
{
    for (const QJsonObject& result : round->results)
        m_history.append(result);

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

QJsonArray OllamaProtocol::extractFallbackToolCall(const QString& text) const
{
    QJsonDocument doc = QJsonDocument::fromJson(text.trimmed().toUtf8());
    if (!doc.isObject())
        return {};

    QJsonObject obj = doc.object();
    QString name = obj["name"].toString();
    if (name.isEmpty() || !m_toolHandlers.contains(name))
        return {};

    QJsonObject args = obj.contains("parameters") ? obj["parameters"].toObject()
                      : obj.contains("arguments")  ? obj["arguments"].toObject()
                      : QJsonObject();

    QJsonObject function;
    function["name"]      = name;
    function["arguments"] = args;

    QJsonObject call;
    call["function"] = function;

    return QJsonArray{ call };
}

void OllamaProtocol::onTransportError(const QString& message)
{
    failTurn(message);
}

void OllamaProtocol::rollbackFailedTurn()
{
    while (m_history.size() > m_historyLenBeforeTurn)
        m_history.removeAt(m_history.size() - 1);
}

void OllamaProtocol::fetchModels()
{
    // A capability check is a multi-step sequence (one /api/show round trip
    // per model) - ignore a redundant call while one is already in flight
    // instead of letting two interleave and corrupt each other's progress.
    if (m_modelsFetchInProgress)
        return;
    m_modelsFetchInProgress = true;

    // Ollama tags endpoint: base URL + /api/tags
    QUrl tagsUrl = m_url;
    tagsUrl.setPath("/api/tags");
    m_modelsTransport->get(tagsUrl, {});
}

void OllamaProtocol::onModelsReplyReceived(const QByteArray& data)
{
    QJsonDocument doc = QJsonDocument::fromJson(data);
    if (doc.isNull()) {
        m_modelsFetchInProgress = false;
        qWarning() << "OllamaProtocol: model list unavailable:" << data.left(200);
        emit modelsFetched({});
        return;
    }

    QJsonObject root = doc.object();
    if (root.contains("error")) {
        m_modelsFetchInProgress = false;
        qWarning() << "OllamaProtocol: model list unavailable:" << root["error"].toString();
        emit modelsFetched({});
        return;
    }

    m_modelsPendingCapabilityCheck.clear();
    QJsonArray arr = root["models"].toArray();
    for (const QJsonValue& val : arr) {
        QString name = val.toObject()["name"].toString();
        if (!name.isEmpty())
            m_modelsPendingCapabilityCheck.append(name);
    }

    m_toolCapableModels.clear();
    checkNextModelCapability();
}

void OllamaProtocol::onModelsTransportError(const QString& message)
{
    m_modelsFetchInProgress = false;
    qWarning() << "OllamaProtocol: model list unavailable:" << message;
    emit modelsFetched({});
}

void OllamaProtocol::checkNextModelCapability()
{
    if (m_modelsPendingCapabilityCheck.isEmpty()) {
        m_modelsFetchInProgress = false;
        emit modelsFetched(m_toolCapableModels);
        return;
    }

    m_capabilityCheckModel = m_modelsPendingCapabilityCheck.takeFirst();

    QUrl showUrl = m_url;
    showUrl.setPath("/api/show");
    QJsonObject body{{"model", m_capabilityCheckModel}};
    m_capabilityTransport->post(showUrl, QJsonDocument(body).toJson(QJsonDocument::Compact));
}

void OllamaProtocol::onCapabilityReplyReceived(const QByteArray& data)
{
    QJsonObject root = QJsonDocument::fromJson(data).object();
    for (const QJsonValue& cap : root["capabilities"].toArray()) {
        if (cap.toString() == QStringLiteral("tools")) {
            m_toolCapableModels.append(m_capabilityCheckModel);
            break;
        }
    }
    checkNextModelCapability();
}

void OllamaProtocol::onCapabilityTransportError(const QString&)
{
    // Can't confirm tool support for this model - exclude it rather than
    // guess, and keep checking the rest.
    checkNextModelCapability();
}

} // namespace QtLLM
