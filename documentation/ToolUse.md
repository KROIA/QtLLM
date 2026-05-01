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

---

## Tool handler rules

- Handlers are called **synchronously** on the Qt event loop thread.
- The return value must be a `QJsonObject`. It is serialised and sent back to the LLM as the tool result.
- Return an error object (e.g. `{{"error", "not found"}}`) to signal failure — the LLM will see this and may retry or report it to the user.
- Long-running work (file I/O, network calls, database queries) should be offloaded and the result delivered asynchronously; keep the handler itself non-blocking.

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
