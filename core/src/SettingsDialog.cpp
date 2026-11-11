#include "SettingsDialog.h"
#include "ToolSettingsWidget.h"
#include "OpenAiProtocol.h"
#include <QListView>
#include "UsageStatsWidget.h"
#include "UsageHistory.h"
#include "Client.h"
#include "Tool.h"
#include "ContextUsageBar.h"
#include "AgentsWidget.h"
#if SH_LIBRARY_AVAILABLE == 1
#include "ScriptEngine.h"
#include "ScriptsWidget.h"
#endif
#include "ClaudeProtocol.h"
#include "OllamaProtocol.h"
#include "ClaudeCodeProtocol.h"
#include <QJsonObject>
#include <QVBoxLayout>
#include <QPushButton>
#include <QTabWidget>
#include <QCheckBox>
#include <QScrollArea>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QGroupBox>
#include <map>

namespace QtLLM
{
    namespace
    {
        struct ProviderEntry
        {
            SettingsDialog::Provider provider;
            const char*              text;
        };
        const ProviderEntry kProviders[] = {
            {SettingsDialog::Provider::Claude, "Claude (Anthropic API key)"},
            {SettingsDialog::Provider::Ollama, "Ollama (local models)"},
            {SettingsDialog::Provider::Foundry, "Microsoft Foundry (Claude)"},
            {SettingsDialog::Provider::ClaudeSubscription, "Claude subscription (Claude Code login)"},
            {SettingsDialog::Provider::OpenAICompatible,
             "OpenAI-compatible server (LM Studio, llama.cpp, vLLM, ...)"},
        };
    }

    SettingsDialog::SettingsDialog(QWidget* parent)
        : QDialog(parent)
    {
        setupUI();
        updateFieldVisibility();
    }

    SettingsDialog::~SettingsDialog()
    {
    }

    void SettingsDialog::setupUI()
    {
        setWindowTitle("LLM Settings");
        setMinimumSize(600, 500);

        QVBoxLayout* mainLayout = new QVBoxLayout(this);

        m_tabWidget = new QTabWidget(this);

        // ---- Settings tab ----
        QWidget* settingsPage = new QWidget(this);
        QVBoxLayout* settingsLayout = new QVBoxLayout(settingsPage);
        QFormLayout* formLayout = new QFormLayout();
        m_formLayout = formLayout;

        m_providerCombo = new QComboBox(settingsPage);
        m_providerCombo->addItem("Claude (Anthropic API key)", int(Provider::Claude));
        m_providerCombo->addItem("Ollama (local models)", int(Provider::Ollama));
        m_providerCombo->addItem("Microsoft Foundry (Claude)", int(Provider::Foundry));
        m_providerCombo->addItem("Claude subscription (Claude Code login)",
                                 int(Provider::ClaudeSubscription));
        m_providerCombo->addItem("OpenAI-compatible server (LM Studio, llama.cpp, vLLM, ...)",
                                 int(Provider::OpenAICompatible));
        formLayout->addRow("Provider:", m_providerCombo);

        m_apiKeyLabel = new QLabel("API Key:", settingsPage);
        m_apiKeyEdit = new QLineEdit(settingsPage);
        m_apiKeyEdit->setEchoMode(QLineEdit::Password);
        formLayout->addRow(m_apiKeyLabel, m_apiKeyEdit);

        m_endpointUrlLabel = new QLabel("Endpoint URL:", settingsPage);
        m_endpointUrlEdit = new QLineEdit(settingsPage);
        m_endpointUrlEdit->setText("https://api.anthropic.com/v1/messages");
        formLayout->addRow(m_endpointUrlLabel, m_endpointUrlEdit);

        m_foundryKeyLabel = new QLabel("Foundry API Key:", settingsPage);
        m_foundryKeyEdit = new QLineEdit(settingsPage);
        m_foundryKeyEdit->setEchoMode(QLineEdit::Password);
        formLayout->addRow(m_foundryKeyLabel, m_foundryKeyEdit);

        m_foundryUrlLabel = new QLabel("Foundry Endpoint:", settingsPage);
        m_foundryUrlEdit = new QLineEdit(settingsPage);
        m_foundryUrlEdit->setPlaceholderText(
            "https://<resource>.services.ai.azure.com/anthropic");
        formLayout->addRow(m_foundryUrlLabel, m_foundryUrlEdit);

        m_openAiUrlLabel = new QLabel("Server URL:", settingsPage);
        m_openAiUrlEdit = new QLineEdit(settingsPage);
        m_openAiUrlEdit->setObjectName("qtllm_openAiUrl");
        m_openAiUrlEdit->setText("http://localhost:1234/v1");
        m_openAiUrlEdit->setToolTip("Base URL of the server's OpenAI-compatible API, e.g. "
                                    "http://localhost:1234/v1 (LM Studio) or http://localhost:11434/v1 (Ollama)");
        formLayout->addRow(m_openAiUrlLabel, m_openAiUrlEdit);

        m_openAiKeyLabel = new QLabel("API Key (optional):", settingsPage);
        m_openAiKeyEdit = new QLineEdit(settingsPage);
        m_openAiKeyEdit->setObjectName("qtllm_openAiKey");
        m_openAiKeyEdit->setEchoMode(QLineEdit::Password);
        formLayout->addRow(m_openAiKeyLabel, m_openAiKeyEdit);

        m_subscriptionLabel = new QLabel("Login:", settingsPage);
        m_subscriptionStatusLabel = new QLabel(settingsPage);
        m_subscriptionStatusLabel->setWordWrap(true);
        m_subscriptionStatusLabel->setTextFormat(Qt::RichText);
        formLayout->addRow(m_subscriptionLabel, m_subscriptionStatusLabel);

        m_modelCombo = new QComboBox(settingsPage);
        m_modelCombo->setEditable(true);
        m_modelCombo->setEditText(Client::defaultModel(QtLLM::Provider::Claude));
        m_detectModelsBtn = new QPushButton(QString::fromUtf16(u"Modelle laden"), settingsPage);
        auto* modelRow = new QHBoxLayout();
        modelRow->addWidget(m_modelCombo, 1);
        modelRow->addWidget(m_detectModelsBtn);
        formLayout->addRow("Model:", modelRow);

        // Shown when Apply is refused because of the model.
        m_modelHintLabel = new QLabel(settingsPage);
        m_modelHintLabel->setObjectName("qtllm_modelHint");
        m_modelHintLabel->setWordWrap(true);
        m_modelHintLabel->setStyleSheet("color: #c0392b;");
        m_modelHintLabel->setVisible(false);
        formLayout->addRow(QString(), m_modelHintLabel);

        m_effortCombo = new QComboBox(settingsPage);
        m_effortCombo->addItem("Model default", int(Effort::Default));
        m_effortCombo->addItem("Low - fastest, cheapest", int(Effort::Low));
        m_effortCombo->addItem("Medium", int(Effort::Medium));
        m_effortCombo->addItem("High", int(Effort::High));
        m_effortCombo->addItem("Extra high", int(Effort::XHigh));
        m_effortCombo->addItem("Maximum", int(Effort::Max));
        m_effortCombo->setToolTip("How much the model thinks before answering. Lower is faster and "
                                  "cheaper; raise it for hard multi-step tasks.");
        m_effortCombo->setObjectName("qtllm_effortCombo");
        formLayout->addRow("Effort:", m_effortCombo);

        m_ollamaUrlLabel = new QLabel("Ollama URL:", settingsPage);
        m_ollamaUrlEdit = new QLineEdit(settingsPage);
        m_ollamaUrlEdit->setText("http://localhost:11434");
        formLayout->addRow(m_ollamaUrlLabel, m_ollamaUrlEdit);

        m_systemPromptEdit = new QTextEdit(settingsPage);
        m_systemPromptEdit->setMinimumHeight(80);
        m_systemPromptEdit->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        formLayout->addRow("System Prompt:", m_systemPromptEdit);

        m_fontSizeLabel = new QLabel("Font Size:", settingsPage);
        m_fontSizeSpinBox = new QSpinBox(settingsPage);
        m_fontSizeSpinBox->setRange(50, 200);
        m_fontSizeSpinBox->setValue(100);
        m_fontSizeSpinBox->setSuffix(" %");
        m_fontSizeSpinBox->setSingleStep(10);
        formLayout->addRow(m_fontSizeLabel, m_fontSizeSpinBox);

        m_showToolCallsCheck = new QCheckBox(
            QString::fromUtf16(u"Tool-Calls im Chat anzeigen"), settingsPage);
        m_showToolCallsCheck->setToolTip(QString::fromUtf16(
            u"Debug: zeigt jeden Tool-Call als aufklappbare Karte im Chat "
            u"(Name, Beschreibung, Parameter, Ergebnis)."));
        formLayout->addRow("Debug:", m_showToolCallsCheck);

        settingsLayout->addLayout(formLayout, 1);
        settingsLayout->addStretch();

        m_tabWidget->addTab(settingsPage, "Settings");

        // ---- Tools tab ----
        // All registered tools with a check box each, once a Client is bound
        // via setClient(). The same widget serves applications that have a
        // settings window of their own.
        m_toolSettings = new ToolSettingsWidget(this);
        m_tabWidget->addTab(m_toolSettings, "Tools");

        // ---- Scripts tab ----
        // Hidden until setScripting() binds a script engine.
#if SH_LIBRARY_AVAILABLE == 1
        m_scriptsPage = new ScriptHost::ScriptsWidget(this);
#else
        m_scriptsPage = new QWidget(this);
#endif
        m_tabWidget->addTab(m_scriptsPage, "Scripts");

        // ---- Statistik tab ----
        m_statsWidget = new UsageStatsWidget(this);
        m_tabWidget->addTab(m_statsWidget, QString::fromUtf16(u"Statistik"));

        // ---- Context tab ----
        QWidget* contextPage = new QWidget(this);
        QVBoxLayout* contextLayout = new QVBoxLayout(contextPage);

        m_contextBar = new ContextUsageBar(contextPage);
        m_contextBar->setBarHeight(24);
        contextLayout->addWidget(m_contextBar);

        m_contextDetailsLabel = new QLabel(contextPage);
        m_contextDetailsLabel->setWordWrap(true);
        m_contextDetailsLabel->setTextFormat(Qt::RichText);
        contextLayout->addWidget(m_contextDetailsLabel);

        // How large the conversation may get, and what happens then.
        QGroupBox* policyBox = new QGroupBox("Context management", contextPage);
        QFormLayout* policyForm = new QFormLayout(policyBox);

        m_fullWindowCheck = new QCheckBox("Use the model's full context window", policyBox);
        m_fullWindowCheck->setChecked(true);
        policyForm->addRow(m_fullWindowCheck);

        m_contextLimitSpin = new QSpinBox(policyBox);
        m_contextLimitSpin->setRange(8000, 2000000);
        m_contextLimitSpin->setSingleStep(10000);
        m_contextLimitSpin->setValue(300000);
        m_contextLimitSpin->setSuffix(" tokens");
        m_contextLimitSpin->setGroupSeparatorShown(true);
        m_contextLimitSpin->setEnabled(false);
        m_contextLimitSpin->setToolTip(
            "The context to work with. A model with a 1,000,000 token window can be held at "
            "300,000: turns get cheaper and the usage bar fills up against this figure.");
        policyForm->addRow("Context limit:", m_contextLimitSpin);

        m_autoCompactCheck = new QCheckBox("Compact the conversation automatically at the limit", policyBox);
        m_autoCompactCheck->setToolTip(
            "Starts when the conversation reaches 90 % of the limit, so the next turn still fits.");
        policyForm->addRow(m_autoCompactCheck);

        m_strategyCombo = new QComboBox(policyBox);
        m_strategyCombo->addItem("Summarize the conversation (recommended)",
                                 int(ContextPolicy::Strategy::Summarize));
        m_strategyCombo->addItem("Drop the oldest messages (no request, nothing remembered)",
                                 int(ContextPolicy::Strategy::DropOldest));
        policyForm->addRow("Strategy:", m_strategyCombo);

        m_compactNowButton = new QPushButton("Compact now", policyBox);
        m_compactNowButton->setEnabled(false);
        policyForm->addRow(m_compactNowButton);

        m_fullWindowCheck->setObjectName("qtllm_fullWindowCheck");
        m_contextLimitSpin->setObjectName("qtllm_contextLimitSpin");
        m_autoCompactCheck->setObjectName("qtllm_autoCompactCheck");
        m_strategyCombo->setObjectName("qtllm_strategyCombo");
        m_compactNowButton->setObjectName("qtllm_compactNowButton");

        connect(m_fullWindowCheck, &QCheckBox::toggled, this,
                [this](bool full) { m_contextLimitSpin->setEnabled(!full); });
        connect(m_compactNowButton, &QPushButton::clicked, this, [this]() {
            if (m_client)
                m_client->compactConversation();
        });

        contextLayout->addWidget(policyBox);
        contextLayout->addStretch();

        m_tabWidget->addTab(contextPage, "Context");

        // ---- Agents tab ----
        // Binds to the global AgentRegistry, so it lists every background agent
        // in the process without the host app wiring anything up.
        m_agentsWidget = new AgentsWidget(this);
        m_tabWidget->addTab(m_agentsWidget, "Agents");
        // By Tab: the tabs are not in the order of the enum.
        m_tabPages = {settingsPage, m_toolSettings, m_statsWidget, contextPage, m_agentsWidget, m_scriptsPage};
        updateScriptsTab();

        mainLayout->addWidget(m_tabWidget, 1);

        m_buttonBox = new QDialogButtonBox(QDialogButtonBox::Apply | QDialogButtonBox::Ok
                                           | QDialogButtonBox::Cancel, this);
        m_buttonBox->button(QDialogButtonBox::Ok)->setText(tr("Apply and close"));
        m_buttonBox->button(QDialogButtonBox::Apply)->setDefault(true);
        m_buttonBox->button(QDialogButtonBox::Apply)->setFocus();
        mainLayout->addWidget(m_buttonBox);

        // Stable names: how GUI tests and widget-addressing tools find these.
        m_providerCombo->setObjectName("qtllm_providerCombo");
        m_apiKeyEdit->setObjectName("qtllm_apiKeyEdit");
        m_endpointUrlEdit->setObjectName("qtllm_endpointUrlEdit");
        m_ollamaUrlEdit->setObjectName("qtllm_ollamaUrlEdit");
        m_foundryKeyEdit->setObjectName("qtllm_foundryKeyEdit");
        m_foundryUrlEdit->setObjectName("qtllm_foundryUrlEdit");
        m_subscriptionStatusLabel->setObjectName("qtllm_subscriptionStatus");
        m_modelCombo->setObjectName("qtllm_modelCombo");
        m_detectModelsBtn->setObjectName("qtllm_detectModelsButton");
        m_systemPromptEdit->setObjectName("qtllm_systemPromptEdit");
        m_fontSizeSpinBox->setObjectName("qtllm_fontSizeSpin");
        m_showToolCallsCheck->setObjectName("qtllm_showToolCallsCheck");
        m_tabWidget->setObjectName("qtllm_settingsTabs");
        m_buttonBox->button(QDialogButtonBox::Apply)->setObjectName("qtllm_applyButton");
        m_buttonBox->button(QDialogButtonBox::Ok)->setObjectName("qtllm_applyCloseButton");
        m_buttonBox->button(QDialogButtonBox::Cancel)->setObjectName("qtllm_cancelButton");

        connect(m_providerCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, &SettingsDialog::onProviderChanged);
        connect(m_detectModelsBtn, &QPushButton::clicked, this, [this]() {
            emit detectModelsRequested();   // back-compat: apps may still react to this
            fetchModelsForCurrentProvider();
        });
        connect(m_buttonBox->button(QDialogButtonBox::Apply), &QPushButton::clicked,
                this, &SettingsDialog::onApply);
        connect(m_buttonBox->button(QDialogButtonBox::Ok), &QPushButton::clicked, this, [this]() {
            if (onApply())
                accept();
        });
        connect(m_buttonBox, &QDialogButtonBox::rejected,
                this, &QDialog::reject);
        // Cancel, Esc or the window's close button: the pending switches go.
#if SH_LIBRARY_AVAILABLE == 1
        connect(this, &QDialog::rejected, scriptsWidget(), &ScriptHost::ScriptsWidget::revert);
#endif
    }

    void SettingsDialog::updateFieldVisibility()
    {
        const Provider p = provider();
        const bool isClaude  = (p == Provider::Claude);
        const bool isOllama  = (p == Provider::Ollama);
        const bool isFoundry = (p == Provider::Foundry);
        const bool isSubscription = (p == Provider::ClaudeSubscription);

        m_apiKeyLabel->setVisible(isClaude);
        m_apiKeyEdit->setVisible(isClaude);
        m_endpointUrlLabel->setVisible(isClaude);
        m_endpointUrlEdit->setVisible(isClaude);

        m_ollamaUrlLabel->setVisible(isOllama);
        m_ollamaUrlEdit->setVisible(isOllama);

        m_foundryKeyLabel->setVisible(isFoundry);
        m_foundryKeyEdit->setVisible(isFoundry);
        m_foundryUrlLabel->setVisible(isFoundry);
        m_foundryUrlEdit->setVisible(isFoundry);

        m_subscriptionLabel->setVisible(isSubscription);
        m_subscriptionStatusLabel->setVisible(isSubscription);

        const bool isOpenAi = (p == Provider::OpenAICompatible);
        m_openAiUrlLabel->setVisible(isOpenAi);
        m_openAiUrlEdit->setVisible(isOpenAi);
        m_openAiKeyLabel->setVisible(isOpenAi);
        m_openAiKeyEdit->setVisible(isOpenAi);

        // An endpoint the application fixed: the address is shown but cannot
        // be changed, and the key is not shown at all. (Only the current
        // provider's fields are visible, so all of them can be set alike.)
        const bool fixed = !providerPolicy(p).endpointEditable;
        for (QLineEdit* url : {m_endpointUrlEdit, m_foundryUrlEdit, m_ollamaUrlEdit, m_openAiUrlEdit})
            url->setEnabled(!fixed);
        if (fixed) {
            for (QWidget* key : QList<QWidget*>{m_apiKeyLabel, m_apiKeyEdit, m_foundryKeyLabel,
                                                m_foundryKeyEdit, m_openAiKeyLabel, m_openAiKeyEdit})
                key->setVisible(false);
        }

        // Single fields the application locked or took out.
        for (Field field : {Field::Provider, Field::Effort, Field::SystemPrompt, Field::FontSize,
                            Field::ShowToolCalls}) {
            QWidget* widget = fieldWidget(field);
            const bool hidden = m_hiddenFields.contains(int(field));
            const bool locked = m_lockedFields.contains(int(field));
            widget->setVisible(!hidden);
            if (QWidget* label = m_formLayout->labelForField(widget))
                label->setVisible(!hidden);
            if (field == Field::SystemPrompt)
                m_systemPromptEdit->setReadOnly(locked);   // still readable and scrollable
            else if (field == Field::Provider)
                widget->setEnabled(!locked && m_providerCombo->count() > 1);
            else
                widget->setEnabled(!locked);
        }
    }

    QWidget* SettingsDialog::fieldWidget(Field field) const
    {
        switch (field) {
        case Field::Provider:      return m_providerCombo;
        case Field::Effort:        return m_effortCombo;
        case Field::SystemPrompt:  return m_systemPromptEdit;
        case Field::FontSize:      return m_fontSizeSpinBox;
        case Field::ShowToolCalls: break;
        }
        return m_showToolCallsCheck;
    }

    void SettingsDialog::setFieldEditable(Field field, bool editable)
    {
        if (editable)
            m_lockedFields.remove(int(field));
        else
            m_lockedFields.insert(int(field));
        updateFieldVisibility();
    }

    void SettingsDialog::setFieldVisible(Field field, bool visible)
    {
        if (visible)
            m_hiddenFields.remove(int(field));
        else
            m_hiddenFields.insert(int(field));
        updateFieldVisibility();
    }

    void SettingsDialog::setProviderPolicy(Provider provider, const ProviderPolicy& policy)
    {
        m_policies[int(provider)] = policy;
        applyPolicies();
    }

    SettingsDialog::ProviderPolicy SettingsDialog::providerPolicy(Provider provider) const
    {
        return m_policies.value(int(provider));
    }

    bool SettingsDialog::isModelAllowed(Provider provider, const QString& model) const
    {
        const ProviderPolicy policy = providerPolicy(provider);
        return (policy.allowedModels.isEmpty() || Client::modelMatches(model, policy.allowedModels))
            && !Client::modelMatches(model, policy.blockedModels);
    }

    void SettingsDialog::applyPolicies()
    {
        // The Provider box holds the offered providers only: a hidden entry
        // could still be reached with the arrow keys.
        const Provider before = provider();
        {
            QSignalBlocker blocker(m_providerCombo);
            m_providerCombo->clear();
            for (const ProviderEntry& entry : kProviders) {
                if (providerPolicy(entry.provider).offered)
                    m_providerCombo->addItem(QString::fromLatin1(entry.text), int(entry.provider));
            }
            const int index = m_providerCombo->findData(int(before));
            m_providerCombo->setCurrentIndex(index >= 0 ? index : 0);
        }
        if (m_providerCombo->count() > 0 && provider() != before) {
            onProviderChanged(m_providerCombo->currentIndex());   // as if the user had switched
            return;
        }
        updateFieldVisibility();
        enforceModelPolicy();
    }

    void SettingsDialog::enforceModelPolicy()
    {
        const Provider p = provider();
        const ProviderPolicy policy = providerPolicy(p);
        QString current = m_modelCombo->currentText();

        for (int i = m_modelCombo->count() - 1; i >= 0; --i) {
            if (!isModelAllowed(p, m_modelCombo->itemText(i)))
                m_modelCombo->removeItem(i);
        }
        // With a whitelist the user chooses from the list and cannot type a
        // name of their own.
        const bool pickOnly = !policy.allowedModels.isEmpty();
        if (m_modelCombo->isEditable() == pickOnly)
            m_modelCombo->setEditable(!pickOnly);
        if (pickOnly) {
            // Entries without wildcards are real names: they can be offered
            // before the provider's own list is in, or when it has none.
            for (const QString& name : policy.allowedModels) {
                if (!name.contains(QLatin1Char('*')) && !name.contains(QLatin1Char('?'))
                    && isModelAllowed(p, name) && m_modelCombo->findText(name) < 0)
                    m_modelCombo->addItem(name);
            }
        }
        if (policy.modelEditable && !isModelAllowed(p, current))
            current = m_modelCombo->count() > 0 ? m_modelCombo->itemText(0) : QString();
        setModel(current);

        m_modelCombo->setEnabled(policy.modelEditable);
        m_detectModelsBtn->setEnabled(policy.modelEditable);
        m_modelHintLabel->setVisible(false);
    }

    void SettingsDialog::setAvailableProviders(const QList<Provider>& providers)
    {
        for (const ProviderEntry& entry : kProviders)
            m_policies[int(entry.provider)].offered = providers.contains(entry.provider);
        applyPolicies();
    }

    void SettingsDialog::refreshSubscriptionStatus()
    {
        if (ClaudeCodeProtocol::findExecutable().isEmpty()) {
            m_subscriptionStatusLabel->setText(
                "<span style='color:#c0392b;'>Claude Code is not installed.</span> Install it from "
                "<a href='https://claude.com/claude-code'>claude.com/claude-code</a>, run "
                "<code>claude</code> in a terminal and log in with your claude.ai account.");
            return;
        }
        m_subscriptionStatusLabel->setText("Checking Claude Code login...");
        ClaudeCodeProtocol::queryAuthStatus(this, [this](const QJsonObject& status) {
            if (!status["loggedIn"].toBool()) {
                m_subscriptionStatusLabel->setText(
                    "<span style='color:#c0392b;'>Not logged in.</span> Run <code>claude</code> "
                    "in a terminal and use <code>/login</code> with your claude.ai account.");
                return;
            }
            const QString plan = status["subscriptionType"].toString();
            if (plan.isEmpty()) {
                // Logged in, but with an API key / Console account: usage is
                // then billed per token, which is not what this option promises.
                m_subscriptionStatusLabel->setText(QString(
                    "<span style='color:#b9770e;'>Claude Code is logged in via %1, not a "
                    "claude.ai subscription</span> - requests are billed to that account.")
                    .arg(status["authMethod"].toString().toHtmlEscaped()));
                return;
            }
            m_subscriptionStatusLabel->setText(QString(
                "<span style='color:#1e8449;'>Logged in</span> as %1 (%2 plan). Requests use "
                "your subscription's limits; no API key is needed.")
                .arg(status["email"].toString().toHtmlEscaped(), plan.toHtmlEscaped()));
        });
    }

    void SettingsDialog::onProviderChanged(int index)
    {
        // Drop the other provider's model list instead of mixing entries
        // from both into one combo.
        Q_UNUSED(index);
        m_modelCombo->clear();
        switch (provider()) {
        case Provider::Ollama:             m_modelCombo->setEditText("llama3.2:latest"); break;
        case Provider::ClaudeSubscription: m_modelCombo->setEditText("sonnet"); break;
        case Provider::OpenAICompatible:   break;   // whatever the server lists
        case Provider::Claude:
        case Provider::Foundry:            m_modelCombo->setEditText(Client::defaultModel(QtLLM::Provider::Claude)); break;
        }

        updateFieldVisibility();
        enforceModelPolicy();
        emit detectModelsRequested();  // back-compat: apps may still react to this
        fetchModelsForCurrentProvider();
    }

    void SettingsDialog::fetchModelsForCurrentProvider()
    {
        // Drop any still-in-flight throwaway fetch from a previous click/switch.
        if (m_modelFetchProtocol)
            delete m_modelFetchProtocol;

        switch (provider()) {
        case Provider::Claude:
            if (apiKey().isEmpty() || endpointUrl().isEmpty())
                return;
            m_modelFetchProtocol = new ClaudeProtocol(apiKey(), QUrl(endpointUrl()), this);
            break;
        case Provider::Foundry:
            // Foundry lists deployments, not models; where the endpoint has no
            // model list the combo simply keeps what the user typed.
            if (foundryApiKey().isEmpty() || foundryEndpointUrl().isEmpty())
                return;
            m_modelFetchProtocol = new ClaudeProtocol(foundryApiKey(), QUrl(foundryEndpointUrl()), this);
            break;
        case Provider::Ollama:
            if (ollamaUrl().isEmpty())
                return;
            // fetchModels() only uses the host/port (it rewrites the path to
            // /api/tags itself), so a full chat-endpoint-style URL here is fine.
            m_modelFetchProtocol = new OllamaProtocol(QUrl(ollamaUrl()), this);
            break;
        case Provider::ClaudeSubscription:
            refreshSubscriptionStatus();
            m_modelFetchProtocol = new ClaudeCodeProtocol(this);
            break;
        case Provider::OpenAICompatible:
            if (openAiUrl().isEmpty())
                return;
            m_modelFetchProtocol = new OpenAiProtocol(openAiUrl(), openAiApiKey(), this);
            break;
        }

        connect(m_modelFetchProtocol, &ProtocolBase::modelsFetched,
                this, &SettingsDialog::setAvailableModels);
        m_modelFetchProtocol->fetchModels();
    }

    bool SettingsDialog::onApply()
    {
        // A typed model the application does not allow: nothing is applied.
        if (providerPolicy(provider()).modelEditable && !isModelAllowed(provider(), model())) {
            m_modelHintLabel->setText(model().isEmpty()
                ? tr("Choose a model.")
                : tr("The model \"%1\" is not available in this application.").arg(model()));
            m_modelHintLabel->setVisible(true);
            m_tabWidget->setCurrentIndex(0);
            return false;
        }
        m_modelHintLabel->setVisible(false);
        applyToolToggles();
#if SH_LIBRARY_AVAILABLE == 1
        scriptsWidget()->apply();
#endif
        emit settingsApplied();
        return true;
    }

    void SettingsDialog::showEvent(QShowEvent* event)
    {
        refreshToolsTab();
#if SH_LIBRARY_AVAILABLE == 1
        scriptsWidget()->refresh();
#endif
        refreshContextTab();
        emit detectModelsRequested();  // back-compat: apps may still react to this
        fetchModelsForCurrentProvider();
        QDialog::showEvent(event);
    }

    void SettingsDialog::setClient(Client* client)
    {
        m_client = client;
        m_compactNowButton->setEnabled(m_client != nullptr);
        m_toolSettings->setClient(client);
        refreshContextTab();
        if (m_client) {
            connect(m_client, &Client::contextChanged, this, &SettingsDialog::refreshContextTab);
        }
    }

    void SettingsDialog::refreshContextTab()
    {
        if (!m_client) {
            m_contextBar->setBreakdown(ContextBreakdown{});
            m_contextDetailsLabel->setText(
                "No client bound - call SettingsDialog::setClient() to show live context usage here.");
            return;
        }

        const ContextBreakdown b = m_client->contextBreakdown();
        m_contextBar->setBreakdown(b);

        const bool exact = b.exactUsedTokens >= 0;
        m_contextDetailsLabel->setText(QString(
            "<b>System prompt:</b> ~%1 tokens<br>"
            "<b>Tools:</b> ~%2 tokens<br>"
            "<b>Messages:</b> ~%3 tokens<br>"
            "<b>Total used:</b> %4%5 / %6 tokens (%7%)<br><br>"
            "<span style='color:#888;'>Per-segment split above is a character-count (chars / 4) "
            "estimate - no tokenizer API breaks a count down by section. %8</span>")
            .arg(b.systemPromptTokens)
            .arg(b.toolsTokens)
            .arg(b.messagesTokens)
            .arg(exact ? "" : "~")
            .arg(b.bestUsedTokens())
            .arg(b.contextWindowTokens)
            .arg(100.0 * b.bestUsedTokens() / qMax(b.contextWindowTokens, 1), 0, 'f', 1)
            .arg(exact
                ? "Total is confirmed by the provider's own tokenizer API."
                : "Total is still an estimate too - waiting on the provider's tokenizer API."));
    }

    void SettingsDialog::refreshToolsTab()
    {
        m_toolSettings->refresh();
    }

    void SettingsDialog::applyToolToggles()
    {
        m_toolSettings->apply();
    }

    void SettingsDialog::setTabVisible(Tab tab, bool visible)
    {
        if (tab == Tab::Scripts) {
            m_scriptsTabWanted = visible;
            updateScriptsTab();
            return;
        }
        m_tabWidget->setTabVisible(m_tabWidget->indexOf(m_tabPages.value(int(tab))), visible);
    }

    bool SettingsDialog::isTabVisible(Tab tab) const
    {
        return m_tabWidget->isTabVisible(m_tabWidget->indexOf(m_tabPages.value(int(tab))));
    }

#if SH_LIBRARY_AVAILABLE == 1
    void SettingsDialog::setScripting(ScriptHost::ScriptEngine* scripting)
    {
        if (m_scripting)
            disconnect(m_scripting, &QObject::destroyed, this, &SettingsDialog::updateScriptsTab);
        m_scripting = scripting;
        if (m_scripting)
            connect(m_scripting, &QObject::destroyed, this, &SettingsDialog::updateScriptsTab);
        scriptsWidget()->setScripting(scripting);
        updateScriptsTab();
    }

    ScriptHost::ScriptsWidget* SettingsDialog::scriptsWidget() const
    {
        return static_cast<ScriptHost::ScriptsWidget*>(m_scriptsPage);
    }
#else
    void SettingsDialog::setScripting(ScriptHost::ScriptEngine*)
    {
    }

    ScriptHost::ScriptsWidget* SettingsDialog::scriptsWidget() const
    {
        return nullptr;
    }
#endif

    void SettingsDialog::updateScriptsTab()
    {
        m_tabWidget->setTabVisible(m_tabWidget->indexOf(m_scriptsPage),
                                   m_scriptsTabWanted && m_scripting);
    }

    SettingsDialog::Provider SettingsDialog::provider() const
    {
        return static_cast<Provider>(m_providerCombo->currentData().toInt());
    }

    QString SettingsDialog::foundryApiKey() const { return m_foundryKeyEdit->text(); }

    QString SettingsDialog::foundryEndpointUrl() const
    {
        QString url = m_foundryUrlEdit->text().trimmed();
        if (url.isEmpty() || url.endsWith("/messages"))
            return url;
        while (url.endsWith('/'))
            url.chop(1);
        return url + (url.endsWith("/v1") ? "/messages" : "/v1/messages");
    }

    void SettingsDialog::setFoundryApiKey(const QString& key) { m_foundryKeyEdit->setText(key); }
    void SettingsDialog::setFoundryEndpointUrl(const QString& url) { m_foundryUrlEdit->setText(url); }

    void SettingsDialog::applyTo(Client* client) const
    {
        if (!client)
            return;

        QtLLM::Provider target = QtLLM::Provider::Claude;
        QString url, key;
        switch (provider()) {
        case Provider::Claude:  url = endpointUrl();        key = apiKey();        break;
        case Provider::Foundry: url = foundryEndpointUrl(); key = foundryApiKey(); break;
        case Provider::Ollama:  url = ollamaUrl(); target = QtLLM::Provider::Ollama; break;
        case Provider::ClaudeSubscription: target = QtLLM::Provider::ClaudeCode;    break;
        case Provider::OpenAICompatible:
            url = openAiUrl(); key = openAiApiKey(); target = QtLLM::Provider::OpenAI;
            break;
        }

        if (client->provider() != target) {
            client->setProvider(target, url, key);
        } else {
            client->setApiKey(key);
            client->setEndpointUrl(url);
        }
        // The client enforces the same model rules the dialog applied.
        const ProviderPolicy modelPolicy = providerPolicy(provider());
        client->setModelFilter(modelPolicy.allowedModels, modelPolicy.blockedModels);
        client->setModel(model());
        client->setSystemPrompt(systemPrompt());
        client->setEffort(effort());

        // Keeps what the dialog has no control for (the trigger ratio).
        ContextPolicy policy = contextPolicy();
        policy.triggerRatio = client->contextPolicy().triggerRatio;
        client->setContextPolicy(policy);
    }

    QString SettingsDialog::apiKey() const { return m_apiKeyEdit->text(); }
    QString SettingsDialog::model() const { return m_modelCombo->currentText(); }
    QString SettingsDialog::endpointUrl() const { return m_endpointUrlEdit->text(); }
    QString SettingsDialog::ollamaUrl() const { return m_ollamaUrlEdit->text(); }
    QString SettingsDialog::openAiUrl() const { return m_openAiUrlEdit->text().trimmed(); }
    QString SettingsDialog::openAiApiKey() const { return m_openAiKeyEdit->text(); }
    QString SettingsDialog::systemPrompt() const { return m_systemPromptEdit->toPlainText(); }
    ContextPolicy SettingsDialog::contextPolicy() const
    {
        ContextPolicy policy;
        policy.autoCompact = m_autoCompactCheck->isChecked();
        policy.limitTokens = m_fullWindowCheck->isChecked() ? 0 : m_contextLimitSpin->value();
        policy.strategy = static_cast<ContextPolicy::Strategy>(m_strategyCombo->currentData().toInt());
        return policy;
    }

    void SettingsDialog::setContextPolicy(const ContextPolicy& policy)
    {
        m_autoCompactCheck->setChecked(policy.autoCompact);
        m_fullWindowCheck->setChecked(policy.limitTokens <= 0);
        if (policy.limitTokens > 0)
            m_contextLimitSpin->setValue(policy.limitTokens);
        m_strategyCombo->setCurrentIndex(m_strategyCombo->findData(int(policy.strategy)));
    }

    Effort SettingsDialog::effort() const { return static_cast<Effort>(m_effortCombo->currentData().toInt()); }
    void SettingsDialog::setEffort(Effort effort) { m_effortCombo->setCurrentIndex(m_effortCombo->findData(int(effort))); }
    int SettingsDialog::fontSizePercent() const { return m_fontSizeSpinBox->value(); }
    bool SettingsDialog::showToolCalls() const { return m_showToolCallsCheck->isChecked(); }

    void SettingsDialog::setProvider(Provider provider)
    {
        // Programmatic selection only restores state: unlike a user changing
        // the combo it must not reset the model field to that provider's default.
        const int index = m_providerCombo->findData(int(provider));
        if (index < 0)
            return;   // not offered by the application
        QSignalBlocker blocker(m_providerCombo);
        m_providerCombo->setCurrentIndex(index);
        updateFieldVisibility();
        enforceModelPolicy();
    }
    void SettingsDialog::setApiKey(const QString& key) { m_apiKeyEdit->setText(key); }
    void SettingsDialog::setModel(const QString& model)
    {
        // A saved model that is no longer allowed is not put back.
        if (!model.isEmpty() && providerPolicy(provider()).modelEditable
            && !isModelAllowed(provider(), model))
            return;
        if (m_modelCombo->isEditable()) {
            m_modelCombo->setEditText(model);
            return;
        }
        if (!model.isEmpty() && m_modelCombo->findText(model) < 0)
            m_modelCombo->addItem(model);
        m_modelCombo->setCurrentText(model);
    }
    void SettingsDialog::setEndpointUrl(const QString& url) { m_endpointUrlEdit->setText(url); }
    void SettingsDialog::setOllamaUrl(const QString& url) { m_ollamaUrlEdit->setText(url); }
    void SettingsDialog::setOpenAiUrl(const QString& url) { m_openAiUrlEdit->setText(url); }
    void SettingsDialog::setOpenAiApiKey(const QString& key) { m_openAiKeyEdit->setText(key); }
    void SettingsDialog::setSystemPrompt(const QString& prompt) { m_systemPromptEdit->setPlainText(prompt); }
    void SettingsDialog::setFontSizePercent(int percent) { m_fontSizeSpinBox->setValue(percent); }

    void SettingsDialog::setShowToolCalls(bool show) { m_showToolCallsCheck->setChecked(show); }

    void SettingsDialog::setFontSizeLabel(const QString& text) { m_fontSizeLabel->setText(text); }

    void SettingsDialog::setUsageHistory(UsageHistory* history)
    {
        if (m_statsWidget)
            m_statsWidget->setUsageHistory(history);
    }

    void SettingsDialog::setAvailableModels(const QStringList& fetched)
    {
        QString current = m_modelCombo->currentText();
        // Only what the application allows for this provider.
        QStringList models;
        for (const QString& name : fetched) {
            if (isModelAllowed(provider(), name))
                models.append(name);
        }
        // Add models not already present
        for (const QString& m : models) {
            if (m_modelCombo->findText(m) < 0)
                m_modelCombo->addItem(m);
        }
        // A fixed model stays what the application set (adding the first item
        // to an empty box selects it, so it has to be put back).
        if (!providerPolicy(provider()).modelEditable) {
            setModel(current);
            return;
        }

        // The current text is often just a hardcoded guess (set on provider
        // switch before the real list is known) - if it isn't actually one of
        // the models this provider offers, auto-select a real one instead of
        // keeping a name that will fail at request time.
        if (!models.isEmpty() && !models.contains(current))
            current = models.first();

        setModel(current);
    }
}
