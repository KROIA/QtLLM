#include "Agent.h"
#include "AgentRegistry.h"
#include "BuiltinTools.h"
#include "Tool.h"
#include "ToolResult.h"
#include <QDateTime>
#include <QJsonArray>
#include <QMultiHash>
#include <QSet>
#include <QJsonDocument>
#include <QPointer>
#include <QStringList>

namespace QtLLM {

namespace {

const char* const kListAgents   = "list_agents";
const char* const kMessageAgent = "message_agent";

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

QString recordedJson(const QJsonObject& object)
{
    const QString text = QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
    if (text.size() <= Agent::kMaxRecordedJsonChars)
        return text;
    return text.left(Agent::kMaxRecordedJsonChars) + QStringLiteral("... (%1 chars)").arg(text.size());
}

int nextRecordId()
{
    static int counter = 0;
    return ++counter;
}

} // namespace


QString Agent::resolveUrl(const AgentConfig& config)
{
    if (!config.url.isEmpty())
        return config.url;
    switch (config.provider) {
    case Provider::Ollama: return QStringLiteral("http://localhost:11434/api/chat");
    case Provider::OpenAI: return QStringLiteral("http://localhost:1234/v1");
    default:               break;
    }
    return QStringLiteral("https://api.anthropic.com/v1/messages");
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

    m_record.id           = nextRecordId();
    m_record.name         = m_config.name;
    m_record.provider     = m_config.provider;
    m_record.systemPrompt = m_config.systemPrompt;
    m_record.spawnedAtMs  = m_spawnedAtMs;

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
    connect(this, &Client::toolInvoked,     this, &Agent::onToolInvoked);
    connect(this, &Client::toolCompleted,   this, &Agent::onToolCompleted);
    connect(this, &Client::modelChanged,    this, [this](const QString& name) { m_record.model = name; });
    m_record.model = model();

    AgentRegistry::instance().add(this);

    if (m_config.messaging)
        registerAgentMessagingTools(this);
}

Agent::~Agent()
{
    // Destroyed mid-turn: the task never got an answer.
    closeTask(QStringLiteral("agent destroyed before the task finished"), false);
    m_record.finishedAtMs = QDateTime::currentMSecsSinceEpoch();
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

    AgentTask task;
    task.prompt      = m_current.prompt;
    task.startedAtMs = QDateTime::currentMSecsSinceEpoch();
    m_record.tasks.append(task);
    while (m_record.tasks.size() > kMaxRecordedTasks)
        m_record.tasks.removeFirst();

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


AgentTask* Agent::openTask()
{
    if (m_record.tasks.isEmpty() || m_record.tasks.last().finished)
        return nullptr;
    return &m_record.tasks.last();
}


void Agent::closeTask(const QString& text, bool ok)
{
    AgentTask* task = openTask();
    if (!task)
        return;
    task->reply      = text;
    task->ok         = ok;
    task->finished   = true;
    task->durationMs = QDateTime::currentMSecsSinceEpoch() - task->startedAtMs;
    ++m_record.turns;
    m_record.workingMs += task->durationMs;
    // Errors come without stats; only an answered turn reports its usage.
    m_awaitingStats = ok;
}


void Agent::onToolInvoked(const QString& toolName, const QJsonObject& input)
{
    AgentTask* task = openTask();
    if (!task)
        return;
    AgentToolCall call;
    call.name        = toolName;
    call.input       = recordedJson(input);
    call.startedAtMs = QDateTime::currentMSecsSinceEpoch();
    task->toolCalls.append(call);
    ++m_record.toolCalls;
}


void Agent::onToolCompleted(const QString& toolName, const QJsonObject& result)
{
    AgentTask* task = openTask();
    if (!task)
        return;
    // ponytail: matched by name, first unanswered call wins. Parallel calls of
    // the same tool that finish out of order swap results; match by tool-use id
    // if Client ever reports it.
    for (AgentToolCall& call : task->toolCalls) {
        if (call.name == toolName && call.result.isEmpty()) {
            call.result     = recordedJson(result);
            call.durationMs = QDateTime::currentMSecsSinceEpoch() - call.startedAtMs;
            return;
        }
    }
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
    m_record.state = state;
    emit stateChanged(m_state);
}


void Agent::onResponseReady(const QString& text)
{
    m_lastReply = text;
    ++m_completedTurns;
    closeTask(text, true);
    finishCurrent(text, true);
    dispatchNext();
    if (!m_hasCurrent && m_state != State::BudgetExceeded)
        setState(State::Idle);
}


void Agent::onErrorOccurred(const QString& message)
{
    m_lastError = message;
    closeTask(message, false);
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
    // A cleared conversation restarts the session total at zero.
    const double turnCost = stats.sessionCostUsd >= m_spentUsd ? stats.sessionCostUsd - m_spentUsd
                                                               : stats.sessionCostUsd;
    m_spentUsd = stats.sessionCostUsd;
    m_record.inputTokens  += stats.inputTokens;
    m_record.outputTokens += stats.outputTokens;
    m_record.costUsd      += turnCost;
    if (m_awaitingStats) {
        m_awaitingStats = false;
        for (int i = m_record.tasks.size() - 1; i >= 0; --i) {
            AgentTask& task = m_record.tasks[i];
            if (task.finished) {
                task.inputTokens  = stats.inputTokens;
                task.outputTokens = stats.outputTokens;
                task.costUsd      = turnCost;
                break;
            }
        }
    }

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


// ------------------------------------------------------------ agents talking to each other

namespace {

// Who is waiting for whose answer right now. An agent can wait for several
// (parallel message_agent calls), hence the multi hash.
QMultiHash<const Client*, const Client*>& waitingFor()
{
    static QMultiHash<const Client*, const Client*> edges;
    return edges;
}

// True when `from` is waiting - directly or through others - for `target`.
bool isWaitingFor(const Client* from, const Client* target, QSet<const Client*>& seen)
{
    if (from == target)
        return true;
    if (seen.contains(from))
        return false;
    seen.insert(from);
    const QList<const Client*> next = waitingFor().values(from);
    for (const Client* other : next) {
        if (isWaitingFor(other, target, seen))
            return true;
    }
    return false;
}

QString senderName(const Client* client)
{
    const auto* agent = qobject_cast<const Agent*>(client);
    return agent ? agent->config().name : QString();
}

QList<Agent*> reachableAgents(const Client* except)
{
    QList<Agent*> agents;
    for (Agent* agent : AgentRegistry::instance().agents()) {
        if (agent != except && agent->config().messaging)
            agents.append(agent);
    }
    return agents;
}

QString stateText(Agent::State state)
{
    switch (state) {
    case Agent::State::Idle:           return QStringLiteral("idle");
    case Agent::State::Starting:       return QStringLiteral("starting");
    case Agent::State::Busy:           return QStringLiteral("busy");
    case Agent::State::Error:          return QStringLiteral("idle (last task failed)");
    case Agent::State::BudgetExceeded: return QStringLiteral("budget exhausted");
    }
    return QString();
}

} // namespace

void registerAgentMessagingTools(Client* client)
{
    if (!client)
        return;
    QPointer<Client> self(client);
    // A sender that goes away is not waiting for anyone any more.
    QObject::connect(client, &QObject::destroyed, [client]() { waitingFor().remove(client); });

    Tool list;
    list.setName(kListAgents)
        .setTitle("List agents")
        .setGroup("Agents")
        .setStatusText("Looking up the agents...")
        .setReadOnly(true)
        .setDescription("List the other agents that are running and can be written to: name, "
                        "what each is for, and whether it is busy. Use it before message_agent.");
    client->registerTool(list, [self](const QJsonObject&) {
        QJsonArray agents;
        for (Agent* agent : reachableAgents(self)) {
            QJsonObject entry{{"name", agent->config().name}, {"state", stateText(agent->state())}};
            if (!agent->config().description.isEmpty())
                entry["description"] = agent->config().description;
            if (agent->queuedPrompts() > 0)
                entry["queued"] = agent->queuedPrompts();
            agents.append(entry);
        }
        QJsonObject result{{"agents", agents}};
        const QString own = senderName(self);
        if (!own.isEmpty())
            result["you"] = own;
        return toolOk(result);
    });

    Tool message;
    message.setName(kMessageAgent)
        .setTitle("Message agent")
        .setGroup("Agents")
        .setStatusText("Waiting for another agent...")
        .setDescription(
            "Send a message to another running agent and get its answer back. The agent keeps "
            "its own conversation, so it remembers earlier messages, but it sees nothing of "
            "yours: write what it needs to know. It works through its messages in order; a "
            "busy agent answers later. Do not message an agent to answer a message it sent "
            "you - your reply is the answer. Names come from list_agents.")
        .addParameter("to", "string", "Name of the agent, as list_agents reports it.", true)
        .addParameter("message", "string", "What you want from it, with the context it needs.", true);
    client->registerAsyncTool(message, [self](const QJsonObject& input, ToolCompletion done) {
        if (!self)
            return;
        const QString to = input.value("to").toString().trimmed();
        QList<Agent*> matches;
        QStringList names;
        for (Agent* agent : reachableAgents(self)) {
            names.append(agent->config().name);
            if (agent->config().name.compare(to, Qt::CaseInsensitive) == 0)
                matches.append(agent);
        }
        if (matches.size() != 1) {
            done.finish(toolError(
                (matches.isEmpty() ? QStringLiteral("no agent '%1' accepts messages")
                                   : QStringLiteral("several agents are called '%1'")).arg(to),
                {{"agents", QJsonArray::fromStringList(names)}}));
            return;
        }
        Agent* target = matches.first();

        // The target would queue this message behind the turn in which it is
        // waiting for us: both would wait until the tool times out.
        QSet<const Client*> seen;
        if (isWaitingFor(target, self, seen)) {
            done.finish(toolError(QStringLiteral(
                "'%1' is waiting for your reply to its own message and cannot take a new one "
                "until then. Answer it by finishing your reply.").arg(target->config().name)));
            return;
        }

        const Client* sender = self.data();
        const QString own = senderName(sender);
        const QString text = (own.isEmpty() ? QStringLiteral("[Message from the main assistant]\n")
                                            : QStringLiteral("[Message from agent '%1']\n").arg(own))
                           + input.value("message").toString();

        waitingFor().insert(sender, target);
        auto settle = [sender, target]() { waitingFor().remove(sender, target); };
        // An agent that is deleted drops its callbacks; without this the
        // sender would wait for the tool's timeout.
        const QMetaObject::Connection gone = QObject::connect(target, &QObject::destroyed, self.data(),
            [done, settle]() {
                settle();
                done.finish(toolError(QStringLiteral("the agent was closed before it answered")));
            });
        const QString targetName = target->config().name;
        target->ask(text, [done, settle, gone, targetName](const QString& reply, bool ok) {
            QObject::disconnect(gone);
            settle();
            done.finish(ok ? toolOk({{"from", targetName}, {"reply", reply}})
                           : toolError(QStringLiteral("'%1' could not answer: %2").arg(targetName, reply)));
        });
    });
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
        // What the application allows the chat, it allows its helpers.
        agent->setModelFilter(owner->allowedModels(), owner->blockedModels());
        if (shareTools) {
            // Not the messaging tools either: their handlers speak in the
            // owner's name. An agent gets its own through AgentConfig::messaging.
            QStringList except{toolName, QString::fromLatin1(kListAgents),
                               QString::fromLatin1(kMessageAgent)};
            for (const QString& offered : owner->toolNames()) {
                if (requiresUserInterface(offered))
                    except.append(offered);
            }
            // Scripts belong to the chat: one runs at a time, in front of the user.
            for (const Tool& offered : owner->registeredTools()) {
                if (offered.group() == QLatin1String("Scripting"))
                    except.append(offered.name());
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
