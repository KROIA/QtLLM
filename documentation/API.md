# API Reference

All public types live in the `QtLLM` namespace. Include the umbrella header:

```cpp
#include <QtLLM.h>
```

Contents:

- [Provider](#qtllmprovider)
- [Client](#qtllmclient)
- [OAuth2Session](#qtllmoauth2session)
- [Agent](#qtllmagent)
- [AgentRegistry](#qtllmagentregistry)
- [AgentsWidget](#qtllmagentswidget)
- [Tool](#qtllmtool)
- [ToolResult helpers](#tool-result-helpers)
- [BuiltinTools](#qtllmbuiltintools)
- [InterviewTool](#qtllminterviewtool)
- [InterviewWidget](#qtllminterviewwidget)
- [ChatDockWidget](#qtllmchatdockwidget)
- [SettingsDialog](#qtllmsettingsdialog)
- [ToolSettingsWidget](#qtllmtoolsettingswidget)
- [UsageStats](#qtllmusagestats)
- [PricingRegistry](#qtllmpricingregistry)
- [UsageHistory](#qtllmusagehistory)
- [OllamaManager](#qtllmollamamanager)
- [Streaming, effort and context](#streaming-effort-and-context)
- [Asynchronous tools](#asynchronous-tools)
- [Image tool results](#image-tool-results)
- [AppInterface](#qtllmappinterface)
- [GuideOverlay and speech bubbles](#guideoverlay-and-speech-bubbles)
- [Switching features on and off](#switching-features-on-and-off)
- [AppKnowledge](#qtllmappknowledge)
- [Scripting](#qtllmscripting)
- [Error handling](#error-handling)

---

## `QtLLM::Provider`

Selects the LLM backend. Set at construction, changeable later with `Client::setProvider()`.

```cpp
enum class Provider { Claude, Ollama, ClaudeCode, OpenAI };
```

| Value | Backend |
|---|---|
| `Provider::Claude` | Anthropic Messages API with an API key — api.anthropic.com, Microsoft Foundry, or a compatible gateway |
| `Provider::Ollama` | Ollama local inference server |
| `Provider::OpenAI` | Any server speaking the OpenAI Chat Completions API: LM Studio, llama.cpp, vLLM, Ollama's `/v1`, api.openai.com. `url` is the base (`http://localhost:1234/v1`); `apiKey` may be empty |
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
| `Client::defaultModel(Provider)` | Model used until the application sets one: `claude-sonnet-5-5` (best value for tool work), `llama3.2` for Ollama, `sonnet` for the subscription, `gpt-4o-mini` for `Provider::OpenAI` (only a placeholder: a local server offers what is loaded there, and the client switches to that once the model list is in) |
| `setMaxTokens(int)` | Maximum tokens in the response (default 16000 for Claude, 2048 for Ollama, 4096 for OpenAI-compatible servers). Maps to `num_predict` for Ollama; ignored by `Provider::ClaudeCode` |
| `isModelAllowed(QString model)` | Whether the model passes the filter set with `setModelFilter()` (see [What the user may change in the settings dialog](#what-the-user-may-change-in-the-settings-dialog)) |
| `static modelMatches(QString model, QStringList patterns)` | True when the model matches one of the patterns (`*` and `?` wildcards, case-insensitive) |
| `setProvider(Provider, url, apiKey)` / `provider()` | Switch backend at runtime. Starts a new conversation; tools, model and system prompt are kept |
| `setApiKey(QString)` / `setEndpointUrl(QString)` | Change credentials without touching the conversation |
| `setAccessTokenProvider(AccessTokenProvider)` | Send the user's bearer token (`Authorization: Bearer ...`) instead of the API key, for a gateway behind an OAuth2 proxy. Asked before every request; an empty token fails the request with "Not signed in". Claude and OpenAI providers; survives `setProvider()`. See [OAuth2Session](#qtllmoauth2session) |
| `setSystemPrompt(QString)` | Prepended to every request. Omitted when empty. May be changed mid-conversation — history is kept, takes effect from the next request (invalidates the prompt-cache prefix) |

### Tool registration

| Method | Description |
|---|---|
| `registerTool(Tool, ToolHandler)` | Register using the `Tool` builder |
| `registerTool(name, description, schema, ToolHandler)` | Register with a raw JSON schema object |
| `unregisterTool(QString)` | Remove a registered tool by name |
| `shareToolsWith(Client* other, QStringList except = {})` | Register this client's tools on `other` too: same definitions, handlers and enabled state. The handlers are shared, so a tool that reports into the chat still does so when `other` calls it |
| `callTool(name, arguments, ToolResultCallback done, ToolCallOptions options = {})` | Run a registered tool from application code as the model would: input repaired and checked against the tool's definition (always, whether or not `setValidateToolInput()` is on), consent, timeout for asynchronous tools. `done(result)` is called at once for an ordinary tool, later for an asynchronous one. An unknown or disabled tool, or input that does not fit, gives an error result without the handler having run; for input that does not fit, the result also carries the tool's `parameters` schema |

`ToolHandler` = `std::function<QJsonObject(const QJsonObject&)>`, `ToolResultCallback` = `std::function<void(const QJsonObject& result)>`

`ToolCallOptions` says how `callTool()` runs the tool. The defaults are what a call by the model gets; a caller that makes many calls on its own account (a script) switches off what is meant for single calls in a conversation:

| Field | Default | Effect |
|---|---|---|
| `announce` | `true` | Emit `toolInvoked()` / `toolCompleted()` |
| `countInTurn` | `true` | Count against `setMaxToolCallsPerTurn()` |
| `capResult` | `true` | Apply `setMaxToolResultChars()` |
| `askConsent` | `true` | Ask the consent handler |

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
| `sendPrompt(QString, QList<QImage>)` / `attachImages(list)` / `supportsImageInput()` | A prompt with pictures (scaled to 1568 px). `attachImages()` adds them to the next `sendPrompt()`, whoever calls it. Not on the subscription provider |
| `removeLastExchange()` | Takes the last prompt and everything after it out of the conversation and returns the prompt - for "regenerate" and "edit" |
| `importConversation(QJsonObject)` | Continue a conversation saved with `exportConversation()`. Same provider family only; thinking blocks are dropped |
| `setCostLimitUsd(double)` | Stop sending once the conversation has cost that much; signal `costLimitReached(spent)` |
| `setParallelToolCalls(bool)` | false: the model is asked to call one tool at a time |
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
| `contextBreakdown()` | [`ContextBreakdown`](#qtllmcontextbreakdown) of the next request: system prompt, tools, messages, the context window and the provider's exact count once it has arrived. Reads cached state only, cheap to call from a UI slot connected to `contextChanged()` |
| `setContextWindowResolver(ContextWindowResolver)` | Your own source for the model's context window size; wins over the provider's answer and the built-in table. Return `<= 0` to fall through. `nullptr` resets. GUI thread only |
| `contextInfoProvider()` | The `ContextInfoProvider` that asks the provider for the context window and an exact token count (Claude and Ollama). `nullptr` for `ClaudeCode` and `OpenAI`, which have no such endpoint; the breakdown then stays on estimates |

```cpp
using ContextWindowResolver = std::function<int(const QString& provider, const QString& model)>;
// provider: "claude", "ollama", "openai" or "claude-code"
```

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
| `toolsChanged()` | A tool was added or removed, or one of its switches changed |
| `contextChanged()` | Something that feeds `contextBreakdown()` changed: system prompt, tool set or conversation. Hook for a live context display |

---

## `QtLLM::OAuth2Session`

Sign-in of the user with an OAuth2 / OpenID Connect identity provider, for a
gateway that authenticates users instead of handing out API keys: an OAuth2
proxy (e.g. [oauth2-proxy](https://oauth2-proxy.github.io/oauth2-proxy/)) checks
the user's token and puts the real API key into the forwarded request. No API
key is stored on the user's machine.

Authorization Code flow with PKCE and a loopback redirect: `signIn()` opens the
system browser at the provider's login page (passed straight through when the
user is already signed in there), the provider redirects to the local
`redirectUri` with a code, and the code is exchanged for tokens. No client
secret is involved. Tokens are refreshed in the background while the refresh
token lasts. They live in memory only unless `setTokenStorePath()` names a file
to keep them between runs.

The library contains no provider URLs or client IDs: the application supplies
them.

```cpp
QtLLM::OAuth2Config cfg;
cfg.issuer      = QUrl("https://idp.example.com/oauth");          // discovery: <issuer>/.well-known/openid-configuration
cfg.clientId    = "<client id>";                                   // public client: not a secret
cfg.scopes      = {"openid", "profile"};
cfg.redirectUri = QUrl("http://localhost:12000/auth/callback");    // registered at the provider, port included

auto* session = new QtLLM::OAuth2Session(cfg, this);
// Optional: sign in without the browser on the next start while the token lasts.
session->setTokenStorePath(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)
                           + "/oauth2-session.dat");
auto* client  = new QtLLM::Client(QtLLM::Provider::Claude,
                                  "https://gateway.example.com/v1/messages", QString(), this);
client->setAccessTokenProvider(session->accessTokenProvider());

connect(session, &QtLLM::OAuth2Session::signedIn, this, [=]() {
    statusBar()->showMessage("Signed in as " + session->displayName());
});
connect(session, &QtLLM::OAuth2Session::signInRequired, session, &QtLLM::OAuth2Session::signIn);
session->signIn();
```

### `QtLLM::OAuth2Config`

| Field | Default | Description |
|---|---|---|
| `issuer` | *(empty)* | Identity provider; endpoints are read from `<issuer>/.well-known/openid-configuration` |
| `authorizationEndpoint` / `tokenEndpoint` / `userInfoEndpoint` | *(empty)* | Set to skip discovery or override it. Userinfo is optional; its claims are merged into `claims()` |
| `clientId` | *(empty)* | Client ID registered at the provider |
| `scopes` | `{"openid"}` | `openid` makes the provider return an ID token with the user's name |
| `redirectUri` | *(empty)* | Loopback URL registered at the provider, exactly, port included. Host must be `localhost`, `127.0.0.1` or `::1` |
| `signInTimeoutMs` | 5 min | A sign-in not completed in the browser within this time fails |
| `refreshMarginSecs` | `120` | The access token is refreshed this long before it expires |

### Methods

| Method | Description |
|---|---|
| `signIn()` | Sign in. Ends in `signedIn()` or `signInFailed()`. With a token store, the stored tokens are tried first: a valid access token signs in at once, an expired one is refreshed silently, only otherwise the browser opens. Called again while waiting, it starts over (the user may have closed the tab) |
| `setTokenStorePath(QString)` / `tokenStorePath()` | Keep the tokens in this file between runs; the application picks a per-user location. On Windows the file is encrypted for the current Windows user (DPAPI); elsewhere it is plain but owner-only. Written after every sign-in and refresh, deleted by `signOut()` and when the session expires. Tokens of another issuer or client ID are ignored. Empty (default) = memory only |
| `cancelSignIn()` / `signOut()` | Abandon the sign-in / forget all tokens |
| `state()` / `isSignedIn()` | `SignedOut`, `SigningIn`, `SignedIn` / a valid access token is present |
| `accessToken()` / `idToken()` / `expiresAt()` | Current tokens; `accessToken()` is empty when expired |
| `accessTokenProvider()` | For `Client::setAccessTokenProvider()` and `AgentConfig::accessTokenProvider` |
| `claims()` | ID token claims merged with the userinfo answer. Decoded without checking the signature: meant for display, the gateway verifies the token |
| `displayName()` / `email()` / `groups()` | Convenience over `claims()`. `email()` also finds an address in `preferred_username`, `website` or `upn` |
| `refresh()` | Refresh now (happens automatically) |
| `setBrowserOpener(fn)` | Replace opening the system browser (tests, embedded web view) |

### Signals

| Signal | When |
|---|---|
| `stateChanged(State)` | Any state change |
| `signedIn()` / `signInFailed(QString)` | End of `signIn()` |
| `tokenRefreshed()` | The access token was renewed |
| `signInRequired(QString)` | The refresh token expired or was revoked; call `signIn()` again |
| `signedOut()` | Tokens were dropped |

### `QtLLM::AccessTokenProvider`

`std::function<QString()>`, declared in `ProtocolBase.h`. Called right before
every request; the result goes out as `Authorization: Bearer <token>`. Return an
empty string for "not signed in". `OAuth2Session::accessTokenProvider()` returns
one that hands out the current access token and an empty string once the
session object is gone. Any other token source (another OAuth library, a token
from the application's own login) can be plugged in the same way.

### Setting up the gateway side

The library only covers the client. The provider registration and the proxy
belong to whoever runs the gateway:

- Register a **public client** for the Authorization Code flow with PKCE, and
  the exact loopback redirect URI including the port. No client secret is used.
- Request the scopes the proxy needs, e.g. a groups scope when access is
  granted per group.
- Run an OAuth2 proxy in front of the API that accepts the bearer token,
  replaces it with the real API key and forwards the request. It may also pass
  the user's e-mail on, so the API's usage can be broken down per user.

### Security notes

- `clientId`, `issuer` and `redirectUri` are not secrets and may be compiled
  into the application.
- The access and refresh tokens are credentials. Without a token store they
  never touch the disk. With one, the file is encrypted for the current Windows
  user (DPAPI): other users and other machines cannot read it, programs running
  as the same user can. Elsewhere the file is plain, readable by the owner only.
- `signOut()` deletes the stored tokens; it does not end the user's session at
  the identity provider in the browser.
- Only loopback redirect URIs are accepted, and a callback whose `state` does
  not match the sign-in in progress is ignored.

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
| `url` | *(provider default)* | Endpoint; empty means `http://localhost:11434/api/chat` for Ollama, `http://localhost:1234/v1` for `Provider::OpenAI`, otherwise `https://api.anthropic.com/v1/messages` |
| `apiKey` | *(empty)* | Ignored for Ollama |
| `accessTokenProvider` | *(empty)* | Bearer token instead of `apiKey`, usually `OAuth2Session::accessTokenProvider()` (see [OAuth2Session](#qtllmoauth2session)) |
| `model` | *(empty)* | Preferred model identifier |
| `fallbackModels` | *(empty)* | Tried in order when the provider does not offer `model` |
| `systemPrompt` | *(empty)* | Omitted when empty |
| `maxTokens` | `1024` | Response cap |
| `costCapUsd` | `0.0` | Spend cap; `0` = unlimited |
| `effort` | `Effort::Default` | How much the model thinks (see [Streaming, effort and context](#streaming-effort-and-context)). Background work is usually simple; `Effort::Low` makes it cheaper and faster |
| `messaging` | `false` | The agent can be found and written to by other agents, and gets the tools to do the same (see [Agents talking to each other](#agents-talking-to-each-other)) |
| `description` | *(empty)* | One line that tells the other agents what this agent is for; shown by `list_agents` |

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

### Agents talking to each other

```cpp
QtLLM::AgentConfig config;
config.name        = "critic";
config.description = "Gives a blunt opinion on a text.";   // what the others see
config.messaging   = true;                                 // off by default
auto* critic = new QtLLM::Agent(config, this);

QtLLM::registerAgentMessagingTools(&chatClient);           // optional: the chat may write to agents too
```

An agent with `messaging` gets two tools and becomes addressable; agents without it are invisible to the others.

| Tool | Does |
|---|---|
| `list_agents` | The agents that accept messages: name, description, state, queue length - and the caller's own name |
| `message_agent(to, message)` | Hands the message to that agent as a task (`[Message from agent 'name']` in front) and returns its reply as the tool result |

The receiver keeps its own conversation and works through its messages in order. A message to an agent that is itself waiting for the sender's reply is refused with an explanation - otherwise both would wait until the tool times out. An agent destroyed before it answers ends the call with an error. Subagents started through `run_subagent` do not inherit these tools; set `messaging` in the config they are spawned from if they should have them. Every message shows up as a task in the receiver's record, and so in the Agents tab.

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
| `finishedAgents()` | `QList<AgentRecord>` of destroyed agents, newest first, at most 200. In memory only: gone when the process ends |
| `clearFinished()` | Forget the history |
| `agentFinished(AgentRecord)` | Signal: an agent was destroyed; its final record (emitted before `agentDestroyed`) |

`Agent::record()` returns the same `AgentRecord` for a living agent. It holds name, provider, model, system prompt, spawn / finish time, totals (turns, tool calls, tokens, cost, working time) and up to 50 tasks (`AgentTask`), each with prompt, reply or error, duration, tokens, cost and its tool calls (`AgentToolCall`: name, input, result, duration; input and result cut at 4000 characters).

---

## `QtLLM::AgentsWidget`

Read-only `QWidget` listing every live [`Agent`](#qtllmagent) in the process: name, provider and **effective** model, state, queue depth, turns, tokens in/out, cost, and uptime, with a detail pane for the selected agent's endpoint, token and cost caps, enabled tools, system prompt, and last prompt/reply/error.

```cpp
auto* widget = new QtLLM::AgentsWidget(this);   // that is the entire setup
```

It binds to `AgentRegistry::instance()` itself, so it needs no wiring and no client — construct it anywhere and every agent in the process appears. [`SettingsDialog`](#qtllmsettingsdialog) already embeds one as its **Agents** tab; construct your own only if you want the list somewhere else, such as a docked debug panel.

The table rebuilds once a second so uptime and in-flight counters keep moving, and immediately on `agentSpawned` / `agentDestroyed`. Selection survives a rebuild, so the detail pane does not jump while it is being read.

Below the running agents is a second table, **Finished agents**: every agent that has ended in this session - including the subagents the model starts through `run_subagent`, which live only for one task. Clicking a row in either table shows what the agent was called for, its chat log, every tool call with input and result, how long it worked, tokens and cost. **Clear history** empties it; `setHistoryVisible(false)` hides that part. Both tables use the full width of the tab.

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
| `parameterSchema()` | The full parameter schema with ranges and defaults, also in strict mode. The schema to hand to `prepareInput()` / `validateAgainstSchema()` |
| `Tool::normalizeEnumCase(schema, args)` | Returns `args` with every enum value rewritten to the spelling in the schema (`"OpenFile"` -> `"openFile"`). Values that match no entry are left alone |
| `Tool::closestName(name, QStringList candidates)` | The candidate closest to `name`, or empty when none is near enough to be a typo. What `did_you_mean` hints are made of |
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

Pictures in a result (see [Image tool results](#image-tool-results)):

| Function | Description |
|---|---|
| `toolImage(QImage, payload = {}, int maxEdge = 1568)` | `{"status":"ok", ...payload}` plus the image, scaled to `maxEdge` on its longer side, PNG or JPEG when that is much smaller |
| `toolAddImage(QJsonObject& result, QImage, int maxEdge = 1568)` | Adds a further image to such a result |
| `toolTakeImages(QJsonObject& result)` | Removes the images and returns them; leaves an `"images": N` note in the JSON |
| `kToolImagesKey` | The reserved key the images are stored under (`"$images"`): `[{"media_type":"image/png","data":"<base64>","width":W,"height":H}, ...]` |

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

Ready-made chat panel (`QDockWidget`): message bubbles with Markdown rendering, input field, Send/Cancel, status line, context usage bar with Clear and Compact, Save and Settings buttons. Assistant messages render tables, inline code, and hex color codes (`#rrggbb`) get a colored rectangle appended.

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
| `appendAssistantDelta(QString)` | Streaming: grow the current answer bubble by a piece of text, creating it on the first piece. `setClient()` connects it to `Client::responseDelta()` |
| `setShowToolCalls(bool)` / `showToolCalls()` | Debug aid: every tool call appears as a collapsed card (tool name, description, parameters, result). Needs `setClient()`. Off by default |
| `setContextBreakdown(ContextBreakdown)` | Update the context usage bar. `setClient()` does this on `Client::contextChanged()` |

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
| `compactRequested()` | Compact clicked and **no** client bound (with a client, `Client::compactConversation()` is called directly) |
| `clearContextRequested()` | Clear clicked and **no** client bound (with a client, `Client::clearConversation()` is called directly) |

Optional features, off by default: `setImageInputEnabled(bool)` (paste, drop or attach pictures; `attachImages()` from code; signal `imagesSubmitted()` when no client is bound), `setRegenerateEnabled(bool)` (regenerate / edit the last message; an edit is given up by clicking the button again or pressing Esc in the input field, and replaces the exchange only when the changed message is sent), `setLoadButtonVisible(bool)` and `loadConversation(QJsonObject)` (continue a saved conversation).

---

## `QtLLM::SettingsDialog`

Ready-made settings dialog (`QDialog`) with six tabs: **Settings** (provider, API key, endpoint, model incl. auto-detection, system prompt, font size), **Tools**, **Scripts** (only while a `Scripting` object is set with `setScripting()` and scripting is built in), **Statistik** (usage charts), **Context**, and **Agents**.

**Apply** applies and leaves the window open, **Apply and close** applies and closes it (`Accepted`), **Cancel** drops what was not applied and closes it. Two ways to open it: `exec()` (or `open()`) is modal, the application waits; `show()` makes it a window beside the application, which stays usable. For such a window, `setAttribute(Qt::WA_DeleteOnClose)` and a `QPointer` to bring the open one forward instead of opening a second (the LibraryExample does this).

The **Agents** tab is a read-only view of every live [`Agent`](#qtllmagent) in the process — name, provider/model, state, queue depth, turns, tokens, cost, and uptime, with a detail pane for the selected agent's system prompt, enabled tools, and last prompt/reply/error. It binds to `AgentRegistry` itself, so it needs no wiring. It has no kill or pause controls on purpose: the application owns its agents, and a dialog deleting an object the app still holds is a crash waiting to happen.

| Method | Description |
|---|---|
| `provider()` / `setProvider(Provider)` | `SettingsDialog::Provider { Claude, Ollama, Foundry, ClaudeSubscription, OpenAICompatible }` — values are stable, safe to persist as int |
| `apiKey()` / `setApiKey`, `model()` / `setModel`, `endpointUrl()` / `setEndpointUrl`, `ollamaUrl()` / `setOllamaUrl`, `foundryApiKey()` / `setFoundryApiKey`, `foundryEndpointUrl()` / `setFoundryEndpointUrl`, `systemPrompt()` / `setSystemPrompt`, `fontSizePercent()` / `setFontSizePercent` | Field access. Each provider keeps its own credentials. `foundryEndpointUrl()` completes a bare resource URL with `/v1/messages` |
| `setTabVisible(Tab, bool)` / `isTabVisible(Tab)` | Hide a tab (`Settings, Tools, Statistics, Context, Agents, Scripts`; `Statistics` is the **Statistik** tab) |
| `setAvailableProviders(QList<Provider>)` | Which providers the user can choose from |
| `toolSettingsWidget()` | The Tools tab's list ([`ToolSettingsWidget`](#qtllmtoolsettingswidget)) |
| `setScripting(ScriptHost::ScriptEngine*)` / `scriptsWidget()` | Bind the application's `Scripting`: adds the **Scripts** tab. `scriptsWidget()` returns that tab's `ScriptsWidget`. Without scripting built in both do nothing |
| `isModelAllowed(Provider, QString model)` | Whether the model passes that provider's `ProviderPolicy` (`allowedModels` / `blockedModels`) |
| `showToolCalls()` / `setShowToolCalls(bool)` | The "Tool-Calls im Chat anzeigen" check box on the Settings tab (debug option: tool calls as cards in the chat, see `ChatDockWidget::setShowToolCalls`) |
| `setFontSizeLabel(QString)` | Text of the font size row's label (default `"Font Size:"`) |
| `openAiUrl()` / `openAiApiKey()` and setters | Fields of the OpenAI-compatible provider |
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

## `QtLLM::ToolSettingsWidget`

The tools of a client, grouped, with a check box each. The Settings dialog's **Tools** tab is one; an application with its own settings window embeds another (example under [Who decides about a tool](#who-decides-about-a-tool)). Tools the application locked with `Client::setToolUserConfigurable(name, false)` are shown greyed out, or left out with `setLockedToolsVisible(false)`.

| Member | Description |
|---|---|
| `setClient(Client*)` / `client()` | The client whose tools are listed |
| `setLockedToolsVisible(bool)` | Locked tools greyed out (default) or not listed |
| `setApplyImmediately(bool)` | `true`: a click changes the client at once. `false` (default): `apply()` writes the choices, for a dialog with OK / Apply / Cancel |
| `apply()` | Writes the user's choices to the client (`Client::setToolEnabledByUser`) |
| `refresh()` | Rebuilds the list from the client; choices not yet applied are kept. Called automatically when the client's tools change |
| signal `changed(QString toolName, bool enabled)` | The user switched a tool (not yet applied unless `applyImmediately()`) |

---

## `QtLLM::UsageStats`

Plain struct populated after each completed turn. Returned by `Client::usageStats()` and carried by the `statsUpdated` signal.

```cpp
struct UsageStats {
    // Per-turn (reset each beginTurn call)
    int    inputTokens;   // prompt tokens consumed
    int    outputTokens;  // completion tokens generated
    int    cacheCreationInputTokens;  // tokens written to the prompt cache this turn
    int    cacheReadInputTokens;      // tokens read from the prompt cache this turn
    int    toolCalls;     // tool invocations within this turn
    qint64 durationMs;    // wall-clock ms from first HTTP send to final response
    int    contextTokens; // conversation size after the turn as the provider counted it; 0 = not reported

    // Session cumulative totals
    int    sessionInputTokens;
    int    sessionOutputTokens;
    int    sessionCacheCreationInputTokens;
    int    sessionCacheReadInputTokens;
    int    sessionToolCalls;
    int    sessionTurnCount;
    double sessionCostUsd;   // estimated USD; always 0 for Ollama (local)

    // Helpers
    int totalTokens()        const;  // inputTokens + outputTokens
    int sessionTotalTokens() const;
};
```

### `QtLLM::ContextBreakdown`

What the next request's context is made of. Returned by `Client::contextBreakdown()`, shown by the chat's context bar.

```cpp
struct ContextBreakdown {
    int systemPromptTokens;    // chars/4 estimate
    int toolsTokens;           // enabled tool schemas, chars/4 estimate
    int messagesTokens;        // conversation, chars/4 estimate
    int contextWindowTokens;   // the model's window (at most ContextPolicy::limitTokens); 200000 until known
    int exactUsedTokens;       // provider's own count of the whole request; -1 until it arrives

    int usedTokens()     const;   // sum of the three estimates
    int bestUsedTokens() const;   // exactUsedTokens when known, else usedTokens()
};
```

The three parts always stay estimates: no tokenizer API breaks its count down by section, only the total is exact.

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
| `samples()` | All recorded samples |
| `samplesInRange(qint64 fromMs, qint64 toMs)` | Samples with a timestamp in `[fromMs, toMs]`, both inclusive |
| `earliestTimestamp()` | Epoch ms of the earliest sample, or 0 |
| `sessionStartMs()` | Epoch ms at which this instance was constructed |
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
| `QtLLM::effortName(Effort)` | The wire name (`"low"` ... `"max"`); empty for `Default` |
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
                  | Cap::SpeechBubbles | Cap::Tours);
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
| `SpeechBubbles` | `ui_show_bubble` |
| `Tours` | `ui_show_tour` (all at once, or one at a time with Back / Next) |
| `WidgetColors` | `ui_set_widget_colors` (background, text, border of one widget), `ui_restore_widget_colors` (one widget, or all) |
| `Themes` | `ui_list_themes`, `ui_set_theme` - only among the themes the application registered |
| `SimulateInput` | `ui_click` (real mouse events, so `clicked()` fires), `ui_type_text`, `ui_press_key` (shortcuts fire while the application is active) |
| `ReadItems` | `ui_get_items`: cells of lists, tables, trees, combo boxes and tabs; paging, filter, tree paths |
| `SelectItems` | `ui_select_item`: by row or text; extend, expand, activate |
| `EditItems` | `ui_set_item_text`: text and check state through the model, where the item allows it |
| `WaitForUi` | `ui_wait_for`: until a widget is visible / hidden / enabled / disabled / exists / gone, a property has a value, or a window opened / closed |

Shared helper tools appear with the capabilities that need them: `ui_close_bubble`, `ui_clear_bubbles`, `ui_list_guidance`.

There are no draw tools (the former `Highlights`, `Shapes`, `Animations`): the model draws over the application with a script on ScriptHost's canvas, see `Scripting::attachCanvas()` in [Scripting.md](Scripting.md#drawing).

| Method | Description |
|---|---|
| `setCapabilityEnabled(cap, bool)` / `setCapabilities(flags)` | The switches |
| `addRoot(QWidget*)` | Further top-level windows the model may see |
| `excludeWidget(QWidget*)` | Subtree hidden from every tool. The chat dock is excluded by default |
| `setWritableProperties(QStringList)` | Properties `ui_set_property` may write (default: text, value, checked, currentIndex, currentText, enabled, visible, toolTip) |
| `resetLayoutChanges()` | Undo every layout modification |
| `clearGuidance()` | Remove all bubbles and tours |
| `setAnimationsEnabled(bool)` | Off: the highlight of a tour step is shown static (reduced motion) |
| `resetWidgetColors()` | Give every widget recoloured by the model its original colours back |
| `setWindowDiscoveryEnabled(bool)` | Dialogs and other windows the application opens become visible to all tools (and to `ui_list_windows`) without `addRoot()` |
| `resetPropertyChanges()` | Undo every `ui_set_property` (the model has `ui_undo_property_change`) |
| `actionLog()` / `clearActionLog()`, signal `actionPerformed(ActionRecord)` | What the model changed in the application: time, tool, input, ok, message. Text typed into password fields is redacted |
| `registerTheme(name, description, apply)` | A theme the model may select; `apply` does whatever switching means in the application |
| `registerTheme(name, description, QPalette, styleName = {}, styleSheet = {})` | The common case: application palette, optionally a `QStyle` ("Fusion") and an application style sheet |
| `registerStandardThemes()` | Registers `default` (the look at the time of the call) and `dark` (Fusion with `AppInterface::darkPalette()`) |
| `setTheme(name)` / `currentTheme()` / `themeNames()`, signal `themeChanged(name)` | The same switch from C++, e.g. for a menu entry that stays in sync |
| `widgetForId(QString id)` | The widget an id names; `nullptr` for unknown, stale, excluded or out-of-root ids |
| `idForWidget(QWidget*)` | The id the tools use for a widget; empty for widgets outside the roots or excluded |
| `isExcluded(QWidget*)` | True when the widget or one of its parents was excluded, or is a `ChatDockWidget` |
| `roots()` | The windows the tools see: the root window, `addRoot()` windows, and with window discovery every visible top-level window |
| `client()` | The client the tools are registered on |

Widgets are addressed by id: the path of object names (`mainWindow/saveButton`), with `ClassName#index` for unnamed widgets. Name the widgets the assistant should find reliably. Password fields are redacted in every read tool and blacked out in screenshots. Read tools and guidance tools are marked `Tool::isReadOnly()`, so a consent handler can ask only for the calls that change something.

Widget colours are applied through a style rule that selects the one widget, so the widgets inside it keep their look; only values that parse as a colour are accepted. The original style sheet is kept and put back on restore. Themes are a closed list: the model cannot define colours for the whole application, it can only pick a registered name.

The public methods named after the tools (`listActions()` ... `listGuidance()`) are what the registered handlers call: each takes the tool's arguments and returns its result.

The drawing classes are usable directly: `GuideOverlay::forWindow(w)->highlight(widget)`, `BubbleManager::forWindow(w)->showBubble(widget, text)`. See [GuideOverlay and speech bubbles](#guideoverlay-and-speech-bubbles).

---

## GuideOverlay and speech bubbles

The drawing classes behind the guidance tools (`GuideOverlay.h`, `SpeechBubble.h`, both in `QtLLM.h`; a one-at-a-time tour highlights its step with `GuideOverlay`). An application can use them without `AppInterface`. Owner thread only.

```cpp
auto* overlay = QtLLM::GuideOverlay::forWindow(mainWindow);
QtLLM::OverlayItem arrow;
arrow.shape  = QtLLM::OverlayItem::Shape::Arrow;
arrow.anchor = saveButton;
arrow.from   = QPointF(40, 40);
arrow.animation.type = QtLLM::OverlayAnimation::Type::Blink;
arrow.durationMs = 8000;
const QString id = overlay->addItem(arrow);     // empty when refused
```

### `QtLLM::GuideOverlay`

Transparent, mouse-transparent layer over a whole top-level window. One per window, created on first use.

| Member | Description |
|---|---|
| `static forWindow(QWidget*)` | The overlay of the window containing the widget |
| `addItem(OverlayItem)` | Returns the item's id, or an empty string when refused: item limit (`kMaxItems`, 20) reached, anchor in another window, no geometry. An item with the same id is replaced. The blink period and `durationMs` are clamped to the limits (`kMinBlinkPeriodMs` 334, `kMaxDurationMs` 5 minutes) |
| `highlight(QWidget* target, QColor color = orange, OverlayAnimation = {}, int durationMs = 0)` | Shorthand: an outline around the widget. Returns the id |
| `removeItem(id)` / `clear()` | Remove one item / all of them |
| `itemIds()` / `item(id)` / `itemRect(id)` | The ids; the stored item after clamping; its painted geometry in window coordinates (empty when not painted) |
| `static setAnimationsEnabled(bool)` / `animationsEnabled()` | For every overlay. Off: each item is shown in its static end state (reduced motion) |
| `static overlayItemFromJson(QJsonObject, OverlayItem*, QString* error = nullptr)` | Applies the keys present in the JSON to `*item`; absent keys leave it unchanged, so the caller pre-fills defaults and sets the anchor. On error `*item` is untouched and `*error` says why. `overlayItemToJson()` is the other direction |
| signal `itemRemoved(QString id)` | Every removal: explicit, `clear()`, timed expiry, anchor destroyed, replaced |

```cpp
struct OverlayItem {
    enum class Shape { Rect, RoundedRect, Ellipse, Line, Arrow, Spotlight };
    QString id;                  // generated when empty
    Shape shape = Shape::RoundedRect;
    QPointer<QWidget> anchor;    // follows the widget; not painted while it is hidden, removed when it is destroyed
    QRectF rect;                 // without anchor: window coordinates
    QPointF from, to;            // Line / Arrow; anchored: ends on the anchor's edge nearest "from"
    QColor stroke = QColor(255, 140, 0);
    QColor fill;                 // invalid = none; Spotlight: the dimming colour
    qreal strokeWidth = 2;
    qreal cornerRadius = 6;
    Qt::PenStyle penStyle = Qt::SolidLine;
    int padding = 4;             // space between anchor and outline
    OverlayAnimation animation;
    int durationMs = 0;          // 0 = until removed
};

struct OverlayAnimation {
    enum class Type { None, Blink, Fade, Pulse, ColorLoop, MarchingAnts, DrawOn };
    Type type = Type::None;
    int periodMs = 1000;         // one cycle; for Fade and DrawOn the transition
    int count = 0;               // cycles; 0 = until the item is removed
    QList<QColor> colors;        // ColorLoop
    bool fadeOut = false;        // Fade of a timed item: also fade out at the end
};
```

The JSON format of `overlayItemFromJson()`:

```json
{ "id": "...", "shape": "rect|roundedRect|ellipse|line|arrow|spotlight",
  "stroke": "#rrggbb", "fill": "...", "strokeWidth": 2, "cornerRadius": 6, "padding": 4,
  "penStyle": "solid|dash|dot|dashDot",
  "rect": { "x", "y", "width", "height" }, "from": { "x", "y" }, "to": { "x", "y" },
  "animation": { "type": "none|blink|fade|pulse|colorLoop|marchingAnts|drawOn",
                 "periodMs": 1000, "count": 0, "colors": ["#f00", "#00f"], "fadeOut": false },
  "durationMs": 0 }
```

`"color"` is accepted for `"stroke"`; colours may also be `#aarrggbb` or a name.

### `QtLLM::BubbleManager`

The speech bubbles and tours of one window, addressed by id. One per window, created on first use.

| Member | Description |
|---|---|
| `static forWindow(QWidget*)` | The manager of the window containing the widget |
| `showBubble(QWidget* target, text, title = {}, int number = 0, Placement = Auto)` | A bubble pointing at the widget; `number > 0` shows a numbered badge. Returns the id, empty when the target is not in this window |
| `showBubble(QPoint windowPoint, text, ...)` | The same, pointing at a fixed point in window coordinates |
| `closeBubble(id)` | Close one bubble |
| `clear()` | Close every bubble and tour, and forget which ones the user closed |
| `bubbleIds()` / `bubble(id)` | The ids; the `SpeechBubble` for an id |
| `showTour(QList<TourStep>, TourMode = TourMode::AllAtOnce)` | `AllAtOnce`: bubbles numbered 1..N. `OneAtATime`: one bubble with Back / Next and "2 / 5", the current target highlighted on the `GuideOverlay`. Steps whose target is gone or in another window are skipped; empty id when none is left. The tour ends after the last step or when its bubble is closed |
| `closeTour(id)` / `tourIds()` | End a tour; the ids |
| `tourStep(id)` / `setTourStep(id, int index)` | Zero-based current step of a `OneAtATime` tour (-1 for an unknown id); jump to a step |
| `describe()` | Bubbles and tours as JSON, including bubbles the user closed |
| signal `bubbleClosed(QString id)` | The user closed a bubble |

```cpp
struct TourStep { QPointer<QWidget> target; QString text; QString title; };
enum class TourMode { AllAtOnce, OneAtATime };   // BubbleManager::TourMode
```

### `QtLLM::SpeechBubble`

The bubble itself: a text box (rich text allowed) with a tail pointing at a widget or a fixed point. It is a child of the target's window and, unlike the overlay, takes mouse input: drag the body to move it (the tail stays on the target), x closes it. It follows the target on resize and move unless the user dragged it, hides while the target is hidden and deletes itself when the target is destroyed. Usually created through `BubbleManager`.

```cpp
SpeechBubble(QWidget* target, const QString& text, const QString& title = QString(),
             int number = 0, Placement placement = Placement::Auto);
SpeechBubble(QWidget* anyWidgetInWindow, const QPoint& point, const QString& text,
             const QString& title = QString(), int number = 0, Placement placement = Placement::Auto);
enum class Placement { Auto, Right, Below, Left, Above };
```

| Member | Description |
|---|---|
| `id()` / `setId`, `text()` / `setText`, `title()` / `setTitle`, `number()` / `setNumber`, `placement()` / `setPlacement` | Content and placement |
| `target()` / `setTarget(QWidget*)` | Point at another widget of the same window; the bubble is placed anew |
| `setFooter(QWidget*)` | Extra controls under the text (the tour buttons); the bubble takes ownership |
| `reposition()` | Place the bubble for the current target geometry; a dragged bubble stays where the user put it |
| `isUserMoved()`, `tailTip()`, `bubbleRect()`, `targetRect()` | State and geometry, in window coordinates |
| `dismiss()` | Close from code: hides and deletes later, without `closed()` |
| signal `closed(QString id)` | The user clicked x; the bubble deletes itself afterwards |
| signal `moved()` | The user finished dragging the bubble |

---

## Switching features on and off

Everything optional is a switch the application sets in code; nothing below is on unless stated.

| What | Switch |
|---|---|
| One tool | `Client::setToolEnabled(name, bool)` - also before the tool is registered; survives re-registration |
| A group of tools | `Client::setToolGroupEnabled(group, bool)` (`Tool::group()`); a tool's own setting wins |
| May the user switch a tool? | `Client::setToolUserConfigurable(name, bool)` / `setToolGroupUserConfigurable(group, bool)`. Locked tools are shown greyed out in the Tools tab (or hidden: `ToolSettingsWidget::setLockedToolsVisible(false)`) and `setToolEnabledByUser()` refuses them |
| Tools on demand | `Client::setToolDeferred(name, bool)` / `setToolGroupDeferred(group, bool)` |
| UI tools | `AppInterface::setCapabilityEnabled(capability, bool)`, one flag per tool family; `setWindowDiscoveryEnabled(bool)` |
| Application help | `AppKnowledge::setEnabled(bool)` |
| Scripting (build) | CMake option `QTLLM_SCRIPTING` in `dependencies/ScriptHost.cmake`; `QTLLM_HAS_SCRIPTING` in code |
| Scripting for the model | `Scripting::setEnabled(bool)`; approval per kind of script with `setApprovalRequired(kind, bool)` or `setApproval(preset)` |
| Each method a script can call | `Scripting::setMethodEnabled(name, bool)` / `setMethodGroupEnabled(group, bool)`; tools as methods with `exposeTool` |
| What the user may change about scripting | `Scripting::setEnabledUserConfigurable`, `setApprovalUserConfigurable`, `setMethodUserConfigurable`, `setUserMayEditScripts`, `setDebuggerEnabled` |
| Subagents | call `registerSubagentTool()` or not; `shareTools` argument |
| Agents talking to each other | `AgentConfig::messaging` per agent; `registerAgentMessagingTools(client)` for the chat |
| Built-in tools | the list passed to `BuiltinTools::registerTools()` |
| Chat: pictures, regenerate / edit, load | `ChatDockWidget::setImageInputEnabled`, `setRegenerateEnabled`, `setLoadButtonVisible` |
| Settings dialog: tabs, providers, endpoints, models, fields | `SettingsDialog::setTabVisible`, `setAvailableProviders`, `setProviderPolicy`, `setFieldEditable`, `setFieldVisible` |
| Models that may be used at all | `Client::setModelFilter(allowed, blocked)` |
| Agents tab history | `AgentsWidget::setHistoryVisible(bool)` |
| Streaming, parallel tool calls, cost limit | `Client::setStreamingEnabled`, `setParallelToolCalls`, `setCostLimitUsd` |

### What the user may change in the settings dialog

The application sets the values with the usual setters and states, per provider, what the user may do with them:

```cpp
QtLLM::SettingsDialog dlg(this);
dlg.setAvailableProviders({P::Foundry, P::Ollama});       // the only sources on offer

QtLLM::SettingsDialog::ProviderPolicy foundry;
foundry.endpointEditable = false;                          // address shown greyed out, key not shown
foundry.allowedModels    = QStringList{"claude-sonnet-*", "claude-haiku-*"};   // whitelist
dlg.setFoundryEndpointUrl(companyEndpoint);
dlg.setFoundryApiKey(companyKey);
dlg.setProviderPolicy(P::Foundry, foundry);

QtLLM::SettingsDialog::ProviderPolicy ollama;
ollama.blockedModels = QStringList{"*:70b", "*:120b"};                // blacklist
dlg.setProviderPolicy(P::Ollama, ollama);

dlg.setFieldEditable(QtLLM::SettingsDialog::Field::SystemPrompt, false);   // readable, not changeable
dlg.setFieldVisible(QtLLM::SettingsDialog::Field::ShowToolCalls, false);
```

| `ProviderPolicy` | Effect |
|---|---|
| `offered = false` | The provider is not in the Provider box (`setAvailableProviders()` is the shorthand). With one provider left the box is locked |
| `endpointEditable = false` | Endpoint and key are the application's: the address is shown greyed out, the key field is hidden |
| `modelEditable = false` | The model is fixed to what `setModel()` gave |
| `allowedModels` | Whitelist, `*` and `?` wildcards, case-insensitive. The user picks from a list and cannot type a name |
| `blockedModels` | Blacklist. Wins over the whitelist. A blocked name typed by the user is refused on Apply |

`applyTo(client)` hands the model rules of the chosen provider to the client, which enforces them everywhere - not only in the dialog: `Client::setModelFilter(allowed, blocked)` makes `setModel()` ignore a model that is not allowed, `modelsAvailable()` list only allowed ones, the automatic fallback choose among those, and `sendPrompt()` refuse to send with a forbidden model. An application without the dialog calls `setModelFilter()` itself. Subagents started through `run_subagent` inherit the chat's filter; agents the application creates get their own.

Fields (`Provider`, `Effort`, `SystemPrompt`, `FontSize`, `ShowToolCalls`) are locked with `setFieldEditable(field, false)` or removed with `setFieldVisible(field, false)`; whole tabs with `setTabVisible()`.

### Who decides about a tool

```cpp
client.setToolEnabled("write_text_file", false);            // the application's decision ...
client.setToolUserConfigurable("write_text_file", false);   // ... and the user cannot change it
client.setToolGroupUserConfigurable("App UI", false);       // a whole group is the application's business

// The user's own choices, kept between sessions:
settings.setValue("tools", QJsonDocument(client.userToolSettings()).toJson());
client.applyUserToolSettings(QJsonDocument::fromJson(settings.value("tools").toByteArray()).object());
```

`userToolSettings()` contains only tools the user may switch, and `applyUserToolSettings()` skips locked ones, so a stored file can never unlock anything. For the user interface use the Settings dialog's Tools tab, or put a `ToolSettingsWidget` into your own settings window:

```cpp
auto* tools = new QtLLM::ToolSettingsWidget(myPreferencesPage);
tools->setClient(&client);
tools->setApplyImmediately(true);     // or call tools->apply() from your OK button
```

### Tools on demand

A deferred tool is not sent with every request. The model gets two small tools instead - `find_tools(query)` (name, description and parameters of matching tools) and `call_tool(name, arguments)` - and the call runs through the same input check, consent hook and result cap as a direct one. The tool list of the request never changes during a conversation, so the prompt cache keeps working. Defer large groups that are needed now and then; keep what most conversations need listed.

---

## `QtLLM::AppKnowledge`

What the application knows about itself, for the model to look up: how-to texts, terms, limits. Topics cost no tokens until read.

```cpp
auto* help = new QtLLM::AppKnowledge(&client, this);
help->addTopic("export-pdf", "Exporting a document as PDF", text, {"save as pdf"});
help->addTopicsFromDirectory(":/help");    // every *.md / *.txt; first line = title
```

Registers `app_help_search(query)` and `app_help_read(id, offset)` while there is at least one topic. `setEnabled(false)` withdraws them.

| Method | Description |
|---|---|
| `addTopic(id, title, text, keywords = {})` | Adding an id again replaces the topic |
| `addTopicsFromDirectory(path)` | Every `*.md` / `*.txt` file: file name without extension = id, first line (without a leading `#`) = title. Returns the number added |
| `removeTopic(id)` / `clear()` | Remove one topic / all of them. Without topics the tools are withdrawn |
| `topicIds()` | The ids of all topics |
| `setEnabled(bool)` / `isEnabled()` | Off: the tools are withdrawn, the topics are kept. Default on |

---

## `QtLLM::Scripting`

Lets the model run a JavaScript program inside the application instead of making one tool call per item. The script calls methods the application defines; only what it returns goes back to the model. Full guide: [Scripting.md](Scripting.md).

The engine is the [ScriptHost](https://github.com/KROIA/ScriptHost) library, an optional dependency. `QtLLM::Scripting` derives from `ScriptHost::ScriptEngine` and adds the model's side; `QtLLM::ScriptMethod`, `ScriptError`, `ScriptLibrary`, `ScriptsWidget`, `ScriptEditorDialog`, `ScriptCanvas` and `ScriptUi` are the ScriptHost classes (ScriptHost 2.3 or later).

```cpp
auto* scripting = new QtLLM::Scripting(&client, this);

QtLLM::ScriptMethod read("db.readObject");
read.setDescription("Reads one object.").addParameter("id", "string", "Object id")
    .setReturns("object", "The object", exampleObject).setReadOnly(true);
scripting->registerMethod(read, [this](const QJsonObject& args) -> QJsonValue {
    return m_db.object(args["id"].toString());      // throw QtLLM::ScriptError to fail the call
});
```

| | |
|---|---|
| Tools for the model | `run_script(code \| name, args, dry_run, background)`, `script_api(query)`, `script_status(run_id)`, `stop_script(run_id)`; with a library `list_scripts`, `read_script`, `save_script` |
| Methods | `registerMethod`, `registerAsyncMethod`, `unregisterMethod`; `exposeTool` / `exposeToolGroup` / `exposeAllTools` make tools callable as `tools.<name>({...})` |
| Built in | `log`, `progress`, `sleep`, `pause`, `args`; `llm.ask` / `llm.choose` after `setLlmConfigProvider` |
| Definition | `apiDefinition(query)`: the enabled methods as TypeScript declarations, for the model and for the editor |
| Before a script runs | `check(code)`: syntax, and that every method it names exists and is enabled. Errors carry the line |
| Running | `run(code, options, callback)` returns a `RunId` (0: did not start); several runs at once; `stop(id)`, `stopAll()`, `isRunning(id)`, `activeRuns()`; `RunOptions::dryRun` records changing calls instead of making them |
| Limits | `setLimits({timeoutMs, maxCalls, maxLogLines})`, `ScriptMethod::setMaxCallsPerRun`, `setLlmCallLimit` |
| Approval | `setApprovalRequired(ScriptKind, bool)` per kind (`NewReading`, `NewChanging`, `SavedReading`, `SavedChanging`, `DryRun`), `setApproval(Never \| ChangingScripts \| Always)` for all, `setApprovalHandler`; the dialog is installed by default, `QtLLM::useApprovalDialog(scripting, parent)` gives it a parent |
| Debugger | `RunOptions::debug` and `statements`, `stepStatement()`, `stepRun()` (to the next call), `continueRun()`, `setBreakpoints()`, signals `runPaused(line, method, arguments, variables)`, `runCallFinished` |
| Saved scripts | `QtLLM::ScriptLibrary`: `setDirectory`, `addReadOnlyDirectory`, `scripts()`, `save`, `remove` |
| The application's window | `QtLLM::forwardInput(scripting, widget)`: keys and clicks as `"key"` / `"mouse"` events, `input.mouse()`. `attachCanvas(widget)` returns a `ScriptCanvas`: `gfx`, drawing and animation over the widget, also for the model's scripts. `QtLLM::exposeWidgets(scripting, widget)` returns a `ScriptUi`: the `ui.*` methods (group "UI") to read and change widgets, build dialogs and connect signals. `run_script` describes each while it is there. See [Scripting.md](Scripting.md#keys-and-the-mouse) |
| User interface | `SettingsDialog::setScripting()` adds the Scripts tab (`QtLLM::ScriptsWidget`); `QtLLM::ScriptEditorDialog` is the editor with runner and call stepping |
| User's choices | `userSettings()` / `applyUserSettings()` |

Built with `QTLLM_SCRIPTING=OFF` none of this exists and `QTLLM_HAS_SCRIPTING` is 0.

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
| `Provider::Claude` without API key and without `AccessTokenProvider` | Emits `errorOccurred` immediately; nothing is sent |
| `AccessTokenProvider` returns an empty token | Emits `errorOccurred` with `Not signed in - ...`; nothing is sent, the prompt is removed from the history |
| Gateway rejects the token (HTTP 401 / 403) | Emits `errorOccurred` with the gateway's message, as any HTTP error |
| `OAuth2Session` sign-in fails (browser denied, timeout, port in use, token endpoint error) | `signInFailed(QString)`; not routed through any `Client` |
| `OAuth2Session` refresh token expired or revoked | `signInRequired(QString)`; tokens and the token store are cleared |
| Tool handler throws | Caught by the protocol layer; the model receives `{"status":"error","message":"Internal error: <what>"}` with `is_error` — the process never crashes |
