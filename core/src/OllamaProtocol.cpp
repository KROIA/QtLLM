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
                               const QMap<QString, ToolHandler>& handlers)
{
    m_toolSchemas  = toolSchemas;
    m_toolHandlers = handlers;
}

void OllamaProtocol::beginTurn(const QString& userMessage)
{
    if (m_turnInProgress) {
        m_pendingTurns.enqueue(userMessage);
        return;
    }
    startNewTurn(userMessage);
}

void OllamaProtocol::startNewTurn(const QString& userMessage)
{
    m_turnInProgress   = true;
    m_turnInputTokens  = 0;
    m_turnOutputTokens = 0;
    m_turnToolCalls    = 0;
    m_turnTimerStarted = false;
    m_historyLenBeforeTurn = m_history.size();

    QJsonObject msg;
    msg["role"]    = "user";
    msg["content"] = userMessage;
    m_history.append(msg);
    sendRequest();
}

void OllamaProtocol::drainQueue()
{
    m_turnInProgress = false;
    if (!m_pendingTurns.isEmpty())
        startNewTurn(m_pendingTurns.dequeue());
}

void OllamaProtocol::sendRequest()
{
    QJsonObject body  = buildRequestBody();
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
    m_transport->post(chatUrl, bytes, {});
}

void OllamaProtocol::clearHistory()
{
    m_history = QJsonArray();
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
        rollbackFailedTurn();
        emit errorOccurred("Failed to parse Ollama response");
        emit requestFinished();
        drainQueue();
        return;
    }

    QJsonObject root = doc.object();

    if (root.contains("error")) {
        rollbackFailedTurn();
        emit errorOccurred(root["error"].toString());
        emit requestFinished();
        drainQueue();
        return;
    }

    processResponse(root);
}

void OllamaProtocol::processResponse(const QJsonObject& responseJson)
{
    // Ollama reports token counts in the top-level response object
    m_turnInputTokens  += responseJson["prompt_eval_count"].toInt();
    m_turnOutputTokens += responseJson["eval_count"].toInt();

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

        executeToolCalls(toolCalls);
        sendRequest();
        return;
    }

    QJsonObject assistantMsg;
    assistantMsg["role"]    = "assistant";
    assistantMsg["content"] = text;
    m_history.append(assistantMsg);

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
        toolResult["role"]    = "tool";
        toolResult["content"] = resultStr;
        toolResults.append(toolResult);
    }

    for (const QJsonValue& val : toolResults) {
        m_history.append(val);
    }
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
    rollbackFailedTurn();
    emit errorOccurred(message);
    emit requestFinished();
    drainQueue();
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
        emit errorOccurred("Failed to parse Ollama models response");
        emit modelsFetched({});
        return;
    }

    QJsonObject root = doc.object();
    if (root.contains("error")) {
        m_modelsFetchInProgress = false;
        emit errorOccurred(root["error"].toString());
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
    emit errorOccurred(message);
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
