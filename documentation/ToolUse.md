# Tool Use / Function Calling

QtLLM handles the entire multi-turn tool-calling loop automatically. You register C++ lambdas, and the library takes care of encoding, invoking, and feeding results back to the model.

---

## How it works

When the LLM decides to call one or more tools, the library:

1. Appends the assistant's tool-call response to conversation history
2. Calls each registered handler synchronously and captures the return value
3. Appends the tool results to history
4. Re-sends the full conversation to the API
5. Repeats steps 1–4 until the model produces a final text response
6. Emits `responseReady` with the assembled text

This loop is transparent — from the caller's perspective only `sendPrompt` and `responseReady` matter.

---

## Registering a tool

### Using the `Tool` builder (recommended)

```cpp
QtLLM::Tool tool;
tool.setName("createCircle")
    .setDescription("Creates a circle on the canvas at the given coordinates.")
    .addParameter("x",      "number", "X coordinate of the centre.", /*required=*/true)
    .addParameter("y",      "number", "Y coordinate of the centre.", /*required=*/true)
    .addParameter("radius", "number", "Radius of the circle.",       /*required=*/true)
    .addParameter("color",  "string", "Optional CSS color string.",  /*required=*/false);

client.registerTool(tool, [](const QJsonObject& args) -> QJsonObject {
    double x = args["x"].toDouble();
    double y = args["y"].toDouble();
    double r = args["radius"].toDouble();
    QString color = args.value("color").toString("black");

    // ... application logic ...

    return QJsonObject{{"status", "ok"}, {"id", 42}};
});
```

### Using a raw JSON schema

```cpp
client.registerTool(
    "getWeather",
    "Returns the current temperature for a city.",
    QJsonObject{
        {"type", "object"},
        {"properties", QJsonObject{
            {"city", QJsonObject{{"type", "string"}, {"description", "City name"}}},
        }},
        {"required", QJsonArray{"city"}}
    },
    [](const QJsonObject& args) -> QJsonObject {
        QString city = args["city"].toString();
        // ... fetch weather ...
        return QJsonObject{{"city", city}, {"celsius", 21}};
    }
);
```

The raw schema overload is useful when you already have a JSON Schema object from another source.

### Enum parameters

Parameters with a fixed value set no longer require the raw-schema overload:

```cpp
QtLLM::Tool tool;
tool.setName("createObject")
    .setDescription("Creates an object of the given type.")
    .addEnumParameter("type", {"SwOption", "Property", "AssignmentTable"},
                      "Object type to create.", /*required=*/true)
    // or the general form with an explicit type:
    .addParameter("mode", "string", "Creation mode.", false, {"fast", "safe"});
```

The `enum` array is emitted in both the Claude and the OpenAI/Ollama schema.

---

## Built-in tools

The library ships a set of predefined tools. **None are registered automatically** — the app opts in per tool via the `BuiltinTool` enum:

```cpp
#include "BuiltinTools.h"

QtLLM::BuiltinTools::registerTools(&client,
    { QtLLM::BuiltinTool::AskUserQuestion,     // interview card in the chat
      QtLLM::BuiltinTool::FileDialog,
      QtLLM::BuiltinTool::CurrentDateTime },
    { /*chat*/ chatDock, /*dialogParent*/ mainWindow });
```

| Enum value | Tool name | What it does |
|---|---|---|
| `AskUserQuestion` | `ask_user_question` | Interactive question form in the chat (see [InterviewTool](#)) — needs `Context::chat` |
| `FileDialog` | `file_dialog` | Native open/save/folder dialog; `mode`, `title`, `startPath`, `filter` |
| `MessageBox` | `show_message` | Info/warning box or yes-no question |
| `ColorPicker` | `pick_color` | `QColorDialog`; returns hex + RGB. The chat renders hex codes (`#rrggbb`) in assistant text as a colored rectangle |
| `ClipboardRead` | `clipboard_read` | Read clipboard text (may expose sensitive data — see below) |
| `ClipboardWrite` | `clipboard_write` | Place text on the clipboard |
| `CurrentDateTime` | `current_datetime` | Local + UTC time, timezone, weekday |
| `SystemInfo` | `system_info` | OS, Qt version, locale, app name, screen size |
| `OpenUrl` | `open_url` | Open URL in the default browser |
| `ListDirectory` | `list_directory` | Directory listing (max 500 entries) — consent-gated |
| `ReadTextFile` | `read_text_file` | Read UTF-8 text file, size-capped (default 64 KiB) — consent-gated |
| `WriteTextFile` | `write_text_file` | Write/append UTF-8 text file — consent-gated |
| `RepeatingTask` | `start_repeating_task`, `cancel_repeating_task`, `list_repeating_tasks` | Periodic message into the chat/status line — local only, **no tokens** |

Notes:

- `Context::dialogParent` parents the modal dialogs (`FileDialog`, `MessageBox`, `ColorPicker`); `Context::chat` is only needed for `AskUserQuestion`.
- Every built-in tool carries `title`/`group`/`statusText` metadata, so it integrates with settings UIs (`registeredTools()`) and the `ChatDockWidget` status line out of the box.
- Built-in tools are ordinary registered tools afterwards: `setToolEnabled`, input validation, the consent hook and the per-turn call cap all apply. `BuiltinTools::toolName(...)` gives the string name for those APIs.
- **Filesystem consent (built-in, default on):** every `ListDirectory`/`ReadTextFile` call asks the user for read consent, every `WriteTextFile` call for write consent, showing the affected path. The dialog offers *"Allow for the rest of this session"* — accepted once with that box checked, further calls of the same access kind (read or write) run silently until the process exits. Declining sends `{"status":"error","message":"user declined"}` to the model. Disable with `Context::confirmFilesystemAccess = false` when the app gates access itself (e.g. via `Client::setToolConsentHandler`).
- **Recommendation:** gate `OpenUrl` and `ClipboardRead` with the [consent hook](#consent-hook) — they touch data outside the chat.
- User cancellation (file dialog, color picker) returns `{"status":"cancelled", ...}` — deliberately *not* an error, so the model treats it as a decision, not a failure.
- **Repeating tasks:** `RepeatingTask` is one enum value but registers **three** tools. The model starts a timer (`intervalMs`, `count` — 0 = until canceled) whose message is rendered each tick with `{i}`/`{count}`/`{id}` placeholders, into the status line (`target: "status"`, overwritten per tick) or as chat messages (`target: "chat"`). Ticks are pure UI updates via the bound `ChatDockWidget` — **no API request, no token cost**. Only `notifyWhenDone: true` triggers a model turn (and tokens) when the task finishes. Needs `Context::chat`.
- Register all desired tools before the first `sendPrompt` — each later change to the tool list invalidates the prompt-cache prefix.

---

## Tool handler rules

- Handlers are called **synchronously** on the Qt event loop thread. This is a guarantee — a handler may open a modal dialog or (like `InterviewTool`) spin a local event loop.
- The return value must be a `QJsonObject`. It is serialised and sent back to the LLM as the tool result.
- Long-running work (file I/O, network calls, database queries) should be offloaded and the result delivered asynchronously; keep the handler itself non-blocking.
- A handler that throws does **not** crash the process: the exception is caught by the protocol layer and converted to `{"status":"error","message":"Internal error: <what>"}` with `is_error` set; the conversation continues.

## Tool result convention

A result object with `"status": "error"` is sent to the model as a `tool_result` with `is_error: true` — the model treats it as a failed call and can self-correct. Everything else counts as success. `ToolResult.h` provides helpers:

```cpp
#include "ToolResult.h"

return QtLLM::toolOk({{"id", 42}});                       // {"status":"ok","id":42}
return QtLLM::toolError("not found", {{"id", askedId}});  // {"status":"error","message":"not found","id":…}
```

---

## Input validation

Opt-in schema validation before the handler runs (default **off** — behaviour is unchanged unless enabled):

```cpp
client.setValidateToolInput(true);
```

When enabled, the client checks required fields, JSON types (`string`, `integer`, `number`, `boolean`, `array`, `object`) and `enum` values (case-insensitively — models often change the casing) against the registered schema. On violation the handler is **not** called; the model directly receives an actionable error:

```json
{"status": "error", "message": "missing required parameter: id",
 "expected": {"id": "integer", "fields": "object"}}
{"status": "error", "message": "invalid value for 'type'",
 "allowed_values": ["SwOption", "Property", "AssignmentTable"]}
```

`Tool::validate(args)` / `Tool::validateAgainstSchema(schema, args)` expose the same check for manual use.

---

## Enabling and disabling tools

Tools can be hidden from the model without losing their definition or handler — no register/unregister ping-pong:

```cpp
client.setToolEnabled("deleteObject", false);   // removed from the next request's schema
client.setToolEnabled("deleteObject", true);    // back, unchanged
client.isToolEnabled("deleteObject");
client.toolNames();          // all registered
client.enabledToolNames();   // currently advertised to the model
```

A disabled tool that the model calls anyway (stale/cached tool list) receives `{"status":"error","message":"tool is disabled"}` — more actionable for the model than "unknown tool".

## Consent hook

Ask the user before a tool executes (accept/decline pattern, like Claude Code permissions):

```cpp
client.setToolConsentHandler([](const QString& name, const QJsonObject& input) -> bool {
    if (name != "deleteObject")
        return true;  // everything else runs unasked
    return QMessageBox::question(nullptr, "Allow tool?", name)
           == QMessageBox::Yes;
});
```

Called synchronously on the GUI thread before each handler, so modal dialogs work. Returning `false` skips the handler and the model receives `{"status":"error","message":"user declined"}`. No handler set (default) = everything allowed.

## Tool call cap per turn

Guard against tool-calling loops:

```cpp
client.setMaxToolCallsPerTurn(40);   // 0 = unlimited (default)
connect(&client, &QtLLM::Client::toolCallLimitReached,
        [](int limit) { qWarning() << "tool call cap hit:" << limit; });
```

Once the cap is reached, further handlers in the same turn are not executed; the model receives `{"status":"error","message":"tool call limit reached"}` and `toolCallLimitReached` is emitted once. The counter resets on every new user turn.

---

## Tool metadata for settings UIs

`Tool` carries optional UI-only fields that are never sent to the model:

```cpp
tool.setTitle("Delete object")            // display name for settings dialogs
    .setGroup("Mutation")                 // grouping for UI lists
    .setStatusText("Deleting object…");   // shown while the tool executes

client.registeredTools();                 // QList<Tool> — basis for a settings widget
```

If a `ChatDockWidget` is bound via `setClient()`, a tool's `statusText` is shown in the chat status line automatically while the tool runs.

The `SettingsDialog` has a ready-made **Tools** tab: bind the client with `settingsDialog.setClient(&client)` and it lists every registered tool grouped by `group()`, showing title, tool name and description, with a checkbox reflecting the enabled state. Toggles are collected and applied in **one batch** on Apply (one `setToolEnabled` sweep — a single prompt-cache invalidation instead of one per click).

---

## Unregistering a tool

```cpp
client.unregisterTool("createCircle");
```

The change takes effect on the next `sendPrompt` call.

---

## Observing tool invocations

```cpp
QObject::connect(&client, &QtLLM::Client::toolInvoked,
    [](const QString& name, const QJsonObject& input) {
        qDebug() << "Tool called:" << name << input;
    });

QObject::connect(&client, &QtLLM::Client::toolCompleted,
    [](const QString& name, const QJsonObject& result) {
        qDebug() << "Tool result:" << name << result;
    });
```

---

## Provider differences

The library internally translates tool schemas between formats:

| | Schema format | Arguments key |
|---|---|---|
| Claude | `"input_schema"` | `input` (object) |
| Ollama | `"function"` → `"parameters"` | `function.arguments` (object or JSON string) |

This is handled automatically — register tools once using `Tool` or a raw schema, and the correct format is sent to each provider.

> Tool calling requires a model that supports it.
> For Ollama, check the model page on [ollama.com](https://ollama.com/library) for capability details.
> Tested working models include `llama3.2`, `mistral-nemo`, `qwen2.5`.

---

## Prompt-cache interaction

For Claude, `tools` and `system` live in the cached request prefix (`cache_control: ephemeral`). **Any change to the advertised tool list — `registerTool`, `unregisterTool`, or `setToolEnabled` — invalidates that prefix**, and the next request pays the full input-token price again. Batch availability changes (e.g. apply a whole settings-dialog result at once) instead of toggling tools one by one between turns.

## Changing the system prompt mid-conversation

`setSystemPrompt()` may be called at any time; it does **not** reset the conversation history and takes effect from the next request. This is guaranteed behaviour — apps can rely on it for dynamic prompt sections (e.g. a "currently disabled tools" section). Note that it also invalidates the prompt-cache prefix, same as tool-list changes.
