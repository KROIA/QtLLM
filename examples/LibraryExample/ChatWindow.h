#pragma once
#include "QtLLM.h"
#include <QWidget>
#include <QTextEdit>
#include <QLineEdit>
#include <QPushButton>
#include <QJsonObject>

// Main chat window — hosts the conversation display and user input area.
class ChatWindow : public QWidget
{
    Q_OBJECT
public:
    explicit ChatWindow(const QString& apiKey,
                        const QString& endpointUrl,
                        QWidget* parent = nullptr);
    ~ChatWindow() override;

private slots:
    void onSendClicked();
    void onResponseReady(const QString& text);
    void onToolInvoked(const QString& toolName, const QJsonObject& args);
    void onErrorOccurred(const QString& message);
    void onRequestStarted();
    void onRequestFinished();

private:
    void buildUi();
    void registerTools();
    void connectSignals();

    // Appends a formatted message block to the chat display.
    void appendMessage(const QString& sender, const QString& text, const QString& color);
    void appendNote(const QString& text);

    QTextEdit*     m_display;
    QLineEdit*     m_input;
    QPushButton*   m_sendBtn;
    QtLLM::Client  m_client;
};
