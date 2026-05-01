# QtLLM

> **Vibe Coding Disclaimer:** This project was built with AI-assisted development (vibe coding).
> The architecture, implementation, and documentation were created through an iterative
> conversation with an AI coding assistant. Use in production at your own discretion.

---

A C++/Qt5 library for integrating LLM APIs into desktop applications.
Supports **Anthropic Claude** and local **Ollama** models out of the box.
Tool/function calling, streaming-free async design, and a Qt-idiomatic signals-and-slots API.

---

## Features

- Single header include (`#include <QtLLM.h>`)
- Async via Qt signals — never blocks the UI thread
- Tool/function calling with automatic multi-turn loop handling
- Supports Claude (cloud) and Ollama (local) — switchable at construction time
- Usage statistics and cost estimation per turn
- Ollama: auto-start server, model discovery, and one-click model downloads
- Builds as shared and static library
- Qt5 only — no extra dependencies beyond OpenSSL 1.1.x

---

## Requirements

| | Version |
|---|---|
| Qt | 5.15.x |
| OpenSSL | 1.1.1x (Windows runtime DLLs) |
| CMake | 3.20+ |
| MSVC | 2019 / 2022 (Windows x64) |

> OpenSSL 3.x is **not** compatible with Qt 5.15. Use *Win64 OpenSSL v1.1.1w Light*.

---

## Quick Start

```cpp
#include <QtLLM.h>

// Claude
QtLLM::Client client(qgetenv("ANTHROPIC_API_KEY"),
                     "https://api.anthropic.com/v1/messages");
client.setModel("claude-sonnet-4-5");

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
| Providers (Claude & Ollama) | [documentation/Providers.md](documentation/Providers.md) |
| Examples | [documentation/Examples.md](documentation/Examples.md) |

---

## License

See [LICENSE](LICENSE).
