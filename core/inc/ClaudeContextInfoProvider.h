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
    //  - fetchContextWindowTokens() is a MOCK: Anthropic has no endpoint that
    //    reports a model's max context window, so this just returns the
    //    published constant (200k for every current Claude model) on a
    //    zero-delay timer to keep the async contract honest. Replace the
    //    body of this one method if Anthropic ever exposes it for real -
    //    nothing else in this class (or its callers) needs to change.
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

    private:
        QString        m_apiKey;
        QUrl           m_messagesUrl;
        HttpTransport* m_countTransport;
        bool           m_countTokensUnsupported = false;
    };
}
