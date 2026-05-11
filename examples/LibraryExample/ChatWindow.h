#pragma once
#include "QtLLM.h"
#include <QMainWindow>
#include <QTextEdit>
#include <QPushButton>
#include <QLabel>
#include <QGroupBox>
#include <QComboBox>
#include <QTimer>
#include <QJsonObject>

class ChatWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit ChatWindow(const QString& apiKey,
                        const QString& endpointUrl,
                        const QString& model = "claude-sonnet-4-5",
                        QWidget* parent = nullptr);
    explicit ChatWindow(QtLLM::Provider provider,
                        const QString& apiKey,
                        const QString& endpointUrl,
                        const QString& model,
                        QWidget* parent = nullptr);
    ~ChatWindow() override;

private slots:
    void onSendClicked(const QString& text);
    void onResponseReady(const QString& text);
    void onToolInvoked(const QString& toolName, const QJsonObject& args);
    void onErrorOccurred(const QString& message);
    void onRequestStarted();
    void onRequestFinished();
    void onStatsUpdated(const QtLLM::UsageStats& stats);

    void onOllamaRunningChecked(bool running);
    void onLocalModelsReady(const QList<QtLLM::OllamaManager::ModelInfo>& models);
    void onModelComboChanged(int index);
    void onManageModelsClicked();

private:
    void buildUi();
    void registerTools();
    void connectSignals();
    void initOllamaIfNeeded();

    bool         m_isOllama = false;
    QString      m_currentModel;
    QString      m_systemPrompt;
    int          m_fontSizePercent = 100;

    QtLLM::ChatDockWidget* m_chatDock = nullptr;
    QtLLM::Client m_client;

    // Model bar (top, Ollama only)
    QComboBox*   m_modelCombo  = nullptr;
    QPushButton* m_modelsBtn   = nullptr;
    QLabel*      m_ollamaStatus= nullptr;

    QtLLM::OllamaManager* m_ollamaManager = nullptr;
    QTimer*               m_retryTimer    = nullptr;
    int                   m_retryCount    = 0;

    // Stats panel labels
    QLabel* m_lblTurnInput;
    QLabel* m_lblTurnOutput;
    QLabel* m_lblTurnTotal;
    QLabel* m_lblTurnTools;
    QLabel* m_lblTurnDuration;
    QLabel* m_lblSessInput;
    QLabel* m_lblSessOutput;
    QLabel* m_lblSessTotal;
    QLabel* m_lblSessTools;
    QLabel* m_lblSessTurns;
    QLabel* m_lblSessCost;
};
