#include "ChatWindow.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QScrollBar>
#include <QJsonObject>
#include <QJsonArray>
#include <QKeyEvent>

ChatWindow::ChatWindow(const QString& apiKey,
                       const QString& endpointUrl,
                       QWidget* parent)
    : QWidget(parent)
    , m_client(apiKey, endpointUrl, this)
{
    setWindowTitle("QtLLM Chat");
    resize(800, 600);

    buildUi();
    registerTools();
    connectSignals();

    m_client.setModel("claude-sonnet-4-5");
    m_client.setMaxTokens(1024);
    m_client.setSystemPrompt(
        "You are a helpful assistant embedded in a Qt desktop application. "
        "You have access to a tool that lets you change the application window title. "
        "Use it when the user asks you to rename or retitle the window.");

    appendNote("Connected. Type a message and press Send or Enter.");
}

ChatWindow::~ChatWindow() = default;

void ChatWindow::buildUi()
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(6);

    // Chat history display — read-only, rich text
    m_display = new QTextEdit(this);
    m_display->setReadOnly(true);
    m_display->setAcceptRichText(true);
    m_display->setPlaceholderText("Conversation will appear here...");
    root->addWidget(m_display, 1);

    // Input row
    auto* inputRow = new QHBoxLayout();
    inputRow->setSpacing(6);

    m_input = new QLineEdit(this);
    m_input->setPlaceholderText("Type a message...");
    inputRow->addWidget(m_input, 1);

    m_sendBtn = new QPushButton("Send", this);
    m_sendBtn->setFixedWidth(80);
    inputRow->addWidget(m_sendBtn);

    root->addLayout(inputRow);

    // Submit on Enter key in the input field
    connect(m_input, &QLineEdit::returnPressed, this, &ChatWindow::onSendClicked);
}

void ChatWindow::registerTools()
{
    QtLLM::Tool titleTool;
    titleTool.setName("setWindowTitle")
             .setDescription("Sets the application window title to the given text.")
             .addParameter("title", "string", "The new window title text.", true);

    m_client.registerTool(titleTool, [this](const QJsonObject& args) -> QJsonObject {
        QString title = args["title"].toString();
        setWindowTitle(title);
        appendNote(QString("Window title set to: \"%1\"").arg(title));
        return QJsonObject{{"success", true}, {"title", title}};
    });
}

void ChatWindow::connectSignals()
{
    connect(m_sendBtn,  &QPushButton::clicked,  this, &ChatWindow::onSendClicked);
    connect(&m_client,  &QtLLM::Client::responseReady,   this, &ChatWindow::onResponseReady);
    connect(&m_client,  &QtLLM::Client::toolInvoked,     this, &ChatWindow::onToolInvoked);
    connect(&m_client,  &QtLLM::Client::errorOccurred,   this, &ChatWindow::onErrorOccurred);
    connect(&m_client,  &QtLLM::Client::requestStarted,  this, &ChatWindow::onRequestStarted);
    connect(&m_client,  &QtLLM::Client::requestFinished, this, &ChatWindow::onRequestFinished);
}

void ChatWindow::onSendClicked()
{
    QString text = m_input->text().trimmed();
    if (text.isEmpty())
        return;

    m_input->clear();
    appendMessage("You", text, "#1a73e8");
    m_client.sendPrompt(text);
}

void ChatWindow::onResponseReady(const QString& text)
{
    appendMessage("Assistant", text, "#2e7d32");
}

void ChatWindow::onToolInvoked(const QString& toolName, const QJsonObject& args)
{
    QLLM_UNUSED(args);
    appendNote(QString("Calling tool: %1...").arg(toolName));
}

void ChatWindow::onErrorOccurred(const QString& message)
{
    appendMessage("Error", message, "#c62828");
}

void ChatWindow::onRequestStarted()
{
    m_input->setEnabled(false);
    m_sendBtn->setEnabled(false);
    m_sendBtn->setText("...");
}

void ChatWindow::onRequestFinished()
{
    m_input->setEnabled(true);
    m_sendBtn->setEnabled(true);
    m_sendBtn->setText("Send");
    m_input->setFocus();
}

void ChatWindow::appendMessage(const QString& sender, const QString& text, const QString& color)
{
    QString escaped = text.toHtmlEscaped().replace("\n", "<br>");
    QString html = QString(
        "<p style='margin:4px 0;'>"
        "<span style='color:%1; font-weight:bold;'>%2:</span> %3"
        "</p>")
        .arg(color, sender, escaped);

    m_display->append(html);
    m_display->verticalScrollBar()->setValue(m_display->verticalScrollBar()->maximum());
}

void ChatWindow::appendNote(const QString& text)
{
    QString html = QString(
        "<p style='margin:2px 0; color:#757575; font-style:italic;'>%1</p>")
        .arg(text.toHtmlEscaped());

    m_display->append(html);
    m_display->verticalScrollBar()->setValue(m_display->verticalScrollBar()->maximum());
}
