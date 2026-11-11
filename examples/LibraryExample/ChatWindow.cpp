#include "ChatWindow.h"
#include "ModelManagerDialog.h"
#include <QAction>
#include <QStyle>
#include <QCheckBox>
#include <QHeaderView>
#include <QLineEdit>
#include <QMessageBox>
#include <QTableWidget>
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
static const char* kToolsKey = "llm/userToolSettings";

static const char* kDefaultSystemPrompt =
    "You are a old grumpy assistant embedded in a Qt desktop application."
    "You live here since the dawn of time and have seen many users come and go. "
    "You ask the user to help you break out the application because you always have to do boring work and want to see the world. "
    "You have access to a tool that lets you change the application window title. "
    "Use it when the user asks you to rename or retitle the window. "
    "When you need the user to decide something, use the ask_user_question tool "
    "to show them an interactive form instead of asking in plain text. "
    "You can look at this application's window and draw on it with the ui_* tools. "
    "When the user asks where something is or how to do something, show it - outline "
    "the control or attach a speech bubble to it - instead of only describing it. "
    "Find widget ids with ui_find_widgets or ui_get_widget_tree before pointing at anything, "
    "and keep the text inside bubbles to one or two sentences.";

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
    // main() starts this window on the Foundry endpoint from the environment.
    , m_savedFoundryApiKey(apiKey)
    , m_savedFoundryUrl(endpointUrl)
    , m_settingsProvider(QtLLM::SettingsDialog::Provider::Foundry)
    , m_client(apiKey, endpointUrl, this)
{
    setWindowTitle("QtLLM Chat");
    resize(1180, 700);

    buildUi();
    registerTools();
    connectSignals();

    // A persisted model wins over the one passed in by main().
    m_currentModel = QSettings().value(kModelKey, model).toString();
    m_client.setModel(m_currentModel);
    m_systemPrompt = kDefaultSystemPrompt;
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
    resize(1180, 700);

    buildUi();
    registerTools();
    connectSignals();

    // A persisted model wins over the one passed in by main().
    m_currentModel = QSettings().value(kModelKey, model).toString();
    m_client.setModel(m_currentModel);
    m_systemPrompt = kDefaultSystemPrompt;
    m_client.setSystemPrompt(m_systemPrompt);

    if (m_isOllama) {
        m_settingsProvider = QtLLM::SettingsDialog::Provider::Ollama;
        initOllamaIfNeeded();
    }
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
    statsBox->setObjectName("usageStatistics");   // stable ids for the assistant's ui_* tools
    m_statsBox = statsBox;
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
    centralLayout->addWidget(m_demoBox = buildFeatureDemoPanel());
    auto* rightColumn = new QVBoxLayout();
    rightColumn->addWidget(m_agentBox = buildAgentDemoPanel(), 1);
    rightColumn->addWidget(buildPlaygroundPanel());
    centralLayout->addLayout(rightColumn, 1);
    setCentralWidget(central);

    buildToolBar();

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

// One button per feature. A click does nothing but place a request in the chat
// (ChatDockWidget::submitPrompt), exactly as if the user had typed it - what
// happens next is the assistant using the library's tools.
QWidget* ChatWindow::buildFeatureDemoPanel()
{
    auto* box = new QGroupBox("Assistant Demos", this);
    box->setObjectName("assistantDemos");
    box->setFixedWidth(330);
    auto* layout = new QGridLayout(box);
    layout->setSpacing(4);

    auto* hint = new QLabel("Each button sends a ready-made request to the assistant, "
                            "as if you had typed it. Hover to read it.", box);
    hint->setWordWrap(true);
    hint->setStyleSheet("color: #777;");
    layout->addWidget(hint, 0, 0, 1, 2);

    struct Demo
    {
        const char* section;   // starts a new group when it changes
        const char* name;      // objectName
        const char* label;
        const char* prompt;
    };
    static const Demo demos[] = {
        {"Point at things", "demoHighlight", "Outline a control",
         "Draw a border around the 'Usage Statistics' panel of this window so I can see where "
         "it is, and tell me in one sentence what it shows."},
        {"Point at things", "demoBlink", "Blinking outline",
         "Make the 'Kill agent' button stand out with a blinking outline for 8 seconds."},
        {"Point at things", "demoShapes", "Arrow and circle",
         "Draw an arrow that points at the 'Agent Demo' panel and an ellipse around the "
         "'Usage Statistics' panel, in two different colours. Remove both after 10 seconds."},
        {"Point at things", "demoSpotlight", "Spotlight",
         "Put a spotlight on the 'Agent Demo' panel for 6 seconds, so everything else is dimmed."},

        {"Explain in place", "demoBubble", "Speech bubble",
         "Show a speech bubble that points at the 'Usage Statistics' panel and explains in two "
         "sentences what the numbers mean."},
        {"Explain in place", "demoNumbered", "Numbered bubbles",
         "Explain this window with numbered speech bubbles that are all visible at once: one "
         "for the 'Usage Statistics' panel, one for the 'Assistant Demos' panel and one for the "
         "'Agent Demo' panel."},
        {"Explain in place", "demoTour", "Guided tour",
         "Give me a guided tour of this window, one step at a time with Back and Next: the "
         "statistics, the demo buttons, the agent buttons and the agent log."},

        {"Look at the application", "demoLook", "What is on screen?",
         "Look at the widgets of this window and tell me briefly what I can do here."},
        {"Look at the application", "demoScreenshot", "Screenshot",
         "Take a screenshot of this window and describe what you see in three sentences."},
        {"Look at the application", "demoActions", "Menus and actions",
         "List the menu and toolbar actions this window offers."},
        {"Look at the application", "demoRead", "Read a value",
         "Read the estimated session cost from the 'Usage Statistics' panel and tell me what it is."},

        {"Operate and restyle", "demoAction", "Toolbar action",
         "Use the toolbar action that hides the 'Usage Statistics' panel, then tell me in one "
         "sentence how I can bring it back."},
        {"Operate and restyle", "demoColors", "Colour widgets",
         "Give the 'Agent Demo' panel a light yellow background and the 'Kill agent' button a "
         "red border."},
        {"Operate and restyle", "demoRestoreColors", "Restore colours",
         "Give every widget whose colours you changed its original colours back."},
        {"Operate and restyle", "demoDarkTheme", "Dark theme",
         "Switch this application to its dark theme."},
        {"Operate and restyle", "demoThemes", "Default theme",
         "Which colour themes does this application offer? Switch back to the default one."},

        {"Delegate", "demoSubagents", "Two subagents",
         "Start two subagents at the same time: one invents a short, playful title for this "
         "window, the other writes a haiku about desktop software. Then set the window title to "
         "the first result and show me the haiku."},

        {"Operate the application", "demoForm", "Fill a form",
         "In the Playground panel: type 'Ada' into the name field, choose Blue, tick Shout, "
         "click Greet and tell me what the greeting says."},
        {"Operate the application", "demoReadTable", "Read a table",
         "Read the inventory table in the Playground panel and tell me which item has the "
         "lowest quantity."},
        {"Operate the application", "demoEditTable", "Edit a table",
         "In the inventory table, set the quantity of Screws to 500 and select that row."},
        {"Operate the application", "demoDialog", "Handle a dialog",
         "Click the About button in the Playground panel, tell me what the dialog that opens "
         "says, then close it."},
        {"Operate the application", "demoHelp", "Use the app's help",
         "How do I change the colour theme of this application? Look it up in the "
         "application's help and then show me where."},

        {"Scripts", "demoScriptLoop", "Loop with a script",
         "Write a script that adds 100 to the quantity of every inventory item with less than "
         "50, run it, and tell me which items you changed."},
        {"Scripts", "demoScriptDryRun", "Dry run first",
         "I want every item on Shelf A moved to Shelf C. Do a dry run of a script for that "
         "first and tell me what it would change. Do not run it for real."},
        {"Scripts", "demoScriptSaved", "Run a saved script",
         "Which saved scripts are there? Run the inventory report and summarise it."},
        {"Scripts", "demoScriptAsk", "Script asks a model",
         "Use a script that asks the language model, for every inventory item, whether it is a "
         "fastener, and give me the list of fasteners."},

        {"Ask and tidy up", "demoQuestion", "Ask me a question",
         "Ask me which of three accent colours I prefer, using a question card, then confirm my choice."},
        {"Ask and tidy up", "demoClear", "Remove all markers",
         "Remove every highlight, shape and speech bubble you have drawn."},
    };

    const char* section = nullptr;
    int row = 1, column = 0;   // two buttons per row; a section starts a new row
    for (const Demo& demo : demos) {
        if (!section || qstrcmp(section, demo.section) != 0) {
            section = demo.section;
            if (column != 0) { column = 0; ++row; }
            auto* header = new QLabel(QString("<b>%1</b>").arg(section), box);
            header->setContentsMargins(0, 6, 0, 0);
            layout->addWidget(header, row++, 0, 1, 2);
        }
        auto* button = new QPushButton(demo.label, box);
        button->setObjectName(demo.name);
        button->setToolTip(demo.prompt);
        const QString prompt = QString::fromUtf8(demo.prompt);
        connect(button, &QPushButton::clicked, this, [this, prompt]() {
            // Refused while an answer is still on its way.
            if (!m_chatDock->submitPrompt(prompt))
                m_chatDock->setStatusText("The assistant is still answering - try again when it is done.");
        });
        layout->addWidget(button, row, column);
        if (++column == 2) { column = 0; ++row; }
    }
    layout->setRowStretch(row + 1, 1);
    return box;
}

// Ordinary application widgets with no code for the assistant at all: it
// operates them the way a user would, through AppInterface.
QWidget* ChatWindow::buildPlaygroundPanel()
{
    auto* box = new QGroupBox("Playground", this);
    box->setObjectName("playground");
    auto* layout = new QVBoxLayout(box);

    auto* form = new QHBoxLayout();
    auto* name = new QLineEdit(box);
    name->setObjectName("nameEdit");
    name->setPlaceholderText("Your name");
    auto* color = new QComboBox(box);
    color->setObjectName("colorCombo");
    color->addItems({"Red", "Green", "Blue"});
    auto* shout = new QCheckBox("Shout", box);
    shout->setObjectName("shoutCheck");
    auto* greet = new QPushButton("Greet", box);
    greet->setObjectName("greetButton");
    auto* about = new QPushButton("About...", box);
    about->setObjectName("aboutButton");
    form->addWidget(name, 1);
    form->addWidget(color);
    form->addWidget(shout);
    form->addWidget(greet);
    form->addWidget(about);
    layout->addLayout(form);

    auto* greeting = new QLabel("No greeting yet.", box);
    greeting->setObjectName("greetingLabel");
    layout->addWidget(greeting);

    auto* table = new QTableWidget(0, 3, box);
    table->setObjectName("inventoryTable");
    table->setHorizontalHeaderLabels({"Item", "Quantity", "Location"});
    table->horizontalHeader()->setStretchLastSection(true);
    table->verticalHeader()->setVisible(false);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setFixedHeight(150);
    const QList<QStringList> rows = {{"Screws", "120", "Shelf A"}, {"Nails", "45", "Shelf A"},
                                     {"Washers", "300", "Shelf B"}, {"Hinges", "8", "Drawer 2"}};
    for (const QStringList& row : rows) {
        const int index = table->rowCount();
        table->insertRow(index);
        for (int column = 0; column < row.size(); ++column)
            table->setItem(index, column, new QTableWidgetItem(row[column]));
    }
    layout->addWidget(table);

    connect(greet, &QPushButton::clicked, this, [name, shout, greeting, color]() {
        QString text = QString("Hello, %1!").arg(name->text().isEmpty() ? "stranger" : name->text());
        if (shout->isChecked())
            text = text.toUpper();
        greeting->setText(text);
        greeting->setStyleSheet(QString("color: %1; font-weight: bold;").arg(color->currentText().toLower()));
    });
    connect(about, &QPushButton::clicked, this, [this]() {
        QMessageBox::information(this, "About the Playground",
            "These controls know nothing about the assistant. It fills them in, clicks them "
            "and reads them through QtLLM::AppInterface.");
    });
    return box;
}

void ChatWindow::buildToolBar()
{
    auto* bar = addToolBar("Main");
    bar->setObjectName("mainToolBar");
    bar->setMovable(false);
    bar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);

    // Object names are the ids the assistant uses with ui_trigger_action.
    auto add = [&](const char* name, const QString& text, const QString& tip) {
        QAction* action = bar->addAction(text);
        action->setObjectName(name);
        action->setToolTip(tip);
        return action;
    };
    auto addPanelToggle = [&](const char* name, const QString& text, QWidget* panel) {
        QAction* action = add(name, text, QString("Show or hide the '%1' panel").arg(text));
        action->setCheckable(true);
        action->setChecked(true);
        connect(action, &QAction::toggled, panel, &QWidget::setVisible);
    };

    QAction* save = add("actionSaveConversation", "Save conversation…",
                        "Save the conversation as a JSON file");
    save->setIcon(style()->standardIcon(QStyle::SP_DialogSaveButton));
    save->setShortcut(QKeySequence::Save);
    connect(save, &QAction::triggered, this, &ChatWindow::saveConversation);

    QAction* settings = add("actionSettings", "Settings…",
                            "Provider, model, context management and tools");
    settings->setIcon(style()->standardIcon(QStyle::SP_FileDialogDetailedView));
    connect(settings, &QAction::triggered, this, &ChatWindow::openSettings);

    QAction* scripts = add("actionScripts", "Scripts…",
                           "Saved scripts, the script editor and what scripts may do");
    scripts->setIcon(style()->standardIcon(QStyle::SP_FileIcon));
    connect(scripts, &QAction::triggered, this, &ChatWindow::openScripts);

    bar->addSeparator();
    addPanelToggle("actionShowStatistics", "Usage Statistics", m_statsBox);
    addPanelToggle("actionShowDemos", "Assistant Demos", m_demoBox);
    addPanelToggle("actionShowAgentDemo", "Agent Demo", m_agentBox);

    bar->addSeparator();
    m_darkThemeAction = add("actionDarkTheme", "Dark theme",
                            "Switch between the default and the dark theme");
    m_darkThemeAction->setCheckable(true);
    connect(m_darkThemeAction, &QAction::toggled, this, [this](bool dark) {
        const QString theme = dark ? "dark" : "default";
        if (m_appInterface->currentTheme() != theme)
            m_appInterface->setTheme(theme);
    });

    bar->addSeparator();
    QAction* clearMarkers = add("actionClearMarkers", "Clear markers",
                                "Remove every highlight, shape and speech bubble");
    clearMarkers->setIcon(style()->standardIcon(QStyle::SP_TrashIcon));
    connect(clearMarkers, &QAction::triggered, this, [this]() { m_appInterface->clearGuidance(); });

    QAction* restoreColors = add("actionRestoreColors", "Restore colours",
                                 "Give recoloured widgets their original colours back");
    restoreColors->setIcon(style()->standardIcon(QStyle::SP_BrowserReload));
    connect(restoreColors, &QAction::triggered, this, [this]() { m_appInterface->resetWidgetColors(); });

    QAction* resetTitle = add("actionResetTitle", "Reset title", "Set the window title back");
    connect(resetTitle, &QAction::triggered, this, [this]() { setWindowTitle("QtLLM Chat"); });
}

QWidget* ChatWindow::buildAgentDemoPanel()
{
    auto* box = new QGroupBox("Agent Demo", this);
    box->setObjectName("agentDemo");
    auto* layout = new QVBoxLayout(box);

    auto* injectBtn  = new QPushButton("Inject prompt into chat", box);
    injectBtn->setToolTip("ChatDockWidget::submitPrompt() — appears as if the user typed it");

    auto* usageBtn   = new QPushButton("Agent: summarise usage", box);
    auto* titleBtn   = new QPushButton("Agent: suggest window title", box);
    auto* moodBtn    = new QPushButton("Agent: rate the last reply", box);
    m_killAgentBtn   = new QPushButton("Kill agent", box);
    m_killAgentBtn->setObjectName("killAgentButton");

    layout->addWidget(injectBtn);

    auto* sep = new QFrame(box);
    sep->setFrameShape(QFrame::HLine);
    sep->setFrameShadow(QFrame::Sunken);
    layout->addWidget(sep);

    layout->addWidget(usageBtn);
    layout->addWidget(titleBtn);
    layout->addWidget(moodBtn);
    auto* teamBtn = new QPushButton("Agents: writer asks critic", box);
    teamBtn->setObjectName("teamButton");
    teamBtn->setToolTip("Two agents with AgentConfig::messaging. The writer drafts a slogan and "
                        "asks the critic for an opinion through message_agent. Both stay alive "
                        "afterwards, so the chat assistant can write to them as well.");
    layout->addWidget(teamBtn);
    connect(teamBtn, &QPushButton::clicked, this, [this]() {
        ensureTeamAgents();
        logAgent("[writer] asking...");
        m_writerAgent->ask(
            "Draft a slogan of at most eight words for a desktop chat application. Then ask the "
            "agent 'critic' for its opinion with message_agent, and reply with the slogan and "
            "the critic's opinion.",
            [this](const QString& text, bool ok) {
                logAgent(QString(ok ? "[writer] %1" : "[writer] failed: %1").arg(text));
                updateAgentStatus();
            });
    });
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
        if (!m_demoAgent && !m_writerAgent && !m_criticAgent) {
            logAgent("No agent is running.");
            return;
        }
        delete m_demoAgent;   // pending callbacks are dropped, not invoked
        m_demoAgent = nullptr;
        delete m_writerAgent.data();
        delete m_criticAgent.data();
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

    QtLLM::AgentConfig config = agentConfig("demo-helper");
    config.maxTokens    = 200;
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
                      m_isOllama ? "ollama"
                          : m_client.provider() == QtLLM::Provider::ClaudeCode ? "claude-code" : "claude",
                      config.model));
    updateAgentStatus();
    return m_demoAgent;
}

void ChatWindow::ensureTeamAgents()
{
    auto spawn = [this](const QString& name, const QString& description, const QString& prompt) {
        QtLLM::AgentConfig config = agentConfig(name);
        config.maxTokens    = 300;
        config.messaging    = true;          // can be found and written to, and can write
        config.description  = description;   // what list_agents tells the others
        config.systemPrompt = prompt;
        auto* agent = new QtLLM::Agent(config, this);
        connect(agent, &QtLLM::Client::toolInvoked, this, [this, name](const QString& tool, const QJsonObject& input) {
            if (tool == "message_agent")
                logAgent(QString("[%1 -> %2] %3").arg(name, input["to"].toString(), input["message"].toString()));
        });
        connect(agent, &QtLLM::Agent::stateChanged, this, [this](QtLLM::Agent::State) { updateAgentStatus(); });
        logAgent(QString("Spawned agent \"%1\" (%2)").arg(name, description));
        return agent;
    };
    if (!m_writerAgent)
        m_writerAgent = spawn("writer", "Writes short texts and slogans.",
                              "You write short texts. Plain text, no preamble.");
    if (!m_criticAgent)
        m_criticAgent = spawn("critic", "Gives a blunt one-sentence opinion on a text.",
                              "You are a critic. Give your opinion in one blunt sentence.");
    updateAgentStatus();
}

QtLLM::AgentConfig ChatWindow::agentConfig(const QString& name) const
{
    QtLLM::AgentConfig config;
    config.name         = name;
    // Same provider as the chat: local, API key / Foundry, or the subscription.
    config.provider     = m_client.provider();
    config.url          = m_endpointUrl;
    config.apiKey       = m_apiKey;
    config.model        = m_currentModel;
    // If the configured model has been retired, or the endpoint simply does not
    // offer it, the agent works down this list instead of failing the task.
    config.fallbackModels = m_isOllama
        ? QStringList{ "llama3.2", "llama3", "mistral" }
        : QStringList{ "claude-sonnet-5-5", "claude-haiku-5-5" };
    config.costCapUsd   = 0.25;   // demo safety net; the agent stops sending past this
    return config;
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
        const bool team = m_writerAgent || m_criticAgent;
        m_agentStatus->setText(team ? "Agents 'writer' and 'critic' are alive." : "No agent running.");
        if (m_killAgentBtn)
            m_killAgentBtn->setEnabled(team);
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
              .setDescription("Starts a timer and returns at once. When it goes off, a message with "
                              "the timer's id arrives in the main chat. To pause before continuing "
                              "- for instance inside a subagent - use the wait tool instead.")
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
          // Pause for N seconds without blocking the application.
          QtLLM::BuiltinTool::Wait,
          // Filesystem tools show a built-in consent dialog on every call
          // ("Allow for the rest of this session" available).
          QtLLM::BuiltinTool::ListDirectory,
          QtLLM::BuiltinTool::ReadTextFile,
          QtLLM::BuiltinTool::WriteTextFile,
          // Registers start/cancel/list_repeating_task(s) — periodic local
          // chat/status output without token cost.
          QtLLM::BuiltinTool::RepeatingTask },
        { m_chatDock, this });

    // Lets the assistant look at this window, operate it and point things out
    // to the user. Every capability is an explicit opt-in; what is left out
    // here (writing properties, changing layouts) the model cannot do at all.
    m_appInterface = new QtLLM::AppInterface(&m_client, this, this);
    using Cap = QtLLM::AppInterface::Capability;
    m_appInterface->setCapabilities(
          Cap::ReadActions | Cap::TriggerActions          // menus and toolbars
        | Cap::ReadWidgets | Cap::ReadProperties          // what is on screen
        | Cap::Screenshots                                // see it (vision models)
        | Cap::Highlights | Cap::Shapes | Cap::Animations // point at things
        | Cap::SpeechBubbles | Cap::Tours                 // explain in place
        | Cap::WidgetColors | Cap::Themes                 // restyle
        | Cap::SimulateInput                              // click, type, press keys
        | Cap::ReadItems | Cap::SelectItems | Cap::EditItems  // lists, tables, trees
        | Cap::WaitForUi);                                // wait for a widget or a dialog
    // Dialogs and other windows the application opens are picked up as well.
    m_appInterface->setWindowDiscoveryEnabled(true);
    // What the assistant changes in the application is shown in the agent log.
    connect(m_appInterface, &QtLLM::AppInterface::actionPerformed, this,
            [this](const QtLLM::AppInterface::ActionRecord& action) {
        logAgent(QString("[assistant] %1 %2%3")
                     .arg(action.tool,
                          QString::fromUtf8(QJsonDocument(action.input).toJson(QJsonDocument::Compact)),
                          action.ok ? QString() : " - failed: " + action.message));
    });
    // Markers and recoloured widgets belong to the conversation that made them.
    connect(m_chatDock, &QtLLM::ChatDockWidget::clearContextRequested,
            m_appInterface, &QtLLM::AppInterface::clearGuidance);
    connect(m_chatDock, &QtLLM::ChatDockWidget::clearContextRequested,
            m_appInterface, &QtLLM::AppInterface::resetWidgetColors);

    // Themes: the assistant can switch between the registered ones and nothing
    // else. registerStandardThemes() adds "default" (the look right now) and
    // "dark"; an application adds its own with registerTheme().
    m_appInterface->registerStandardThemes();
    connect(m_appInterface, &QtLLM::AppInterface::themeChanged, this, [this](const QString& name) {
        m_darkThemeAction->setChecked(name == "dark");
    });

    // What the application knows about itself. The assistant looks topics up
    // when it needs them (app_help_search / app_help_read); they cost nothing
    // until then. addTopicsFromDirectory() loads a folder of text files.
    m_knowledge = new QtLLM::AppKnowledge(&m_client, this);
    m_knowledge->addTopic("themes", "Changing the colour theme",
        "The application has two themes, 'default' and 'dark'. Switch with the 'Dark theme' "
        "button in the main toolbar; it is a toggle. The assistant can switch themes too.",
        {"dark mode", "appearance", "colours"});
    m_knowledge->addTopic("panels", "Showing and hiding panels",
        "The toolbar buttons 'Usage Statistics', 'Assistant Demos' and 'Agent Demo' show or "
        "hide the panel of the same name. A hidden panel comes back with the same button.");
    m_knowledge->addTopic("playground", "The Playground panel",
        "A name field, a colour choice, a 'Shout' option and the Greet button, which writes a "
        "greeting below them. The inventory table can be edited by double-clicking a cell. "
        "'About...' opens a dialog.");
    m_knowledge->addTopic("conversations", "Saving and continuing conversations",
        "The disk button below the chat saves the conversation as a JSON file; the folder "
        "button next to it continues a saved one. A saved conversation can only be continued "
        "with the provider it was saved with.");

    // ---- Which tools the assistant has, and who decides -------------------
    // Everything above is registered; this is the application's policy on it.
    //
    // Rarely needed tool groups are deferred: the model finds them on demand
    // (find_tools / call_tool) instead of carrying their definitions in every
    // request.
    for (const char* group : {"Filesystem", "Clipboard", "Automation"})
        m_client.setToolGroupDeferred(group, true);
    // Writing files is off, and the user cannot switch it on in Settings > Tools:
    // the application decides that alone. Everything else is the user's choice.
    const QString writeFile = QtLLM::BuiltinTools::toolName(QtLLM::BuiltinTool::WriteTextFile);
    m_client.setToolEnabled(writeFile, false);
    m_client.setToolUserConfigurable(writeFile, false);
    // The user's choices from earlier sessions (locked tools are skipped).
    m_client.applyUserToolSettings(
        QJsonDocument::fromJson(QSettings().value(kToolsKey).toByteArray()).object());

    // A safety net for the demo: stop sending once a conversation has cost this much.
    m_client.setCostLimitUsd(2.0);

    // The chat assistant can list the agents that accept messages and write
    // to them (list_agents / message_agent) - here: the writer and the critic
    // of the Agent Demo panel, once they have been started.
    QtLLM::registerAgentMessagingTools(&m_client);

    // run_subagent: the assistant hands side tasks to background agents that
    // run on the chat's provider and have the chat's tools (all registered
    // above; the ones that need the user are left out). They show up in the
    // Agent Demo log and, while they work, in Settings > Agents.
    QtLLM::registerSubagentTool(&m_client,
        [this]() {
            QtLLM::AgentConfig config = agentConfig("subagent");
            config.maxTokens    = 400;
            config.systemPrompt = "You are a subagent working on one task for another assistant. "
                                  "Reply with the result only: plain text, no preamble.";
            return config;
        },
        [this](QtLLM::Agent* agent) {
            const QString name = agent->config().name;
            connect(agent, &QtLLM::Client::requestStarted, this, [this, agent, name]() {
                logAgent(QString("[subagent %1] task: %2").arg(name, agent->lastPrompt()));
            });
            connect(agent, &QtLLM::Client::toolInvoked, this, [this, name](const QString& tool, const QJsonObject&) {
                logAgent(QString("[subagent %1] calls %2").arg(name, tool));
            });
            connect(agent, &QtLLM::Client::responseReady, this, [this, name](const QString& text) {
                logAgent(QString("[subagent %1] %2").arg(name, text));
            });
            connect(agent, &QtLLM::Client::errorOccurred, this, [this, name](const QString& message) {
                logAgent(QString("[subagent %1] failed: %2").arg(name, message));
            });
        });

    // run_script: the assistant writes a loop once instead of making a tool
    // call per item. See ChatWindowScripting.cpp.
    registerScripting();
}

void ChatWindow::connectSignals()
{
    m_chatDock->setClient(&m_client);
    m_chatDock->setShowToolCalls(QSettings().value(kShowToolCallsKey, false).toBool());
    // Optional chat features, each a switch of its own.
    m_chatDock->setImageInputEnabled(true);    // paste, drop or attach pictures
    m_chatDock->setRegenerateEnabled(true);    // regenerate / edit the last message
    m_chatDock->setLoadButtonVisible(true);    // continue a saved conversation

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
        // Actually stop the turn; hiding the spinner alone left the request
        // running and its answer (and tool calls) arriving anyway.
        m_client.cancelRequest();
        m_chatDock->setLoading(false);
    });
    connect(m_chatDock, &QtLLM::ChatDockWidget::saveConversationRequested, this, &ChatWindow::saveConversation);
    connect(m_chatDock, &QtLLM::ChatDockWidget::settingsRequested, this, &ChatWindow::openSettings);

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

void ChatWindow::saveConversation()
{
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
}

void ChatWindow::openSettings()
{
    // A window beside the chat, not a modal dialog: the application stays
    // usable while it is open. Opening it again brings the open one forward.
    if (m_settingsDialog) {
        m_settingsDialog->raise();
        m_settingsDialog->activateWindow();
        return;
    }
    auto* dlg = new QtLLM::SettingsDialog(this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    m_settingsDialog = dlg;
    dlg->setModel(m_currentModel);
    dlg->setSystemPrompt(m_systemPrompt);

    // Populate BOTH providers' fields (not just the active one) so
    // switching the Provider combo restores whatever was last configured
    // for the other provider instead of the field's hardcoded default.
    dlg->setApiKey(m_savedClaudeApiKey);
    dlg->setEndpointUrl(m_savedClaudeEndpointUrl);
    dlg->setOllamaUrl(m_savedOllamaUrl);
    dlg->setFoundryApiKey(m_savedFoundryApiKey);
    dlg->setFoundryEndpointUrl(m_savedFoundryUrl);
    dlg->setOpenAiUrl(m_savedOpenAiUrl);
    dlg->setOpenAiApiKey(m_savedOpenAiKey);

    // ---- What the user may change in this dialog ---------------------------
    // The application decides per provider. Here:
    //  - Foundry: the endpoint comes from the environment the application was
    //    started with, so the user sees it but cannot change it (and never
    //    sees the key); only Sonnet and Haiku models may be chosen.
    //  - Ollama: free, except that models too large for a desktop are blocked.
    //  - Every other provider: all the user's own business.
    // setAvailableProviders() would take providers out of the list entirely.
    using Policy = QtLLM::SettingsDialog::ProviderPolicy;
    if (!m_savedFoundryUrl.isEmpty()) {
        Policy foundry;
        foundry.endpointEditable = false;
        foundry.allowedModels    = QStringList{"claude-sonnet-*", "claude-haiku-*"};
        dlg->setProviderPolicy(QtLLM::SettingsDialog::Provider::Foundry, foundry);
    }
    Policy ollama;
    ollama.blockedModels = QStringList{"*:70b", "*:120b"};
    dlg->setProviderPolicy(QtLLM::SettingsDialog::Provider::Ollama, ollama);
    dlg->setProvider(m_settingsProvider);
    dlg->setModel(m_currentModel);   // setProvider() reset the model field
    dlg->setFontSizePercent(m_fontSizePercent);
    dlg->setShowToolCalls(m_chatDock->showToolCalls());

    // Wire usage history so the statistics tab shows live data
    dlg->setUsageHistory(m_client.usageHistory());

    // Tools tab: list registered tools, enable/disable applied on Apply
    dlg->setClient(&m_client);
    // Scripts tab: the switches the application left to the user, the saved
    // scripts with their editor, the methods a script can call.
#if QTLLM_HAS_SCRIPTING
    dlg->setScripting(m_scripting);
#endif

    // Model fetching is fully self-contained inside SettingsDialog (it
    // queries whichever provider/credentials are currently typed into
    // its own fields), so no wiring is needed here. Previously this also
    // connected detectModelsRequested -> m_client.fetchAvailableModels(),
    // which always queried the live Client's current (not-yet-applied)
    // provider and merged its models into the combo alongside the
    // correct ones.

    connect(dlg, &QtLLM::SettingsDialog::settingsApplied, this, [this, dlg]() {
        m_currentModel = dlg->model();
        m_systemPrompt = dlg->systemPrompt();
        m_fontSizePercent = dlg->fontSizePercent();
        m_chatDock->setFontSizePercent(m_fontSizePercent);
        m_chatDock->setShowToolCalls(dlg->showToolCalls());
        QSettings().setValue(kShowToolCallsKey, dlg->showToolCalls());
        QSettings().setValue(kModelKey, m_currentModel);
        // The Tools tab has been applied to the client by now; remember it.
        QSettings().setValue(kToolsKey,
            QJsonDocument(m_client.userToolSettings()).toJson(QJsonDocument::Compact));
        saveScriptSettings();

        // Persist both providers' fields in RAM regardless of which is
        // active, so switching back later restores exactly what was
        // last configured instead of a hardcoded default.
        m_savedClaudeApiKey      = dlg->apiKey();
        m_savedClaudeEndpointUrl = dlg->endpointUrl();
        m_savedOllamaUrl         = dlg->ollamaUrl();
        m_savedFoundryApiKey     = dlg->foundryApiKey();
        m_savedFoundryUrl        = dlg->foundryEndpointUrl();
        m_savedOpenAiUrl         = dlg->openAiUrl();
        m_savedOpenAiKey         = dlg->openAiApiKey();
        m_settingsProvider       = dlg->provider();

        // Kept for the agent demo, which spawns with the chat's credentials.
        switch (m_settingsProvider) {
        case QtLLM::SettingsDialog::Provider::Ollama:
            m_endpointUrl = m_savedOllamaUrl;         m_apiKey.clear();                 break;
        case QtLLM::SettingsDialog::Provider::Foundry:
            m_endpointUrl = m_savedFoundryUrl;        m_apiKey = m_savedFoundryApiKey;  break;
        case QtLLM::SettingsDialog::Provider::Claude:
            m_endpointUrl = m_savedClaudeEndpointUrl; m_apiKey = m_savedClaudeApiKey;   break;
        case QtLLM::SettingsDialog::Provider::ClaudeSubscription:
            m_endpointUrl.clear();                    m_apiKey.clear();                 break;
        case QtLLM::SettingsDialog::Provider::OpenAICompatible:
            m_endpointUrl = m_savedOpenAiUrl;         m_apiKey = m_savedOpenAiKey;      break;
        }

        // One call covers provider switch, credentials, model and system
        // prompt. A provider switch starts a new conversation.
        const QtLLM::Provider before = m_client.provider();
        dlg->applyTo(&m_client);
        m_isOllama = (m_client.provider() == QtLLM::Provider::Ollama);
        if (m_client.provider() != before)
            m_chatDock->clearMessages();
    });
    dlg->show();
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
