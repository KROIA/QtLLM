#pragma once
#include "QtLLM.h"
#include <QWidget>
#include <QTextEdit>
#include <QLineEdit>
#include <QPushButton>
#include <QLabel>
#include <QGroupBox>
#include <QComboBox>
#include <QTimer>
#include <QJsonObject>

// Main chat window — hosts the conversation display, user input area, and a stats panel.
class ChatWindow : public QWidget
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
    void onSendClicked();
    void onResponseReady(const QString& text);
    void onToolInvoked(const QString& toolName, const QJsonObject& args);
    void onErrorOccurred(const QString& message);
    void onRequestStarted();
    void onRequestFinished();
    void onStatsUpdated(const QtLLM::UsageStats& stats);

    // Ollama model management
    void onOllamaRunningChecked(bool running);
    void onLocalModelsReady(const QList<QtLLM::OllamaManager::ModelInfo>& models);
    void onModelComboChanged(int index);
    void onManageModelsClicked();

private:
    void buildUi();
    void registerTools();
    void connectSignals();
    void initOllamaIfNeeded();

    // Appends a formatted message block to the chat display.
    void appendMessage(const QString& sender, const QString& text, const QString& color);
    void appendNote(const QString& text);

    bool         m_isOllama = false;
    QString      m_currentModel;

    QTextEdit*   m_display;
    QLineEdit*   m_input;
    QPushButton* m_sendBtn;
    QtLLM::Client m_client;

    // Model bar (top, Ollama only)
    QComboBox*   m_modelCombo  = nullptr;
    QPushButton* m_modelsBtn   = nullptr;
    QLabel*      m_ollamaStatus= nullptr;

    // Ollama manager (null when using Claude)
    QtLLM::OllamaManager* m_ollamaManager = nullptr;
    QTimer*               m_retryTimer    = nullptr;
    int                   m_retryCount    = 0;

    // Stats panel labels — last turn
    QLabel* m_lblTurnInput;
    QLabel* m_lblTurnOutput;
    QLabel* m_lblTurnTotal;
    QLabel* m_lblTurnTools;
    QLabel* m_lblTurnDuration;
    // Stats panel labels — session totals
    QLabel* m_lblSessInput;
    QLabel* m_lblSessOutput;
    QLabel* m_lblSessTotal;
    QLabel* m_lblSessTools;
    QLabel* m_lblSessTurns;
    QLabel* m_lblSessCost;
};
