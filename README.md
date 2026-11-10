# QtLLM

> **Vibe Coding Disclaimer:** This project was built with AI-assisted development (vibe coding).
> The architecture, implementation, and documentation were created through an iterative
> conversation with an AI coding assistant. Use in production at your own discretion.

---

A C++/Qt library for integrating LLM APIs into desktop applications.
Supports **Anthropic Claude** (API key, Microsoft Foundry, or a claude.ai subscription through Claude Code),
local **Ollama** models and any **OpenAI-compatible** server out of the box.
Tool/function calling, streamed answers (on by default), and a Qt-idiomatic signals-and-slots API.

---

## Features

- Single header include (`#include <QtLLM.h>`)
- Async via Qt signals — never blocks the UI thread
- Tool/function calling with automatic multi-turn loop handling
- Built-in tools, individually opt-in (`BuiltinTools`): user interview forms in the chat, file dialogs, message boxes, color picker, clipboard, date/time, filesystem access (consent-gated), repeating tasks, and more
- Tool safety rails: input validation, per-turn call cap, consent hook, enable/disable without re-registration
- Ready-made widgets: chat dock (Markdown bubbles, inline interview cards, color swatches), settings dialog with tool manager and usage charts
- Providers `Claude`, `ClaudeCode`, `Ollama` and `OpenAI`, switchable at run time with `Client::setProvider()`
- Usage statistics, persistent usage history, and cost estimation per turn
- User sign-in instead of API keys (`OAuth2Session`): OAuth2 / OpenID Connect Authorization Code flow with PKCE in the system browser, background token refresh, optional encrypted token store; requests carry the user's bearer token for a gateway behind an OAuth2 proxy
- Ollama: auto-start server, model discovery, and one-click model downloads
- Builds as shared and static library
- Scripting: the model writes one script instead of many tool calls (optional, `QTLLM_SCRIPTING`)
- Builds with Qt 5.15 (default) and Qt 6 (verified with Qt 6.11). Needs the OpenSSL run-time DLLs for https, which the build copies next to the programs; with the default `QTLLM_SCRIPTING=ON` also the [ScriptHost](https://github.com/KROIA/ScriptHost) library and the Qt module Qml (see [Building.md](documentation/Building.md#options))

---

## Requirements

| | Version |
|---|---|
| Qt | 5.15.x (default) or 6.x |
| OpenSSL | Run-time DLLs for https: 1.1.1 for Qt 5, 3.x for Qt 6 |
| CMake | 3.20+ |
| MSVC | 2019 / 2022 (Windows x64) |

> Qt 5.15 loads OpenSSL 1.1.1 and cannot use 3.x; Qt 6 needs 3.x. The build picks the matching pair and copies it next to the programs, also for an application that uses QtLLM as a dependency (see [Building.md](documentation/Building.md#openssl)).

---

## Quick Start

```cpp
#include <QtLLM.h>

// Claude
QtLLM::Client client(qgetenv("ANTHROPIC_API_KEY"),
                     "https://api.anthropic.com/v1/messages");
client.setModel("claude-sonnet-5-5");

// or Ollama (local)
QtLLM::Client client(QtLLM::Provider::Ollama,
                     "http://localhost:11434/api/chat");
client.setModel("llama3.2");

QObject::connect(&client, &QtLLM::Client::responseReady,
                 [](const QString& text) { qDebug() << text; });

client.sendPrompt("Hello!");
```

---

## Documentation

Detailed documentation is in the [`documentation/`](documentation/) folder:

| Topic | File |
|---|---|
| Building & installation | [documentation/Building.md](documentation/Building.md) |
| API reference | [documentation/API.md](documentation/API.md) |
| Tool use / function calling | [documentation/ToolUse.md](documentation/ToolUse.md) |
| Providers (Claude, Foundry, Claude subscription, Ollama, OpenAI-compatible) | [documentation/Providers.md](documentation/Providers.md) |
| Scripting | [documentation/Scripting.md](documentation/Scripting.md) |
| Examples | [documentation/Examples.md](documentation/Examples.md) |


