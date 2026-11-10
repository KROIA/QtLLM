# Providers

QtLLM supports the backends below. The public API is identical for all — only the constructor (or `Client::setProvider()`) and the model name differ.

| Backend | `Provider` | Credentials | Billing |
|---|---|---|---|
| Anthropic API | `Claude` | API key | per token |
| Microsoft Foundry | `Claude` + Foundry endpoint | Foundry API key | per token (Azure) |
| Ollama | `Ollama` | none | free, local |
| Claude subscription | `ClaudeCode` | Claude Code login | flat rate (Pro / Max / Team plan) |
| OpenAI-compatible server | `OpenAI` | API key, optional | depends on the server |

In `SettingsDialog` these are five entries of the **Provider** combo, each with its own fields; `SettingsDialog::applyTo(&client)` applies the choice.

---

## Anthropic Claude

```cpp
QtLLM::Client client(
    qgetenv("ANTHROPIC_API_KEY"),
    "https://api.anthropic.com/v1/messages"
);
client.setModel("claude-sonnet-5-5");   // also the default when none is set
```

### Endpoint

The default URL (`https://api.anthropic.com/v1/messages`) is pre-filled when omitted. For private deployments or API proxies, pass the full endpoint URL explicitly.

### Authentication

The API key is sent as the `x-api-key` header. Store it in an environment variable — never hard-code it in source.

For a gateway where users sign in instead (an OAuth2 proxy in front of the API
or of a LiteLLM instance), pass no API key and give the client the user's token:
`client.setAccessTokenProvider(session->accessTokenProvider())`. The request then
carries `Authorization: Bearer <token>` and no `x-api-key`. See
[OAuth2Session](API.md#qtllmoauth2session).

### Current models (as of November 2026)

| Model ID | Context | Price in / out per 1M | Notes |
|---|---|---|---|
| `claude-sonnet-5-5` | 1M | $2 / $10 | **Library default** — best value for tool work |
| `claude-haiku-5-5` | 1M | $0.10 / $0.50 | Cheapest; first fallback when the default is not offered |
| `claude-opus-5-5` | 1M | $4 / $20 | Most capable Opus |
| `claude-fable-5-2` | 1M | $8 / $40 | Most capable overall |

A model the endpoint does not offer is replaced automatically (`modelChanged()` reports it): first by the default, then by the next cheaper current model, and only then by whatever the endpoint lists first.

See [docs.anthropic.com/models](https://docs.anthropic.com/en/docs/about-claude/models) for the current list.

### Cost tracking

`UsageStats::sessionCostUsd` is estimated from token counts and the prices of the [`PricingRegistry`](API.md#qtllmpricingregistry) (online LiteLLM table, own prices, or a built-in fallback). See [API.md](API.md#cost-estimation-claude-only) for the table and caveats.

---

## Microsoft Foundry

Claude deployed in a Foundry resource speaks the same Messages protocol, so it uses `Provider::Claude` with the resource's endpoint and key:

```cpp
QtLLM::Client client(
    qgetenv("ANTHROPIC_FOUNDRY_API_KEY"),
    qgetenv("ANTHROPIC_FOUNDRY_BASE_URL") + QString("/v1/messages")
);
client.setModel("claude-sonnet-5-5");   // the deployment name
```

The model name is the **deployment** name. Where the endpoint offers a model list, an unknown model is replaced as on the Anthropic API (`Client::pickFallbackModel()`): by the default model, then the next cheaper current one, and only then by the first one listed. `modelChanged()` reports it. Token counting (`count_tokens`) is often not available there; the context display then stays on estimates.

---

## Claude subscription (Claude Code login)

```cpp
QtLLM::Client client(QtLLM::Provider::ClaudeCode, QString());
client.setModel("sonnet");   // alias: sonnet | opus | haiku | fable, or a full model id
```

A claude.ai plan cannot call the Messages API — Anthropic accepts those credentials only from Claude Code. So this provider runs each turn through the installed CLI (`claude -p`), which owns the login and the conversation.

**Setup, once:** install [Claude Code](https://claude.com/claude-code), run `claude` in a terminal, `/login` with the claude.ai account. `SettingsDialog` shows the login state; `ClaudeCodeProtocol::queryAuthStatus()` gives it to application code.

How it behaves:

- **Tools work unchanged.** Registered handlers are served to the CLI over MCP on a loopback-only port protected by a random token. They still run synchronously on the GUI thread; consent handler, validation and call cap apply as usual. The model sees them as `mcp__qtllm__<name>`.
- **Isolated from the coding setup.** The CLI starts without its built-in tools (shell, file edits), user settings, hooks, plugins and `CLAUDE.md`. The model gets the application's system prompt and tools only.
- **Environment.** `ANTHROPIC_API_KEY`, `ANTHROPIC_AUTH_TOKEN` and the cloud-provider switches are removed for the child process; otherwise the CLI would silently bill those instead of the plan.
- **Cost is reported as 0.** Usage counts against the plan's limits, not dollars. Token counts are still recorded. `AgentConfig::costCapUsd` therefore never trips.
- **A process launch per turn** adds latency, and `setMaxTokens()` has no effect.
- **Limits.** When the plan's usage limit is reached the turn fails with the CLI's message via `errorOccurred()`.

The login is personal. An application distributed to other people needs each of them to have Claude Code and their own plan; check Anthropic's terms before shipping that as a feature.

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

## OpenAI-compatible servers

`Provider::OpenAI` speaks the OpenAI Chat Completions API, which most local servers offer: LM Studio (`http://localhost:1234/v1`), llama.cpp server, vLLM, Ollama (`http://localhost:11434/v1`), and hosted services.

```cpp
QtLLM::Client client(QtLLM::Provider::OpenAI, "http://localhost:1234/v1");   // key optional
```

- The url may be the base or the full `.../chat/completions`; a bare host gets `/v1`.
- Models come from `GET <base>/models`; a model the server does not list is replaced by one it does.
- Streaming, tools (also asynchronous), pictures in prompts and in tool results, `setParallelToolCalls(false)`, effort as `reasoning_effort`.
- An optional request field the server rejects (`stream_options`, `reasoning_effort`, `parallel_tool_calls`, `max_tokens` vs `max_completion_tokens`) is left out from then on.
- No tokenizer endpoint: the context bar shows estimates. Cost is 0 for models the price table does not know.

---

## Switching providers at runtime

```cpp
client.setProvider(QtLLM::Provider::ClaudeCode, QString());            // subscription
client.setProvider(QtLLM::Provider::Ollama, "http://localhost:11434"); // local
client.setProvider(QtLLM::Provider::Claude, foundryUrl, foundryKey);   // Foundry / API key
```

Registered tools, the system prompt and settings are kept. The conversation is not: message formats differ between backends, so a switch starts a new one. Moving between the Anthropic API and Foundry needs no switch at all — `setEndpointUrl()` + `setApiKey()` keep the conversation.

With the settings dialog, one call does all of it:

```cpp
connect(&dlg, &QtLLM::SettingsDialog::settingsApplied, this, [&]() { dlg.applyTo(&client); });
```
