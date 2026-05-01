#pragma once
#include "QtLLM_base.h"
#include <QObject>
#include <QStringList>
#include <QNetworkAccessManager>

namespace QtLLM {

// Manages the Ollama server process and model inventory.
// Communicates directly with the Ollama REST API without going through HttpTransport,
// because model pull responses are streaming NDJSON and need incremental reading.
class QT_LLM_API OllamaManager : public QObject
{
    Q_OBJECT
public:
    struct ModelInfo {
        QString name;       // e.g. "llama3.2:latest"
        QString displayName;
        qint64  sizeBytes = 0;
        bool    installed = false;
    };

    explicit OllamaManager(const QString& baseUrl = QStringLiteral("http://localhost:11434"),
                           QObject* parent = nullptr);
    ~OllamaManager() override;

    // Async: checks whether the Ollama HTTP server is reachable. Emits isRunningChecked().
    void checkIsRunning();

    // Launches `ollama serve` as a detached background process.
    // Returns true if the process was started (does not guarantee the server is ready yet).
    static bool startServer();

    // Async: fetches the list of locally installed models via GET /api/tags. Emits localModelsReady().
    void fetchLocalModels();

    // Async: streams a model pull from the Ollama library via POST /api/pull.
    // Emits pullProgress() repeatedly and pullFinished() when done.
    void pullModel(const QString& modelName);

    // Returns a curated list of popular Ollama models (installed or not).
    static QList<ModelInfo> popularModels();

    // Convenience: human-readable byte size string (e.g. "4.7 GB").
    static QString formatSize(qint64 bytes);

signals:
    // Emitted after checkIsRunning() completes.
    void isRunningChecked(bool running);
    // Emitted after fetchLocalModels() completes. Each string is "name:tag".
    void localModelsReady(const QList<QtLLM::OllamaManager::ModelInfo>& models);
    // Emitted during a pull. progressPercent is -1 if the server didn't report a total.
    void pullProgress(const QString& modelName, const QString& status, int progressPercent);
    // Emitted when a pull completes or fails.
    void pullFinished(const QString& modelName, bool success, const QString& errorMessage);

private slots:
    void onCheckReplyFinished();
    void onTagsReplyFinished();
    void onPullReadyRead();
    void onPullFinished();
    void onPullError();

private:
    QString                 m_baseUrl;
    QNetworkAccessManager   m_nam;
    QNetworkReply*          m_pullReply  = nullptr;
    QString                 m_pullingModel;
};

} // namespace QtLLM
