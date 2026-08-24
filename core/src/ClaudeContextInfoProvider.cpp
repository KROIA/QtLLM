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
    {
        connect(m_countTransport, &HttpTransport::replyReceived, this, &ClaudeContextInfoProvider::onCountReplyReceived);
        connect(m_countTransport, &HttpTransport::errorOccurred, this, &ClaudeContextInfoProvider::onCountTransportError);
    }

    void ClaudeContextInfoProvider::fetchContextWindowTokens(const QString&)
    {
        // Mocked: no Anthropic endpoint reports this. Every current Claude
        // model publishes 200k tokens (docs only, not queryable).
        QTimer::singleShot(0, this, [this]() { emit contextWindowTokensReady(200000); });
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
        headers.append({"x-api-key", m_apiKey.toUtf8()});
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
