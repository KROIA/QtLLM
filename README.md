# QtLLM

A C++ Qt5 library that provides a clean, asynchronous interface for connecting desktop applications to Large Language Model (LLM) APIs such as Anthropic Claude.

The library's primary feature is **tool/function calling**: your application registers C++ functions with descriptions and parameter schemas, and the LLM can invoke those functions in response to user prompts. This makes it straightforward to build chat-driven interfaces that control application behaviour.

---

## Features

- Simple Qt-idiomatic API — one header to include, one class to instantiate
- **Tool use / function calling** — register any C++ lambda as a callable tool for the LLM
- Fully **asynchronous** via Qt signals and slots — never blocks the UI thread
- Uses only **Qt5 modules** (Core, Network) — no libcurl, no third-party JSON library
- Provider-agnostic design — supports **Anthropic Claude** and **Ollama** (local inference)
- Builds as both a **shared** and **static** library

---

## Requirements

| Dependency | Version | Notes |
|---|---|---|
| Qt | 5.15.x | Core, Network (+ Widgets for examples) |
| OpenSSL | 1.1.1x | Runtime DLLs `libssl-1_1-x64.dll` / `libcrypto-1_1-x64.dll` |
| CMake | 3.20+ | |
| MSVC | 2019 / 2022 | Windows x64 |

> **OpenSSL note:** Qt 5.15.x requires OpenSSL **1.1.x** specifically. OpenSSL 3.x is not compatible.  
> Install *Win64 OpenSSL v1.1.1w Light* from Shining Light Productions. After installing, delete the CMake cache and reconfigure — the build system will locate and deploy the DLLs automatically.

---

## Building

```bash
# Configure (Ninja, debug)
cmake --preset x64-Debug

# Build
cmake --build --preset x64-Debug

# Or use Visual Studio 2022: open the folder, select a configuration, Build → Install QtLLM
```

The `build.bat` convenience script builds both Debug and Release and installs to `installation/`.

---

## Quick Start

```cpp
#include <QCoreApplication>
#include <QtLLM.h>

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);

    QtLLM::Client client(
        qgetenv("ANTHROPIC_API_KEY"),
        "https://api.anthropic.com/v1/messages"
    );

    client.setModel("claude-sonnet-4-5");
    client.setSystemPrompt("You are a helpful assistant.");

    QObject::connect(&client, &QtLLM::Client::responseReady, [](const QString& text) {
        qDebug() << "Assistant:" << text;
        qApp->quit();
    });

    QObject::connect(&client, &QtLLM::Client::errorOccurred, [](const QString& err) {
        qDebug() << "Error:" << err;
        qApp->quit();
    });

    client.sendPrompt("Hello! What can you do?");
    return app.exec();
}
```

---

## Ollama (Local LLM)

To connect to a locally running [Ollama](https://ollama.com) server instead of Claude, pass the `Provider::Ollama` enum value to the second constructor:

```cpp
#include <QtLLM.h>

QtLLM::Client client(
    QtLLM::Provider::Ollama,
    "http://localhost:11434/api/chat",
    ""   // no API key needed for local Ollama
);

client.setModel("llama3.2");
client.setSystemPrompt("You are a helpful assistant.");

QObject::connect(&client, &QtLLM::Client::responseReady, [](const QString& text) {
    qDebug() << "Assistant:" << text;
});

client.sendPrompt("Hello!");
```

Tool use works identically for Ollama — register tools the same way and the library picks the correct schema format automatically.

> **Note:** Tool calling requires a model that supports it (e.g. `llama3.2`, `mistral-nemo`). Check the Ollama model page for capability details.

---

## Tool Use (Function Calling)

Register C++ functions that the LLM can call during a conversation. The library handles the full multi-turn tool-use loop automatically.

```cpp
// Register a tool using the Tool builder
QtLLM::Tool tool;
tool.setName("createCircle")
    .setDescription("Creates a circle on the canvas at the given coordinates.")
    .addParameter("x",      "number", "X coordinate of the centre.", true)
    .addParameter("y",      "number", "Y coordinate of the centre.", true)
    .addParameter("radius", "number", "Radius of the circle.",       true);

client.registerTool(tool, [](const QJsonObject& args) -> QJsonObject {
    double x = args["x"].toDouble();
    double y = args["y"].toDouble();
    double r = args["radius"].toDouble();
    // ... your application logic ...
    return QJsonObject{{"status", "ok"}, {"id", 42}};
});

// Or register using a raw JSON schema directly
client.registerTool(
    "getTemperature",
    "Returns the current temperature for a city.",
    QJsonObject{
        {"type", "object"},
        {"properties", QJsonObject{
            {"city", QJsonObject{{"type", "string"}}}
        }},
        {"required", QJsonArray{"city"}}
    },
    [](const QJsonObject& args) -> QJsonObject {
        return QJsonObject{{"city", args["city"].toString()}, {"celsius", 21}};
    }
);

client.sendPrompt("Draw a circle at (100, 200) with radius 50, then tell me the weather in Berlin.");
```

When the LLM decides to call a tool, the library:

1. Appends the assistant's response to the conversation history
2. Calls the registered handler synchronously and captures the result
3. Appends the tool result to the history
4. Re-issues the request automatically
5. Repeats until the LLM produces a final text response
6. Emits `responseReady` with the assembled text

> **Note:** Tool handlers are called synchronously on the Qt event loop thread. Long-running work should be offloaded by the host application.

---

## API Reference

### `QtLLM::Client`

The primary class. Inherits `QObject`.

```cpp
// Claude (default endpoint)
explicit Client(const QString& apiKey,
                const QString& url = "https://api.anthropic.com/v1/messages",
                QObject* parent = nullptr);

// Explicit provider selection (Ollama or Claude)
explicit Client(Provider provider,
                const QString& url,
                const QString& apiKey = QString(),
                QObject* parent = nullptr);
```

#### `QtLLM::Provider`

| Value | Description |
|---|---|
| `Provider::Claude` | Anthropic Claude Messages API (default) |
| `Provider::Ollama` | Ollama local inference server (`/api/chat`) |

#### Configuration

| Method | Description |
|---|---|
| `setModel(QString)` | Model name, e.g. `"claude-sonnet-4-5"`. Default: `"claude-opus-4-5"` |
| `setMaxTokens(int)` | Maximum tokens in the response. Default: `1024` |
| `setSystemPrompt(QString)` | System prompt sent with every request. Omitted when empty |

#### Tools

| Method | Description |
|---|---|
| `registerTool(Tool, ToolHandler)` | Register a tool using the `Tool` builder |
| `registerTool(name, description, schema, ToolHandler)` | Register a tool using a raw JSON schema |
| `unregisterTool(QString)` | Remove a previously registered tool by name |

`ToolHandler` is `std::function<QJsonObject(const QJsonObject&)>`.

#### Conversation

| Method | Description |
|---|---|
| `sendPrompt(QString)` | Append a user message and send the full conversation to the API |
| `clearConversation()` | Reset conversation history; registered tools and settings are preserved |
| `conversationHistory()` | Returns the current `QJsonArray` of conversation messages |

#### Signals

| Signal | Emitted when |
|---|---|
| `responseReady(QString text)` | The LLM produces a final text response (after all tool calls are resolved) |
| `toolInvoked(QString name, QJsonObject args)` | A tool handler is about to be called |
| `toolCompleted(QString name, QJsonObject result)` | A tool handler has returned |
| `errorOccurred(QString message)` | A network, HTTP, or parsing error occurs |
| `requestStarted()` | An HTTP request is sent |
| `requestFinished()` | The current turn is fully resolved (success or error) |

---

### `QtLLM::Tool`

A builder class for defining tool schemas. Not a `QObject` — safe to copy and store by value.

```cpp
QtLLM::Tool tool;
tool.setName("myTool")
    .setDescription("Does something useful.")
    .addParameter("value",  "number",  "The input value.",      /*required=*/true)
    .addParameter("label",  "string",  "An optional label.",    /*required=*/false);
```

#### Methods

| Method | Returns | Description |
|---|---|---|
| `setName(QString)` | `Tool&` | Sets the tool name (used by the LLM to invoke it) |
| `setDescription(QString)` | `Tool&` | Human-readable description for the LLM |
| `addParameter(name, type, description, required)` | `Tool&` | Adds a JSON Schema parameter. `type` is a JSON Schema primitive: `"string"`, `"number"`, `"integer"`, `"boolean"`, `"array"`, `"object"` |
| `name()` | `QString` | Returns the tool name |
| `description()` | `QString` | Returns the description |
| `toApiObject()` | `QJsonObject` | Produces the Claude API `tools` array element |
| `toOpenAiApiObject()` | `QJsonObject` | Produces the tool object for OpenAI-compatible APIs (Ollama). Schema is wrapped under `"function"` → `"parameters"` |

---

## Error Handling

All errors are reported via the `errorOccurred(QString)` signal — the library never throws.

| Situation | Behaviour |
|---|---|
| Network error | Emits `errorOccurred` with the Qt network error string |
| HTTP 4xx / 5xx | Parses the JSON error body and emits `errorOccurred` with the API message |
| Unknown tool name | Emits `errorOccurred` and sends an error result back to the LLM so it can recover |
| Malformed JSON response | Emits `errorOccurred`, does not crash |

---

## Examples

### `LibraryExample` — Chat Window

A Qt Widgets GUI application with a full chat interface. The LLM has access to a `setWindowTitle` tool that lets it rename the application window on request.

Set the required environment variables and run:

```bash
set ANTHROPIC_FOUNDRY_API_KEY=your_key_here
set ANTHROPIC_FOUNDRY_BASE_URL=https://your-endpoint.example.com/api/anthropic
installation\bin\LibraryExample.exe
```

### `FoundryDemo` — Console Demo

A minimal console application that registers two tools (`add` and `get_weather`) and sends a single prompt that exercises both. Exits after the first complete response.

```bash
set ANTHROPIC_FOUNDRY_API_KEY=your_key_here
set ANTHROPIC_FOUNDRY_BASE_URL=https://your-endpoint.example.com/api/anthropic
installation\bin\FoundryDemo.exe
```

---

## Project Structure

```
QtLLM/
├── core/
│   ├── inc/
│   │   ├── QtLLM.h            # Umbrella public header — include this
│   │   ├── Client.h           # QtLLM::Client
│   │   └── Tool.h             # QtLLM::Tool
│   └── src/
│       ├── Client.cpp
│       ├── Tool.cpp
│       ├── ProtocolBase.h/cpp     # Internal — abstract base for provider protocols
│       ├── ClaudeProtocol.h/cpp   # Internal — Claude Messages API implementation
│       ├── OllamaProtocol.h/cpp   # Internal — Ollama /api/chat implementation
│       └── HttpTransport.h/cpp    # Internal — QNetworkAccessManager wrapper
├── examples/
│   ├── LibraryExample/        # Qt Widgets chat window demo
│   └── FoundryDemo/           # Console two-tool demo
├── unittests/
│   ├── QtLLMTest/             # Unit tests (no network)
│   └── IntegrationTest/       # Live API tests (requires env vars)
└── dependencies/
    └── OpenSSL.cmake          # Locates and deploys OpenSSL 1.1.x DLLs
```

---

## License

See [LICENSE](LICENSE).
