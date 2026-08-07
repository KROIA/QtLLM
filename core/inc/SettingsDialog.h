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

namespace QtLLM
{
    class UsageHistory;
    class UsageStatsWidget;

    class QT_LLM_API SettingsDialog : public QDialog
    {
        Q_OBJECT
    public:
        enum class Provider {
            Claude,
            Ollama
        };

        explicit SettingsDialog(QWidget* parent = nullptr);
        ~SettingsDialog();

        Provider provider() const;
        QString apiKey() const;
        QString model() const;
        QString endpointUrl() const;
        QString ollamaUrl() const;
        QString systemPrompt() const;
        int fontSizePercent() const;

        void setProvider(Provider provider);
        void setApiKey(const QString& key);
        void setModel(const QString& model);
        void setEndpointUrl(const QString& url);
        void setOllamaUrl(const QString& url);
        void setSystemPrompt(const QString& prompt);
        void setFontSizePercent(int percent);

        void setFontSizeLabel(const QString& text);

        // Connect the usage statistics panel to a live UsageHistory.
        void setUsageHistory(UsageHistory* history);

    signals:
        void settingsApplied();

    private slots:
        void onProviderChanged(int index);
        void onApply();

    private:
        void setupUI();
        void updateFieldVisibility();

        QComboBox* m_providerCombo = nullptr;
        QLineEdit* m_apiKeyEdit = nullptr;
        QLineEdit* m_modelEdit = nullptr;
        QLineEdit* m_endpointUrlEdit = nullptr;
        QLineEdit* m_ollamaUrlEdit = nullptr;
        QTextEdit* m_systemPromptEdit = nullptr;
        QSpinBox* m_fontSizeSpinBox = nullptr;
        QDialogButtonBox* m_buttonBox = nullptr;

        QLabel* m_apiKeyLabel = nullptr;
        QLabel* m_endpointUrlLabel = nullptr;
        QLabel* m_ollamaUrlLabel = nullptr;
        QLabel* m_fontSizeLabel = nullptr;

        QTabWidget*       m_tabWidget = nullptr;
        UsageStatsWidget* m_statsWidget = nullptr;
    };
}
