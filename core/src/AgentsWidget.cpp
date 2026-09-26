#include "AgentsWidget.h"
#include "Agent.h"
#include "AgentRegistry.h"
#include <QVBoxLayout>
#include <QTableWidget>
#include <QHeaderView>
#include <QLabel>
#include <QTextEdit>
#include <QDateTime>
#include <QStringList>
#include <QVariant>

namespace QtLLM {

namespace {

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
    return provider == Provider::Ollama ? QStringLiteral("ollama")
                                        : QStringLiteral("claude");
}

QString uptimeText(qint64 spawnedAtMs)
{
    qint64 seconds = (QDateTime::currentMSecsSinceEpoch() - spawnedAtMs) / 1000;
    if (seconds < 60)
        return QStringLiteral("%1s").arg(seconds);
    if (seconds < 3600)
        return QStringLiteral("%1m %2s").arg(seconds / 60).arg(seconds % 60);
    return QStringLiteral("%1h %2m").arg(seconds / 3600).arg((seconds % 3600) / 60);
}

} // namespace


AgentsWidget::AgentsWidget(QWidget* parent)
    : QWidget(parent)
{
    QVBoxLayout* layout = new QVBoxLayout(this);

    m_empty = new QLabel(tr("No background agents are running."), this);
    m_empty->setAlignment(Qt::AlignCenter);
    layout->addWidget(m_empty);

    const QStringList headers = {
        tr("Name"), tr("Provider / Model"), tr("State"), tr("Queue"),
        tr("Turns"), tr("Tokens in / out"), tr("Cost"), tr("Uptime")
    };
    m_table = new QTableWidget(0, headers.size(), this);
    m_table->setHorizontalHeaderLabels(headers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->verticalHeader()->setVisible(false);
    m_table->horizontalHeader()->setStretchLastSection(true);
    layout->addWidget(m_table, 2);

    m_details = new QTextEdit(this);
    m_details->setReadOnly(true);
    layout->addWidget(m_details, 1);

    connect(m_table, &QTableWidget::itemSelectionChanged,
            this, &AgentsWidget::onSelectionChanged);

    AgentRegistry& registry = AgentRegistry::instance();
    connect(&registry, &AgentRegistry::agentSpawned,   this, &AgentsWidget::refresh);
    connect(&registry, &AgentRegistry::agentDestroyed, this, &AgentsWidget::refresh);

    // ponytail: full table rebuild once a second. Fine for the handful of
    // agents an app realistically runs; switch to per-row updates driven by
    // Agent::stateChanged if that ever stops being true.
    m_tick.setInterval(1000);
    connect(&m_tick, &QTimer::timeout, this, &AgentsWidget::refresh);
    m_tick.start();

    refresh();
}

AgentsWidget::~AgentsWidget() = default;


void AgentsWidget::refresh()
{
    const QList<Agent*> agents = AgentRegistry::instance().agents();

    m_empty->setVisible(agents.isEmpty());
    m_table->setVisible(!agents.isEmpty());
    m_details->setVisible(!agents.isEmpty());

    // Selection survives the rebuild, so the detail pane does not jump around
    // while the user is reading it.
    const Agent* previous = selectedAgent();

    m_table->setRowCount(agents.size());
    for (int row = 0; row < agents.size(); ++row) {
        Agent* agent = agents.at(row);
        const AgentConfig& config = agent->config();
        const UsageStats stats = agent->usageStats();

        const QStringList cells = {
            config.name,
            // The effective model, not the configured one — they differ once a
            // fallback has kicked in, and the effective one is what matters.
            providerText(config.provider) + QStringLiteral(" / ") + agent->model(),
            stateText(agent->state()),
            QString::number(agent->queuedPrompts()),
            QString::number(stats.sessionTurnCount),
            QStringLiteral("%1 / %2").arg(stats.sessionInputTokens)
                                     .arg(stats.sessionOutputTokens),
            QStringLiteral("$%1").arg(agent->spentUsd(), 0, 'f', 4),
            uptimeText(agent->spawnedAtMs())
        };

        for (int column = 0; column < cells.size(); ++column) {
            QTableWidgetItem* item = new QTableWidgetItem(cells.at(column));
            if (column == 0)
                item->setData(Qt::UserRole, QVariant::fromValue(static_cast<void*>(agent)));
            m_table->setItem(row, column, item);
        }

        if (agent == previous)
            m_table->selectRow(row);
    }

    m_table->resizeColumnsToContents();
    updateDetails();
}


Agent* AgentsWidget::selectedAgent() const
{
    const int row = m_table->currentRow();
    if (row < 0)
        return nullptr;
    QTableWidgetItem* item = m_table->item(row, 0);
    if (!item)
        return nullptr;
    Agent* agent = static_cast<Agent*>(item->data(Qt::UserRole).value<void*>());
    // Rows are rebuilt from the registry, but the application owns its agents
    // and can delete one at any moment. Validating here means no caller can
    // dereference a pointer the registry no longer knows about.
    return AgentRegistry::instance().agents().contains(agent) ? agent : nullptr;
}


void AgentsWidget::onSelectionChanged()
{
    updateDetails();
}


void AgentsWidget::updateDetails()
{
    Agent* agent = selectedAgent();
    if (!agent) {
        m_details->clear();
        return;
    }

    const AgentConfig& config = agent->config();
    QStringList lines;
    lines << tr("Endpoint: %1").arg(config.url);
    lines << tr("Max tokens: %1").arg(config.maxTokens);
    lines << tr("Cost cap: %1").arg(config.costCapUsd > 0.0
              ? QStringLiteral("$%1").arg(config.costCapUsd, 0, 'f', 4)
              : tr("none"));
    lines << tr("Enabled tools: %1").arg(agent->enabledToolNames().isEmpty()
              ? tr("none")
              : agent->enabledToolNames().join(QStringLiteral(", ")));
    lines << QString();
    lines << tr("System prompt:");
    lines << (config.systemPrompt.isEmpty() ? tr("(none)") : config.systemPrompt);
    lines << QString();
    lines << tr("Last prompt:");
    lines << (agent->lastPrompt().isEmpty() ? tr("(none)") : agent->lastPrompt());
    lines << QString();
    lines << tr("Last reply:");
    lines << (agent->lastReply().isEmpty() ? tr("(none)") : agent->lastReply());
    if (!agent->lastError().isEmpty()) {
        lines << QString();
        lines << tr("Last error: %1").arg(agent->lastError());
    }

    m_details->setPlainText(lines.join(QStringLiteral("\n")));
}

} // namespace QtLLM
