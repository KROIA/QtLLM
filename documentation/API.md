# API Reference

All public types live in the `QtLLM` namespace. Include the umbrella header:

```cpp
#include <QtLLM.h>
```

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

// Explicit provider selection
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
| `setSystemPrompt(QString)` | Prepended to every request. Omitted when empty |

### Tool Registration

| Method | Description |
|---|---|
| `registerTool(Tool, ToolHandler)` | Register using the `Tool` builder |
| `registerTool(name, description, schema, ToolHandler)` | Register with a raw JSON schema object |
| `unregisterTool(QString)` | Remove a registered tool by name |

`ToolHandler` = `std::function<QJsonObject(const QJsonObject&)>`

Handlers are called synchronously on the Qt thread. The return value is sent back to the LLM as the tool result.

### Conversation

| Method | Description |
|---|---|
| `sendPrompt(QString)` | Append user message and send the full conversation to the API |
| `clearConversation()` | Reset history and session statistics. Tools and settings are kept |
| `conversationHistory()` | Returns `QJsonArray` of `{role, content}` messages |

### Statistics

| Method | Description |
|---|---|
| `usageStats()` | Returns a `UsageStats` snapshot from the most recently completed turn |

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

---

## `QtLLM::Tool`

Builder for LLM tool schemas. Not a `QObject` — safe to copy and store by value.

```cpp
QtLLM::Tool tool;
tool.setName("createCircle")
    .setDescription("Creates a circle at the given position.")
    .addParameter("x",      "number", "X coordinate.", true)
    .addParameter("y",      "number", "Y coordinate.", true)
    .addParameter("radius", "number", "Radius.",       true)
    .addParameter("label",  "string", "Optional label.", false);
```

### Methods

| Method | Returns | Description |
|---|---|---|
| `setName(QString)` | `Tool&` | Tool name used by the LLM to invoke it |
| `setDescription(QString)` | `Tool&` | Natural-language description for the LLM |
| `addParameter(name, type, description, required)` | `Tool&` | JSON Schema primitive type: `"string"` `"number"` `"integer"` `"boolean"` `"array"` `"object"` |
| `name()` | `QString` | Returns the tool name |
| `description()` | `QString` | Returns the description |
| `toApiObject()` | `QJsonObject` | Claude wire format — uses `"input_schema"` key |
| `toOpenAiApiObject()` | `QJsonObject` | OpenAI/Ollama wire format — wraps parameters under `"function"` → `"parameters"` |

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
