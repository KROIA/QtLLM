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
#include <QSet>
#include "ProtocolBase.h"
#include "Client.h"

class QCheckBox;
class QVBoxLayout;

namespace ScriptHost
{
class ScriptEngine;
class ScriptsWidget;
}

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

        // ---- What the user may change ---------------------------------------
        // The application decides, per provider, whether it is offered, whether
        // the user may change where requests go, and which models may be used.
        // Values are set with the usual setters (setFoundryEndpointUrl(),
        // setModel(), ...); the policy says what the user may do with them.
        struct ProviderPolicy
        {
            // false: not in the Provider box.
            bool offered = true;
            // false: endpoint and API key belong to the application. The
            // address is shown greyed out, the key is not shown at all.
            bool endpointEditable = true;
            // false: the model is fixed to what setModel() gave.
            bool modelEditable = true;
            // Model names with * and ? wildcards, case-insensitive. A model
            // must match allowedModels (when not empty) and must not match
            // blockedModels. With a whitelist the user picks from a list and
            // cannot type a name; applyTo() hands both lists to the client
            // (Client::setModelFilter), which enforces them.
            QStringList allowedModels;
            QStringList blockedModels;
        };
        void setProviderPolicy(Provider provider, const ProviderPolicy& policy);
        ProviderPolicy providerPolicy(Provider provider) const;
        bool isModelAllowed(Provider provider, const QString& model) const;

        // Shorthand for ProviderPolicy::offered: exactly these are offered.
        // With a single one the Provider box is locked.
        void setAvailableProviders(const QList<Provider>& providers);

        // Single fields of the Settings tab: shown but locked, or not shown.
        // (Endpoint, key and model are per provider - see ProviderPolicy; the
        // other tabs: setTabVisible(); tools: Client::setToolUserConfigurable().)
        enum class Field { Provider, Effort, SystemPrompt, FontSize, ShowToolCalls };
        void setFieldEditable(Field field, bool editable);
        void setFieldVisible(Field field, bool visible);

        // Apply applies and leaves it open, "Apply and close" applies and
        // closes it (Accepted), Cancel drops what was not applied and closes
        // it. Open it modal with exec(), or with show() as a window beside
        // the application, which stays usable. For one that lives as long
        // as it is open: setAttribute(Qt::WA_DeleteOnClose).
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

        // Bind the application's script engine (a QtLLM::Scripting): the
        // "Scripts" tab then shows its switches, saved scripts, methods and
        // runs. The switches are applied with the Tools tab's (Apply) and
        // dropped on Cancel. The tab is ScriptHost's ScriptsWidget. In a build
        // without scripting (QTLLM_SCRIPTING=OFF) both do nothing and there is
        // no tab.
        void setScripting(ScriptHost::ScriptEngine* scripting);
        ScriptHost::ScriptsWidget* scriptsWidget() const;

        // Hide a tab the application does not want to offer. All visible by default.
        // Scripts sits after Tools; it is shown only while a Scripting object
        // is set (setScripting) and scripting is built in.
        enum class Tab { Settings, Tools, Statistics, Context, Agents, Scripts };
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
        // false: nothing applied (a model the application does not allow).
        bool onApply();

    protected:
        void showEvent(QShowEvent* event) override;

    private:
        void setupUI();
        void updateFieldVisibility();
        void applyPolicies();
        void enforceModelPolicy();
        QWidget* fieldWidget(Field field) const;
        void refreshToolsTab();
        void applyToolToggles();
        void refreshContextTab();
        void updateScriptsTab();
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
        // The ScriptsWidget, or an empty page in a build without scripting.
        // Typed plainly so that this header is the same in both builds.
        QWidget*                  m_scriptsPage = nullptr;
        QPointer<QObject>         m_scripting;
        bool                      m_scriptsTabWanted = true;    // setTabVisible(Tab::Scripts, ...)
        QList<QWidget*>           m_tabPages;                   // by Tab
        QFormLayout*              m_formLayout = nullptr;
        QLabel*                   m_modelHintLabel = nullptr;
        QMap<int, ProviderPolicy> m_policies;       // by Provider
        QSet<int>                 m_lockedFields;   // by Field
        QSet<int>                 m_hiddenFields;
    };
}
