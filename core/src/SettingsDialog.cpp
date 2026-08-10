#include "SettingsDialog.h"
#include "UsageStatsWidget.h"
#include "UsageHistory.h"
#include "Client.h"
#include "Tool.h"
#include <QVBoxLayout>
#include <QPushButton>
#include <QTabWidget>
#include <QCheckBox>
#include <QScrollArea>
#include <QShowEvent>
#include <map>

namespace QtLLM
{
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

        m_providerCombo = new QComboBox(settingsPage);
        m_providerCombo->addItem("Claude (Anthropic)");
        m_providerCombo->addItem("Ollama (Local)");
        formLayout->addRow("Provider:", m_providerCombo);

        m_apiKeyLabel = new QLabel("API Key:", settingsPage);
        m_apiKeyEdit = new QLineEdit(settingsPage);
        m_apiKeyEdit->setEchoMode(QLineEdit::Password);
        formLayout->addRow(m_apiKeyLabel, m_apiKeyEdit);

        m_endpointUrlLabel = new QLabel("Endpoint URL:", settingsPage);
        m_endpointUrlEdit = new QLineEdit(settingsPage);
        m_endpointUrlEdit->setText("https://api.anthropic.com/v1/messages");
        formLayout->addRow(m_endpointUrlLabel, m_endpointUrlEdit);

        m_modelCombo = new QComboBox(settingsPage);
        m_modelCombo->setEditable(true);
        m_modelCombo->setEditText("claude-haiku-4-5");
        m_detectModelsBtn = new QPushButton(QString::fromUtf16(u"Modelle laden"), settingsPage);
        auto* modelRow = new QHBoxLayout();
        modelRow->addWidget(m_modelCombo, 1);
        modelRow->addWidget(m_detectModelsBtn);
        formLayout->addRow("Model:", modelRow);

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

        settingsLayout->addLayout(formLayout, 1);
        settingsLayout->addStretch();

        m_tabWidget->addTab(settingsPage, "Settings");

        // ---- Tools tab ----
        // Scrollable list of all registered tools, filled by refreshToolsTab()
        // once a Client is bound via setClient().
        QScrollArea* toolsScroll = new QScrollArea(this);
        toolsScroll->setWidgetResizable(true);
        toolsScroll->setFrameShape(QFrame::NoFrame);
        m_toolsPage = new QWidget(toolsScroll);
        m_toolsLayout = new QVBoxLayout(m_toolsPage);
        m_toolsLayout->setSpacing(4);
        m_toolsLayout->addStretch();
        toolsScroll->setWidget(m_toolsPage);
        m_tabWidget->addTab(toolsScroll, "Tools");

        // ---- Statistik tab ----
        m_statsWidget = new UsageStatsWidget(this);
        m_tabWidget->addTab(m_statsWidget, QString::fromUtf16(u"Statistik"));

        mainLayout->addWidget(m_tabWidget, 1);

        m_buttonBox = new QDialogButtonBox(QDialogButtonBox::Apply | QDialogButtonBox::Cancel, this);
        m_buttonBox->button(QDialogButtonBox::Apply)->setDefault(true);
        m_buttonBox->button(QDialogButtonBox::Apply)->setFocus();
        mainLayout->addWidget(m_buttonBox);

        connect(m_providerCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, &SettingsDialog::onProviderChanged);
        connect(m_detectModelsBtn, &QPushButton::clicked,
                this, &SettingsDialog::detectModelsRequested);
        connect(m_buttonBox->button(QDialogButtonBox::Apply), &QPushButton::clicked,
                this, &SettingsDialog::onApply);
        connect(m_buttonBox, &QDialogButtonBox::rejected,
                this, &QDialog::reject);
    }

    void SettingsDialog::updateFieldVisibility()
    {
        bool isClaude = (provider() == Provider::Claude);

        m_apiKeyLabel->setVisible(isClaude);
        m_apiKeyEdit->setVisible(isClaude);
        m_endpointUrlLabel->setVisible(isClaude);
        m_endpointUrlEdit->setVisible(isClaude);

        m_ollamaUrlLabel->setVisible(!isClaude);
        m_ollamaUrlEdit->setVisible(!isClaude);
    }

    void SettingsDialog::onProviderChanged(int index)
    {
        if (static_cast<Provider>(index) == Provider::Claude)
            m_modelCombo->setEditText("claude-haiku-4-5");
        else
            m_modelCombo->setEditText("llama3.2:latest");

        updateFieldVisibility();
    }

    void SettingsDialog::onApply()
    {
        applyToolToggles();
        emit settingsApplied();
        accept();
    }

    void SettingsDialog::showEvent(QShowEvent* event)
    {
        refreshToolsTab();
        QDialog::showEvent(event);
    }

    void SettingsDialog::setClient(Client* client)
    {
        m_client = client;
        refreshToolsTab();
    }

    void SettingsDialog::refreshToolsTab()
    {
        // Clear previous entries (everything except the trailing stretch).
        // Delete synchronously — deleteLater() posted before exec() enters its
        // event loop is not processed until the modal dialog closes, leaving
        // the removed (never laid out) widgets visible as a pile at (0,0).
        m_toolChecks.clear();
        while (m_toolsLayout->count() > 1) {
            QLayoutItem* item = m_toolsLayout->takeAt(0);
            if (item->widget()) {
                item->widget()->hide();
                delete item->widget();
            }
            delete item;
        }

        auto addInfoLabel = [this](const QString& text) {
            QLabel* label = new QLabel(text, m_toolsPage);
            label->setWordWrap(true);
            label->setStyleSheet("color: #888;");
            m_toolsLayout->insertWidget(m_toolsLayout->count() - 1, label);
        };

        if (!m_client) {
            addInfoLabel("No client bound — call SettingsDialog::setClient() "
                         "to list the registered tools here.");
            return;
        }

        const QList<Tool> tools = m_client->registeredTools();
        if (tools.isEmpty()) {
            addInfoLabel("No tools registered.");
            return;
        }

        // Group tools by their (optional) group metadata; ungrouped last.
        std::map<QString, QList<Tool>> groups;
        for (const Tool& tool : tools)
            groups[tool.group()].append(tool);

        auto addGroup = [this](const QString& title, const QList<Tool>& groupTools) {
            QLabel* header = new QLabel(title, m_toolsPage);
            header->setStyleSheet("font-weight: bold; margin-top: 8px;");
            m_toolsLayout->insertWidget(m_toolsLayout->count() - 1, header);

            for (const Tool& tool : groupTools) {
                const QString display = tool.title().isEmpty() ? tool.name()
                                                               : tool.title();
                QCheckBox* check = new QCheckBox(
                    QString("%1  (%2)").arg(display, tool.name()), m_toolsPage);
                check->setChecked(m_client && m_client->isToolEnabled(tool.name()));
                m_toolsLayout->insertWidget(m_toolsLayout->count() - 1, check);
                m_toolChecks.insert(tool.name(), check);

                if (!tool.description().isEmpty()) {
                    QLabel* desc = new QLabel(tool.description(), m_toolsPage);
                    desc->setWordWrap(true);
                    desc->setStyleSheet("color: #888; margin-left: 24px;");
                    m_toolsLayout->insertWidget(m_toolsLayout->count() - 1, desc);
                }
            }
        };

        for (const auto& entry : groups) {
            if (!entry.first.isEmpty())
                addGroup(entry.first, entry.second);
        }
        auto ungrouped = groups.find(QString());
        if (ungrouped != groups.end())
            addGroup(QString::fromUtf16(u"Other"), ungrouped->second);
    }

    void SettingsDialog::applyToolToggles()
    {
        if (!m_client)
            return;
        for (auto it = m_toolChecks.constBegin(); it != m_toolChecks.constEnd(); ++it)
            m_client->setToolEnabled(it.key(), it.value()->isChecked());
    }

    SettingsDialog::Provider SettingsDialog::provider() const
    {
        return static_cast<Provider>(m_providerCombo->currentIndex());
    }

    QString SettingsDialog::apiKey() const { return m_apiKeyEdit->text(); }
    QString SettingsDialog::model() const { return m_modelCombo->currentText(); }
    QString SettingsDialog::endpointUrl() const { return m_endpointUrlEdit->text(); }
    QString SettingsDialog::ollamaUrl() const { return m_ollamaUrlEdit->text(); }
    QString SettingsDialog::systemPrompt() const { return m_systemPromptEdit->toPlainText(); }
    int SettingsDialog::fontSizePercent() const { return m_fontSizeSpinBox->value(); }

    void SettingsDialog::setProvider(Provider provider) { m_providerCombo->setCurrentIndex(static_cast<int>(provider)); }
    void SettingsDialog::setApiKey(const QString& key) { m_apiKeyEdit->setText(key); }
    void SettingsDialog::setModel(const QString& model) { m_modelCombo->setEditText(model); }
    void SettingsDialog::setEndpointUrl(const QString& url) { m_endpointUrlEdit->setText(url); }
    void SettingsDialog::setOllamaUrl(const QString& url) { m_ollamaUrlEdit->setText(url); }
    void SettingsDialog::setSystemPrompt(const QString& prompt) { m_systemPromptEdit->setPlainText(prompt); }
    void SettingsDialog::setFontSizePercent(int percent) { m_fontSizeSpinBox->setValue(percent); }

    void SettingsDialog::setFontSizeLabel(const QString& text) { m_fontSizeLabel->setText(text); }

    void SettingsDialog::setUsageHistory(UsageHistory* history)
    {
        if (m_statsWidget)
            m_statsWidget->setUsageHistory(history);
    }

    void SettingsDialog::setAvailableModels(const QStringList& models)
    {
        QString current = m_modelCombo->currentText();
        // Add models not already present
        for (const QString& m : models) {
            if (m_modelCombo->findText(m) < 0)
                m_modelCombo->addItem(m);
        }
        // Preserve the user's typed/selected value
        m_modelCombo->setEditText(current);
    }
}
