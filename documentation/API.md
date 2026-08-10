# API Reference

All public types live in the `QtLLM` namespace. Include the umbrella header:

```cpp
#include <QtLLM.h>
```

Contents: [Provider](#qtllmprovider) · [Client](#qtllmclient) · [Tool](#qtllmtool) · [ToolResult helpers](#tool-result-helpers) · [BuiltinTools](#qtllmbuiltintools) · [InterviewTool](#qtllminterviewtool) · [InterviewWidget](#qtllminterviewwidget) · [ChatDockWidget](#qtllmchatdockwidget) · [SettingsDialog](#qtllmsettingsdialog) · [UsageStats](#qtllmusagestats) · [UsageHistory](#qtllmusagehistory) · [OllamaManager](#qtllmollamamanager) · [Error handling](#error-handling)

---

## `QtLLM::Provider`

Selects the LLM backend at construction time.

```cpp
enum class Provider { Claude, Ollama };
```

| Value | Backend |
|---|---|
| `Provider::Claude` | Anthropic Claude Messages API |
| `Provider::Ollama` | Ollama local inference server |

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
| `setModel(QString)` | Model identifier. Provider-specific (e.g. `"claude-sonnet-4-5"` or `"llama3.2"`) |
| `setMaxTokens(int)` | Maximum tokens in the response. Maps to `num_predict` for Ollama |
| `setSystemPrompt(QString)` | Prepended to every request. Omitted when empty. May be changed mid-conversation — history is kept, takes effect from the next request (invalidates the prompt-cache prefix) |

### Tool registration

| Method | Description |
|---|---|
| `registerTool(Tool, ToolHandler)` | Register using the `Tool` builder |
| `registerTool(name, description, schema, ToolHandler)` | Register with a raw JSON schema object |
| `unregisterTool(QString)` | Remove a registered tool by name |

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
| `clearConversation()` | Reset history and session statistics. Tools and settings are kept |
| `conversationHistory()` | `QJsonArray` of `{role, content}` messages |
| `exportConversation()` | Rich JSON export: raw messages (incl. `tool_use`/`tool_result` blocks), usage statistics, per-turn timeline |

### Statistics & models

| Method | Description |
|---|---|
| `usageStats()` | `UsageStats` snapshot from the most recently completed turn |
| `usageHistory()` | Pointer to the persistent per-turn [`UsageHistory`](#qtllmusagehistory) (JSONL-backed) |
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
| `toolCallLimitReached(int limit)` | Once per turn when the tool-call cap is exceeded |

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
| `messageSent(QString)` | User pressed Send |
| `cancelRequested()` | User pressed Cancel |
| `settingsRequested()` | User clicked the settings button |
| `saveConversationRequested()` | Save clicked and **no** client bound (fallback for custom handling) |
| `interviewFinished(QJsonObject result)` | An interview card was submitted or skipped |

---

## `QtLLM::SettingsDialog`

Ready-made settings dialog (`QDialog`) with three tabs: **Settings** (provider, API key, endpoint, model incl. auto-detection, system prompt, font size), **Tools**, **Statistik** (usage charts).

| Method | Description |
|---|---|
| `provider()` / `setProvider(Provider)` | `SettingsDialog::Provider { Claude, Ollama }` |
| `apiKey()` / `setApiKey`, `model()` / `setModel`, `endpointUrl()` / `setEndpointUrl`, `ollamaUrl()` / `setOllamaUrl`, `systemPrompt()` / `setSystemPrompt`, `fontSizePercent()` / `setFontSizePercent` | Field access |
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

Cost is estimated from the model name at runtime using approximate mid-2025 pricing:

| Model family | Input (per 1M) | Output (per 1M) |
|---|---|---|
| claude-opus-4 | $15.00 | $75.00 |
| claude-sonnet-4 | $3.00 | $15.00 |
| claude-haiku-4 | $0.80 | $4.00 |
| claude-opus-3 | $15.00 | $75.00 |
| claude-sonnet-3 | $3.00 | $15.00 |
| claude-haiku-3 | $0.25 | $1.25 |

Prices are hard-coded approximations. Always verify against the [Anthropic pricing page](https://www.anthropic.com/pricing).

---

## `QtLLM::UsageHistory`

Persistent per-turn usage log (JSONL file, one sample per completed turn), shared across sessions and apps. Obtained via `Client::usageHistory()`; feeds the statistics tab of `SettingsDialog`.

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

## Error handling

The library never throws. All errors surface through the `errorOccurred(QString)` signal.

| Situation | Behaviour |
|---|---|
| Network error | Emits `errorOccurred` with Qt error string; `requestFinished` also fires |
| HTTP 4xx / 5xx | Parses JSON error body and emits `errorOccurred` with the API message |
| Unknown tool | Emits `errorOccurred` and sends an error result to the LLM so it can recover |
| Malformed JSON | Emits `errorOccurred`, does not crash |
| Tool handler throws | Caught by the protocol layer; the model receives `{"status":"error","message":"Internal error: <what>"}` with `is_error` — the process never crashes |
