#pragma once
#include "QtLLM_base.h"
#include "Client.h"
#include <QQueue>
#include <QString>
#include <functional>

namespace QtLLM {

// Everything an agent needs at spawn time. Provider, model and endpoint are
// chosen per agent, so a process can mix a cheap local Ollama model for simple
// work with a remote Claude model for the hard cases.
struct QT_LLM_API AgentConfig
{
    QString  name;                        // shown in the Agents tab; generated if empty
    Provider provider   = Provider::Ollama;
    QString  url;                         // empty selects the provider default
    QString  apiKey;                      // ignored for Ollama
    // Bearer token instead of apiKey, for a gateway behind an OAuth2 proxy -
    // see Client::setAccessTokenProvider(). Usually
    // OAuth2Session::accessTokenProvider() of the app's sign-in session.
    AccessTokenProvider accessTokenProvider;
    QString  model;
    // Tried in order when the provider does not offer `model`. If none of the
    // candidates is offered either, Client::pickFallbackModel() chooses - an
    // agent that runs on the wrong model still beats one that cannot run.
    QStringList fallbackModels;
    QString  systemPrompt;
    int      maxTokens  = 1024;
    double   costCapUsd = 0.0;            // 0 = unlimited
    // Background work is usually simple; Effort::Low makes it cheaper and faster.
    Effort   effort     = Effort::Default;
    // Agents talking to each other, off by default. On: other agents (and a
    // chat with registerAgentMessagingTools()) can find this agent and send it
    // messages, and it gets the tools to do the same - see below.
    bool     messaging  = false;
    // One line that tells the others what this agent is for; it is what they
    // choose by. Shown by list_agents.
    QString  description;
};

// One tool call an agent made. Input and result are compact JSON, cut at
// Agent::kMaxRecordedJsonChars so a huge result does not stay in memory.
struct QT_LLM_API AgentToolCall
{
    QString name;
    QString input;
    QString result;              // empty while the tool is still running
    qint64  startedAtMs = 0;
    qint64  durationMs  = 0;
};

// One ask() the agent worked on: the prompt, what came back, and what it cost.
struct QT_LLM_API AgentTask
{
    QString prompt;
    QString reply;               // the answer, or the error text when !ok
    bool    ok          = false;
    bool    finished    = false; // false while the turn is in flight
    qint64  startedAtMs = 0;
    qint64  durationMs  = 0;
    int     inputTokens  = 0;
    int     outputTokens = 0;
    double  costUsd      = 0.0;
    QList<AgentToolCall> toolCalls;
};

// A headless LLM conversation the user never sees.
//
// Agent is a Client that nobody connected to a chat widget, so the whole Client
// API applies unchanged — registerTool(), setToolEnabled(),
// BuiltinTools::registerTools(), exportConversation() and every signal. On top
// of that it adds a spawn-time configuration, a queued request/callback call
// style suited to background work, a spend cap, and registration in
// AgentRegistry so the Settings dialog can show what is running.
//
// Lifetime is plain Qt ownership: constructing spawns, deleting kills. There is
// deliberately no kill() method — a second mechanism could only disagree with
// the destructor. Destroying an agent mid-request is safe: the network stack is
// a child of the Client and is torn down with it, and no pending callback fires
// afterwards.
class QT_LLM_API Agent : public Client
{
    Q_OBJECT
public:
    enum class State {
        Idle,            // nothing in flight, queue empty
        Starting,        // work is waiting for the provider's model list
        Busy,            // a turn is in flight
        Error,           // last turn failed; the queue continues
        BudgetExceeded   // costCapUsd passed; no further sends
    };
    Q_ENUM(State)

    // What an agent did, kept in memory only. Updated while the agent lives
    // (record()) and handed to AgentRegistry::finishedAgents() when it dies.
    // Nested because it carries a State; QtLLM::AgentRecord names it outside.
    struct QT_LLM_API Record
    {
        int      id = 0;                 // unique per process, stable across copies
        QString  name;
        Provider provider = Provider::Ollama;
        QString  model;                  // the model actually used
        QString  systemPrompt;
        qint64   spawnedAtMs  = 0;
        qint64   finishedAtMs = 0;       // 0 while the agent is alive
        State    state = State::Idle;    // the final state once finishedAtMs is set
        // The most recent kMaxRecordedTasks asks, oldest first.
        QList<AgentTask> tasks;

        // Totals over the agent's whole life, including tasks no longer kept.
        int      turns        = 0;       // finished tasks, ok or not
        int      toolCalls    = 0;
        int      inputTokens  = 0;
        int      outputTokens = 0;
        double   costUsd      = 0.0;
        qint64   workingMs    = 0;       // sum of the finished tasks' durations
    };

    static constexpr int kMaxRecordedTasks     = 50;
    static constexpr int kMaxRecordedJsonChars = 4000;

    // ok is false when the turn failed or was refused; text then holds the reason.
    using AskCallback = std::function<void(const QString& text, bool ok)>;

    explicit Agent(const AgentConfig& config, QObject* parent = nullptr);
    ~Agent() override;

    const AgentConfig& config() const { return m_config; }

    State   state()         const { return m_state; }
    int     queuedPrompts() const { return m_queue.size(); }
    double  spentUsd()      const { return m_spentUsd; }
    qint64  spawnedAtMs()   const { return m_spawnedAtMs; }
    int     completedTurns() const { return m_completedTurns; }
    QString lastPrompt()    const { return m_lastPrompt; }
    QString lastReply()     const { return m_lastReply; }
    QString lastError()     const { return m_lastError; }
    const Record& record()  const { return m_record; }

    // Queued one-shot call; the callback runs on the GUI thread once the turn
    // resolves. Conversation history persists across calls, so the agent
    // remembers earlier asks — call clearConversation() to forget.
    //
    // Calls are served strictly in order: a Client holds one history and one
    // in-flight turn, so a second ask() waits rather than being rejected.
    void ask(const QString& prompt, AskCallback done);

signals:
    void stateChanged(QtLLM::Agent::State state);
    // Emitted once when spendUsd passes config().costCapUsd.
    void budgetExceeded(double spentUsd);

private slots:
    void onResponseReady(const QString& text);
    void onErrorOccurred(const QString& message);
    void onStatsUpdated(const QtLLM::UsageStats& stats);
    // Picks the model to actually run on and releases the queue. Replaces the
    // blind "use the first one" correction Client applies by default.
    void onModelsAvailable(const QStringList& models);

private:
    struct PendingAsk
    {
        QString     prompt;
        AskCallback done;
    };

    // Resolved before the Client base is constructed, so it has to be static.
    static QString resolveUrl(const AgentConfig& config);
    static QString generateName();

    void setState(State state);
    void dispatchNext();
    void finishCurrent(const QString& text, bool ok);
    void drainQueue(const QString& reason);
    AgentTask* openTask();
    void closeTask(const QString& text, bool ok);
    void onToolInvoked(const QString& toolName, const QJsonObject& input);
    void onToolCompleted(const QString& toolName, const QJsonObject& result);

    AgentConfig        m_config;
    Record             m_record;
    // Stats arrive after responseReady, when the next queued ask may already
    // be running - so they belong to the last finished task, not the open one.
    bool               m_awaitingStats = false;
    State              m_state = State::Idle;
    // Nothing is sent until the provider has answered with its model list;
    // sending before that is what produces "Model not found" on the very first
    // task, before Client's self-correction has had anything to work with.
    bool               m_modelResolved = false;
    QQueue<PendingAsk> m_queue;
    PendingAsk         m_current;
    bool               m_hasCurrent    = false;
    double             m_spentUsd      = 0.0;
    int                m_completedTurns = 0;
    qint64             m_spawnedAtMs   = 0;
    QString            m_lastPrompt;
    QString            m_lastReply;
    QString            m_lastError;
};

using AgentRecord = Agent::Record;

// Lets the model of `client` delegate work: registers the tool "run_subagent".
// Each call spawns an Agent from configProvider() (asked per call, so it can
// follow the provider the user selected), gives it the task and returns its
// answer as the tool result. Several calls in one response run side by side.
//
// A subagent starts with an empty conversation. With shareTools it gets the
// tools `client` has at that moment (Client::shareToolsWith) - except
// run_subagent itself, so agents cannot multiply, and the tools that need the
// user in front of them (question card, dialogs, repeating tasks), which an
// agent refuses anyway. onSpawn runs after that: the place to add or remove
// tools and to connect to the agent's signals.
//
// The agent is a child of `client`, listed in AgentRegistry while it works and
// deleted when it has answered. At most maxParallel run at a time; further
// calls are refused with an error the model can react to.
// Agents talking to each other. Registers two tools on `client`:
//
//   list_agents                 the agents that accept messages: name,
//                               description, state
//   message_agent(to, message)  hands the message to that agent as a task
//                               and returns its answer as the tool result
//
// An agent with AgentConfig::messaging gets both automatically and is the
// only kind that can be addressed; call this function yourself to let another
// client - typically the chat - talk to those agents too. The receiver sees
// who is writing ("[Message from agent 'name']"), works through its queue in
// order and answers with its normal reply.
//
// Two agents waiting for each other would never finish, so a message to an
// agent that is itself waiting for the sender's answer is refused with an
// error the model can act on. An agent that is destroyed before it answers
// ends the call with an error as well.
QT_LLM_API void registerAgentMessagingTools(Client* client);

QT_LLM_API void registerSubagentTool(Client* client,
                                     std::function<AgentConfig()> configProvider,
                                     std::function<void(Agent*)> onSpawn = nullptr,
                                     int maxParallel = 4,
                                     bool shareTools = true);

} // namespace QtLLM

Q_DECLARE_METATYPE(QtLLM::AgentRecord)
