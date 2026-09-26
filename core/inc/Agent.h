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
    QString  model;
    // Tried in order when the provider does not offer `model`. If none of the
    // candidates is offered either, the provider's first model is used — an
    // agent that runs on the wrong model still beats one that cannot run.
    QStringList fallbackModels;
    QString  systemPrompt;
    int      maxTokens  = 1024;
    double   costCapUsd = 0.0;            // 0 = unlimited
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

    AgentConfig        m_config;
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

} // namespace QtLLM
