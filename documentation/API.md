# API Reference

All public types live in the `QtLLM` namespace. Include the umbrella header:

```cpp
#include <QtLLM.h>
```

Contents: [Provider](#qtllmprovider) · [Client](#qtllmclient) · [Agent](#qtllmagent) · [AgentRegistry](#qtllmagentregistry) · [AgentsWidget](#qtllmagentswidget) · [Tool](#qtllmtool) · [ToolResult helpers](#tool-result-helpers) · [BuiltinTools](#qtllmbuiltintools) · [InterviewTool](#qtllminterviewtool) · [InterviewWidget](#qtllminterviewwidget) · [ChatDockWidget](#qtllmchatdockwidget) · [SettingsDialog](#qtllmsettingsdialog) · [UsageStats](#qtllmusagestats) · [PricingRegistry](#qtllmpricingregistry) · [UsageHistory](#qtllmusagehistory) · [OllamaManager](#qtllmollamamanager) · [Error handling](#error-handling)

---

## `QtLLM::Provider`

Selects the LLM backend. Set at construction, changeable later with `Client::setProvider()`.

```cpp
enum class Provider { Claude, Ollama, ClaudeCode };
```

| Value | Backend |
|---|---|
| `Provider::Claude` | Anthropic Messages API with an API key — api.anthropic.com, Microsoft Foundry, or a compatible gateway |
| `Provider::Ollama` | Ollama local inference server |
| `Provider::ClaudeCode` | The user's claude.ai subscription, through the installed Claude Code CLI. `url` and `apiKey` are unused. See [Providers.md](Providers.md#claude-subscription-claude-code-login) |

---

## `QtLLM::Client`

The main class. Inherits `QObject`. Manages conversation history, tool registration, and the full request/response cycle including multi-turn tool loops.

### Constructors

```cpp
// Claude — API key + endpoint URL
explicit Client(const QString& apiKey,
                const QString& url = "https://api.anthropic.com/v1/messages",
                QObject* parent = nullptr);

// Explicit provider selection (Ollama: apiKey may stay empty)
explicit Client(Provider provider,
                const QString& url,
                const QString& apiKey = QString(),
                QObject* parent = nullptr);
```

### Configuration

| Method | Description |
|---|---|
| `setModel(QString)` | Model identifier. Provider-specific (e.g. `"claude-sonnet-5-5"` or `"llama3.2"`) |
| `model()` | The model requests actually go out with. Not necessarily the one you set — a model the provider does not offer is replaced by `pickFallbackModel()` |
| `Client::defaultModel(Provider)` | Model used until the application sets one: `claude-sonnet-5-5` (best value for tool work), `llama3.2`, or `sonnet` for the subscription |
| `setMaxTokens(int)` | Maximum tokens in the response (default 16000 for Claude, 2048 for Ollama). Maps to `num_predict` for Ollama; ignored by `Provider::ClaudeCode` |
| `setProvider(Provider, url, apiKey)` / `provider()` | Switch backend at runtime. Starts a new conversation; tools, model and system prompt are kept |
| `setApiKey(QString)` / `setEndpointUrl(QString)` | Change credentials without touching the conversation |
| `setSystemPrompt(QString)` | Prepended to every request. Omitted when empty. May be changed mid-conversation — history is kept, takes effect from the next request (invalidates the prompt-cache prefix) |

### Tool registration

| Method | Description |
|---|---|
| `registerTool(Tool, ToolHandler)` | Register using the `Tool` builder |
| `registerTool(name, description, schema, ToolHandler)` | Register with a raw JSON schema object |
| `unregisterTool(QString)` | Remove a registered tool by name |
| `shareToolsWith(Client* other, QStringList except = {})` | Register this client's tools on `other` too: same definitions, handlers and enabled state. The handlers are shared, so a tool that reports into the chat still does so when `other` calls it |

`ToolHandler` = `std::function<QJsonObject(const QJsonObject&)>`

Handlers are called synchronously on the Qt event loop thread — this is guaranteed, so a handler may open a modal dialog or spin a local event loop. The return value is sent back to the LLM as the tool result; `{"status":"error", ...}` results are marked `is_error`. A handler that throws is caught by the protocol layer and converted to an error result (see [ToolUse.md](ToolUse.md)).

### Enabling / disabling tools

| Method | Description |
|---|---|
| `setToolEnabled(QString, bool)` | Hide/show a tool without losing definition or handler. Takes effect on the next request. A disabled tool called anyway receives `{"status":"error","message":"tool is disabled"}` |
| `isToolEnabled(QString)` | `false` for unknown names |
| `toolNames()` | All registered tool names |
| `enabledToolNames()` | Names currently advertised to the model |
| `registeredTools()` | `QList<Tool>` incl. UI metadata (title/group/statusText) — basis for settings UIs. Raw-schema registrations appear with name/description only |

### Tool safety & gating

| Method | Description |
|---|---|
| `setValidateToolInput(bool)` / `validateToolInput()` | Opt-in schema validation before the handler runs (required fields, types, enum values case-insensitively). Default **off** |
| `setMaxToolCallsPerTurn(int)` / `maxToolCallsPerTurn()` | Cap on executed tool calls per user turn; `0` = unlimited (default). Over the cap, handlers are skipped and `toolCallLimitReached(int)` is emitted once per turn |
| `setToolConsentHandler(ToolConsentHandler)` | Accept/decline gate called synchronously on the GUI thread before every handler. Return `false` → handler skipped, model receives `"user declined"`. `nullptr` resets (default: everything allowed) |

```cpp
using ToolConsentHandler =
    std::function<bool(const QString& toolName, const QJsonObject& input)>;
```

Execution order per tool call: **call cap → validation → consent → handler**.

### Conversation

| Method | Description |
|---|---|
| `sendPrompt(QString)` | Append user message and send the full conversation to the API |
| `sendToolMessage(QString toolName, QJsonObject input)` | Start a model turn from application code, formatted as a tool event (e.g. "timer finished"). Costs tokens like any request |
| `cancelRequest()` | Abort the turn in flight: request cancelled, queued prompts dropped, the unanswered prompt removed from the history. Emits `requestFinished()` only. A handler already running is not interrupted |
| `clearConversation()` | Reset history and session statistics. Tools and settings are kept |
| `conversationHistory()` | `QJsonArray` of `{role, content}` messages |
| `exportConversation()` | Rich JSON export: raw messages (incl. `tool_use`/`tool_result` blocks), usage statistics, per-turn timeline |

### Statistics & models

| Method | Description |
|---|---|
| `usageStats()` | `UsageStats` snapshot from the most recently completed turn |
| `usageHistory()` | Pointer to the persistent per-turn [`UsageHistory`](#qtllmusagehistory) (JSONL-backed) |
| `setUsageAppTag(QString)` / `usageAppTag()` | Producer name recorded in `UsageSample::app`. Defaults to the application name; [`Agent`](#qtllmagent) sets `"agent:<name>"` so background spend is separable in the usage charts |
| `fetchAvailableModels()` | Async model list from the current provider; results via `modelsAvailable()` |

### Signals

| Signal | When emitted |
|---|---|
| `responseReady(QString text)` | Final LLM reply after all tool calls in the turn are resolved |
| `toolInvoked(QString name, QJsonObject input)` | Just before a registered handler is called |
| `toolCompleted(QString name, QJsonObject result)` | After a handler returns |
| `errorOccurred(QString message)` | Network, HTTP, or parse error — `requestFinished` also fires |
| `requestStarted()` | Each HTTP send, including tool-loop re-sends |
| `requestFinished()` | Turn fully resolved (success or error) |
| `statsUpdated(UsageStats stats)` | Once per completed turn with token counts, timing, and cost |
| `modelsAvailable(QStringList models)` | Result of `fetchAvailableModels()` |
| `modelChanged(QString model)` | The active model changed — including the automatic correction when the provider does not offer the configured one. Connect this to keep your own model UI honest |
| `toolCallLimitReached(int limit)` | Once per turn when the tool-call cap is exceeded |

---

## `QtLLM::Agent`

A headless conversation the user never sees. `Agent` **is** a `Client` — the entire `Client` API above applies unchanged, including `registerTool()`, `setToolEnabled()`, `BuiltinTools::registerTools()`, `exportConversation()`, and every signal. On top of that it adds spawn-time configuration, a queued request/callback call style, a spend cap, and automatic registration in [`AgentRegistry`](#qtllmagentregistry).

Use it for work that should not appear in the chat: classifying input, analysing data, summarising application state. Provider and model are chosen per agent, so a cheap local Ollama model can serve simple tasks while a remote Claude model handles the hard ones.

```cpp
QtLLM::AgentConfig config;
config.name         = "log-classifier";
config.provider     = QtLLM::Provider::Ollama;   // local, no API key needed
config.model        = "llama3.2";
config.systemPrompt = "Classify log lines as INFO, WARN or CRITICAL. Reply with one word.";
config.costCapUsd   = 0.50;

auto* agent = new QtLLM::Agent(config, this);    // spawns; appears in the Agents tab
QtLLM::BuiltinTools::registerTools(agent, { QtLLM::BuiltinTool::ReadTextFile });

agent->ask("disk usage at 97% on /var", [](const QString& reply, bool ok) {
    if (ok) qDebug() << "classified as" << reply;
});

delete agent;   // kills it; pending callbacks are dropped, not invoked
```

### `QtLLM::AgentConfig`

| Field | Default | Description |
|---|---|---|
| `name` | *(generated)* | Shown in the Agents tab. An empty name gets `"agent-N"` so the registry never holds an unidentifiable entry |
| `provider` | `Provider::Ollama` | Backend for this agent |
| `url` | *(provider default)* | Endpoint; empty means `http://localhost:11434/api/chat` or `https://api.anthropic.com/v1/messages` |
| `apiKey` | *(empty)* | Ignored for Ollama |
| `model` | *(empty)* | Preferred model identifier |
| `fallbackModels` | *(empty)* | Tried in order when the provider does not offer `model` |
| `systemPrompt` | *(empty)* | Omitted when empty |
| `maxTokens` | `1024` | Response cap |
| `costCapUsd` | `0.0` | Spend cap; `0` = unlimited |

### Lifetime

Constructing spawns, `delete` (or `deleteLater()`) kills. There is deliberately no `kill()` method — Qt object ownership already is the lifetime, and a second mechanism could only disagree with it. Destroying an agent mid-request is safe: the network stack is a child of the `Client` and is torn down with it, and no pending callback fires afterwards.

### Methods

| Method | Description |
|---|---|
| `ask(QString prompt, AskCallback done)` | Queue a turn. `done(text, ok)` runs on the GUI thread when it resolves; `ok == false` means `text` holds the reason. History persists across calls — call `clearConversation()` to forget |
| `config()` | The `AgentConfig` in effect, with generated name and resolved URL filled in |
| `state()` | `Idle`, `Starting`, `Busy`, `Error`, or `BudgetExceeded` |
| `queuedPrompts()` | Prompts waiting behind the in-flight turn |
| `spentUsd()` / `completedTurns()` / `spawnedAtMs()` | Running totals for display |
| `lastPrompt()` / `lastReply()` / `lastError()` | Most recent values, shown in the Agents tab |

Calls are served strictly in order. A `Client` holds one history and permits one in-flight turn, so a second `ask()` waits rather than being rejected — background callers have no good way to coordinate among themselves.

### Signals

| Signal | When emitted |
|---|---|
| `stateChanged(Agent::State state)` | Any state transition |
| `budgetExceeded(double spentUsd)` | Once, when spend passes `costCapUsd` |

### Subagents: letting the model delegate

```cpp
QtLLM::registerSubagentTool(&client,
    [this]() { return agentConfigForCurrentProvider(); },        // asked per call
    [this](QtLLM::Agent* agent) { /* connect to its signals, add or remove tools */ },
    4,                                                            // at most 4 at a time
    true);                                                        // share the client's tools
```

Registers the tool `run_subagent(task, name?)` on the client. Each call spawns an `Agent` with an empty conversation, gives it the task and returns its answer as the tool result; several calls in one response run side by side. The agent is a child of the client, listed in `AgentRegistry` while it works and deleted when it has answered, when the call times out or when the turn is cancelled.

With `shareTools` (default) the agent gets the tools the client has at that moment, except `run_subagent` itself (agents cannot multiply) and the tools that need the user in front of them (question card, dialogs, repeating tasks). `onSpawn` runs afterwards. A call beyond `maxParallel` is refused with an error the model can react to.

### Model resolution and fallbacks

An agent typically fires its first task within milliseconds of being spawned, long before it could know what the endpoint offers. Sending straight away is how a configured-but-retired model turns into `"Model not found"` on the very first task.

So an agent sends nothing until the provider has answered with its model list. Until then it reports `State::Starting` and holds the queue. Once the list arrives:

1. If the provider offers `config.model`, it is kept.
2. Otherwise `config.fallbackModels` is walked in order and the first one the provider offers wins.
3. If no candidate matches, `Client::pickFallbackModel()` decides: the default model (`claude-sonnet-5-5`), then the next cheaper current one, and only then the provider's first — an agent running on an unexpected model still beats one that cannot run.
4. If the list comes back **empty**, the fetch failed rather than the provider being empty. Guessing from no information would be worse than trying what was configured, so the configured model is kept and the queue is released.

Any replacement emits `Client::modelChanged(QString)`, and `model()` returns what is actually in use — `config().model` still holds what you asked for, so comparing the two tells you a fallback happened.

```cpp
config.model          = "claude-sonnet-5-5";
config.fallbackModels = { "claude-haiku-5-5" };

connect(agent, &QtLLM::Client::modelChanged, this, [agent](const QString& effective) {
    qWarning() << agent->config().model << "unavailable, running on" << effective;
});
```

Failures that are not about the model — auth, rate limits, timeouts, malformed replies — are reported to the `ask()` callback with `ok == false` and through `Client::errorOccurred()`. They do **not** trigger a model switch, and they do not discard the rest of the queue; the next task is dispatched as usual. Deciding what to do about them is the application's call.

### Budget cap

`costCapUsd` is a **soft cap**. Token cost is only known once a turn completes and `statsUpdated()` fires, so the cap refuses *further* sends rather than preventing the overrun that tripped it. On exceeding, the agent enters `State::BudgetExceeded` and drains its queue, invoking every pending callback with `ok == false`.

The cap exists because an agent nobody is watching is the one that quietly runs up a bill.

### Tool safety

Tool policy needs no agent-specific API: `registerTool()` and `setToolEnabled()` already express which tools an agent may use. Two defaults protect the invisible case:

- Tools that can only complete by putting something in front of the user — `ask_user_question`, `file_dialog`, `message_box`, `color_picker`, and the repeating-task trio — are **denied by default**, because a dialog raised on a background agent's behalf appears with nothing to explain where it came from. Install your own `setToolConsentHandler()` to override.
- Everything else behaves exactly as on a normal `Client`.

---

## `QtLLM::AgentRegistry`

Process-wide list of live agents. Agents add themselves on construction and remove themselves on destruction, so applications never call into it — it exists so [`SettingsDialog`](#qtllmsettingsdialog) can show every agent without the app wiring anything up.

| Member | Description |
|---|---|
| `AgentRegistry::instance()` | The singleton |
| `agents()` | Live agents, in spawn order |
| `agentSpawned(Agent*)` | Signal: an agent was constructed |
| `agentDestroyed(QString name)` | Signal: an agent was destroyed |

---

## `QtLLM::AgentsWidget`

Read-only `QWidget` listing every live [`Agent`](#qtllmagent) in the process: name, provider and **effective** model, state, queue depth, turns, tokens in/out, cost, and uptime, with a detail pane for the selected agent's endpoint, token and cost caps, enabled tools, system prompt, and last prompt/reply/error.

```cpp
auto* widget = new QtLLM::AgentsWidget(this);   // that is the entire setup
```

It binds to `AgentRegistry::instance()` itself, so it needs no wiring and no client — construct it anywhere and every agent in the process appears. [`SettingsDialog`](#qtllmsettingsdialog) already embeds one as its **Agents** tab; construct your own only if you want the list somewhere else, such as a docked debug panel.

The table rebuilds once a second so uptime and in-flight counters keep moving, and immediately on `agentSpawned` / `agentDestroyed`. Selection survives a rebuild, so the detail pane does not jump while it is being read.

There are no kill or pause controls, on purpose: the application owns its agents, and a widget deleting an object the app still holds a pointer to is a crash waiting to happen. Delete the agent yourself and the row disappears.

---

## `QtLLM::Tool`

Builder for LLM tool schemas. Not a `QObject` — safe to copy and store by value.

```cpp
QtLLM::Tool tool;
tool.setName("createCircle")
    .setDescription("Creates a circle at the given position.")
    .addParameter("x",      "number", "X coordinate.", true)
    .addParameter("y",      "number", "Y coordinate.", true)
    .addEnumParameter("style", {"solid", "dashed"}, "Line style.", false)
    .setTitle("Create circle")          // UI-only metadata
    .setGroup("Drawing")
    .setStatusText("Drawing circle…");
```

### Schema building

| Method | Returns | Description |
|---|---|---|
| `setName(QString)` | `Tool&` | Tool name used by the LLM to invoke it |
| `setDescription(QString)` | `Tool&` | Natural-language description for the LLM |
| `addParameter(name, type, description, required = false, enumValues = {})` | `Tool&` | JSON Schema primitive type: `"string"` `"number"` `"integer"` `"boolean"` `"array"` `"object"`. Non-empty `enumValues` adds `"enum": [...]` |
| `addEnumParameter(name, allowedValues, description, required = false)` | `Tool&` | Convenience: string parameter restricted to a fixed value set |
| `toApiObject()` | `QJsonObject` | Claude wire format — uses `"input_schema"` key |
| `toOpenAiApiObject()` | `QJsonObject` | OpenAI/Ollama wire format — wraps parameters under `"function"` → `"parameters"` |

### Richer schemas and input checking

| Method | Description |
|---|---|
| `addArrayParameter(name, itemType \| itemSchema, description, required)` | Array with a declared element type |
| `addObjectParameter(name, Tool fields, description, required)` | Nested object; its properties are the parameters of `fields` |
| `addRawParameter(name, QJsonObject schema, required)` | Any JSON Schema for one parameter |
| `setRange(name, min, max)` / `setDefault(name, value)` | `minimum`/`maximum`; a default that is applied when the model omits the parameter |
| `addExample(QJsonObject input)` | Example call, rendered into the description the model reads |
| `setStrict(bool)` | Claude strict mode: schema-valid input guaranteed by the API |
| `Tool::prepareInput(schema, args, &error)` | What `Client::setValidateToolInput(true)` runs: converts `"5"`/`"true"`, normalises enum spelling, applies defaults, validates recursively and rejects unknown parameter names with a `did_you_mean` hint |
| `Tool::isValidName(name)` | Names must match `[a-zA-Z0-9_-]{1,64}`; a violation is logged at registration |
| `setReadOnly(bool)` / `setMaxResultChars(int)` / `setTimeoutMs(int)` | Behaviour hints: consent, result cap, asynchronous timeout |

### UI metadata (never sent to the model)

| Method | Description |
|---|---|
| `setTitle(QString)` / `title()` | Display name for settings dialogs |
| `setGroup(QString)` / `group()` | Grouping for UI lists |
| `setStatusText(QString)` / `statusText()` | Shown in the chat status line while the tool executes (see `ChatDockWidget::setClient`) |
| `name()` / `description()` | Getters |

### Validation

| Method | Description |
|---|---|
| `validate(QJsonObject args)` | Empty object when valid, otherwise a ready-to-send error result: `{"status":"error","message":"missing required parameter: id","expected":{...}}` or `{"status":"error","message":"invalid value for 'type'","allowed_values":[...]}`. Enum comparison is case-insensitive |
| `static validateAgainstSchema(QJsonObject schema, QJsonObject args)` | Same check driven by a raw JSON-Schema object |

---

## Tool result helpers

`ToolResult.h` — free functions for the tool-result convention (`"status": "error"` → sent to the model with `is_error: true`):

```cpp
QJsonObject QtLLM::toolOk(const QJsonObject& payload = {});
// -> payload + {"status":"ok"}

QJsonObject QtLLM::toolError(const QString& message, const QJsonObject& extra = {});
// -> extra + {"status":"error","message":message}
```

---

## `QtLLM::BuiltinTools`

Registrar for the predefined out-of-the-box tools. Nothing is registered automatically — the app opts in per `BuiltinTool` enum value. See [ToolUse.md — Built-in tools](ToolUse.md#built-in-tools) for the full behaviour table.

```cpp
enum class BuiltinTool {
    AskUserQuestion,  // interview form in the chat (needs Context::chat)
    FileDialog,       // native open/save/folder dialog
    MessageBox,       // info/warning/yes-no dialog
    ColorPicker,      // QColorDialog
    ClipboardRead, ClipboardWrite,
    CurrentDateTime, SystemInfo, OpenUrl,
    Wait,             // pause for N seconds (asynchronous; the application stays responsive)
    ListDirectory, ReadTextFile, WriteTextFile,  // consent-gated (see below)
    RepeatingTask,    // registers 3 tools: start/cancel/list_repeating_task(s)
};
```

```cpp
struct BuiltinTools::Context {
    ChatDockWidget* chat = nullptr;          // AskUserQuestion, RepeatingTask
    QWidget* dialogParent = nullptr;         // modal dialogs
    bool confirmFilesystemAccess = true;     // built-in consent dialog for
                                             // ListDirectory/ReadTextFile (read)
                                             // and WriteTextFile (write); offers
                                             // "Allow for the rest of this session"
};
```

| Method | Description |
|---|---|
| `static registerTool(Client*, BuiltinTool, Context = {})` | Register one built-in tool. Missing context requirement → the tool returns an error result when called |
| `static registerTools(Client*, QList<BuiltinTool>, Context = {})` | Register several at once (do this before the first `sendPrompt` to keep the prompt cache warm) |
| `static toolName(BuiltinTool)` | Model-facing name, e.g. `"file_dialog"`. `RepeatingTask` returns its primary tool `"start_repeating_task"` (it also registers `cancel_repeating_task` and `list_repeating_tasks`) |
| `static allTools()` | All enum values in declaration order — basis for settings UIs |

Every built-in tool carries `title`/`group`/`statusText` metadata and behaves like a user tool afterwards (`setToolEnabled`, validation, consent hook, call cap all apply).

---

## `QtLLM::InterviewTool`

The `ask_user_question` tool: lets the model interview the user through an interactive card inside the chat — the same pattern as Claude Code's AskUserQuestion. Registered directly or via `BuiltinTool::AskUserQuestion`.

| Method | Description |
|---|---|
| `static name()` | `"ask_user_question"` |
| `static description()` | Tool description sent to the model |
| `static parameterSchema()` | Raw JSON Schema (questions 1–4, options 2–4, `multiSelect`, `allowCustom`) |
| `static registerOn(Client*, ChatDockWidget*)` | Register on the client, wired to the chat. If the chat is destroyed while a question is pending, the tool returns an error result |

The handler blocks in a local event loop (like `QDialog::exec`) until the user submits or skips; the UI stays responsive.

---

## `QtLLM::InterviewWidget`

Inline interview card (`QFrame`) used by `InterviewTool`; can also be embedded manually.

```cpp
explicit InterviewWidget(const QJsonObject& request, QWidget* parent = nullptr);
```

Request format (same as the `ask_user_question` tool input):

```json
{ "questions": [ {
    "question":    "Which database should we use?",
    "header":      "Database",
    "multiSelect": false,
    "allowCustom": true,
    "options": [ { "label": "Postgres", "description": "Relational, full SQL" },
                 { "label": "SQLite",   "description": "Embedded, zero setup" } ]
} ] }
```

| Member | Description |
|---|---|
| `isAnswered()` | `true` once the user submitted or skipped |
| `result()` | `{"status":"answered","answers":{"<question>": "label" \| ["l1","l2"]}}` or `{"status":"skipped"}`; empty until the user acted |
| signal `finished(QJsonObject result)` | Emitted exactly once, on submit or skip |

---

## `QtLLM::ChatDockWidget`

Ready-made chat panel (`QDockWidget`): message bubbles with Markdown rendering, input field, Send/Cancel, status line, token counter, Save and Settings buttons. Assistant messages render tables, inline code, and hex color codes (`#rrggbb`) get a colored rectangle appended.

### Messages

| Method | Description |
|---|---|
| `addUserMessage(QString)` / `addAssistantMessage(QString)` | Append a bubble (local only — no API call) |
| `submitPrompt(QString)` | Inject a prompt as if the user had typed it and pressed Send: renders the user bubble and emits `messageSent()`. Returns `false` and sends nothing when the text is blank or a response is in flight |
| `setInputText(QString)` / `inputText()` | Prefill the input field without sending, so the user can edit a suggestion and send it themselves |
| `clearMessages()` | Remove all bubbles |
| `addInterviewWidget(QJsonObject request)` | Insert an interview card (non-blocking); result via `interviewFinished()` |
| `execInterview(QJsonObject request)` | Blocking convenience for tool handlers: local event loop until submit/skip. Returns the result; `{"status":"cancelled"}` if the widget dies while waiting. GUI thread only |

### State & status

| Method | Description |
|---|---|
| `setLoading(bool)` | Show/hide the loading indicator, toggle Send/Cancel |
| `setStatusText(QString)` / `clearStatus()` | One-line status under the conversation |
| `updateTokenUsage(int in, int out)` | Update the token counter |
| `setFontSizePercent(int)` | Scale chat font (50–200 %) |
| `setClient(Client*)` | Bind a client: Save button exports the conversation itself, and a tool's `statusText` metadata is shown automatically while the tool runs |

### Texts

`setAssistantName` / `setUserName` (empty hides the bubble header), `setSendButtonText`, `setCancelButtonText`, `setSettingsButtonTooltip`, `setInputPlaceholderText`, `setLoadingText`, `setCancelledText`, `setBusyWarningText`.

### Signals

| Signal | When emitted |
|---|---|
| `messageSent(QString)` | User pressed Send, or `submitPrompt()` injected a prompt |
| `cancelRequested()` | User pressed Cancel |
| `settingsRequested()` | User clicked the settings button |
| `saveConversationRequested()` | Save clicked and **no** client bound (fallback for custom handling) |
| `interviewFinished(QJsonObject result)` | An interview card was submitted or skipped |

---

## `QtLLM::SettingsDialog`

Ready-made settings dialog (`QDialog`) with five tabs: **Settings** (provider, API key, endpoint, model incl. auto-detection, system prompt, font size), **Tools**, **Statistik** (usage charts), **Context**, and **Agents**.

The **Agents** tab is a read-only view of every live [`Agent`](#qtllmagent) in the process — name, provider/model, state, queue depth, turns, tokens, cost, and uptime, with a detail pane for the selected agent's system prompt, enabled tools, and last prompt/reply/error. It binds to `AgentRegistry` itself, so it needs no wiring. It has no kill or pause controls on purpose: the application owns its agents, and a dialog deleting an object the app still holds is a crash waiting to happen.

| Method | Description |
|---|---|
| `provider()` / `setProvider(Provider)` | `SettingsDialog::Provider { Claude, Ollama, Foundry, ClaudeSubscription }` — values are stable, safe to persist as int |
| `apiKey()` / `setApiKey`, `model()` / `setModel`, `endpointUrl()` / `setEndpointUrl`, `ollamaUrl()` / `setOllamaUrl`, `foundryApiKey()` / `setFoundryApiKey`, `foundryEndpointUrl()` / `setFoundryEndpointUrl`, `systemPrompt()` / `setSystemPrompt`, `fontSizePercent()` / `setFontSizePercent` | Field access. Each provider keeps its own credentials. `foundryEndpointUrl()` completes a bare resource URL with `/v1/messages` |
| `applyTo(Client*)` | Push provider, credentials, model and system prompt into a client in one call. Switches the client's provider only when the kind of backend changed |
| `effort()` / `setEffort`, `contextPolicy()` / `setContextPolicy` | Effort combo on the Settings tab; context limit, automatic compaction and strategy on the Context tab (which also has **Compact now**) |
| `setAvailableModels(QStringList)` | Populate the model combo (preserves current text) |
| `setUsageHistory(UsageHistory*)` | Wire the statistics tab to live data |
| `setClient(Client*)` | Fill the **Tools** tab: every registered tool grouped by `group()`, with title, name, description and an enable checkbox. Toggles are applied in one batch on Apply |

| Signal | When emitted |
|---|---|
| `settingsApplied()` | Apply clicked (after tool toggles were applied) |
| `detectModelsRequested()` | "Modelle laden" clicked — connect to `Client::fetchAvailableModels`, feed `modelsAvailable` back into `setAvailableModels` |

---

## `QtLLM::UsageStats`

Plain struct populated after each completed turn. Returned by `Client::usageStats()` and carried by the `statsUpdated` signal.

```cpp
struct UsageStats {
    // Per-turn (reset each beginTurn call)
    int    inputTokens;   // prompt tokens consumed
    int    outputTokens;  // completion tokens generated
    int    toolCalls;     // tool invocations within this turn
    qint64 durationMs;    // wall-clock ms from first HTTP send to final response

    // Session cumulative totals
    int    sessionInputTokens;
    int    sessionOutputTokens;
    int    sessionToolCalls;
    int    sessionTurnCount;
    double sessionCostUsd;   // estimated USD; always 0 for Ollama (local)

    // Helpers
    int totalTokens()        const;  // inputTokens + outputTokens
    int sessionTotalTokens() const;
};
```

### Cost estimation (Claude only)

Cost is resolved through the [`PricingRegistry`](#qtllmpricingregistry) across all token categories (input, output, cache read ≈ 10 % of input, cache write ≈ 125 %). The first `Client` triggers `fetchOnlinePricing()` automatically (disk cache, weekly refresh); until it returns, a hard-coded table (Anthropic list prices, 2026-11) is used. Cache reads use the model's own rate where it differs from 10 %. Inject your own prices to override either. It is always an estimate — Anthropic has no billing/pricing API; verify against the [Anthropic pricing page](https://www.anthropic.com/pricing).

---

## `QtLLM::PricingRegistry`

Process-wide model-price registry (singleton) behind all cost estimation. Every layer is optional — without configuration the behaviour equals the old hard-coded table.

Resolution order for `pricingFor(model)`:

1. **Custom resolver** (`setResolver`) — any app-defined source; return an invalid `ModelPricing` to fall through
2. **Injected entries** (`setPricing`) — per model or substring pattern
3. **Loaded table** (`loadLiteLlmJson` / `fetchOnlinePricing`) — LiteLLM community JSON
4. **Built-in fallback** — hard-coded approximations

```cpp
struct ModelPricing {
    double inputPerMTok      = 0.0;   // USD per 1M tokens
    double outputPerMTok     = 0.0;
    double cacheReadPerMTok  = -1.0;  // -1 = derive: 10% of input
    double cacheWritePerMTok = -1.0;  // -1 = derive: 125% of input
    bool   isValid() const;           // any price > 0
};

using PricingResolver = std::function<ModelPricing(const QString& model)>;
```

| Method | Description |
|---|---|
| `static instance()` | The singleton |
| `pricingFor(QString model)` | Resolved pricing; model ids are matched case-insensitively, provider prefixes (`anthropic/`) stripped, substring both directions (date-suffixed keys match short ids) |
| `estimateCostUsd(model, in, out, cacheRead = 0, cacheWrite = 0)` | USD estimate across all categories |
| `setPricing(QString pattern, ModelPricing)` | Inject/override one model's prices |
| `setResolver(PricingResolver)` | Install a custom first-priority source; `nullptr` removes |
| `clearCustomPricing()` | Drop injected + loaded entries (built-in fallback stays) |
| `loadLiteLlmJson(QJsonObject)` | Load a LiteLLM-format object (`input_cost_per_token` per single token); also usable for own files in that format. Returns models loaded |
| `fetchOnlinePricing(url = defaultLiteLlmUrl(), maxCacheAgeDays = 7)` | Async fetch of the LiteLLM community JSON. Disk cache is loaded immediately; network hit only when the cache is missing/stale. Failure keeps previous data |
| `static defaultLiteLlmUrl()` | `https://raw.githubusercontent.com/BerriAI/litellm/main/model_prices_and_context_window.json` |
| `cacheFilePath()` | On-disk cache location (`GenericDataLocation/QtLLM/litellm_pricing.json`) |

| Signal | When emitted |
|---|---|
| `pricingUpdated(int modelCount, QString source)` | Table changed; source is `"cache"`, `"network"`, `"json"` or `"manual"` |
| `fetchFailed(QString error)` | Online fetch failed — previous/fallback data stays active |

```cpp
// Typical startup:
QtLLM::PricingRegistry::instance().fetchOnlinePricing();

// Own prices (e.g. negotiated rates or an internal source):
QtLLM::PricingRegistry::instance().setPricing("claude-sonnet-4", {3.0, 15.0});
QtLLM::PricingRegistry::instance().setResolver([](const QString& model) {
    return myBackend.priceFor(model);   // invalid result -> next layer
});
```

> The LiteLLM JSON is community-maintained (not Anthropic-official) but the de-facto standard and usually updated within days of price changes.

---

## `QtLLM::UsageHistory`

Persistent per-turn usage log (JSONL file, one sample per completed turn), shared across sessions and apps. Obtained via `Client::usageHistory()`; feeds the statistics tab of `SettingsDialog`.

All instances in a process share one in-memory store per file: the file is parsed once, and a sample appended through any `Client` or `Agent` reaches every instance's `sampleAppended()`. Samples older than two years are dropped when the file is loaded.

| Member | Description |
|---|---|
| `setFilePath(QString)` | Override the default storage location |
| `append(UsageSample)` | Add a sample (done automatically by `Client`) |
| `distinctModels()` / `distinctApps()` | Filter values seen in the history |
| `reload()` / `clear()` | Re-read / wipe the backing file |
| signal `sampleAppended(UsageSample)` | Live update hook for charts |

---

## `QtLLM::OllamaManager`

Utility class for managing the Ollama server process and model inventory. Only relevant when using `Provider::Ollama`.

```cpp
QtLLM::OllamaManager manager("http://localhost:11434", parent);
```

### Methods

| Method | Description |
|---|---|
| `checkIsRunning()` | Async ping. Emits `isRunningChecked(bool)` |
| `static startServer()` | Launch `ollama serve` hidden in the background. Returns `false` if the executable cannot be found |
| `fetchLocalModels()` | Async GET `/api/tags`. Emits `localModelsReady(QList<ModelInfo>)` |
| `pullModel(QString)` | Stream-download a model via POST `/api/pull`. Emits `pullProgress` and `pullFinished` |
| `static popularModels()` | Returns a curated `QList<ModelInfo>` of 16 popular models |
| `static formatSize(qint64)` | Human-readable size string, e.g. `"4.7 GB"` |

### Signals

| Signal | Description |
|---|---|
| `isRunningChecked(bool)` | Result of `checkIsRunning()` |
| `localModelsReady(QList<ModelInfo>)` | Result of `fetchLocalModels()` |
| `pullProgress(name, status, percent)` | Incremental pull update; `percent` is -1 if total unknown |
| `pullFinished(name, bool, errorMessage)` | Pull complete or failed |

### `OllamaManager::ModelInfo`

```cpp
struct ModelInfo {
    QString name;         // e.g. "llama3.2:latest"
    QString displayName;  // e.g. "Llama 3.2 (3B)"
    qint64  sizeBytes;
    bool    installed;
};
```

### Server discovery

`startServer()` searches for `ollama.exe` in this order:
1. Qt process PATH (`QStandardPaths::findExecutable`)
2. `%LOCALAPPDATA%\Programs\Ollama\` (default installer location)
3. `HKEY_CURRENT_USER\Environment\Path` (user PATH from Windows registry)
4. `HKEY_LOCAL_MACHINE\...\Environment\Path` (system PATH from Windows registry)

---

## Streaming, effort and context

| Method | Description |
|---|---|
| `setStreamingEnabled(bool)` | Deliver the answer while it is generated. On by default; an endpoint that cannot stream is detected and used without it |
| signal `responseDelta(QString)` | A piece of answer text as it arrives. `responseReady()` still fires once with all of it |
| `setEffort(Effort)` | `Default, Low, Medium, High, XHigh, Max` — how much the model thinks before answering; the main cost lever. A model that rejects the setting is used without it |
| `setMaxToolResultChars(int)` | Cap for one serialized tool result (default 20000, 0 = unlimited). A larger result reaches the model as `{"truncated":true,"original_chars":N,"content_head":...,"hint":...}`. `Tool::setMaxResultChars()` overrides per tool; signal `toolResultTruncated()` |
| `setContextPolicy(ContextPolicy)` | `limitTokens` (0 = the model's window), `autoCompact`, `triggerRatio` (0.9), `strategy` (`Summarize` / `DropOldest`) |
| `compactConversation()` | Shrink the conversation now. `Summarize` sends one request and replaces the whole history with the returned summary; `DropOldest` removes whole turns from the front without a request. Signals `compactionStarted()`, `compactionFinished(before, after)`, `compactionFailed(message)` |

`UsageStats::contextTokens` is the provider's own count of the conversation after a turn (last request's input incl. cache, plus its output); it drives the context bar and the automatic trigger without extra requests.

With the chat widget bound to a client (`ChatDockWidget::setClient`), streaming needs no application code: deltas grow one bubble, and the usual `addAssistantMessage()` on `responseReady()` completes that same bubble.

---

## Asynchronous tools

```cpp
client.registerAsyncTool(tool, [](const QJsonObject& input, QtLLM::ToolCompletion done) {
    startLongOperation(input, [done](const QJsonObject& result) { done.finish(result); });
});
client.registerBackgroundTool(tool, [](const QJsonObject& input) {   // runs on a worker thread
    return QtLLM::toolOk({{"result", crunch(input)}});
});
```

| | |
|---|---|
| `ToolCompletion::finish(result)` | Reports the result; any thread; only the first call counts |
| `ToolCompletion::progress(text)` | Status line while working (`Client::toolProgress`) |
| `ToolCompletion::isFinished()` | True once answered or timed out |
| `Tool::setTimeoutMs(int)` | Default 5 minutes; a tool that has not answered is reported to the model as timed out |

The turn waits for every call of a round and sends the results in the order of the calls. A result that arrives after `cancelRequest()` is ignored.

---

## Image tool results

```cpp
return QtLLM::toolImage(widget->grab().toImage(), {{"what", "main window"}});
```

The image travels in the provider's own image format (Claude: image block in the tool result; subscription: MCP image content), scaled to at most 1568 px on the long edge. Ollama currently receives the JSON with an `"images": N` note only. The result cap does not count images.

---

## `QtLLM::AppInterface`

Lets the model inspect, operate and point at the host application's user interface. Everything is off until the application enables it, capability by capability; a disabled capability registers no tools.

```cpp
auto* ui = new QtLLM::AppInterface(&client, mainWindow, mainWindow);
using Cap = QtLLM::AppInterface::Capability;
ui->setCapabilities(Cap::ReadActions | Cap::ReadWidgets | Cap::Screenshots
                  | Cap::Highlights | Cap::SpeechBubbles | Cap::Tours);
```

| Capability | Tools |
|---|---|
| `ReadActions` | `ui_list_actions` |
| `TriggerActions` | `ui_trigger_action` |
| `ReadWidgets` | `ui_get_widget_tree`, `ui_find_widgets`, `ui_get_widget` |
| `ReadProperties` | `ui_get_properties` |
| `WriteProperties` | `ui_set_property` (allow-list, see `setWritableProperties`) |
| `ReadLayouts` | `ui_get_layout` |
| `ModifyLayouts` | `ui_modify_layout`, `ui_undo_layout_change` |
| `Screenshots` | `ui_screenshot` (`annotate: true` numbers the interactive widgets and returns number -> id) |
| `Highlights` | `ui_highlight_widget` |
| `Shapes` | `ui_draw_shape` (rect, roundedRect, ellipse, line, arrow, spotlight) |
| `Animations` | adds the `animation` parameter to the two above (blink, fade, pulse, colorLoop, marchingAnts, drawOn) |
| `SpeechBubbles` | `ui_show_bubble` |
| `Tours` | `ui_show_tour` (all at once, or one at a time with Back / Next) |
| `WidgetColors` | `ui_set_widget_colors` (background, text, border of one widget), `ui_restore_widget_colors` (one widget, or all) |
| `Themes` | `ui_list_themes`, `ui_set_theme` - only among the themes the application registered |

Shared helper tools appear with the capabilities that need them: `ui_remove_overlay`, `ui_clear_overlays`, `ui_close_bubble`, `ui_clear_bubbles`, `ui_list_guidance`.

| Method | Description |
|---|---|
| `setCapabilityEnabled(cap, bool)` / `setCapabilities(flags)` | The switches |
| `addRoot(QWidget*)` | Further top-level windows the model may see |
| `excludeWidget(QWidget*)` | Subtree hidden from every tool. The chat dock is excluded by default |
| `setWritableProperties(QStringList)` | Properties `ui_set_property` may write (default: text, value, checked, currentIndex, currentText, enabled, visible, toolTip) |
| `resetLayoutChanges()` | Undo every layout modification |
| `clearGuidance()` | Remove all highlights, shapes, bubbles and tours |
| `setAnimationsEnabled(bool)` | Off: overlays are shown static (reduced motion) |
| `resetWidgetColors()` | Give every widget recoloured by the model its original colours back |
| `registerTheme(name, description, apply)` | A theme the model may select; `apply` does whatever switching means in the application |
| `registerTheme(name, description, QPalette, styleName = {}, styleSheet = {})` | The common case: application palette, optionally a `QStyle` ("Fusion") and an application style sheet |
| `registerStandardThemes()` | Registers `default` (the look at the time of the call) and `dark` (Fusion with `AppInterface::darkPalette()`) |
| `setTheme(name)` / `currentTheme()` / `themeNames()`, signal `themeChanged(name)` | The same switch from C++, e.g. for a menu entry that stays in sync |

Widgets are addressed by id: the path of object names (`mainWindow/saveButton`), with `ClassName#index` for unnamed widgets. Name the widgets the assistant should find reliably. Password fields are redacted in every read tool and blacked out in screenshots. Read tools and guidance tools are marked `Tool::isReadOnly()`, so a consent handler can ask only for the calls that change something.

Widget colours are applied through a style rule that selects the one widget, so the widgets inside it keep their look; only values that parse as a colour are accepted. The original style sheet is kept and put back on restore. Themes are a closed list: the model cannot define colours for the whole application, it can only pick a registered name.

The drawing classes are usable directly: `GuideOverlay::forWindow(w)->highlight(widget)`, `BubbleManager::forWindow(w)->showBubble(widget, text)`.

---

## Error handling

The library never throws. All errors surface through the `errorOccurred(QString)` signal.

| Situation | Behaviour |
|---|---|
| Network error | Emits `errorOccurred` with Qt error string; `requestFinished` also fires |
| HTTP 4xx / 5xx | Parses JSON error body and emits `errorOccurred` with the API message |
| HTTP 429 / 5xx / `overloaded_error` | Retried twice with backoff (2 s, 8 s) before `errorOccurred` |
| Unknown tool | The LLM gets an error result naming the available tools and recovers; no `errorOccurred`, the turn continues |
| Response cut off (`max_tokens`) | `responseReady` with a `[Response truncated ...]` note; a half-written tool call is dropped from the history |
| Model declines (`refusal`) | Emits `errorOccurred`; the prompt is removed from the history |
| Tool loop exceeds 25 rounds | `responseReady` with a `[Stopped: tool-use iteration limit ...]` note |
| Model list unavailable | `modelsAvailable({})`; logged, no `errorOccurred` |
| Malformed JSON | Emits `errorOccurred`, does not crash |
| Tool handler throws | Caught by the protocol layer; the model receives `{"status":"error","message":"Internal error: <what>"}` with `is_error` — the process never crashes |
