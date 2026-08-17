> **Status: historical.** This is the original pre-implementation design proposal
> for v1.0.0, kept for context on why the library is shaped the way it is.
>
> **It does not describe the current API.** The shipped `QtLLM::Client` has since
> gained a `Provider` overload, a `registerTool(const Tool&, ToolHandler)` form,
> tool gating (`setToolEnabled`, `setValidateToolInput`, `setMaxToolCallsPerTurn`,
> `setToolConsentHandler`), plus Ollama support, pricing, usage history, and the
> chat widgets — none of which appear below.
>
> For the real API see `core/inc/` and `documentation/API.md`.

# QtLLM — C++ Qt5 Library for LLM Integration

## Project Overview

**QtLLM** is a C++ library built on **Qt5** that provides a clean, asynchronous interface for connecting C++ applications to Large Language Model (LLM) APIs such as **Anthropic Claude**, **OpenAI**, or compatible endpoints.

The library's primary feature is **tool/function calling**: the host application registers C++ functions (with descriptions and parameter schemas), and the LLM can invoke these functions in response to user prompts. This enables building chat-driven interfaces that control application behavior.

## Goals for v1.0.0

- Provide a simple, Qt-idiomatic API for sending prompts to an LLM and receiving responses.
- Support **tool use / function calling**, where the LLM can call registered C++ functions.
- Be **fully asynchronous** using Qt's signal/slot mechanism — never block the UI thread.
- Use **only Qt5 modules** (no libcurl, no nlohmann/json) to minimize dependencies.
- Be **provider-agnostic** in design, with an initial implementation targeting the Claude (Anthropic) Messages API.
- Be usable as both a **static** and **shared** library.


## Secondary Goals for v2.0.0
- Local model inference (llama.cpp integration).


---

## Public API Design

### Namespace
All public classes live in the `QtLLM` namespace.

### Main Classes

#### `QtLLM::Client`

The primary class. Inherits from `QObject`. Manages the HTTP connection, conversation history, and tool registry.
This is only an example, maybe you design a better architecture.

**Constructor:**
```cpp
explicit Client(const QString& apiKey,
                const QString& url = "https://api.anthropic.com/v1/messages",
                QObject* parent = nullptr);
```

**Public methods:**
```cpp
void setModel(const QString& modelName);   // e.g. "claude-sonnet-4-5"
void setMaxTokens(int maxTokens);
void setSystemPrompt(const QString& prompt);

void registerTool(const QString& name,
                  const QString& description,
                  const QJsonObject& parameterSchema,
                  std::function<QJsonObject(const QJsonObject&)> handler);

void unregisterTool(const QString& name);

void sendPrompt(const QString& userMessage);
void clearConversation();

QJsonArray conversationHistory() const;
```

**Signals:**
```cpp
void responseReady(const QString& assistantText);
void toolInvoked(const QString& toolName, const QJsonObject& arguments);
void toolCompleted(const QString& toolName, const QJsonObject& result);
void errorOccurred(const QString& message);
void requestStarted();
void requestFinished();
```

#### `QtLLM::Tool` (optional helper struct)

A convenience struct for defining tools with builder-style construction:
```cpp
Tool tool;
tool.setName("functionA")
    .setDescription("...")
    .addParameter("value", "number", "Input value", /*required=*/true);
client.registerTool(tool, handler);
```

## Behavioral Requirements

### Tool Use Loop

When the API returns `stop_reason == "tool_use"`, the library must:

1. Append the assistant's response (containing tool_use blocks) to the conversation history.
2. For each `tool_use` content block:
   - Look up the registered handler by name.
   - Emit `toolInvoked(name, args)` signal.
   - Call the handler synchronously and capture the result.
   - Emit `toolCompleted(name, result)` signal.
3. Append a `user` message containing all `tool_result` blocks to the conversation.
4. Re-issue the request with the updated conversation.
5. Repeat until `stop_reason` is `"end_turn"` or another terminal reason.
6. Emit `responseReady` with the final assembled text.

### Error Handling

- Network errors → `errorOccurred(QString)` with descriptive message.
- HTTP 4xx/5xx responses → parse the JSON error body if present, emit `errorOccurred`.
- Unknown tool name → emit `errorOccurred` and send a tool_result with an error payload back to the LLM so it can recover.
- Malformed JSON → emit `errorOccurred`, do not crash.

### Conversation Management

- The `Client` maintains an internal `QJsonArray` of messages (the conversation).
- `sendPrompt()` appends the user message before sending.
- The full history is sent with every request.
- `clearConversation()` resets the history but preserves registered tools and settings.
- An optional system prompt is sent as the top-level `system` field per Claude API.

### Threading

- All public methods must be called from the thread that owns the `Client` (typically the GUI thread).
- Network I/O happens via `QNetworkAccessManager`, which integrates with the event loop.
- Tool handlers run synchronously on the calling thread. Long-running tool work should be offloaded by the host application.

---

## Project Structure

```
QtLLM/
├── CMakeLists.txt
├── README.md
├── LICENSE
├── SPEC.md                      (this file)
├── include/
│   └── QtLLM/
│       ├── Client.h
│       ├── Tool.h
│       └── QtLLM.h              (umbrella header)
├── src/
│   ├── Client.cpp
│   ├── Tool.cpp
│   └── internal/
│       ├── HttpTransport.h
│       ├── HttpTransport.cpp
│       ├── ClaudeProtocol.h     (Claude-specific request/response handling)
│       └── ClaudeProtocol.cpp
├── tests/
│   ├── CMakeLists.txt
│   ├── tst_client.cpp
│   ├── tst_tool.cpp
│   └── tst_protocol.cpp
└── examples/
    ├── CMakeLists.txt
    ├── simple_chat/
    │   ├── main.cpp
    │   └── ChatWindow.{h,cpp,ui}
    └── tool_demo/
        └── main.cpp
```

---

## Build Requirements

### CMake

- Minimum CMake version: **3.16**
- Provide a `QtLLMConfig.cmake` so consumers can do:
  ```cmake
  find_package(QtLLM REQUIRED)
  target_link_libraries(myapp PRIVATE QtLLM::QtLLM)
  ```
- Support both `BUILD_SHARED_LIBS=ON` and `OFF`.
- Options:
  - `QTLLM_BUILD_TESTS` (default ON)
  - `QTLLM_BUILD_EXAMPLES` (default ON)

### Qt Modules

Required: `Core`, `Network`
For tests: `Test`
For examples: `Widgets`

---

## API Usage Example

The library should support this exact usage pattern:

```cpp
#include <QtLLM/Client.h>
#include <QApplication>

int main(int argc, char** argv) {
    QApplication app(argc, argv);

    QtLLM::Client client(qgetenv("ANTHROPIC_API_KEY"));
    client.setModel("claude-sonnet-4-5");
    client.setSystemPrompt("You are a helpful assistant controlling a CAD app.");

    // Register a function the LLM can call
    client.registerTool(
        "createCircle",
        "Creates a circle on the canvas at the given coordinates.",
        QJsonObject{
            {"type", "object"},
            {"properties", QJsonObject{
                {"x",      QJsonObject{{"type", "number"}}},
                {"y",      QJsonObject{{"type", "number"}}},
                {"radius", QJsonObject{{"type", "number"}}}
            }},
            {"required", QJsonArray{"x", "y", "radius"}}
        },
        [](const QJsonObject& args) -> QJsonObject {
            double x = args.value("x").toDouble();
            double y = args.value("y").toDouble();
            double r = args.value("radius").toDouble();
            // ... actual application logic ...
            return QJsonObject{{"status", "ok"}, {"id", 42}};
        }
    );

    QObject::connect(&client, &QtLLM::Client::responseReady,
                     [](const QString& text) {
        qDebug() << "Assistant:" << text;
    });

    QObject::connect(&client, &QtLLM::Client::toolInvoked,
                     [](const QString& name, const QJsonObject& args) {
        qDebug() << "Tool called:" << name << args;
    });

    QObject::connect(&client, &QtLLM::Client::errorOccurred,
                     [](const QString& err) {
        qDebug() << "Error:" << err;
    });

    client.sendPrompt("Draw a circle at (100, 200) with radius 50.");

    return app.exec();
}
```

---

## Testing Requirements

- Unit tests for `Tool` schema generation.
- Unit tests for the conversation builder (request payload format).
- Unit tests for the response parser (handling `text` and `tool_use` blocks).
- Mock-based tests for the tool-use loop (no real network calls).
- Optional integration test gated behind an env var `QTLLM_RUN_INTEGRATION_TESTS=1` that hits the real API.





