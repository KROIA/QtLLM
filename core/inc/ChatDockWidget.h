#pragma once
#include "QtLLM_base.h"
#include <QDockWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QLabel>
#include <QTimer>

namespace QtLLM
{
    class Client;

    class QT_LLM_API ChatDockWidget : public QDockWidget
    {
        Q_OBJECT
    public:
        explicit ChatDockWidget(QWidget* parent = nullptr);
        ~ChatDockWidget();

        void addUserMessage(const QString& message);
        void addAssistantMessage(const QString& message);
        void setLoading(bool loading);
        void setStatusText(const QString& text);
        void clearStatus();
        void clearMessages();
        void updateTokenUsage(int inputTokens, int outputTokens);
        void setFontSizePercent(int percent);

        // Bubble name labels. Set to an empty string to hide the header entirely
        // on that side; otherwise the given text is shown (HTML-escaped).
        void setAssistantName(const QString& name);
        QString assistantName() const;
        void setUserName(const QString& name);
        QString userName() const;

        // Bind the conversation source. When set, the Save button saves the
        // conversation itself (file dialog + JSON export) — no app wiring needed.
        // If no client is set, the button falls back to emitting
        // saveConversationRequested() for custom handling.
        void setClient(Client* client);

        void setSendButtonText(const QString& text);
        void setCancelButtonText(const QString& text);
        void setSettingsButtonTooltip(const QString& text);
        void setInputPlaceholderText(const QString& text);
        void setLoadingText(const QString& text);
        void setCancelledText(const QString& text);
        void setBusyWarningText(const QString& text);

    signals:
        void messageSent(const QString& message);
        void cancelRequested();
        void settingsRequested();
        void saveConversationRequested();

    private slots:
        void onSendClicked();
        void onCancelClicked();
        void onCancelTimerTimeout();
        void onSaveClicked();

    protected:
        void resizeEvent(QResizeEvent* event) override;

    private:
        void setupUI();
        void scrollToBottom();
        void updateBubbleWidths();
        QWidget* createMessageBubble(const QString& text, bool isUser);

        QWidget* m_centralWidget = nullptr;
        QVBoxLayout* m_messagesLayout = nullptr;
        QScrollArea* m_scrollArea = nullptr;
        QTextEdit* m_inputField = nullptr;
        QPushButton* m_sendButton = nullptr;
        QPushButton* m_cancelButton = nullptr;
        QPushButton* m_saveButton = nullptr;
        QPushButton* m_settingsButton = nullptr;
        Client* m_client = nullptr;  // optional conversation source for built-in Save
        QLabel* m_loadingLabel = nullptr;
        QLabel* m_statusLabel = nullptr;
        QLabel* m_tokenLabel = nullptr;
        QTimer m_cancelTimer;
        QTimer m_tooltipTimer;
        bool m_autoScroll = true;
        bool m_programmaticScroll = false;
        bool m_isLoading = false;
        int m_fontSizePercent = 100;

        QString m_assistantName = QStringLiteral("Assistant");
        QString m_userName = QStringLiteral("You");
        QString m_cancelledText = QStringLiteral("Request cancelled.");
        QString m_busyWarningText = QStringLiteral("Please wait — a response is still being processed.");
    };
}
