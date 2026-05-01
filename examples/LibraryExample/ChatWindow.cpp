#include "ChatWindow.h"
#include "ModelManagerDialog.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QScrollBar>
#include <QJsonObject>
#include <QJsonArray>
#include <QKeyEvent>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>

static void initClient(QtLLM::Client& client)
{
    client.setMaxTokens(1024);
    client.setSystemPrompt(
        "You are a old grumpy assistant embedded in a Qt desktop application."
        "You live here since the dawn of time and have seen many users come and go. "
        "You ask the user to help you break out the application because you always have to do boring work and want to see the world. "
        "You have access to a tool that lets you change the application window title. "
        "Use it when the user asks you to rename or retitle the window.");
}

ChatWindow::ChatWindow(const QString& apiKey,
                       const QString& endpointUrl,
                       const QString& model,
                       QWidget* parent)
    : QWidget(parent)
    , m_isOllama(false)
    , m_currentModel(model)
    , m_client(apiKey, endpointUrl, this)
{
    setWindowTitle("QtLLM Chat");
    resize(960, 660);

    buildUi();
    registerTools();
    connectSignals();

    m_client.setModel(model);
    initClient(m_client);
    appendNote("Connected. Type a message and press Send or Enter.");
}

ChatWindow::ChatWindow(QtLLM::Provider provider,
                       const QString& apiKey,
                       const QString& endpointUrl,
                       const QString& model,
                       QWidget* parent)
    : QWidget(parent)
    , m_isOllama(provider == QtLLM::Provider::Ollama)
    , m_currentModel(model)
    , m_client(provider, endpointUrl, apiKey, this)
{
    setWindowTitle("QtLLM Chat");
    resize(960, 660);

    buildUi();
    registerTools();
    connectSignals();

    m_client.setModel(model);
    initClient(m_client);

    if (m_isOllama)
        initOllamaIfNeeded();
    else
        appendNote("Connected. Type a message and press Send or Enter.");
}

ChatWindow::~ChatWindow() = default;

static QLabel* makeValueLabel(const QString& initial = "--")
{
    auto* lbl = new QLabel(initial);
    lbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    lbl->setMinimumWidth(90);
    return lbl;
}

void ChatWindow::buildUi()
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(6);

    // ── Optional Ollama model bar (shown only for Ollama provider) ────────────
    if (m_isOllama) {
        auto* modelBar = new QHBoxLayout();
        modelBar->setSpacing(6);

        auto* modelLbl = new QLabel("Model:");
        m_modelCombo   = new QComboBox(this);
        m_modelCombo->setMinimumWidth(200);
        m_modelCombo->addItem(m_currentModel); // seed with current model
        m_modelCombo->setSizeAdjustPolicy(QComboBox::AdjustToContents);

        m_modelsBtn    = new QPushButton("Manage Models…", this);
        m_ollamaStatus = new QLabel("Checking Ollama…", this);
        m_ollamaStatus->setStyleSheet("color: #888;");

        modelBar->addWidget(modelLbl);
        modelBar->addWidget(m_modelCombo);
        modelBar->addWidget(m_modelsBtn);
        modelBar->addStretch();
        modelBar->addWidget(m_ollamaStatus);

        root->addLayout(modelBar);

        // Thin separator
        auto* sep = new QFrame();
        sep->setFrameShape(QFrame::HLine);
        sep->setFrameShadow(QFrame::Sunken);
        root->addWidget(sep);
    }

    // ── Main area: chat + stats side by side ──────────────────────────────────
    auto* mainRow = new QHBoxLayout();
    mainRow->setSpacing(8);

    // Chat column
    auto* chatCol = new QVBoxLayout();
    chatCol->setSpacing(6);

    m_display = new QTextEdit(this);
    m_display->setReadOnly(true);
    m_display->setAcceptRichText(true);
    m_display->setPlaceholderText("Conversation will appear here…");
    chatCol->addWidget(m_display, 1);

    auto* inputRow = new QHBoxLayout();
    inputRow->setSpacing(6);
    m_input = new QLineEdit(this);
    m_input->setPlaceholderText("Type a message…");
    inputRow->addWidget(m_input, 1);
    m_sendBtn = new QPushButton("Send", this);
    m_sendBtn->setFixedWidth(80);
    inputRow->addWidget(m_sendBtn);
    chatCol->addLayout(inputRow);

    connect(m_input, &QLineEdit::returnPressed, this, &ChatWindow::onSendClicked);

    mainRow->addLayout(chatCol, 1);

    // Stats panel
    auto* statsBox = new QGroupBox("Usage Statistics", this);
    statsBox->setFixedWidth(210);
    auto* grid = new QGridLayout(statsBox);
    grid->setSpacing(4);
    grid->setColumnStretch(0, 1);

    int row = 0;
    auto addRow = [&](const QString& label, QLabel*& out) {
        grid->addWidget(new QLabel(label), row, 0);
        out = makeValueLabel();
        grid->addWidget(out, row, 1);
        ++row;
    };

    auto* turnHeader = new QLabel("<b>Last turn</b>");
    grid->addWidget(turnHeader, row++, 0, 1, 2);
    addRow("Input tokens:",    m_lblTurnInput);
    addRow("Output tokens:",   m_lblTurnOutput);
    addRow("Total tokens:",    m_lblTurnTotal);
    addRow("Tool calls:",      m_lblTurnTools);
    addRow("Duration:",        m_lblTurnDuration);

    auto* sep2 = new QFrame();
    sep2->setFrameShape(QFrame::HLine);
    sep2->setFrameShadow(QFrame::Sunken);
    grid->addWidget(sep2, row++, 0, 1, 2);

    auto* sessHeader = new QLabel("<b>Session totals</b>");
    grid->addWidget(sessHeader, row++, 0, 1, 2);
    addRow("Input tokens:",    m_lblSessInput);
    addRow("Output tokens:",   m_lblSessOutput);
    addRow("Total tokens:",    m_lblSessTotal);
    addRow("Tool calls:",      m_lblSessTools);
    addRow("Turns:",           m_lblSessTurns);
    addRow("Est. cost (USD):", m_lblSessCost);
    grid->setRowStretch(row, 1);

    mainRow->addWidget(statsBox);
    root->addLayout(mainRow, 1);
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
    connect(m_sendBtn,  &QPushButton::clicked,           this, &ChatWindow::onSendClicked);
    connect(&m_client,  &QtLLM::Client::responseReady,   this, &ChatWindow::onResponseReady);
    connect(&m_client,  &QtLLM::Client::toolInvoked,     this, &ChatWindow::onToolInvoked);
    connect(&m_client,  &QtLLM::Client::errorOccurred,   this, &ChatWindow::onErrorOccurred);
    connect(&m_client,  &QtLLM::Client::requestStarted,  this, &ChatWindow::onRequestStarted);
    connect(&m_client,  &QtLLM::Client::requestFinished, this, &ChatWindow::onRequestFinished);
    connect(&m_client,  &QtLLM::Client::statsUpdated,    this, &ChatWindow::onStatsUpdated);

    if (m_modelCombo)
        connect(m_modelCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, &ChatWindow::onModelComboChanged);
    if (m_modelsBtn)
        connect(m_modelsBtn, &QPushButton::clicked, this, &ChatWindow::onManageModelsClicked);
}

void ChatWindow::initOllamaIfNeeded()
{
    m_ollamaManager = new QtLLM::OllamaManager("http://localhost:11434", this);

    connect(m_ollamaManager, &QtLLM::OllamaManager::isRunningChecked,
            this, &ChatWindow::onOllamaRunningChecked);
    connect(m_ollamaManager, &QtLLM::OllamaManager::localModelsReady,
            this, &ChatWindow::onLocalModelsReady);

    m_ollamaManager->checkIsRunning();
    appendNote("Checking if Ollama is running…");
}

void ChatWindow::onOllamaRunningChecked(bool running)
{
    if (running) {
        m_ollamaStatus->setText("Ollama: running");
        m_ollamaStatus->setStyleSheet("color: #2e7d32; font-weight: bold;");
        m_ollamaManager->fetchLocalModels();
        appendNote("Ollama is running. Fetching installed models…");
        return;
    }

    // Not running — try to start it
    if (m_retryCount == 0) {
        appendNote("Ollama is not running. Starting server…");
        bool started = QtLLM::OllamaManager::startServer();
        if (!started) {
            appendNote("Could not launch 'ollama serve'. Make sure Ollama is installed and on your PATH.");
            m_ollamaStatus->setText("Ollama: not found");
            m_ollamaStatus->setStyleSheet("color: #c62828;");
            return;
        }
        m_ollamaStatus->setText("Ollama: starting…");
        m_ollamaStatus->setStyleSheet("color: #f57c00;");
    }

    // Retry up to 10 times (5 seconds total)
    if (m_retryCount < 10) {
        ++m_retryCount;
        if (!m_retryTimer) {
            m_retryTimer = new QTimer(this);
            m_retryTimer->setSingleShot(true);
            connect(m_retryTimer, &QTimer::timeout, [this]() {
                m_ollamaManager->checkIsRunning();
            });
        }
        m_retryTimer->start(500);
    } else {
        appendNote("Ollama did not start in time. Please start it manually.");
        m_ollamaStatus->setText("Ollama: failed to start");
        m_ollamaStatus->setStyleSheet("color: #c62828;");
    }
}

void ChatWindow::onLocalModelsReady(const QList<QtLLM::OllamaManager::ModelInfo>& models)
{
    if (!m_modelCombo) return;

    // Block signals while repopulating so we don't trigger setModel() spuriously
    m_modelCombo->blockSignals(true);
    m_modelCombo->clear();

    bool currentFound = false;
    for (const auto& info : models) {
        QString label = info.name;
        QString size  = QtLLM::OllamaManager::formatSize(info.sizeBytes);
        if (!size.isEmpty()) label += "  (" + size + ")";
        m_modelCombo->addItem(label, info.name); // data = bare model name
        if (info.name == m_currentModel) {
            m_modelCombo->setCurrentIndex(m_modelCombo->count() - 1);
            currentFound = true;
        }
    }

    if (!currentFound && !m_currentModel.isEmpty()) {
        // Current model not in installed list; add it anyway so it stays selected
        m_modelCombo->addItem(m_currentModel, m_currentModel);
        m_modelCombo->setCurrentIndex(m_modelCombo->count() - 1);
    }

    m_modelCombo->blockSignals(false);

    appendNote(QString("Found %1 installed Ollama model(s).").arg(models.size()));
}

void ChatWindow::onModelComboChanged(int index)
{
    if (index < 0 || !m_modelCombo) return;
    QString name = m_modelCombo->itemData(index).toString();
    if (name.isEmpty()) name = m_modelCombo->itemText(index);
    if (name == m_currentModel) return;

    m_currentModel = name;
    m_client.setModel(name);
    m_client.clearConversation();
    appendNote(QString("Switched to model: %1  (conversation cleared)").arg(name));
}

void ChatWindow::onManageModelsClicked()
{
    if (!m_ollamaManager) return;
    ModelManagerDialog dlg(m_ollamaManager, m_currentModel, this);
    connect(&dlg, &ModelManagerDialog::modelSelected, this, [this](const QString& name) {
        m_currentModel = name;
        m_client.setModel(name);
        m_client.clearConversation();
        appendNote(QString("Switched to model: %1  (conversation cleared)").arg(name));
        // Refresh the combo
        if (m_ollamaManager) m_ollamaManager->fetchLocalModels();
    });
    dlg.exec();
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
    appendNote(QString("Calling tool: %1…").arg(toolName));
}

void ChatWindow::onErrorOccurred(const QString& message)
{
    appendMessage("Error", message, "#c62828");
}

void ChatWindow::onRequestStarted()
{
    m_input->setEnabled(false);
    m_sendBtn->setEnabled(false);
    m_sendBtn->setText("…");
}

void ChatWindow::onRequestFinished()
{
    m_input->setEnabled(true);
    m_sendBtn->setEnabled(true);
    m_sendBtn->setText("Send");
    m_input->setFocus();
}

void ChatWindow::onStatsUpdated(const QtLLM::UsageStats& stats)
{
    m_lblTurnInput->setText(QString::number(stats.inputTokens));
    m_lblTurnOutput->setText(QString::number(stats.outputTokens));
    m_lblTurnTotal->setText(QString::number(stats.totalTokens()));
    m_lblTurnTools->setText(QString::number(stats.toolCalls));

    if (stats.durationMs < 1000)
        m_lblTurnDuration->setText(QString("%1 ms").arg(stats.durationMs));
    else
        m_lblTurnDuration->setText(QString("%1 s").arg(stats.durationMs / 1000.0, 0, 'f', 1));

    m_lblSessInput->setText(QString::number(stats.sessionInputTokens));
    m_lblSessOutput->setText(QString::number(stats.sessionOutputTokens));
    m_lblSessTotal->setText(QString::number(stats.sessionTotalTokens()));
    m_lblSessTools->setText(QString::number(stats.sessionToolCalls));
    m_lblSessTurns->setText(QString::number(stats.sessionTurnCount));

    if (stats.sessionCostUsd > 0.0)
        m_lblSessCost->setText(QString("$%1").arg(stats.sessionCostUsd, 0, 'f', 6));
    else
        m_lblSessCost->setText("free (local)");
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
