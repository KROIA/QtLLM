#include "SettingsDialog.h"
#include <QVBoxLayout>
#include <QPushButton>

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
        setMinimumSize(450, 400);

        QVBoxLayout* mainLayout = new QVBoxLayout(this);
        QFormLayout* formLayout = new QFormLayout();

        m_providerCombo = new QComboBox(this);
        m_providerCombo->addItem("Claude (Anthropic)");
        m_providerCombo->addItem("Ollama (Local)");
        formLayout->addRow("Provider:", m_providerCombo);

        m_apiKeyLabel = new QLabel("API Key:", this);
        m_apiKeyEdit = new QLineEdit(this);
        m_apiKeyEdit->setEchoMode(QLineEdit::Password);
        formLayout->addRow(m_apiKeyLabel, m_apiKeyEdit);

        m_endpointUrlLabel = new QLabel("Endpoint URL:", this);
        m_endpointUrlEdit = new QLineEdit(this);
        m_endpointUrlEdit->setText("https://api.anthropic.com/v1/messages");
        formLayout->addRow(m_endpointUrlLabel, m_endpointUrlEdit);

        m_modelEdit = new QLineEdit(this);
        m_modelEdit->setText("claude-haiku-4-5");
        formLayout->addRow("Model:", m_modelEdit);

        m_ollamaUrlLabel = new QLabel("Ollama URL:", this);
        m_ollamaUrlEdit = new QLineEdit(this);
        m_ollamaUrlEdit->setText("http://localhost:11434");
        formLayout->addRow(m_ollamaUrlLabel, m_ollamaUrlEdit);

        m_systemPromptEdit = new QTextEdit(this);
        m_systemPromptEdit->setMinimumHeight(80);
        m_systemPromptEdit->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        formLayout->addRow("System Prompt:", m_systemPromptEdit);

        m_fontSizeLabel = new QLabel("Font Size:", this);
        m_fontSizeSpinBox = new QSpinBox(this);
        m_fontSizeSpinBox->setRange(50, 200);
        m_fontSizeSpinBox->setValue(100);
        m_fontSizeSpinBox->setSuffix(" %");
        m_fontSizeSpinBox->setSingleStep(10);
        formLayout->addRow(m_fontSizeLabel, m_fontSizeSpinBox);

        mainLayout->addLayout(formLayout, 1);
        mainLayout->addStretch();

        m_buttonBox = new QDialogButtonBox(QDialogButtonBox::Apply | QDialogButtonBox::Cancel, this);
        m_buttonBox->button(QDialogButtonBox::Apply)->setDefault(true);
        m_buttonBox->button(QDialogButtonBox::Apply)->setFocus();
        mainLayout->addWidget(m_buttonBox);

        connect(m_providerCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, &SettingsDialog::onProviderChanged);
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
            m_modelEdit->setText("claude-haiku-4-5");
        else
            m_modelEdit->setText("llama3.2:latest");

        updateFieldVisibility();
    }

    void SettingsDialog::onApply()
    {
        emit settingsApplied();
        accept();
    }

    SettingsDialog::Provider SettingsDialog::provider() const
    {
        return static_cast<Provider>(m_providerCombo->currentIndex());
    }

    QString SettingsDialog::apiKey() const { return m_apiKeyEdit->text(); }
    QString SettingsDialog::model() const { return m_modelEdit->text(); }
    QString SettingsDialog::endpointUrl() const { return m_endpointUrlEdit->text(); }
    QString SettingsDialog::ollamaUrl() const { return m_ollamaUrlEdit->text(); }
    QString SettingsDialog::systemPrompt() const { return m_systemPromptEdit->toPlainText(); }
    int SettingsDialog::fontSizePercent() const { return m_fontSizeSpinBox->value(); }

    void SettingsDialog::setProvider(Provider provider) { m_providerCombo->setCurrentIndex(static_cast<int>(provider)); }
    void SettingsDialog::setApiKey(const QString& key) { m_apiKeyEdit->setText(key); }
    void SettingsDialog::setModel(const QString& model) { m_modelEdit->setText(model); }
    void SettingsDialog::setEndpointUrl(const QString& url) { m_endpointUrlEdit->setText(url); }
    void SettingsDialog::setOllamaUrl(const QString& url) { m_ollamaUrlEdit->setText(url); }
    void SettingsDialog::setSystemPrompt(const QString& prompt) { m_systemPromptEdit->setPlainText(prompt); }
    void SettingsDialog::setFontSizePercent(int percent) { m_fontSizeSpinBox->setValue(percent); }

    void SettingsDialog::setFontSizeLabel(const QString& text) { m_fontSizeLabel->setText(text); }
}
