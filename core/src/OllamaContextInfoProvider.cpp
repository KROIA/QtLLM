#include "OllamaContextInfoProvider.h"
#include "HttpTransport.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QStringList>

namespace QtLLM
{
    OllamaContextInfoProvider::OllamaContextInfoProvider(const QUrl& baseUrl, QObject* parent)
        : ContextInfoProvider(parent)
        , m_baseUrl(baseUrl)
        , m_showTransport(new HttpTransport(this))
        , m_embedTransport(new HttpTransport(this))
        , m_psTransport(new HttpTransport(this))
    {
        m_psTransport->setTimeoutMs(5000);
        connect(m_showTransport, &HttpTransport::replyReceived, this, &OllamaContextInfoProvider::onShowReplyReceived);
        connect(m_showTransport, &HttpTransport::errorOccurred, this, &OllamaContextInfoProvider::onShowTransportError);
        connect(m_psTransport, &HttpTransport::replyReceived, this, &OllamaContextInfoProvider::onPsReplyReceived);
        connect(m_psTransport, &HttpTransport::errorOccurred, this, &OllamaContextInfoProvider::onPsTransportError);
        connect(m_embedTransport, &HttpTransport::replyReceived, this, &OllamaContextInfoProvider::onEmbedReplyReceived);
        connect(m_embedTransport, &HttpTransport::errorOccurred, this, &OllamaContextInfoProvider::onEmbedTransportError);
    }

    void OllamaContextInfoProvider::fetchContextWindowTokens(const QString& model)
    {
        m_pendingShowModel = model;
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
        int archMax = 0;
        const QJsonObject modelInfo = root["model_info"].toObject();
        for (auto it = modelInfo.constBegin(); it != modelInfo.constEnd(); ++it) {
            if (it.key().endsWith(".context_length")) {
                archMax = it.value().toInt();
                break;
            }
        }

        // Ollama truncates at num_ctx, not at the architecture maximum. A
        // Modelfile num_ctx shows up in the "parameters" text as
        // "num_ctx                        8192".
        const QStringList paramLines = root["parameters"].toString().split('\n');
        for (const QString& line : paramLines) {
            const QStringList parts = line.simplified().split(' ');
            if (parts.size() >= 2 && parts.at(0) == QLatin1String("num_ctx")) {
                const int numCtx = parts.at(1).toInt();
                if (numCtx > 0) {
                    emitWindow(numCtx, archMax);
                    return;
                }
            }
        }

        // No Modelfile override: the runtime value (server default /
        // OLLAMA_CONTEXT_LENGTH) is only visible on the loaded model.
        m_pendingArchMax = archMax;
        QUrl url = m_baseUrl;
        url.setPath("/api/ps");
        m_psTransport->get(url);
    }

    void OllamaContextInfoProvider::onShowTransportError(const QString& message)
    {
        emit contextInfoErrorOccurred(message);
    }

    void OllamaContextInfoProvider::onPsReplyReceived(const QByteArray& data)
    {
        const QJsonArray models = QJsonDocument::fromJson(data).object()["models"].toArray();
        // "llama3.2" is listed as "llama3.2:latest" once loaded
        const QString wanted = m_pendingShowModel.contains(':')
                                   ? m_pendingShowModel
                                   : m_pendingShowModel + QStringLiteral(":latest");
        for (const QJsonValue& val : models) {
            const QJsonObject obj = val.toObject();
            const QString name = obj["name"].toString();
            const QString model = obj["model"].toString();
            if (name != m_pendingShowModel && name != wanted
                && model != m_pendingShowModel && model != wanted)
                continue;
            const int loadedCtx = obj["context_length"].toInt();
            if (loadedCtx > 0) {
                emitWindow(loadedCtx, m_pendingArchMax);
                return;
            }
            break;
        }
        emitWindow(0, m_pendingArchMax);
    }

    void OllamaContextInfoProvider::onPsTransportError(const QString& /*message*/)
    {
        // /api/ps is only a refinement; fall back to the architecture maximum
        emitWindow(0, m_pendingArchMax);
    }

    void OllamaContextInfoProvider::emitWindow(int effective, int archMax)
    {
        int window = effective > 0 ? effective : archMax;
        if (archMax > 0 && window > archMax)
            window = archMax;
        if (window > 0)
            emit contextWindowTokensReady(window);
        else
            emit contextInfoErrorOccurred(QStringLiteral("model_info has no *.context_length field"));
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
