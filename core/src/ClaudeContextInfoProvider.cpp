#include "ClaudeContextInfoProvider.h"
#include "HttpTransport.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>

namespace QtLLM
{
    ClaudeContextInfoProvider::ClaudeContextInfoProvider(const QString& apiKey, const QUrl& messagesUrl, QObject* parent)
        : ContextInfoProvider(parent)
        , m_apiKey(apiKey)
        , m_messagesUrl(messagesUrl)
        , m_countTransport(new HttpTransport(this))
        , m_modelTransport(new HttpTransport(this))
    {
        connect(m_modelTransport, &HttpTransport::replyReceived, this, &ClaudeContextInfoProvider::onModelReplyReceived);
        connect(m_modelTransport, &HttpTransport::errorOccurred, this, &ClaudeContextInfoProvider::onModelTransportError);
        connect(m_countTransport, &HttpTransport::replyReceived, this, &ClaudeContextInfoProvider::onCountReplyReceived);
        connect(m_countTransport, &HttpTransport::errorOccurred, this, &ClaudeContextInfoProvider::onCountTransportError);
    }

    // Published context window per model family, for endpoints that cannot
    // be asked. Everything from the 4.6 line on has 1M; older models and
    // Haiku 4.5 have 200k. Unknown names get the smaller, safer figure.
    static int publishedContextWindow(const QString& model)
    {
        static const char* const largeFamilies[] = {
            "fable", "mythos", "opus-5", "sonnet-5", "haiku-5",
            "opus-4-6", "opus-4-7", "opus-4-8", "sonnet-4-6"};
        for (const char* family : largeFamilies) {
            if (model.contains(QLatin1String(family)))
                return 1000000;
        }
        return 200000;
    }

    QPair<QByteArray, QByteArray> ClaudeContextInfoProvider::authHeader() const
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

    void ClaudeContextInfoProvider::fetchContextWindowTokens(const QString& model)
    {
        QUrl url = m_messagesUrl;
        QString path = url.path();
        const QPair<QByteArray, QByteArray> auth = authHeader();
        if (model.isEmpty() || auth.first.isEmpty() || !path.endsWith("/messages") || m_modelTransport->isBusy()) {
            const int tokens = publishedContextWindow(model);
            QTimer::singleShot(0, this, [this, tokens]() { emit contextWindowTokensReady(tokens); });
            return;
        }
        path.chop(int(qstrlen("/messages")));
        url.setPath(path + "/models/" + model);

        m_windowModel = model;
        QList<QPair<QByteArray, QByteArray>> headers;
        headers.append(auth);
        headers.append({QByteArray("anthropic-version"), QByteArray("2023-06-01")});
        m_modelTransport->get(url, headers);
    }

    void ClaudeContextInfoProvider::onModelReplyReceived(const QByteArray& data)
    {
        const int reported = QJsonDocument::fromJson(data).object()["max_input_tokens"].toInt();
        emit contextWindowTokensReady(reported > 0 ? reported : publishedContextWindow(m_windowModel));
    }

    void ClaudeContextInfoProvider::onModelTransportError(const QString&)
    {
        emit contextWindowTokensReady(publishedContextWindow(m_windowModel));
    }

    void ClaudeContextInfoProvider::countTokens(const QString& model,
                                                 const QString& systemPrompt,
                                                 const QJsonArray& toolSchemas,
                                                 const QJsonArray& messages)
    {
        // count_tokens requires a non-empty messages array (same rule as a
        // real Messages request); before the first turn there's nothing to
        // send yet, so report the (small) system+tools cost directly instead
        // of firing a request the API would reject.
        // Some proxies/gateways (Azure/Foundry-style deployments) don't
        // implement count_tokens at all and 403/404 on every call - once
        // that's confirmed, stop retrying it every turn.
        if (m_countTokensUnsupported) {
            emit contextInfoErrorOccurred(QStringLiteral("count_tokens not supported by this endpoint (cached)"));
            return;
        }

        if (messages.isEmpty()) {
            qsizetype chars = systemPrompt.toUtf8().size();
            if (!toolSchemas.isEmpty())
                chars += QJsonDocument(toolSchemas).toJson(QJsonDocument::Compact).size();
            QTimer::singleShot(0, this, [this, chars]() {
                emit tokenCountReady(static_cast<int>(chars / 4));
            });
            return;
        }

        QJsonObject body;
        body["model"] = model;
        if (!systemPrompt.isEmpty())
            body["system"] = systemPrompt;
        if (!toolSchemas.isEmpty())
            body["tools"] = toolSchemas;
        body["messages"] = messages;

        QUrl url = m_messagesUrl;
        QString path = url.path();
        if (path.endsWith("/messages"))
            path += "/count_tokens";
        else
            path += "/messages/count_tokens";
        url.setPath(path);

        QList<QPair<QByteArray, QByteArray>> headers;
        const QPair<QByteArray, QByteArray> auth = authHeader();
        if (!auth.first.isEmpty())
            headers.append(auth);
        headers.append({QByteArray("anthropic-version"), QByteArray("2023-06-01")});
        m_countTransport->post(url, QJsonDocument(body).toJson(QJsonDocument::Compact), headers);
    }

    void ClaudeContextInfoProvider::onCountReplyReceived(const QByteArray& data)
    {
        QJsonObject root = QJsonDocument::fromJson(data).object();
        if (root.contains("input_tokens")) {
            emit tokenCountReady(root["input_tokens"].toInt());
            return;
        }

        // Real Anthropic errors are {"error":{"message":...}}; some gateways
        // use a flatter {"detail": "..."} shape instead (e.g. a 403 "Model
        // not found" from a proxy that doesn't implement this endpoint).
        // Either way: no input_tokens means this deployment can't answer,
        // so stop asking for the rest of the session.
        m_countTokensUnsupported = true;
        QString message = root.contains("error")
            ? root["error"].toObject()["message"].toString()
            : root.value("detail").toString();
        if (message.isEmpty())
            message = QStringLiteral("count_tokens response has no input_tokens");
        emit contextInfoErrorOccurred(message);
    }

    void ClaudeContextInfoProvider::onCountTransportError(const QString& message)
    {
        emit contextInfoErrorOccurred(message);
    }
}
