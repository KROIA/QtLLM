# Background Agents and Chat Prompt Injection — Design

Date: 2026-09-26
Status: Approved for planning

## Purpose

QtLLM today exposes exactly one conversation: the user types into
`ChatDockWidget`, the host app forwards the text to a `Client`, the reply comes
back as a bubble. Everything the library can do is driven by a human typing.

Two capabilities are missing:

1. **The app cannot speak for the user.** There is no way for application code
   to place a prompt into the conversation so that it reads, from the user's
   side, exactly as if they had typed it themselves.
2. **The app cannot use an LLM privately.** Work such as analysing data,
   classifying input, or summarising state has no home. Routing it through the
   visible chat pollutes the conversation; not routing it anywhere means the app
   gets no LLM help at all.

Applications benefit from LLM capability that is not user-driven. This design
adds both, keeps them independent, and gives the user visibility into the
private half so background LLM activity is never invisible.

### Success criteria

- Host app can inject a prompt that appears in the chat as a user message and is
  answered normally.
- Host app can spawn a long-lived agent with its own model, provider, system
  prompt, and tool set; interact with it over multiple turns; and destroy it.
- Agents may mix local (Ollama) and remote (Claude) models, chosen per spawn, so
  cheap work runs locally.
- Agent activity never renders in the chat.
- Every live agent is listed in a Settings tab with enough detail for the user to
  understand what is running and what it costs.

### Constraints

- Qt5-idiomatic, `QObject`-based, GUI thread only.
- Must not restructure `Client`, `ChatDockWidget`, or the protocol layer.
- Must not introduce threads: all provider I/O is already asynchronous.

## Key finding: `Client` is already a headless agent

`Client` has no UI coupling. It owns its protocol, provider, model, system
prompt, tool registry, conversation history, usage stats, and context tracking,
and communicates purely through signals. A background agent is a `Client` that
nobody connected to a chat widget.

The design therefore adds no second engine. It adds only what `Client` genuinely
lacks: a spawn-time configuration struct, a request queue with a completion
callback, lifecycle registration, and a viewer.

`Agent` derives from `Client` rather than wrapping it. Inheriting the full
surface means `BuiltinTools::registerTools(Client*, ...)`, `setToolEnabled()`,
`setToolConsentHandler()`, `registeredTools()`, and `exportConversation()` all
work on an agent with no forwarding code and no overloads. The cost is that
agents also expose chat-oriented calls such as `sendPrompt()`; this is accepted.

## Component 1 — Chat prompt injection

`ChatDockWidget` gains two methods:

```cpp
// Renders the user bubble and emits messageSent(), exactly as if the Send
// button had been pressed. Returns false if a response is already in flight
// (the busy warning is shown and nothing is sent).
bool submitPrompt(const QString& text);

// Prefills the input field without sending. The user may edit the text and
// press Send themselves.
void setInputText(const QString& text);
```

`onSendClicked()` is refactored to read the input field and then delegate to
`submitPrompt()`, so bubble rendering, the loading state, the busy guard, and
input clearing exist in one place only.

No new signal. Injected prompts travel the existing `messageSent()` path, so
apps already wired to that signal need no changes.

## Component 2 — `Agent`

```cpp
struct QT_LLM_API AgentConfig
{
    QString  name;                      // required; shown in the Agents tab
    Provider provider = Provider::Ollama;
    QString  url;                       // empty selects the provider default
    QString  apiKey;                    // ignored for Ollama
    QString  model;
    QString  systemPrompt;
    int      maxTokens  = 1024;
    double   costCapUsd = 0.0;          // 0 = unlimited
};

class QT_LLM_API Agent : public Client
{
    Q_OBJECT
public:
    enum class State { Idle, Busy, Error, BudgetExceeded };

    explicit Agent(const AgentConfig& config, QObject* parent = nullptr);
    ~Agent() override;

    const AgentConfig& config() const;
    State   state() const;
    int     queuedPrompts() const;
    double  spentUsd() const;
    qint64  spawnedAtMs() const;
    QString lastPrompt() const;
    QString lastReply() const;
    QString lastError() const;

    // Queued one-shot call. The callback runs on the GUI thread. When ok is
    // false, text holds the error message. Conversation history persists
    // across calls, so the agent remembers earlier asks.
    void ask(const QString& prompt,
             std::function<void(const QString& text, bool ok)> done);

signals:
    void stateChanged(QtLLM::Agent::State state);
    void budgetExceeded(double spentUsd);
};
```

### Lifecycle

Construction spawns; `delete` or `deleteLater()` kills. There is no `kill()`
method — Qt object ownership already is the lifecycle, and a second mechanism
would only be able to disagree with it.

Destroying an agent with a request in flight is safe. `QNetworkAccessManager` is
a child of `HttpTransport`, which is a child of the protocol, which is a child of
`Client`; destroying the agent tears down the whole chain and aborts the reply.
Pending `ask()` callbacks are guarded by `QPointer<Agent>` so none fire after
destruction.

### Queueing

`Client` holds a single conversation history and permits one in-flight turn, so
concurrent `ask()` calls cannot be served in parallel. Rather than rejecting
them — background callers have no good way to coordinate — `ask()` appends to a
FIFO queue and the next entry is popped when `responseReady` or `errorOccurred`
resolves the current turn.

### Budget cap

`costCapUsd` is a **soft cap**. Token cost is known only after a turn completes
and `statsUpdated()` fires, so the cap refuses *further* sends once exceeded
rather than preventing the overrun itself. On exceeding, the agent enters
`State::BudgetExceeded`, emits `budgetExceeded()`, and drains its queue by
invoking every pending callback with `ok = false`. A hard pre-flight cap would
require estimating token cost before sending and is not worth the complexity.

The cap exists because invisible agents spending money unobserved is the primary
risk this feature introduces.

### Timeouts

No new timeout code. `HttpTransport` already defaults to a 60s timeout and
exposes `setTimeoutMs()`; agents inherit both.

### Tool access

Tool policy needs no new API. `Client::registerTool()` and
`Client::setToolEnabled()` already express "which tools this agent may use", and
`BuiltinTools::registerTools()` takes a `Client*`, so it accepts an `Agent`
unchanged.

Two safety rules apply:

- `BuiltinTool::AskUserQuestion` and `BuiltinTool::RepeatingTask` both require a
  `ChatDockWidget`. Registering either on an `Agent` is refused; if the model
  calls one anyway it receives an error result.
- Consent prompts must identify their origin. `Client::setToolConsentHandler()`
  and `BuiltinTools::Context::confirmFilesystemAccess` raise modal dialogs; from
  an invisible agent, such a dialog appears with no explanation. The `Agent`
  constructor installs a default consent handler that denies any tool requiring
  UI, and consent dialog text includes the agent name when the host app installs
  its own handler.

## Component 3 — `AgentRegistry`

```cpp
class QT_LLM_API AgentRegistry : public QObject
{
    Q_OBJECT
public:
    static AgentRegistry& instance();
    QList<Agent*> agents() const;       // live agents only

signals:
    void agentSpawned(QtLLM::Agent* agent);
    void agentDestroyed(const QString& name);
};
```

The `Agent` constructor registers and the destructor deregisters, so host apps
never interact with the registry.

It is a singleton deliberately. `SettingsDialog` must be able to list every live
agent in a library where the host app may create agents anywhere; the
alternative is plumbing a manager object through the app to the dialog. This
matches the existing "bind it and it works" style of `ChatDockWidget::setClient()`
and `SettingsDialog::setUsageHistory()`. The trade-off is global state and
single-`QApplication` scope, which is acceptable for a UI registry.

## Component 4 — `AgentsWidget` and the Settings tab

A dedicated widget, following the existing `UsageStatsWidget` and
`ContextUsageBar` pattern rather than inlining the UI into `SettingsDialog`.

Table, one row per live agent:

| Name | Provider / Model | State | Queue | Turns | In / Out tokens | Cost | Uptime |

Selecting a row reveals a detail pane showing the system prompt, the enabled tool
names, the last prompt, the last reply, and the last error.

The widget binds to `AgentRegistry::instance()` and refreshes on
`agentSpawned`, `agentDestroyed`, and each agent's `stateChanged` and
`statsUpdated`. It is read-only.

Kill and pause controls are omitted. The host app owns the agent pointer, and a
Settings dialog deleting an object the app still holds is a crash waiting to
happen.

`SettingsDialog` adds the tab unconditionally — no setter is needed, because the
registry is global.

## Component 5 — `Client` change

One addition:

```cpp
// Identifies the producer in recorded usage samples.
// Default: QCoreApplication::applicationName().
void setUsageAppTag(const QString& tag);
```

`Client::recordSample()` currently hardcodes `QCoreApplication::applicationName()`
into `UsageSample::app`. It uses the tag instead. The `Agent` constructor sets
the tag to `"agent:" + config.name`.

This is the whole integration with usage tracking. Every `Client` constructs its
own `UsageHistory` writing the same JSONL file, but appends are sequential within
one process, so multiple agents do not corrupt it, and `UsageHistory::reload()`
already exists for precisely this case ("lets a read-only viewer pick up samples
appended by another instance"). Agent spend therefore reaches the existing Stats
tab for free, and the existing `distinctApps()` filter separates agent spend from
chat spend.

The one cost is that each `Agent` loads the entire history file at construction.
This is acceptable for a handful of agents and carries a `ponytail:` comment
naming lazy loading as the upgrade path if agent counts grow.

## Data flow

```
Host app ──spawn──> Agent ──HTTP──> Ollama / Claude
    ^                 │
    │                 ├─ registers in ─> AgentRegistry ──> AgentsWidget (Settings tab)
    │                 │
    └──ask() callback─┘

Host app ──submitPrompt()──> ChatDockWidget ──messageSent()──> Client ──> chat reply
```

The two halves share no code path. An agent result the user should see is
delivered by the host app calling `ChatDockWidget::addAssistantMessage()` or
`submitPrompt()` from the `ask()` callback. Neither component depends on the
other.

## Error handling

- Network, HTTP, and parse failures surface through the inherited
  `Client::errorOccurred()`, move the agent to `State::Error`, and complete the
  current `ask()` callback with `ok = false`.
- A queued `ask()` is not cancelled by an error on a previous entry; the queue
  continues, because a transient network failure should not discard unrelated
  pending work. Budget exhaustion is the one case that drains the queue.
- `lastError()` retains the most recent message for display in the Agents tab.
- An `Agent` constructed with an empty `config.name` is rejected at construction
  with a logged error and a generated fallback name, so the registry never holds
  unidentifiable entries.

## Testing

`unittests/QtLLMTest/tests/TST_Agent.h`, KROIA UnitTest framework, no network
access:

- Two `ask()` calls serialize: the second begins only after the first resolves.
- Budget cap transitions state and drains pending callbacks with `ok = false`.
- Registry adds on construction and removes on destruction.
- Destroying an agent with a pending `ask()` fires no callback.
- `submitPrompt()` emits `messageSent()` and adds a bubble; it returns false and
  sends nothing while a response is loading.

## Files

New:

- `core/inc/Agent.h`, `core/src/Agent.cpp`
- `core/inc/AgentRegistry.h`, `core/src/AgentRegistry.cpp`
- `core/inc/AgentsWidget.h`, `core/src/AgentsWidget.cpp`
- `unittests/QtLLMTest/tests/TST_Agent.h`

Modified:

- `core/inc/ChatDockWidget.h`, `core/src/ChatDockWidget.cpp`
- `core/inc/Client.h`, `core/src/Client.cpp`
- `core/inc/SettingsDialog.h`, `core/src/SettingsDialog.cpp`
- `core/inc/QtLLM.h`
- `documentation/API.md`
- `examples/LibraryExample/` — demonstrate one injected prompt and one agent

CMake requires no edits. `core/CMakeLists.txt:33-34` globs `*.h` and `*.cpp`; a
reconfigure picks the new files up.

## Out of scope

Deliberately excluded, with the condition that would justify revisiting:

- **Prompt template system** with placeholders and variables — add once the app
  actually maintains a library of stored prompts.
- **Shared tool catalog** mapping names to handlers across agents —
  `BuiltinTools::registerTools(Client*)` already registers per agent.
- **Threading.** All provider I/O is asynchronous already; `QThread` would add
  signal-safety hazards and buy nothing.
- **Kill and pause controls** in the Agents tab — see Component 4.
- **Agent persistence across restarts, scheduling priorities, and inter-agent
  messaging** — no stated need.
