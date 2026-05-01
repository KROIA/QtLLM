#pragma once
#include "QtLLM.h"
#include <QDialog>
#include <QListWidget>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QLineEdit>
#include <QStackedWidget>

// Dialog for browsing installed Ollama models and downloading new ones.
// Emits modelSelected() when the user picks a model to use.
class ModelManagerDialog : public QDialog
{
    Q_OBJECT
public:
    explicit ModelManagerDialog(QtLLM::OllamaManager* manager,
                                const QString& currentModel,
                                QWidget* parent = nullptr);

signals:
    void modelSelected(const QString& modelName);

private slots:
    void onLocalModelsReady(const QList<QtLLM::OllamaManager::ModelInfo>& models);
    void onPullProgress(const QString& modelName, const QString& status, int percent);
    void onPullFinished(const QString& modelName, bool success, const QString& errorMessage);
    void onInstalledItemDoubleClicked(QListWidgetItem* item);
    void onDownloadClicked();
    void onCustomPullClicked();
    void onSelectClicked();
    void onRefreshClicked();
    void onInstalledSelectionChanged();
    void onAvailableSelectionChanged();

private:
    void buildUi();
    void populateAvailableList(const QList<QtLLM::OllamaManager::ModelInfo>& installed);
    void setDownloading(bool active, const QString& modelName = QString());

    QtLLM::OllamaManager* m_manager;
    QString m_currentModel;

    // Installed tab
    QListWidget*  m_installedList;
    QPushButton*  m_selectBtn;
    QPushButton*  m_refreshBtn;

    // Available tab
    QListWidget*  m_availableList;
    QPushButton*  m_downloadBtn;
    QLineEdit*    m_customNameEdit;
    QPushButton*  m_customPullBtn;

    // Progress area (shared)
    QLabel*       m_statusLabel;
    QProgressBar* m_progressBar;

    QList<QtLLM::OllamaManager::ModelInfo> m_localModels;
};
