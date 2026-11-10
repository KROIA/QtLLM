#pragma once
#include "QtLLM.h"
#include <QMainWindow>
#include <QPointer>
#include <QTextEdit>
#include <QPushButton>
#include <QLabel>
#include <QGroupBox>
#include <QComboBox>
#include <QTimer>
#include <QJsonObject>
#include <functional>

class ChatWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit ChatWindow(const QString& apiKey,
                        const QString& endpointUrl,
                        const QString& model = "claude-sonnet-5-5",
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
    // Main toolbar: actions the assistant can list and trigger (ui_list_actions,
    // ui_trigger_action) - and the user can click.
    void buildToolBar();
    void openSettings();
    void saveConversation();
    // Controls for the assistant to operate: a small form, a table, a dialog.
    QWidget* buildPlaygroundPanel();
    QWidget* buildAgentDemoPanel();
    // Buttons that send ready-made requests to the assistant, one per library
    // feature: highlights, shapes, speech bubbles, tours, looking at the UI.
    QWidget* buildFeatureDemoPanel();
    void registerTools();
    void connectSignals();
    void initOllamaIfNeeded();

    // Background-agent demo. The agent is spawned on first use and lives until
    // the Kill button (or this window) destroys it, so it shows up in
    // Settings > Agents while it exists.
    QtLLM::Agent* demoAgent();
    // Spawns the writer and the critic if they are not alive.
    void ensureTeamAgents();
    // Provider, credentials and model of the chat, for an agent spawned now.
    QtLLM::AgentConfig agentConfig(const QString& name) const;
    void runAgentTask(const QString& label,
                      const QString& prompt,
                      std::function<void(const QString&)> onSuccess = nullptr);
    void logAgent(const QString& line);
    void updateAgentStatus();

    bool         m_isOllama = false;
    QString      m_currentModel;
    QString      m_systemPrompt;
    QString      m_apiKey;
    QString      m_endpointUrl;   // full Claude messages URL, or full Ollama chat URL
    int          m_fontSizePercent = 100;

    // Remembered per-provider so switching Settings' Provider combo back and
    // forth restores exactly what was last configured for each one, instead
    // of resetting to a hardcoded default (in-RAM only, not persisted to disk).
    QString      m_savedClaudeApiKey;
    QString      m_savedClaudeEndpointUrl;
    QString      m_savedOllamaUrl;
    QString      m_savedFoundryApiKey;
    QString      m_savedFoundryUrl;
    QString      m_savedOpenAiUrl = QStringLiteral("http://localhost:1234/v1");
    QString      m_savedOpenAiKey;
    // What the Settings dialog's Provider combo last had selected. Finer
    // grained than Client::provider(): Anthropic API and Foundry are the same
    // protocol with different credentials.
    QtLLM::SettingsDialog::Provider m_settingsProvider = QtLLM::SettingsDialog::Provider::Claude;

    QtLLM::ChatDockWidget* m_chatDock = nullptr;
    QtLLM::Client m_client;

    // Agent demo panel
    QString       m_lastAssistantReply;
    QtLLM::Agent* m_demoAgent   = nullptr;
    // Two agents that talk to each other (AgentConfig::messaging).
    QPointer<QtLLM::Agent> m_writerAgent;
    QPointer<QtLLM::Agent> m_criticAgent;
    QtLLM::AppInterface* m_appInterface = nullptr;
    QtLLM::AppKnowledge* m_knowledge = nullptr;
    QTextEdit*    m_agentLog    = nullptr;
    QLabel*       m_agentStatus = nullptr;
    QPushButton*  m_killAgentBtn = nullptr;

    // Panels the toolbar can show and hide
    QWidget*      m_statsBox = nullptr;
    QWidget*      m_demoBox  = nullptr;
    QWidget*      m_agentBox = nullptr;
    QAction*      m_darkThemeAction = nullptr;

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
