#include "OllamaManager.h"
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QProcess>
#include <QStandardPaths>

namespace QtLLM {

OllamaManager::OllamaManager(const QString& baseUrl, QObject* parent)
    : QObject(parent)
    , m_baseUrl(baseUrl)
{
}

OllamaManager::~OllamaManager() = default;

void OllamaManager::checkIsRunning()
{
    QNetworkRequest req(QUrl(m_baseUrl + "/api/tags"));
    req.setTransferTimeout(3000);
    QNetworkReply* reply = m_nam.get(req);
    connect(reply, &QNetworkReply::finished, this, &OllamaManager::onCheckReplyFinished);
}

void OllamaManager::onCheckReplyFinished()
{
    QNetworkReply* reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply) return;
    reply->deleteLater();

    bool running = (reply->error() == QNetworkReply::NoError);
    emit isRunningChecked(running);
}

bool OllamaManager::startServer()
{
    // QProcess::startDetached("ollama", ...) on Windows does not search the user's PATH
    // the same way CMD does. Resolve the full path explicitly first.
    QString exe = QStandardPaths::findExecutable("ollama");
    if (exe.isEmpty())
        return false;
    return QProcess::startDetached(exe, QStringList{"serve"});
}

void OllamaManager::fetchLocalModels()
{
    QNetworkRequest req(QUrl(m_baseUrl + "/api/tags"));
    QNetworkReply* reply = m_nam.get(req);
    connect(reply, &QNetworkReply::finished, this, &OllamaManager::onTagsReplyFinished);
}

void OllamaManager::onTagsReplyFinished()
{
    QNetworkReply* reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply) return;
    reply->deleteLater();

    QList<ModelInfo> result;

    if (reply->error() != QNetworkReply::NoError) {
        emit localModelsReady(result);
        return;
    }

    QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
    QJsonArray models = doc.object()["models"].toArray();
    for (const QJsonValue& val : models) {
        QJsonObject obj = val.toObject();
        ModelInfo info;
        info.name        = obj["name"].toString();
        info.displayName = info.name;
        info.sizeBytes   = obj["size"].toVariant().toLongLong();
        info.installed   = true;
        result.append(info);
    }

    emit localModelsReady(result);
}

void OllamaManager::pullModel(const QString& modelName)
{
    if (m_pullReply) {
        // A pull is already in progress; ignore
        return;
    }

    m_pullingModel = modelName;

    QJsonObject body;
    body["name"]   = modelName;
    body["stream"] = true;

    QByteArray data = QJsonDocument(body).toJson(QJsonDocument::Compact);

    QNetworkRequest req(QUrl(m_baseUrl + "/api/pull"));
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

    m_pullReply = m_nam.post(req, data);
    connect(m_pullReply, &QNetworkReply::readyRead, this, &OllamaManager::onPullReadyRead);
    connect(m_pullReply, &QNetworkReply::finished,  this, &OllamaManager::onPullFinished);
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    connect(m_pullReply, &QNetworkReply::errorOccurred, this, &OllamaManager::onPullError);
#else
    connect(m_pullReply, static_cast<void(QNetworkReply::*)(QNetworkReply::NetworkError)>(&QNetworkReply::error),
            this, &OllamaManager::onPullError);
#endif
}

void OllamaManager::onPullReadyRead()
{
    if (!m_pullReply) return;

    // The reply is streaming NDJSON — each line is a JSON object
    while (m_pullReply->canReadLine()) {
        QByteArray line = m_pullReply->readLine().trimmed();
        if (line.isEmpty()) continue;

        QJsonDocument doc = QJsonDocument::fromJson(line);
        if (doc.isNull()) continue;

        QJsonObject obj = doc.object();
        QString status  = obj["status"].toString();

        int percent = -1;
        if (obj.contains("total") && obj.contains("completed")) {
            qint64 total     = obj["total"].toVariant().toLongLong();
            qint64 completed = obj["completed"].toVariant().toLongLong();
            if (total > 0)
                percent = static_cast<int>((completed * 100) / total);
        }

        emit pullProgress(m_pullingModel, status, percent);
    }
}

void OllamaManager::onPullFinished()
{
    if (!m_pullReply) return;

    // Drain any remaining lines
    onPullReadyRead();

    bool success = (m_pullReply->error() == QNetworkReply::NoError);
    QString errMsg;
    if (!success)
        errMsg = m_pullReply->errorString();

    m_pullReply->deleteLater();
    m_pullReply = nullptr;

    emit pullFinished(m_pullingModel, success, errMsg);
    m_pullingModel.clear();
}

void OllamaManager::onPullError()
{
    // onPullFinished() handles cleanup; this slot is just for early error detection
    if (m_pullReply) {
        QString errMsg = m_pullReply->errorString();
        emit pullFinished(m_pullingModel, false, errMsg);
        m_pullReply->deleteLater();
        m_pullReply = nullptr;
        m_pullingModel.clear();
    }
}

QList<OllamaManager::ModelInfo> OllamaManager::popularModels()
{
    // Curated list of popular Ollama models with approximate download sizes
    return {
        {"llama3.2",          "Llama 3.2 (3B)",          2'000'000'000LL, false},
        {"llama3.2:1b",       "Llama 3.2 (1B)",            770'000'000LL, false},
        {"llama3.1:8b",       "Llama 3.1 (8B)",          4'900'000'000LL, false},
        {"llama3.1:70b",      "Llama 3.1 (70B)",        40'000'000'000LL, false},
        {"mistral",           "Mistral (7B)",            4'100'000'000LL, false},
        {"mistral-nemo",      "Mistral Nemo (12B)",      7'100'000'000LL, false},
        {"qwen2.5:7b",        "Qwen 2.5 (7B)",           4'700'000'000LL, false},
        {"qwen2.5:14b",       "Qwen 2.5 (14B)",          9'000'000'000LL, false},
        {"phi4",              "Phi-4 (14B)",             9'100'000'000LL, false},
        {"phi4-mini",         "Phi-4 Mini (3.8B)",       2'500'000'000LL, false},
        {"gemma3:4b",         "Gemma 3 (4B)",            3'300'000'000LL, false},
        {"gemma3:12b",        "Gemma 3 (12B)",           8'100'000'000LL, false},
        {"deepseek-r1:7b",    "DeepSeek-R1 (7B)",        4'700'000'000LL, false},
        {"deepseek-r1:14b",   "DeepSeek-R1 (14B)",       9'000'000'000LL, false},
        {"codellama:7b",      "CodeLlama (7B)",          3'800'000'000LL, false},
        {"nomic-embed-text",  "Nomic Embed Text",          274'000'000LL, false},
    };
}

QString OllamaManager::formatSize(qint64 bytes)
{
    if (bytes <= 0)  return QString();
    if (bytes < 1024LL * 1024)                          return QString("%1 KB").arg(bytes / 1024);
    if (bytes < 1024LL * 1024 * 1024)                  return QString("%1 MB").arg(bytes / (1024 * 1024));
    return QString("%1 GB").arg(bytes / 1'073'741'824.0, 0, 'f', 1);
}

} // namespace QtLLM
