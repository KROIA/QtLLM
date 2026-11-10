#include "Agent.h"
#include "AgentRegistry.h"
#include "BuiltinTools.h"
#include "Tool.h"
#include "ToolResult.h"
#include <QDateTime>
#include <QPointer>
#include <QStringList>

namespace QtLLM {

namespace {

// Tools that can only complete by putting something in front of the user.
// A background agent has no chat and no window, so a dialog raised on its
// behalf would appear with nothing to explain where it came from.
bool requiresUserInterface(const QString& toolName)
{
    static const QStringList uiTools = {
        BuiltinTools::toolName(BuiltinTool::AskUserQuestion),
        BuiltinTools::toolName(BuiltinTool::FileDialog),
        BuiltinTools::toolName(BuiltinTool::MessageBox),
        BuiltinTools::toolName(BuiltinTool::ColorPicker),
        QStringLiteral("start_repeating_task"),
        QStringLiteral("cancel_repeating_task"),
        QStringLiteral("list_repeating_tasks"),
    };
    return uiTools.contains(toolName);
}

} // namespace


QString Agent::resolveUrl(const AgentConfig& config)
{
    if (!config.url.isEmpty())
        return config.url;
    return config.provider == Provider::Ollama
        ? QStringLiteral("http://localhost:11434/api/chat")
        : QStringLiteral("https://api.anthropic.com/v1/messages");
}

QString Agent::generateName()
{
    static int counter = 0;
    return QStringLiteral("agent-%1").arg(++counter);
}


Agent::Agent(const AgentConfig& config, QObject* parent)
    : Client(config.provider, resolveUrl(config), config.apiKey, parent)
    , m_config(config)
{
    // The registry must never hold an entry the user cannot identify.
    if (m_config.name.isEmpty())
        m_config.name = generateName();
    m_config.url = resolveUrl(config);
    m_spawnedAtMs = QDateTime::currentMSecsSinceEpoch();

    setUsageAppTag(QStringLiteral("agent:") + m_config.name);

    if (!m_config.model.isEmpty())
        setModel(m_config.model);
    if (m_config.maxTokens > 0)
        setMaxTokens(m_config.maxTokens);
    if (!m_config.systemPrompt.isEmpty())
        setSystemPrompt(m_config.systemPrompt);
    if (m_config.effort != Effort::Default)
        setEffort(m_config.effort);

    // Replaceable: an app that wants an agent to raise dialogs installs its own
    // handler afterwards.
    setToolConsentHandler([](const QString& toolName, const QJsonObject&) {
        return !requiresUserInterface(toolName);
    });

    // Client corrects an unavailable model by blindly taking the provider's
    // first one. An agent has a configured preference order instead, so its own
    // resolution replaces that rule rather than running after it.
    disconnect(this, &Client::modelsAvailable, this, nullptr);

    connect(this, &Client::responseReady,   this, &Agent::onResponseReady);
    connect(this, &Client::errorOccurred,   this, &Agent::onErrorOccurred);
    connect(this, &Client::statsUpdated,    this, &Agent::onStatsUpdated);
    connect(this, &Client::modelsAvailable, this, &Agent::onModelsAvailable);

    AgentRegistry::instance().add(this);
}

Agent::~Agent()
{
    AgentRegistry::instance().remove(this);
    // Pending callbacks are dropped, not invoked: the caller is destroying the
    // agent, so it is not waiting for answers any more, and the objects those
    // callbacks capture may already be gone.
    m_queue.clear();
    m_hasCurrent = false;
}


void Agent::ask(const QString& prompt, AskCallback done)
{
    const QString trimmed = prompt.trimmed();
    if (trimmed.isEmpty()) {
        if (done)
            done(QStringLiteral("empty prompt"), false);
        return;
    }

    if (m_state == State::BudgetExceeded) {
        if (done)
            done(QStringLiteral("agent budget exhausted"), false);
        return;
    }

    m_queue.enqueue(PendingAsk{ trimmed, std::move(done) });
    dispatchNext();
}


void Agent::dispatchNext()
{
    if (m_hasCurrent || m_queue.isEmpty())
        return;

    if (m_state == State::BudgetExceeded) {
        drainQueue(QStringLiteral("agent budget exhausted"));
        return;
    }

    // Hold everything until the provider has said which models exist. The
    // request would otherwise go out with an unverified model and come back as
    // "Model not found" — a failure the fallback list exists to prevent.
    if (!m_modelResolved) {
        setState(State::Starting);
        return;
    }

    m_current    = m_queue.dequeue();
    m_hasCurrent = true;
    m_lastPrompt = m_current.prompt;
    setState(State::Busy);
    sendPrompt(m_current.prompt);
}


void Agent::finishCurrent(const QString& text, bool ok)
{
    if (!m_hasCurrent)
        return;

    // Clear the slot before invoking, so a callback that calls ask() again
    // sees an agent ready to dispatch rather than one still marked busy.
    PendingAsk finished = std::move(m_current);
    m_current    = PendingAsk{};
    m_hasCurrent = false;

    if (finished.done)
        finished.done(text, ok);
}


void Agent::drainQueue(const QString& reason)
{
    while (!m_queue.isEmpty()) {
        PendingAsk pending = m_queue.dequeue();
        if (pending.done)
            pending.done(reason, false);
    }
}


void Agent::setState(State state)
{
    if (m_state == state)
        return;
    m_state = state;
    emit stateChanged(m_state);
}


void Agent::onResponseReady(const QString& text)
{
    m_lastReply = text;
    ++m_completedTurns;
    finishCurrent(text, true);
    dispatchNext();
    if (!m_hasCurrent && m_state != State::BudgetExceeded)
        setState(State::Idle);
}


void Agent::onErrorOccurred(const QString& message)
{
    m_lastError = message;
    // A failed turn reports failure to its own caller only. The rest of the
    // queue is unrelated work and a transient network fault should not discard
    // it, so the next entry is dispatched as usual.
    finishCurrent(message, false);
    setState(State::Error);
    dispatchNext();
}


void Agent::onModelsAvailable(const QStringList& models)
{
    m_modelResolved = true;

    // An empty list means the fetch failed, not that the provider is empty.
    // Guessing from no information would be worse than trying what the
    // application configured, so the queue is simply released.
    if (!models.isEmpty() && !models.contains(model())) {
        QString chosen;
        for (const QString& candidate : m_config.fallbackModels) {
            if (models.contains(candidate)) {
                chosen = candidate;
                break;
            }
        }
        // Running on an unexpected model still beats not running at all.
        setModel(chosen.isEmpty() ? pickFallbackModel(provider(), models) : chosen);
    }

    dispatchNext();
    if (!m_hasCurrent && m_state == State::Starting)
        setState(State::Idle);
}


void Agent::onStatsUpdated(const UsageStats& stats)
{
    m_spentUsd = stats.sessionCostUsd;

    if (m_config.costCapUsd <= 0.0 || m_spentUsd < m_config.costCapUsd)
        return;
    if (m_state == State::BudgetExceeded)
        return;

    // A soft cap: cost is only known once a turn has completed, so this stops
    // further sends rather than preventing the overrun that tripped it.
    setState(State::BudgetExceeded);
    emit budgetExceeded(m_spentUsd);
    drainQueue(QStringLiteral("agent budget exhausted"));
}


void registerSubagentTool(Client* client,
                          std::function<AgentConfig()> configProvider,
                          std::function<void(Agent*)> onSpawn,
                          int maxParallel,
                          bool shareTools)
{
    if (!client || !configProvider)
        return;

    Tool tool;
    tool.setName("run_subagent")
        .setTitle("Subagent")
        .setStatusText("A subagent is working…")
        .setDescription(
            "Hand a self-contained task to a background agent and get its answer back. The "
            "agent starts with an empty conversation and sees nothing of this chat, so put "
            "everything it needs into 'task' and say what form the answer should have. It has "
            "the same tools as you, except this one and those that ask the user. To run "
            "tasks in parallel, call this tool several times in the same response. Use it for "
            "side work that is independent of the conversation; do simple things yourself.")
        .addParameter("task", "string", "The complete task, including all context needed.", true)
        .addParameter("name", "string",
                      "Short label for the agent, shown to the user (e.g. \"title-writer\").");

    QPointer<Client> owner(client);
    const QString toolName = tool.name();
    client->registerAsyncTool(tool, [owner, configProvider, onSpawn, maxParallel, shareTools, toolName]
                                    (const QJsonObject& input, ToolCompletion done) {
        if (!owner)
            return;
        if (owner->findChildren<Agent*>(QString(), Qt::FindDirectChildrenOnly).size() >= maxParallel) {
            done.finish(toolError(QStringLiteral("%1 subagents are already running; wait for "
                                                 "their results first").arg(maxParallel)));
            return;
        }

        AgentConfig config = configProvider();
        const QString name = input.value("name").toString().trimmed();
        if (!name.isEmpty())
            config.name = name;
        auto* agent = new Agent(config, owner);
        if (shareTools) {
            QStringList except{toolName};
            for (const QString& offered : owner->toolNames()) {
                if (requiresUserInterface(offered))
                    except.append(offered);
            }
            owner->shareToolsWith(agent, except);
        }
        if (onSpawn)
            onSpawn(agent);

        agent->ask(input.value("task").toString(), [agent, done](const QString& text, bool ok) {
            done.finish(ok ? toolOk({{"answer", text}}) : toolError(text));
            agent->deleteLater();
        });

        // The turn can end without the agent: the call timed out, or the user
        // cancelled. Nobody is waiting for the answer then, so stop paying for it.
        QObject::connect(owner.data(), &Client::requestFinished, agent, &QObject::deleteLater);
    });
}

} // namespace QtLLM
