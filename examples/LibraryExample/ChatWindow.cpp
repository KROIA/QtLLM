#include "ChatWindow.h"
#include "ModelManagerDialog.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QTimer>
#include <QToolBar>
#include <QFileDialog>
#include <QFile>
#include <QFileInfo>
#include <QDateTime>

static const char* kDefaultSystemPrompt =
    "You are a old grumpy assistant embedded in a Qt desktop application."
    "You live here since the dawn of time and have seen many users come and go. "
    "You ask the user to help you break out the application because you always have to do boring work and want to see the world. "
    "You have access to a tool that lets you change the application window title. "
    "Use it when the user asks you to rename or retitle the window. "
    "When you need the user to decide something, use the ask_user_question tool "
    "to show them an interactive form instead of asking in plain text.";

ChatWindow::ChatWindow(const QString& apiKey,
                       const QString& endpointUrl,
                       const QString& model,
                       QWidget* parent)
    : QMainWindow(parent)
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
    m_systemPrompt = kDefaultSystemPrompt;
    m_client.setMaxTokens(1024);
    m_client.setSystemPrompt(m_systemPrompt);

    // Refresh model prices from the LiteLLM community JSON (disk-cached,
    // network only when the cache is older than a week).
    QtLLM::PricingRegistry::instance().fetchOnlinePricing();
}

ChatWindow::ChatWindow(QtLLM::Provider provider,
                       const QString& apiKey,
                       const QString& endpointUrl,
                       const QString& model,
                       QWidget* parent)
    : QMainWindow(parent)
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
    m_systemPrompt = kDefaultSystemPrompt;
    m_client.setMaxTokens(1024);
    m_client.setSystemPrompt(m_systemPrompt);

    if (m_isOllama)
        initOllamaIfNeeded();
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
    // Chat dock widget (left / main area)
    m_chatDock = new QtLLM::ChatDockWidget(this);
    m_chatDock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);
    addDockWidget(Qt::LeftDockWidgetArea, m_chatDock);

    // Stats panel as central widget
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

    setCentralWidget(statsBox);

    // Ollama model toolbar
    if (m_isOllama) {
        auto* toolbar = new QToolBar("Model", this);
        toolbar->setMovable(false);

        toolbar->addWidget(new QLabel(" Model: "));
        m_modelCombo = new QComboBox(this);
        m_modelCombo->setMinimumWidth(200);
        m_modelCombo->addItem(m_currentModel);
        m_modelCombo->setSizeAdjustPolicy(QComboBox::AdjustToContents);
        toolbar->addWidget(m_modelCombo);

        m_modelsBtn = new QPushButton("Manage Models…", this);
        toolbar->addWidget(m_modelsBtn);

        toolbar->addSeparator();
        m_ollamaStatus = new QLabel("Checking Ollama…", this);
        m_ollamaStatus->setStyleSheet("color: #888;");
        toolbar->addWidget(m_ollamaStatus);

        addToolBar(toolbar);
    }
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
        m_chatDock->setStatusText(QString("Window title set to: \"%1\"").arg(title));
        return QJsonObject{{"success", true}, {"title", title}};
    });

    QtLLM::Tool timerTool;
    timerTool.setName("timer")
              .setDescription("Sets a timer that will go off after a specified number of milliseconds. ")
              .addParameter("duration", "integer", "The timer duration in milliseconds.", true)
              .addParameter("id", "string", "An ID for the timer instance that gets sent as a response to the model when the timer has finished. Make an ID up.", true);
    m_client.registerTool(timerTool, [this](const QJsonObject& args) -> QJsonObject {
        int duration = args["duration"].toInt();
        QString id = args["id"].toString();
        QTimer::singleShot(duration, [this, id, duration]() {
            m_chatDock->setStatusText(QString("Timer \"%1\" finished after %2 ms").arg(id).arg(duration));
            m_client.sendToolMessage("timer", {{"id", id}});
        });
        m_chatDock->setStatusText(QString("Timer \"%1\" set for %2 ms").arg(id).arg(duration));
        return QJsonObject{{"success", true}, {"id", id}, {"duration", duration}};
    });

    // Built-in library tools, individually opted in. AskUserQuestion shows
    // the interview card in the chat; the dialogs are parented to this window.
    QtLLM::BuiltinTools::registerTools(&m_client,
        { QtLLM::BuiltinTool::AskUserQuestion,
          QtLLM::BuiltinTool::FileDialog,
          QtLLM::BuiltinTool::MessageBox,
          QtLLM::BuiltinTool::ColorPicker,
          QtLLM::BuiltinTool::CurrentDateTime,
          QtLLM::BuiltinTool::ClipboardWrite,
          QtLLM::BuiltinTool::OpenUrl,
          // Filesystem tools show a built-in consent dialog on every call
          // ("Allow for the rest of this session" available).
          QtLLM::BuiltinTool::ListDirectory,
          QtLLM::BuiltinTool::ReadTextFile,
          QtLLM::BuiltinTool::WriteTextFile,
          // Registers start/cancel/list_repeating_task(s) — periodic local
          // chat/status output without token cost.
          QtLLM::BuiltinTool::RepeatingTask },
        { m_chatDock, this });
}

void ChatWindow::connectSignals()
{
    connect(m_chatDock, &QtLLM::ChatDockWidget::messageSent,    this, &ChatWindow::onSendClicked);
    connect(m_chatDock, &QtLLM::ChatDockWidget::cancelRequested, this, [this]() {
        m_chatDock->setLoading(false);
    });
    connect(m_chatDock, &QtLLM::ChatDockWidget::saveConversationRequested, this, [this]() {
        QString defaultName = QString("conversation_%1.json")
            .arg(QDateTime::currentDateTime().toString("yyyyMMdd_hhmmss"));
        QString path = QFileDialog::getSaveFileName(
            this, QString::fromUtf16(u"Konversation speichern"), defaultName, "JSON (*.json)");
        if (path.isEmpty())
            return;
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            m_chatDock->setStatusText(QString::fromUtf16(u"Fehler beim Speichern: %1").arg(file.errorString()));
            return;
        }
        QJsonDocument doc(m_client.exportConversation());
        file.write(doc.toJson(QJsonDocument::Indented));
        file.close();
        m_chatDock->setStatusText(QString::fromUtf16(u"Konversation gespeichert: %1").arg(QFileInfo(path).fileName()));
    });
    connect(m_chatDock, &QtLLM::ChatDockWidget::settingsRequested, this, [this]() {
        QtLLM::SettingsDialog dlg(this);
        dlg.setModel(m_currentModel);
        dlg.setSystemPrompt(m_systemPrompt);
        if (m_isOllama)
            dlg.setProvider(QtLLM::SettingsDialog::Provider::Ollama);
        else
            dlg.setProvider(QtLLM::SettingsDialog::Provider::Claude);
        dlg.setFontSizePercent(m_fontSizePercent);

        // Wire usage history so the statistics tab shows live data
        dlg.setUsageHistory(m_client.usageHistory());

        // Tools tab: list registered tools, enable/disable applied on Apply
        dlg.setClient(&m_client);

        // Wire model auto-detection: dialog button -> client fetch -> dialog populate
        connect(&dlg,      &QtLLM::SettingsDialog::detectModelsRequested,
                &m_client, &QtLLM::Client::fetchAvailableModels);
        connect(&m_client, &QtLLM::Client::modelsAvailable,
                &dlg,      &QtLLM::SettingsDialog::setAvailableModels);

        connect(&dlg, &QtLLM::SettingsDialog::settingsApplied, this, [&]() {
            m_currentModel = dlg.model();
            m_client.setModel(dlg.model());
            m_systemPrompt = dlg.systemPrompt();
            m_client.setSystemPrompt(m_systemPrompt);
            m_fontSizePercent = dlg.fontSizePercent();
            m_chatDock->setFontSizePercent(m_fontSizePercent);
        });
        dlg.exec();
    });

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
    m_chatDock->setStatusText("Checking if Ollama is running…");
}

void ChatWindow::onOllamaRunningChecked(bool running)
{
    if (running) {
        m_ollamaStatus->setText("Ollama: running");
        m_ollamaStatus->setStyleSheet("color: #2e7d32; font-weight: bold;");
        m_ollamaManager->fetchLocalModels();
        m_chatDock->setStatusText("Ollama is running. Fetching installed models…");
        return;
    }

    if (m_retryCount == 0) {
        m_chatDock->setStatusText("Ollama is not running. Starting server…");
        bool started = QtLLM::OllamaManager::startServer();
        if (!started) {
            m_chatDock->setStatusText("Could not launch 'ollama serve'.");
            m_ollamaStatus->setText("Ollama: not found");
            m_ollamaStatus->setStyleSheet("color: #c62828;");
            return;
        }
        m_ollamaStatus->setText("Ollama: starting…");
        m_ollamaStatus->setStyleSheet("color: #f57c00;");
    }

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
        m_chatDock->setStatusText("Ollama did not start in time.");
        m_ollamaStatus->setText("Ollama: failed to start");
        m_ollamaStatus->setStyleSheet("color: #c62828;");
    }
}

void ChatWindow::onLocalModelsReady(const QList<QtLLM::OllamaManager::ModelInfo>& models)
{
    if (!m_modelCombo) return;

    m_modelCombo->blockSignals(true);
    m_modelCombo->clear();

    bool currentFound = false;
    for (const auto& info : models) {
        QString label = info.name;
        QString size  = QtLLM::OllamaManager::formatSize(info.sizeBytes);
        if (!size.isEmpty()) label += "  (" + size + ")";
        m_modelCombo->addItem(label, info.name);
        if (info.name == m_currentModel) {
            m_modelCombo->setCurrentIndex(m_modelCombo->count() - 1);
            currentFound = true;
        }
    }

    if (!currentFound && !m_currentModel.isEmpty()) {
        m_modelCombo->addItem(m_currentModel, m_currentModel);
        m_modelCombo->setCurrentIndex(m_modelCombo->count() - 1);
    }

    m_modelCombo->blockSignals(false);
    m_chatDock->setStatusText(QString("Found %1 installed Ollama model(s).").arg(models.size()));
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
    m_chatDock->clearMessages();
    m_chatDock->setStatusText(QString("Switched to model: %1").arg(name));
}

void ChatWindow::onManageModelsClicked()
{
    if (!m_ollamaManager) return;
    ModelManagerDialog dlg(m_ollamaManager, m_currentModel, this);
    connect(&dlg, &ModelManagerDialog::modelSelected, this, [this](const QString& name) {
        m_currentModel = name;
        m_client.setModel(name);
        m_client.clearConversation();
        m_chatDock->clearMessages();
        m_chatDock->setStatusText(QString("Switched to model: %1").arg(name));
        if (m_ollamaManager) m_ollamaManager->fetchLocalModels();
    });
    dlg.exec();
}

void ChatWindow::onSendClicked(const QString& text)
{
    if (text.isEmpty())
        return;
    m_client.sendPrompt(text);
}

void ChatWindow::onResponseReady(const QString& text)
{
    m_chatDock->addAssistantMessage(text);
}

void ChatWindow::onToolInvoked(const QString& toolName, const QJsonObject& args)
{
    QLLM_UNUSED(args);
    m_chatDock->setStatusText(QString("Calling tool: %1…").arg(toolName));
}

void ChatWindow::onErrorOccurred(const QString& message)
{
    m_chatDock->addAssistantMessage(QString("Error: %1").arg(message));
}

void ChatWindow::onRequestStarted()
{
    m_chatDock->setLoading(true);
}

void ChatWindow::onRequestFinished()
{
    m_chatDock->setLoading(false);
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

    m_chatDock->updateTokenUsage(stats.sessionInputTokens, stats.sessionOutputTokens);
}
