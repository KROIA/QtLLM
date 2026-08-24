#include "OllamaContextInfoProvider.h"
#include "HttpTransport.h"
#include <QJsonDocument>
#include <QJsonObject>

namespace QtLLM
{
    OllamaContextInfoProvider::OllamaContextInfoProvider(const QUrl& baseUrl, QObject* parent)
        : ContextInfoProvider(parent)
        , m_baseUrl(baseUrl)
        , m_showTransport(new HttpTransport(this))
        , m_embedTransport(new HttpTransport(this))
    {
        connect(m_showTransport, &HttpTransport::replyReceived, this, &OllamaContextInfoProvider::onShowReplyReceived);
        connect(m_showTransport, &HttpTransport::errorOccurred, this, &OllamaContextInfoProvider::onShowTransportError);
        connect(m_embedTransport, &HttpTransport::replyReceived, this, &OllamaContextInfoProvider::onEmbedReplyReceived);
        connect(m_embedTransport, &HttpTransport::errorOccurred, this, &OllamaContextInfoProvider::onEmbedTransportError);
    }

    void OllamaContextInfoProvider::fetchContextWindowTokens(const QString& model)
    {
        QUrl url = m_baseUrl;
        url.setPath("/api/show");
        QJsonObject body{{"model", model}};
        m_showTransport->post(url, QJsonDocument(body).toJson(QJsonDocument::Compact));
    }

    void OllamaContextInfoProvider::onShowReplyReceived(const QByteArray& data)
    {
        QJsonObject root = QJsonDocument::fromJson(data).object();
        if (root.contains("error")) {
            emit contextInfoErrorOccurred(root["error"].toString());
            return;
        }

        // Key is architecture-prefixed (e.g. "llama.context_length",
        // "qwen2.context_length") - scan for whatever this model uses.
        const QJsonObject modelInfo = root["model_info"].toObject();
        for (auto it = modelInfo.constBegin(); it != modelInfo.constEnd(); ++it) {
            if (it.key().endsWith(".context_length")) {
                emit contextWindowTokensReady(it.value().toInt());
                return;
            }
        }
        emit contextInfoErrorOccurred(QStringLiteral("model_info has no *.context_length field"));
    }

    void OllamaContextInfoProvider::onShowTransportError(const QString& message)
    {
        emit contextInfoErrorOccurred(message);
    }

    void OllamaContextInfoProvider::countTokens(const QString& model,
                                                 const QString& systemPrompt,
                                                 const QJsonArray& toolSchemas,
                                                 const QJsonArray& messages)
    {
        // Ollama has no standalone tokenize endpoint. /api/embed reports
        // prompt_eval_count - the real tokenizer's count for arbitrary text -
        // without generating a response, so it doubles as a token counter.
        // Concatenating the pieces isn't byte-identical to the chat template
        // Ollama wraps them in internally, but it's the same tokenizer on
        // (almost) the same text, far closer than a chars/4 guess.
        // Generation-only models (most chat models, e.g. gpt-oss, llama3.2)
        // reject /api/embed outright. Once a model has told us so, stop
        // asking - it'll just fail identically every turn otherwise.
        if (m_embeddingUnsupported.contains(model)) {
            emit contextInfoErrorOccurred(QStringLiteral("model does not support embeddings (cached)"));
            return;
        }

        QString combined = systemPrompt;
        if (!toolSchemas.isEmpty())
            combined += QString::fromUtf8(QJsonDocument(toolSchemas).toJson(QJsonDocument::Compact));
        if (!messages.isEmpty())
            combined += QString::fromUtf8(QJsonDocument(messages).toJson(QJsonDocument::Compact));

        m_pendingCountModel = model;
        QUrl url = m_baseUrl;
        url.setPath("/api/embed");
        QJsonObject body{{"model", model}, {"input", combined}};
        m_embedTransport->post(url, QJsonDocument(body).toJson(QJsonDocument::Compact));
    }

    void OllamaContextInfoProvider::onEmbedReplyReceived(const QByteArray& data)
    {
        QJsonObject root = QJsonDocument::fromJson(data).object();
        if (root.contains("error")) {
            QString message = root["error"].toString();
            if (message.contains(QStringLiteral("does not support embeddings")) && !m_pendingCountModel.isEmpty())
                m_embeddingUnsupported.insert(m_pendingCountModel);
            emit contextInfoErrorOccurred(message);
            return;
        }
        if (!root.contains("prompt_eval_count")) {
            emit contextInfoErrorOccurred(QStringLiteral("embed response has no prompt_eval_count"));
            return;
        }
        emit tokenCountReady(root["prompt_eval_count"].toInt());
    }

    void OllamaContextInfoProvider::onEmbedTransportError(const QString& message)
    {
        emit contextInfoErrorOccurred(message);
    }
}
