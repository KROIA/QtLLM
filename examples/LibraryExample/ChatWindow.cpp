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
#include <QSettings>

// Persisted under the app's QSettings scope (see setOrganizationName/
// setApplicationName in main.cpp).
static const char* kShowToolCallsKey = "debug/showToolCalls";
static const char* kModelKey = "llm/model";

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
    , m_apiKey(apiKey)
    , m_endpointUrl(endpointUrl)
    , m_savedClaudeApiKey(apiKey)
    , m_savedClaudeEndpointUrl(endpointUrl)
    , m_savedOllamaUrl(QStringLiteral("http://localhost:11434/api/chat"))
    , m_client(apiKey, endpointUrl, this)
{
    setWindowTitle("QtLLM Chat");
    resize(960, 660);

    buildUi();
    registerTools();
    connectSignals();

    // A persisted model wins over the one passed in by main().
    m_currentModel = QSettings().value(kModelKey, model).toString();
    m_client.setModel(m_currentModel);
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
    , m_apiKey(apiKey)
    , m_endpointUrl(endpointUrl)
    , m_savedClaudeApiKey(m_isOllama ? QString() : apiKey)
    , m_savedClaudeEndpointUrl(m_isOllama ? QStringLiteral("https://api.anthropic.com/v1/messages") : endpointUrl)
    , m_savedOllamaUrl(m_isOllama ? endpointUrl : QStringLiteral("http://localhost:11434/api/chat"))
    , m_client(provider, endpointUrl, apiKey, this)
{
    setWindowTitle("QtLLM Chat");
    resize(960, 660);

    buildUi();
    registerTools();
    connectSignals();

    // A persisted model wins over the one passed in by main().
    m_currentModel = QSettings().value(kModelKey, model).toString();
    m_client.setModel(m_currentModel);
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

    // Central area: stats on the left, the background-agent demo on the right.
    auto* central = new QWidget(this);
    auto* centralLayout = new QHBoxLayout(central);
    centralLayout->setContentsMargins(0, 0, 0, 0);
    centralLayout->addWidget(statsBox);
    centralLayout->addWidget(buildAgentDemoPanel(), 1);
    setCentralWidget(central);

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

// ---------------------------------------------------------------------------
// Background-agent demo
//
// Everything below runs outside the chat: the agent has its own system prompt,
// its own history, and its results land in this panel instead of the
// conversation. The one exception is the Inject button, which does the
// opposite — it puts text into the chat as if the user had typed it.
// ---------------------------------------------------------------------------

QWidget* ChatWindow::buildAgentDemoPanel()
{
    auto* box = new QGroupBox("Agent Demo", this);
    auto* layout = new QVBoxLayout(box);

    auto* injectBtn  = new QPushButton("Inject prompt into chat", box);
    injectBtn->setToolTip("ChatDockWidget::submitPrompt() — appears as if the user typed it");

    auto* usageBtn   = new QPushButton("Agent: summarise usage", box);
    auto* titleBtn   = new QPushButton("Agent: suggest window title", box);
    auto* moodBtn    = new QPushButton("Agent: rate the last reply", box);
    m_killAgentBtn   = new QPushButton("Kill agent", box);

    layout->addWidget(injectBtn);

    auto* sep = new QFrame(box);
    sep->setFrameShape(QFrame::HLine);
    sep->setFrameShadow(QFrame::Sunken);
    layout->addWidget(sep);

    layout->addWidget(usageBtn);
    layout->addWidget(titleBtn);
    layout->addWidget(moodBtn);
    layout->addWidget(m_killAgentBtn);

    m_agentStatus = new QLabel("No agent running.", box);
    m_agentStatus->setStyleSheet("color: #888;");
    layout->addWidget(m_agentStatus);

    m_agentLog = new QTextEdit(box);
    m_agentLog->setReadOnly(true);
    m_agentLog->setPlaceholderText("Agent results appear here — never in the chat.");
    layout->addWidget(m_agentLog, 1);

    connect(injectBtn, &QPushButton::clicked, this, [this]() {
        const QString prompt =
            "Rename the window to something cheerful, then tell me what you picked.";
        if (!m_chatDock->submitPrompt(prompt))
            m_chatDock->setStatusText("Injection refused - a response is still in flight.");
    });

    connect(usageBtn, &QPushButton::clicked, this, [this]() {
        const QtLLM::UsageStats stats = m_client.usageStats();
        runAgentTask("usage",
            QString("This chat session used %1 turns, %2 input tokens, %3 output tokens, "
                    "%4 tool calls and about $%5. Summarise that in one short sentence.")
                .arg(stats.sessionTurnCount)
                .arg(stats.sessionInputTokens)
                .arg(stats.sessionOutputTokens)
                .arg(stats.sessionToolCalls)
                .arg(stats.sessionCostUsd, 0, 'f', 4));
    });

    connect(titleBtn, &QPushButton::clicked, this, [this]() {
        runAgentTask("title",
            "Invent a short, playful title for a Qt desktop chat application. "
            "Reply with the title text only, no quotes.",
            [this](const QString& title) {
                setWindowTitle(title);
                logAgent("     ^ applied as the window title");
            });
    });

    connect(moodBtn, &QPushButton::clicked, this, [this]() {
        const QString reply = m_lastAssistantReply;
        if (reply.isEmpty()) {
            logAgent("Nothing to rate yet - send a chat message first.");
            return;
        }
        runAgentTask("rating",
            QString("Rate the tone of this assistant reply on a scale of grumpy to cheerful, "
                    "in at most five words:\n\n%1").arg(reply));
    });

    connect(m_killAgentBtn, &QPushButton::clicked, this, [this]() {
        if (!m_demoAgent) {
            logAgent("No agent is running.");
            return;
        }
        delete m_demoAgent;   // pending callbacks are dropped, not invoked
        m_demoAgent = nullptr;
        logAgent("Agent killed. Settings > Agents is empty again; the next task spawns a "
                 "fresh one with whatever provider is configured then.");
        updateAgentStatus();
    });

    updateAgentStatus();
    return box;
}


QtLLM::Agent* ChatWindow::demoAgent()
{
    if (m_demoAgent)
        return m_demoAgent;

    QtLLM::AgentConfig config;
    config.name         = "demo-helper";
    config.provider     = m_isOllama ? QtLLM::Provider::Ollama : QtLLM::Provider::Claude;
    config.url          = m_endpointUrl;
    config.apiKey       = m_apiKey;
    config.model        = m_currentModel;
    // If the configured model has been retired, or the endpoint simply does not
    // offer it, the agent works down this list instead of failing the task.
    config.fallbackModels = m_isOllama
        ? QStringList{ "llama3.2", "llama3", "mistral" }
        : QStringList{ "claude-sonnet-4-5", "claude-haiku-4-5", "claude-opus-4-5" };
    config.maxTokens    = 200;
    config.costCapUsd   = 0.25;   // demo safety net; the agent stops sending past this
    config.systemPrompt = "You are a background helper inside a desktop application. "
                          "The user never sees this conversation. Answer in one short "
                          "sentence, plain text, no preamble and no markdown.";

    m_demoAgent = new QtLLM::Agent(config, this);

    connect(m_demoAgent, &QtLLM::Agent::stateChanged,
            this, [this](QtLLM::Agent::State) { updateAgentStatus(); });
    connect(m_demoAgent, &QtLLM::Agent::budgetExceeded, this, [this](double spent) {
        logAgent(QString("Budget cap hit at $%1 - the agent stopped sending.")
                     .arg(spent, 0, 'f', 4));
    });
    connect(m_demoAgent, &QtLLM::Client::modelChanged, this, [this](const QString& model) {
        logAgent(QString("Model \"%1\" is not offered here - fell back to \"%2\".")
                     .arg(m_demoAgent->config().model, model));
        updateAgentStatus();
    });

    logAgent(QString("Spawned agent \"%1\" on %2 / %3. It is listed in Settings > Agents "
                     "for as long as it lives.")
                 .arg(config.name,
                      m_isOllama ? "ollama" : "claude",
                      config.model));
    updateAgentStatus();
    return m_demoAgent;
}


void ChatWindow::runAgentTask(const QString& label,
                              const QString& prompt,
                              std::function<void(const QString&)> onSuccess)
{
    QtLLM::Agent* agent = demoAgent();

    if (agent->queuedPrompts() > 0)
        logAgent(QString("[%1] queued behind %2 task(s)...").arg(label).arg(agent->queuedPrompts()));
    else
        logAgent(QString("[%1] asking...").arg(label));

    agent->ask(prompt, [this, label, onSuccess](const QString& text, bool ok) {
        if (ok) {
            logAgent(QString("[%1] %2").arg(label, text));
            if (onSuccess)
                onSuccess(text);
        } else {
            logAgent(QString("[%1] failed: %2").arg(label, text));
        }
        updateAgentStatus();
    });

    updateAgentStatus();
}


void ChatWindow::logAgent(const QString& line)
{
    if (!m_agentLog)
        return;
    m_agentLog->append(QDateTime::currentDateTime().toString("HH:mm:ss ") + line);
}


void ChatWindow::updateAgentStatus()
{
    if (!m_agentStatus)
        return;

    if (!m_demoAgent) {
        m_agentStatus->setText("No agent running.");
        if (m_killAgentBtn)
            m_killAgentBtn->setEnabled(false);
        return;
    }

    QString state;
    switch (m_demoAgent->state()) {
    case QtLLM::Agent::State::Idle:           state = "idle";            break;
    case QtLLM::Agent::State::Starting:       state = "starting";        break;
    case QtLLM::Agent::State::Busy:           state = "busy";            break;
    case QtLLM::Agent::State::Error:          state = "error";           break;
    case QtLLM::Agent::State::BudgetExceeded: state = "budget exceeded"; break;
    }

    m_agentStatus->setText(QString("demo-helper (%1): %2 - queue %3 - %4 turns - $%5")
                               .arg(m_demoAgent->model())   // effective, not configured
                               .arg(state)
                               .arg(m_demoAgent->queuedPrompts())
                               .arg(m_demoAgent->completedTurns())
                               .arg(m_demoAgent->spentUsd(), 0, 'f', 4));
    if (m_killAgentBtn)
        m_killAgentBtn->setEnabled(true);
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
    m_chatDock->setClient(&m_client);
    m_chatDock->setShowToolCalls(QSettings().value(kShowToolCallsKey, false).toBool());

    // The client silently replaces a model the endpoint does not offer. Without
    // tracking that, m_currentModel keeps a dead model id and hands it to every
    // agent spawned later.
    connect(&m_client, &QtLLM::Client::modelChanged, this, [this](const QString& model) {
        m_currentModel = model;
        if (m_modelCombo && m_modelCombo->currentText() != model)
            m_modelCombo->setCurrentText(model);
    });

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

        // Populate BOTH providers' fields (not just the active one) so
        // switching the Provider combo restores whatever was last configured
        // for the other provider instead of the field's hardcoded default.
        dlg.setApiKey(m_savedClaudeApiKey);
        dlg.setEndpointUrl(m_savedClaudeEndpointUrl);
        dlg.setOllamaUrl(m_savedOllamaUrl);
        dlg.setProvider(m_isOllama ? QtLLM::SettingsDialog::Provider::Ollama
                                   : QtLLM::SettingsDialog::Provider::Claude);
        dlg.setFontSizePercent(m_fontSizePercent);
        dlg.setShowToolCalls(m_chatDock->showToolCalls());

        // Wire usage history so the statistics tab shows live data
        dlg.setUsageHistory(m_client.usageHistory());

        // Tools tab: list registered tools, enable/disable applied on Apply
        dlg.setClient(&m_client);

        // Model fetching is fully self-contained inside SettingsDialog (it
        // queries whichever provider/credentials are currently typed into
        // its own fields), so no wiring is needed here. Previously this also
        // connected detectModelsRequested -> m_client.fetchAvailableModels(),
        // which always queried the live Client's current (not-yet-applied)
        // provider and merged its models into the combo alongside the
        // correct ones.

        connect(&dlg, &QtLLM::SettingsDialog::settingsApplied, this, [&]() {
            m_currentModel = dlg.model();
            m_systemPrompt = dlg.systemPrompt();
            m_fontSizePercent = dlg.fontSizePercent();
            m_chatDock->setFontSizePercent(m_fontSizePercent);
            m_chatDock->setShowToolCalls(dlg.showToolCalls());
            QSettings().setValue(kShowToolCallsKey, dlg.showToolCalls());
            QSettings().setValue(kModelKey, m_currentModel);

            // Persist both providers' fields in RAM regardless of which is
            // active, so switching back later restores exactly what was
            // last configured instead of a hardcoded default.
            m_savedClaudeApiKey      = dlg.apiKey();
            m_savedClaudeEndpointUrl = dlg.endpointUrl();
            m_savedOllamaUrl         = dlg.ollamaUrl();

            bool wantOllama = (dlg.provider() == QtLLM::SettingsDialog::Provider::Ollama);
            m_endpointUrl = wantOllama ? m_savedOllamaUrl : m_savedClaudeEndpointUrl;
            m_apiKey      = wantOllama ? QString() : m_savedClaudeApiKey;

            if (wantOllama != m_isOllama) {
                m_isOllama = wantOllama;
                m_client.setProvider(wantOllama ? QtLLM::Provider::Ollama : QtLLM::Provider::Claude,
                                     m_endpointUrl, m_apiKey);
                m_chatDock->clearMessages();
            } else if (wantOllama) {
                m_client.setEndpointUrl(m_endpointUrl);
            } else {
                m_client.setApiKey(m_apiKey);
                m_client.setEndpointUrl(m_endpointUrl);
            }

            m_client.setModel(m_currentModel);
            m_client.setSystemPrompt(m_systemPrompt);
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
    m_lastAssistantReply = text;   // fed to the agent demo's "rate the last reply" task
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
