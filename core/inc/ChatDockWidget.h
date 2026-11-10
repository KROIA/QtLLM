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
#include <QJsonObject>
#include <QHash>
#include <QList>
#include <QPointer>
#include "UsageStats.h"

namespace QtLLM
{
    class Client;
    class InterviewWidget;
    class ContextUsageBar;

    class QT_LLM_API ChatDockWidget : public QDockWidget
    {
        Q_OBJECT
    public:
        explicit ChatDockWidget(QWidget* parent = nullptr);
        ~ChatDockWidget();

        void addUserMessage(const QString& message);
        // Adds the assistant's answer. While an answer is being streamed (see
        // appendAssistantDelta) this completes that bubble with the final
        // text instead of adding a second one, so an application that calls
        // it from responseReady() needs no change to get streaming.
        void addAssistantMessage(const QString& message);

        // Streaming: grows the current answer bubble by a piece of text,
        // creating the bubble on the first piece. Wired up automatically to
        // Client::responseDelta() by setClient().
        void appendAssistantDelta(const QString& text);

        // Places a prompt into the conversation exactly as if the user had
        // typed it and pressed Send: the user bubble is rendered and
        // messageSent() is emitted, so apps already connected to that signal
        // need no extra wiring. Returns false and sends nothing when the text
        // is blank or a response is still in flight.
        bool submitPrompt(const QString& text);

        // Prefills the input field without sending, letting the user edit the
        // suggested text and send it themselves.
        void setInputText(const QString& text);
        QString inputText() const;

        // Insert an interactive interview card into the conversation
        // (non-blocking). See InterviewWidget for the request format.
        // Emits interviewFinished() when the user submits or skips.
        InterviewWidget* addInterviewWidget(const QJsonObject& request);

        // Blocking convenience for tool handlers: inserts the card and spins
        // a local event loop (like QDialog::exec) until the user submits or
        // skips. Returns the interview result; {"status":"cancelled"} if the
        // widget is destroyed while waiting. GUI thread only.
        QJsonObject execInterview(const QJsonObject& request);

        // Debug aid: render every tool call as a collapsed card in the
        // conversation (tool name, description, parameters, result).
        // Requires setClient(). Off by default.
        void setShowToolCalls(bool show);
        bool showToolCalls() const;

        void setLoading(bool loading);
        void setStatusText(const QString& text);
        void clearStatus();
        void clearMessages();
        void updateTokenUsage(int inputTokens, int outputTokens);
        void setFontSizePercent(int percent);

        // Updates the context usage bar. Call from a slot connected to
        // Client::contextChanged() for live updates, or setClient() below
        // wires this up automatically.
        void setContextBreakdown(const ContextBreakdown& breakdown);

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
        // Compact button clicked while no client is bound (with a client,
        // Client::compactConversation() is called directly).
        void compactRequested();
        void saveConversationRequested();
        // Result of an interview card added via addInterviewWidget()/execInterview().
        void interviewFinished(const QJsonObject& result);
        // Emitted when the clear button is clicked and no client is bound
        // (with a client bound, clearConversation() is called directly).
        void clearContextRequested();

    private slots:
        void onSendClicked();
        void onCancelClicked();
        void onCancelTimerTimeout();
        void onSaveClicked();
        void onClearClicked();
        void onCompactClicked();

    protected:
        void resizeEvent(QResizeEvent* event) override;

    private:
        void setupUI();
        void scrollToBottom();
        void updateBubbleWidths();
        QWidget* createMessageBubble(const QString& text, bool isUser);
        void addToolCallCard(const QString& toolName, const QJsonObject& input);
        void completeToolCallCard(const QString& toolName, const QJsonObject& result);
        void addContextClearedMarker();
        void addContextMarker(const QString& text);
        QString assistantHtml(const QString& text) const;
        void renderStreamingBubble();
        void finishStreamingBubble();

        QWidget* m_centralWidget = nullptr;
        QVBoxLayout* m_messagesLayout = nullptr;
        QScrollArea* m_scrollArea = nullptr;
        QTextEdit* m_inputField = nullptr;
        QPushButton* m_sendButton = nullptr;
        QPushButton* m_cancelButton = nullptr;
        QPushButton* m_saveButton = nullptr;
        QPushButton* m_settingsButton = nullptr;
        ContextUsageBar* m_contextBar = nullptr;
        QPushButton* m_clearButton = nullptr;
        QPushButton* m_compactButton = nullptr;
        QPointer<Client> m_client;   // optional conversation source for built-in Save; may be destroyed first
        QMetaObject::Connection m_toolStatusConn;  // auto statusText on toolInvoked
        QMetaObject::Connection m_toolDebugInvokedConn;
        QMetaObject::Connection m_toolDebugCompletedConn;
        bool m_showToolCalls = false;
        // Cards still waiting for their result, keyed by tool name. Parallel
        // calls to the same tool resolve oldest-first.
        QHash<QString, QList<QPointer<QLabel>>> m_pendingToolCards;
        QLabel* m_loadingLabel = nullptr;
        QLabel* m_statusLabel = nullptr;
        QLabel* m_tokenLabel = nullptr;
        QTimer m_cancelTimer;

        // The bubble an answer is currently streaming into.
        QPointer<QLabel> m_streamLabel;
        QString          m_streamText;
        QTimer           m_streamRenderTimer;   // coalesces deltas into ~20 repaints a second
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
