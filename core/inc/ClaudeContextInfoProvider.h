#pragma once
#include "QtLLM_base.h"
#include "ContextInfoProvider.h"
#include <QUrl>

namespace QtLLM
{
    class HttpTransport;

    // Anthropic-backed implementation:
    //  - countTokens() is real: POST /v1/messages/count_tokens, the same
    //    endpoint/tokenizer the Messages API itself uses, returns exact
    //    input_tokens for a given system/tools/messages combination.
    //  - fetchContextWindowTokens() asks the Models API (GET /v1/models/{id},
    //    field max_input_tokens). Gateways without that endpoint fall back
    //    to the published size for the model family: 1M for the current
    //    generation, 200k for older models.
    class QT_LLM_API ClaudeContextInfoProvider : public ContextInfoProvider
    {
        Q_OBJECT
    public:
        explicit ClaudeContextInfoProvider(const QString& apiKey, const QUrl& messagesUrl, QObject* parent = nullptr);

        // Kept in sync with ClaudeProtocol::setApiKey/setUrl by Client so a
        // Settings-dialog key/endpoint change also applies to count_tokens.
        void setApiKey(const QString& apiKey) { m_apiKey = apiKey; }
        void setMessagesUrl(const QUrl& url) { m_messagesUrl = url; }

        void fetchContextWindowTokens(const QString& model) override;
        void countTokens(const QString& model,
                          const QString& systemPrompt,
                          const QJsonArray& toolSchemas,
                          const QJsonArray& messages) override;

    private slots:
        void onCountReplyReceived(const QByteArray& data);
        void onCountTransportError(const QString& message);
        void onModelReplyReceived(const QByteArray& data);
        void onModelTransportError(const QString& message);

    private:
        QString        m_apiKey;
        QUrl           m_messagesUrl;
        HttpTransport* m_countTransport;
        HttpTransport* m_modelTransport;
        QString        m_windowModel;   // model the in-flight window lookup is for
        bool           m_countTokensUnsupported = false;
    };
}
