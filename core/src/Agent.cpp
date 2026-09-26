#include "Agent.h"
#include "AgentRegistry.h"
#include "BuiltinTools.h"
#include <QDateTime>
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
        setModel(chosen.isEmpty() ? models.first() : chosen);
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

} // namespace QtLLM
