# Providers

QtLLM supports two LLM providers selected at `Client` construction time. The public API is identical for both — only the constructor and model name differ.

---

## Anthropic Claude

```cpp
QtLLM::Client client(
    qgetenv("ANTHROPIC_API_KEY"),
    "https://api.anthropic.com/v1/messages"
);
client.setModel("claude-sonnet-4-5");
```

### Endpoint

The default URL (`https://api.anthropic.com/v1/messages`) is pre-filled when omitted. For private deployments or API proxies, pass the full endpoint URL explicitly.

### Authentication

The API key is sent as the `x-api-key` header. Store it in an environment variable — never hard-code it in source.

### Supported models (as of mid-2025)

| Model ID | Context | Notes |
|---|---|---|
| `claude-opus-4-7` | 200k | Most capable |
| `claude-sonnet-4-6` | 200k | Balanced performance / cost |
| `claude-haiku-4-5` | 200k | Fastest and cheapest |

See [docs.anthropic.com/models](https://docs.anthropic.com/en/docs/about-claude/models) for the current list.

### Cost tracking

`UsageStats::sessionCostUsd` is estimated from token counts and a hard-coded price table. See [API.md](API.md#cost-estimation-claude-only) for the table and caveats.

---

## Ollama (local inference)

```cpp
QtLLM::Client client(
    QtLLM::Provider::Ollama,
    "http://localhost:11434/api/chat"
    // apiKey omitted — not required for local Ollama
);
client.setModel("llama3.2");
```

### Server management

The `OllamaManager` class handles server lifecycle:

```cpp
// Check if the server is already running
QtLLM::OllamaManager mgr;
mgr.checkIsRunning();  // → emits isRunningChecked(bool)

// Start if not running (no console window on Windows)
QtLLM::OllamaManager::startServer();

// List locally installed models
mgr.fetchLocalModels();  // → emits localModelsReady(...)

// Download a model
mgr.pullModel("llama3.2");  // → emits pullProgress / pullFinished
```

The `LibraryExample` chat window does this automatically on startup when Ollama is selected as the provider.

### Auto-start executable search order

`OllamaManager::startServer()` searches for `ollama.exe` in this order and uses the first match:

1. Qt process PATH
2. `%LOCALAPPDATA%\Programs\Ollama\ollama.exe` (default installer path)
3. `HKEY_CURRENT_USER\Environment\Path` (Windows user PATH from registry)
4. `HKEY_LOCAL_MACHINE\...\Environment\Path` (Windows system PATH from registry)

The process is started with `DETACHED_PROCESS | CREATE_NO_WINDOW` flags so no console window appears.

### Tool calling with Ollama

Tool calling works the same way as with Claude. The library automatically translates tool schemas to the OpenAI-compatible format that Ollama expects (`"function"` → `"parameters"`).

Not all Ollama models support tool calling. Models known to work:
- `llama3.2`, `llama3.1:8b`
- `mistral-nemo`
- `qwen2.5:7b`, `qwen2.5:14b`

### Cost

`UsageStats::sessionCostUsd` is always `0.0` for Ollama — local inference has no API cost.

Token counts are populated from Ollama's `prompt_eval_count` and `eval_count` response fields.

---

## Switching providers at runtime

`Client` is bound to a provider at construction. To switch providers, create a new `Client` instance.

```cpp
// Swap from Ollama to Claude
m_client = std::make_unique<QtLLM::Client>(
    qgetenv("ANTHROPIC_API_KEY"),
    "https://api.anthropic.com/v1/messages",
    this
);
```

Re-register any tools and re-apply `setModel` / `setSystemPrompt` after the swap.
