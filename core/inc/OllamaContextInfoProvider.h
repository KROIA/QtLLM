#pragma once
#include "QtLLM_base.h"
#include "ContextInfoProvider.h"
#include <QUrl>
#include <QSet>

namespace QtLLM
{
    class HttpTransport;

    // Real implementation backed by Ollama's local HTTP API:
    //  - fetchContextWindowTokens(): POST /api/show, reads
    //    model_info["<arch>.context_length"] (e.g. "llama.context_length").
    //  - countTokens(): POST /api/embed with the serialized request as
    //    "input" and reads back prompt_eval_count - the exact number of
    //    tokens Ollama's own tokenizer produced for that text, without
    //    paying for any generation.
    class QT_LLM_API OllamaContextInfoProvider : public ContextInfoProvider
    {
        Q_OBJECT
    public:
        explicit OllamaContextInfoProvider(const QUrl& baseUrl, QObject* parent = nullptr);

        // Kept in sync with OllamaProtocol::setUrl by Client.
        void setBaseUrl(const QUrl& url) { m_baseUrl = url; }

        void fetchContextWindowTokens(const QString& model) override;
        void countTokens(const QString& model,
                          const QString& systemPrompt,
                          const QJsonArray& toolSchemas,
                          const QJsonArray& messages) override;

    private slots:
        void onShowReplyReceived(const QByteArray& data);
        void onShowTransportError(const QString& message);
        void onEmbedReplyReceived(const QByteArray& data);
        void onEmbedTransportError(const QString& message);

    private:
        QUrl           m_baseUrl;
        HttpTransport* m_showTransport;
        HttpTransport* m_embedTransport;
        QString        m_pendingCountModel;  // model the in-flight /api/embed call is for
        QSet<QString>  m_embeddingUnsupported;  // models known to reject /api/embed
    };
}
