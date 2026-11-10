#pragma once
#include "QtLLM_base.h"
#include <QDialog>
#include <QComboBox>
#include <QLineEdit>
#include <QTextEdit>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QSpinBox>
#include <QTabWidget>
#include <QPushButton>
#include <QPointer>
#include <QMap>
#include "ProtocolBase.h"
#include "Client.h"

class QCheckBox;
class QVBoxLayout;

namespace QtLLM
{
    class Client;
    class UsageHistory;
    class UsageStatsWidget;
    class ContextUsageBar;
    class ProtocolBase;
    class AgentsWidget;
    class ToolSettingsWidget;

    class QT_LLM_API SettingsDialog : public QDialog
    {
        Q_OBJECT
    public:
        // Where requests go. Values are stable (safe to persist as int).
        enum class Provider {
            Claude,             // Anthropic API key (api.anthropic.com or a compatible gateway)
            Ollama,             // local models
            Foundry,            // Claude deployed in Microsoft Foundry, own key + endpoint
            ClaudeSubscription, // personal claude.ai plan via the installed Claude Code CLI
            OpenAICompatible    // LM Studio, llama.cpp, vLLM, ... (OpenAI Chat Completions API)
        };

        // Which providers the user can choose from. All by default; an
        // application that, say, must stay local offers only those.
        void setAvailableProviders(const QList<Provider>& providers);

        explicit SettingsDialog(QWidget* parent = nullptr);
        ~SettingsDialog();

        Provider provider() const;
        QString apiKey() const;
        QString model() const;
        QString endpointUrl() const;
        QString ollamaUrl() const;
        QString foundryApiKey() const;
        // Messages endpoint of the Foundry resource. A bare resource/base URL
        // typed by the user is completed with "/v1/messages".
        QString foundryEndpointUrl() const;
        QString openAiUrl() const;
        QString openAiApiKey() const;
        QString systemPrompt() const;
        Effort effort() const;
        // Context tab: limit, automatic compaction, strategy.
        ContextPolicy contextPolicy() const;
        int fontSizePercent() const;
        // Debug option: render tool calls as collapsible cards in the chat.
        bool showToolCalls() const;

        void setProvider(Provider provider);
        void setApiKey(const QString& key);
        void setModel(const QString& model);
        void setEndpointUrl(const QString& url);
        void setOllamaUrl(const QString& url);
        void setFoundryApiKey(const QString& key);
        void setFoundryEndpointUrl(const QString& url);
        void setOpenAiUrl(const QString& url);
        void setOpenAiApiKey(const QString& key);
        void setSystemPrompt(const QString& prompt);
        void setEffort(Effort effort);
        void setContextPolicy(const ContextPolicy& policy);
        void setFontSizePercent(int percent);
        void setShowToolCalls(bool show);

        void setFontSizeLabel(const QString& text);

        // Connect the usage statistics panel to a live UsageHistory.
        void setUsageHistory(UsageHistory* history);

        // Bind a Client: the "Tools" tab then lists every registered tool
        // (grouped, with description) and lets the user enable/disable them.
        // Toggles are applied in one batch when Apply is clicked.
        void setClient(Client* client);

        // The Tools tab's list, e.g. to hide the tools the application locked
        // (ToolSettingsWidget::setLockedToolsVisible). Which tools the user may
        // switch at all is decided on the Client: setToolUserConfigurable().
        ToolSettingsWidget* toolSettingsWidget() const { return m_toolSettings; }

        // Hide a tab the application does not want to offer. All visible by default.
        enum class Tab { Settings, Tools, Statistics, Context, Agents };
        void setTabVisible(Tab tab, bool visible);
        bool isTabVisible(Tab tab) const;

        // Push the chosen provider, its credentials, the model and the system
        // prompt into a Client. Switches the Client's provider (which starts a
        // new conversation) only when the kind of provider really changed;
        // moving between the Anthropic API and Foundry keeps the conversation.
        // Typically called from a settingsApplied() handler.
        void applyTo(Client* client) const;

        // Populate the model combo with available models (preserves current text).
        void setAvailableModels(const QStringList& models);

    signals:
        void settingsApplied();
        // Emitted when the user clicks "Modelle laden".
        void detectModelsRequested();

    private slots:
        void onProviderChanged(int index);
        void onApply();

    protected:
        void showEvent(QShowEvent* event) override;

    private:
        void setupUI();
        void updateFieldVisibility();
        void refreshToolsTab();
        void applyToolToggles();
        void refreshContextTab();
        // Fetches models for whichever provider/credentials are currently
        // typed into this dialog's own fields - independent of whatever
        // provider the app's live Client happens to be bound to, since that
        // may not match the combo the user just switched to (Client is
        // provider-locked at construction; switching this dialog's Provider
        // combo doesn't reconstruct it). Self-contained: spins up a throwaway
        // protocol instance just to list models, then discards it.
        void fetchModelsForCurrentProvider();
        void refreshSubscriptionStatus();

        QComboBox*   m_providerCombo = nullptr;
        QLineEdit*   m_apiKeyEdit = nullptr;
        QComboBox*   m_modelCombo = nullptr;
        QComboBox*   m_effortCombo = nullptr;
        QPushButton* m_detectModelsBtn = nullptr;
        QLineEdit*   m_endpointUrlEdit = nullptr;
        QLineEdit*   m_ollamaUrlEdit = nullptr;
        QLineEdit*   m_foundryKeyEdit = nullptr;
        QLineEdit*   m_foundryUrlEdit = nullptr;
        QLineEdit*   m_openAiUrlEdit = nullptr;
        QLineEdit*   m_openAiKeyEdit = nullptr;
        QLabel*      m_openAiUrlLabel = nullptr;
        QLabel*      m_openAiKeyLabel = nullptr;
        QTextEdit* m_systemPromptEdit = nullptr;
        QSpinBox* m_fontSizeSpinBox = nullptr;
        QCheckBox* m_showToolCallsCheck = nullptr;
        QDialogButtonBox* m_buttonBox = nullptr;

        QLabel* m_apiKeyLabel = nullptr;
        QLabel* m_endpointUrlLabel = nullptr;
        QLabel* m_ollamaUrlLabel = nullptr;
        QLabel* m_foundryKeyLabel = nullptr;
        QLabel* m_foundryUrlLabel = nullptr;
        QLabel* m_subscriptionLabel = nullptr;        // row label
        QLabel* m_subscriptionStatusLabel = nullptr;  // login state of the Claude Code CLI
        QLabel* m_fontSizeLabel = nullptr;

        QTabWidget*       m_tabWidget = nullptr;
        UsageStatsWidget* m_statsWidget = nullptr;
        AgentsWidget*     m_agentsWidget = nullptr;
        ContextUsageBar*  m_contextBar = nullptr;
        QLabel*           m_contextDetailsLabel = nullptr;
        QCheckBox*        m_autoCompactCheck = nullptr;
        QCheckBox*        m_fullWindowCheck = nullptr;
        QSpinBox*         m_contextLimitSpin = nullptr;
        QComboBox*        m_strategyCombo = nullptr;
        QPushButton*      m_compactNowButton = nullptr;

        QPointer<Client>          m_client;
        QPointer<ProtocolBase>    m_modelFetchProtocol;  // throwaway, see fetchModelsForCurrentProvider()
        ToolSettingsWidget*       m_toolSettings = nullptr;
    };
}
