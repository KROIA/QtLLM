#include "AgentsWidget.h"
#include "Agent.h"
#include "AgentRegistry.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSplitter>
#include <QTableWidget>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTextBrowser>
#include <QScrollBar>
#include <QDateTime>
#include <QStringList>
#include <QSignalBlocker>

namespace QtLLM {

namespace {

// The column that takes the width the others leave over, in both tables.
constexpr int kTaskColumn = 2;

QString stateText(Agent::State state)
{
    switch (state) {
    case Agent::State::Idle:           return QStringLiteral("idle");
    case Agent::State::Starting:       return QStringLiteral("starting");
    case Agent::State::Busy:           return QStringLiteral("busy");
    case Agent::State::Error:          return QStringLiteral("error");
    case Agent::State::BudgetExceeded: return QStringLiteral("budget exceeded");
    }
    return QString();
}

QString providerText(Provider provider)
{
    if (provider == Provider::ClaudeCode)
        return QStringLiteral("claude-code");
    if (provider == Provider::OpenAI)
        return QStringLiteral("openai");
    return provider == Provider::Ollama ? QStringLiteral("ollama")
                                        : QStringLiteral("claude");
}

QString durationText(qint64 ms)
{
    if (ms < 1000)
        return QStringLiteral("%1 ms").arg(ms);
    const qint64 seconds = ms / 1000;
    if (seconds < 60)
        return QStringLiteral("%1 s").arg(ms / 1000.0, 0, 'f', 1);
    if (seconds < 3600)
        return QStringLiteral("%1m %2s").arg(seconds / 60).arg(seconds % 60);
    return QStringLiteral("%1h %2m").arg(seconds / 3600).arg((seconds % 3600) / 60);
}

QString costText(double usd)
{
    return QStringLiteral("$%1").arg(usd, 0, 'f', 4);
}

QString timeText(qint64 msSinceEpoch)
{
    return QDateTime::fromMSecsSinceEpoch(msSinceEpoch).toString(QStringLiteral("HH:mm:ss"));
}

// One line for a table cell; the view elides it, the tooltip has the rest.
QString cellText(const QString& text)
{
    return text.left(500).simplified();
}

QString block(const QString& text, bool monospace = false)
{
    return QStringLiteral("<div style=\"white-space:pre-wrap;%1\">%2</div>")
        .arg(monospace ? QStringLiteral(" font-family:monospace;") : QString(), text.toHtmlEscaped());
}

QTableWidget* makeTable(const QStringList& headers, const char* objectName, QWidget* parent)
{
    QTableWidget* table = new QTableWidget(0, headers.size(), parent);
    table->setObjectName(QString::fromLatin1(objectName));
    table->setHorizontalHeaderLabels(headers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setWordWrap(false);
    table->verticalHeader()->setVisible(false);
    // Section modes instead of resizeColumnsToContents(): calling that on
    // every refresh pinned the stretched section to its content width, which
    // is why the table used to stop short of the tab's right edge.
    QHeaderView* header = table->horizontalHeader();
    header->setStretchLastSection(false);
    header->setSectionResizeMode(QHeaderView::ResizeToContents);
    header->setSectionResizeMode(kTaskColumn, QHeaderView::Stretch);
    return table;
}

// Writes a row in place, so an unchanged table is left alone and keeps its
// selection and scroll position.
void setRow(QTableWidget* table, int row, const QStringList& cells, int id, const QString& task)
{
    for (int column = 0; column < cells.size(); ++column) {
        QTableWidgetItem* item = table->item(row, column);
        if (!item) {
            item = new QTableWidgetItem;
            table->setItem(row, column, item);
        }
        if (item->text() != cells.at(column))
            item->setText(cells.at(column));
    }
    table->item(row, 0)->setData(Qt::UserRole, id);
    table->item(row, kTaskColumn)->setToolTip(task.left(2000));
}

int rowId(QTableWidget* table, int row)
{
    QTableWidgetItem* item = table->item(row, 0);
    return item ? item->data(Qt::UserRole).toInt() : 0;
}

// Selection follows the agent, not the row index: rows shift when agents come
// and go, and a kept row index would silently show a different agent.
void syncSelection(QTableWidget* table, int id)
{
    int row = -1;
    for (int r = 0; r < table->rowCount() && row < 0; ++r) {
        if (rowId(table, r) == id)
            row = r;
    }
    const QList<QTableWidgetItem*> selected = table->selectedItems();
    const int current = selected.isEmpty() ? -1 : selected.first()->row();
    if (current == row)
        return;
    QSignalBlocker blocker(table);
    if (row < 0)
        table->clearSelection();
    else
        table->selectRow(row);
}

QString recordHtml(const AgentRecord& record, const Agent* live)
{
    QString html;
    html += QStringLiteral("<h3>%1</h3>").arg(record.name.toHtmlEscaped());
    html += QStringLiteral("<p>%1<br/>%2<br/>%3</p>")
        .arg(AgentsWidget::tr("Provider / model: %1 / %2")
                 .arg(providerText(record.provider), record.model.toHtmlEscaped()),
             AgentsWidget::tr("State: %1").arg(stateText(record.state)),
             record.finishedAtMs > 0
                 ? AgentsWidget::tr("Spawned %1, finished %2")
                       .arg(timeText(record.spawnedAtMs), timeText(record.finishedAtMs))
                 : AgentsWidget::tr("Spawned %1, still running").arg(timeText(record.spawnedAtMs)));

    html += QStringLiteral("<p><b>%1</b></p>").arg(AgentsWidget::tr("Called for"));
    html += block(record.tasks.isEmpty() ? AgentsWidget::tr("(no task yet)")
                                         : record.tasks.first().prompt);

    html += QStringLiteral("<p><b>%1</b><br/>%2<br/>%3<br/>%4</p>")
        .arg(AgentsWidget::tr("Totals"),
             AgentsWidget::tr("Worked %1 in %2 task(s), %3 tool call(s)")
                 .arg(durationText(record.workingMs)).arg(record.turns).arg(record.toolCalls),
             AgentsWidget::tr("Tokens in / out: %1 / %2").arg(record.inputTokens).arg(record.outputTokens),
             AgentsWidget::tr("Cost: %1").arg(costText(record.costUsd)));

    if (live) {
        const AgentConfig& config = live->config();
        const QStringList tools = live->enabledToolNames();
        html += QStringLiteral("<p>%1<br/>%2<br/>%3</p>")
            .arg(AgentsWidget::tr("Endpoint: %1").arg(config.url.toHtmlEscaped()),
                 AgentsWidget::tr("Cost cap: %1").arg(config.costCapUsd > 0.0 ? costText(config.costCapUsd)
                                                                            : AgentsWidget::tr("none")),
                 AgentsWidget::tr("Enabled tools: %1").arg(tools.isEmpty()
                     ? AgentsWidget::tr("none") : tools.join(QStringLiteral(", ")).toHtmlEscaped()));
    }

    if (!record.systemPrompt.isEmpty()) {
        html += QStringLiteral("<p><b>%1</b></p>").arg(AgentsWidget::tr("System prompt"));
        html += block(record.systemPrompt);
    }

    html += QStringLiteral("<hr/><h3>%1</h3>").arg(AgentsWidget::tr("Chat log"));
    int kept = 0;
    for (const AgentTask& task : record.tasks)
        kept += task.finished ? 1 : 0;
    if (record.turns > kept) {
        html += QStringLiteral("<p><i>%1</i></p>")
            .arg(AgentsWidget::tr("%1 earlier task(s) are not kept.").arg(record.turns - kept));
    }

    for (int i = 0; i < record.tasks.size(); ++i) {
        const AgentTask& task = record.tasks.at(i);
        const QString timing = task.finished
            ? AgentsWidget::tr("started %1, took %2, tokens in / out %3 / %4, cost %5")
                  .arg(timeText(task.startedAtMs), durationText(task.durationMs))
                  .arg(task.inputTokens).arg(task.outputTokens).arg(costText(task.costUsd))
            : AgentsWidget::tr("started %1, running").arg(timeText(task.startedAtMs));
        html += QStringLiteral("<h4>%1</h4><p>%2</p>")
            .arg(AgentsWidget::tr("Task %1").arg(record.turns - kept + i + 1), timing);

        html += QStringLiteral("<p><b>%1</b></p>").arg(AgentsWidget::tr("Prompt"));
        html += block(task.prompt);

        for (const AgentToolCall& call : task.toolCalls) {
            html += QStringLiteral("<p><b>%1</b> %2</p>")
                .arg(AgentsWidget::tr("Tool call: %1").arg(call.name.toHtmlEscaped()),
                     call.result.isEmpty() ? AgentsWidget::tr("(running)")
                                           : QStringLiteral("(%1)").arg(durationText(call.durationMs)));
            html += QStringLiteral("<p>%1</p>").arg(AgentsWidget::tr("Input:"));
            html += block(call.input, true);
            if (!call.result.isEmpty()) {
                html += QStringLiteral("<p>%1</p>").arg(AgentsWidget::tr("Result:"));
                html += block(call.result, true);
            }
        }

        if (task.finished) {
            html += QStringLiteral("<p><b>%1</b></p>")
                .arg(task.ok ? AgentsWidget::tr("Reply") : AgentsWidget::tr("Error"));
            html += block(task.reply);
        }
    }
    return html;
}

} // namespace


AgentsWidget::AgentsWidget(QWidget* parent)
    : QWidget(parent)
{
    QVBoxLayout* layout = new QVBoxLayout(this);
    QSplitter* splitter = new QSplitter(Qt::Vertical, this);
    splitter->setChildrenCollapsible(false);
    layout->addWidget(splitter);

    // Section layouts have no margins of their own, so the tables run from the
    // left to the right edge of the tab.
    QWidget* runningPage = new QWidget(splitter);
    QVBoxLayout* runningLayout = new QVBoxLayout(runningPage);
    runningLayout->setContentsMargins(0, 0, 0, 0);
    QLabel* runningTitle = new QLabel(tr("Running agents"), runningPage);
    QFont titleFont = runningTitle->font();
    titleFont.setBold(true);
    runningTitle->setFont(titleFont);
    runningLayout->addWidget(runningTitle);
    m_empty = new QLabel(tr("No background agents are running."), runningPage);
    runningLayout->addWidget(m_empty);
    m_running = makeTable({ tr("Name"), tr("Provider / Model"), tr("Task"), tr("State"), tr("Queue"),
                            tr("Turns"), tr("Tokens in / out"), tr("Cost"), tr("Uptime") },
                          "qtllm_agentsRunningTable", runningPage);
    runningLayout->addWidget(m_running);
    splitter->addWidget(runningPage);

    m_history = new QWidget(splitter);
    QVBoxLayout* historyLayout = new QVBoxLayout(m_history);
    historyLayout->setContentsMargins(0, 0, 0, 0);
    QHBoxLayout* historyHeader = new QHBoxLayout();
    QLabel* historyTitle = new QLabel(tr("Finished agents"), m_history);
    historyTitle->setFont(titleFont);
    historyHeader->addWidget(historyTitle);
    historyHeader->addStretch();
    QPushButton* clearButton = new QPushButton(tr("Clear history"), m_history);
    clearButton->setObjectName(QStringLiteral("qtllm_agentsClearHistory"));
    historyHeader->addWidget(clearButton);
    historyLayout->addLayout(historyHeader);
    m_finished = makeTable({ tr("Name"), tr("Model"), tr("Task"), tr("Turns"), tr("Tool calls"),
                             tr("Working time"), tr("Cost"), tr("Finished at") },
                           "qtllm_agentsFinishedTable", m_history);
    historyLayout->addWidget(m_finished);
    splitter->addWidget(m_history);

    m_details = new QTextBrowser(splitter);
    m_details->setObjectName(QStringLiteral("qtllm_agentsDetails"));
    m_details->setOpenLinks(false);
    splitter->addWidget(m_details);

    connect(m_running, &QTableWidget::itemSelectionChanged, this, [this]() { selectFrom(m_running); });
    connect(m_finished, &QTableWidget::itemSelectionChanged, this, [this]() { selectFrom(m_finished); });
    connect(clearButton, &QPushButton::clicked, this, [this]() {
        AgentRegistry::instance().clearFinished();
        refresh();
    });

    AgentRegistry& registry = AgentRegistry::instance();
    connect(&registry, &AgentRegistry::agentSpawned,   this, &AgentsWidget::refresh);
    connect(&registry, &AgentRegistry::agentDestroyed, this, &AgentsWidget::refresh);

    // ponytail: refills both tables once a second (cells only change when the
    // text does). Fine for the handful of live agents and 200 finished ones;
    // switch to per-row updates driven by Agent signals if that stops being true.
    m_tick.setInterval(1000);
    connect(&m_tick, &QTimer::timeout, this, &AgentsWidget::refresh);
    m_tick.start();

    refresh();
}

AgentsWidget::~AgentsWidget() = default;


void AgentsWidget::setHistoryVisible(bool visible)
{
    m_history->setVisible(visible);
    refresh();
}


bool AgentsWidget::isHistoryVisible() const
{
    return !m_history->isHidden();
}


void AgentsWidget::refresh()
{
    fillRunning();
    if (isHistoryVisible())
        fillFinished();
    updateDetails();
}


void AgentsWidget::fillRunning()
{
    const QList<Agent*> agents = AgentRegistry::instance().agents();
    m_empty->setVisible(agents.isEmpty());

    const int scroll = m_running->verticalScrollBar()->value();
    {
        QSignalBlocker blocker(m_running);
        m_running->setRowCount(agents.size());
    }
    for (int row = 0; row < agents.size(); ++row) {
        const Agent* agent = agents.at(row);
        const UsageStats stats = agent->usageStats();
        setRow(m_running, row, {
            agent->config().name,
            // The effective model, not the configured one — they differ once a
            // fallback has kicked in, and the effective one is what matters.
            providerText(agent->config().provider) + QStringLiteral(" / ") + agent->model(),
            cellText(agent->lastPrompt()),
            stateText(agent->state()),
            QString::number(agent->queuedPrompts()),
            QString::number(stats.sessionTurnCount),
            QStringLiteral("%1 / %2").arg(stats.sessionInputTokens).arg(stats.sessionOutputTokens),
            costText(agent->spentUsd()),
            durationText(QDateTime::currentMSecsSinceEpoch() - agent->spawnedAtMs())
        }, agent->record().id, agent->lastPrompt());
    }
    syncSelection(m_running, m_selectedId);
    m_running->verticalScrollBar()->setValue(scroll);
}


void AgentsWidget::fillFinished()
{
    const QList<AgentRecord> records = AgentRegistry::instance().finishedAgents();

    const int scroll = m_finished->verticalScrollBar()->value();
    {
        QSignalBlocker blocker(m_finished);
        m_finished->setRowCount(records.size());
    }
    for (int row = 0; row < records.size(); ++row) {
        const AgentRecord& record = records.at(row);
        const QString task = record.tasks.isEmpty() ? QString() : record.tasks.first().prompt;
        setRow(m_finished, row, {
            record.name,
            record.model,
            cellText(task),
            QString::number(record.turns),
            QString::number(record.toolCalls),
            durationText(record.workingMs),
            costText(record.costUsd),
            timeText(record.finishedAtMs)
        }, record.id, task);
    }
    syncSelection(m_finished, m_selectedId);
    m_finished->verticalScrollBar()->setValue(scroll);
}


void AgentsWidget::selectFrom(QTableWidget* table)
{
    const QList<QTableWidgetItem*> selected = table->selectedItems();
    if (selected.isEmpty())
        return;
    m_selectedId = rowId(table, selected.first()->row());

    // One selection across both tables: the details pane shows one agent.
    QTableWidget* other = table == m_running ? m_finished : m_running;
    {
        QSignalBlocker blocker(other);
        other->clearSelection();
    }
    updateDetails();
}


void AgentsWidget::updateDetails()
{
    // A live agent first: when the selected agent finishes, its row moves to
    // the finished table and the details carry on from its final record.
    QString html;
    for (const Agent* agent : AgentRegistry::instance().agents()) {
        if (m_selectedId != 0 && agent->record().id == m_selectedId)
            html = recordHtml(agent->record(), agent);
    }
    if (html.isEmpty() && m_selectedId != 0 && isHistoryVisible()) {
        for (const AgentRecord& record : AgentRegistry::instance().finishedAgents()) {
            if (record.id == m_selectedId) {
                html = recordHtml(record, nullptr);
                break;
            }
        }
    }
    if (html.isEmpty())
        m_selectedId = 0;

    // The timer refreshes every second; resetting identical HTML would throw
    // away the user's scroll position and text selection each time.
    if (html == m_shownHtml)
        return;
    const bool sameAgent = m_shownId == m_selectedId;
    const int scroll = m_details->verticalScrollBar()->value();
    if (html.isEmpty())
        m_details->clear();
    else
        m_details->setHtml(html);
    if (sameAgent)
        m_details->verticalScrollBar()->setValue(scroll);
    m_shownHtml = html;
    m_shownId   = m_selectedId;
}

} // namespace QtLLM
