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

class QCheckBox;
class QVBoxLayout;

namespace QtLLM
{
    class Client;
    class UsageHistory;
    class UsageStatsWidget;
    class ContextUsageBar;
    class ProtocolBase;

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

        // Bind a Client: the "Tools" tab then lists every registered tool
        // (grouped, with description) and lets the user enable/disable them.
        // Toggles are applied in one batch when Apply is clicked.
        void setClient(Client* client);

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

        QComboBox*   m_providerCombo = nullptr;
        QLineEdit*   m_apiKeyEdit = nullptr;
        QComboBox*   m_modelCombo = nullptr;
        QPushButton* m_detectModelsBtn = nullptr;
        QLineEdit*   m_endpointUrlEdit = nullptr;
        QLineEdit*   m_ollamaUrlEdit = nullptr;
        QTextEdit* m_systemPromptEdit = nullptr;
        QSpinBox* m_fontSizeSpinBox = nullptr;
        QDialogButtonBox* m_buttonBox = nullptr;

        QLabel* m_apiKeyLabel = nullptr;
        QLabel* m_endpointUrlLabel = nullptr;
        QLabel* m_ollamaUrlLabel = nullptr;
        QLabel* m_fontSizeLabel = nullptr;

        QTabWidget*       m_tabWidget = nullptr;
        UsageStatsWidget* m_statsWidget = nullptr;
        ContextUsageBar*  m_contextBar = nullptr;
        QLabel*           m_contextDetailsLabel = nullptr;

        QPointer<Client>          m_client;
        QPointer<ProtocolBase>    m_modelFetchProtocol;  // throwaway, see fetchModelsForCurrentProvider()
        QWidget*                  m_toolsPage = nullptr;
        QVBoxLayout*              m_toolsLayout = nullptr;
        QMap<QString, QCheckBox*> m_toolChecks;   // tool name -> checkbox
    };
}
