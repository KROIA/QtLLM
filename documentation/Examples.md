# Examples

The `examples/` directory contains two runnable applications demonstrating the library.

---

## LibraryExample — Chat Window

`examples/LibraryExample/`

A full Qt Widgets GUI chat application with:

- Multi-turn conversation display
- Live usage statistics panel (tokens per turn, session totals, estimated cost)
- Ollama model selector with one-click download
- A registered `setWindowTitle` tool the LLM can invoke to rename the window
- `AppInterface`: the model reads the window, clicks and types, edits lists and tables, recolours widgets, switches themes and explains with speech bubbles and guided tours
- **Settings…** opens the `SettingsDialog` beside the window: **Apply** keeps it open, **Apply and close** closes it

### Running with Claude

```bat
set ANTHROPIC_FOUNDRY_API_KEY=your_key_here
set ANTHROPIC_FOUNDRY_BASE_URL=https://your-endpoint.example.com/api/anthropic
installation\bin\LibraryExample.exe
```

### Running with Ollama

`examples/LibraryExample/main.cpp` picks the provider with `USED_DEMO`. The default is `DEMO_ANTHROPIC_FOUNDRY`; set it to `DEMO_OLLAMA` to use this constructor:

```cpp
#define USED_DEMO DEMO_OLLAMA
...
ChatWindow window(QtLLM::Provider::Ollama, "", "http://localhost:11434/api/chat", "gpt-oss:20b");
```

Change the model name to one you have installed. The provider can also be changed at run time in the Settings dialog.

Build and run. The application will:
1. Check if Ollama is running
2. Start `ollama serve` in the background if not
3. Populate the model combo box with locally installed models
4. Allow switching models and downloading new ones via **Manage Models…**

### Scripting demo

`ChatWindowScripting.cpp` shows what an application does to use [scripting](Scripting.md). It is compiled only when `QTLLM_HAS_SCRIPTING` is 1.

- **Methods:** the window's inventory table is the object `inventory` with `list`, `get` (read-only), `setQuantity`, `setLocation`, `add` and `remove`. `inventory.remove` is switched off and locked against the user. The tool `setWindowTitle` is exposed as `tools.setWindowTitle`, and `llm.ask` / `llm.choose` run on the chat's provider with `Effort::Low`.
- **Approval:** `Approval::ChangingScripts`; the user can change it in the Scripts window.
- **The window:** scripts get its keys and clicks (`forwardInput`), draw over it with `gfx` (`attachCanvas`) - this is how the model outlines, points at and animates things - and read and change its widgets with `ui.*` (`exposeWidgets`). The chat dock is hidden from `ui.*`.
- **Saved scripts:** "Restock low items" and "Inventory report" are shipped as read-only scripts; the user's own go to `scripts/` in the application data folder.
- **Scripts window:** the toolbar action **Scripts…** opens a `QtLLM::ScriptsWidget` in a window of its own, applying changes at once. The user's choices are kept in `QSettings`.
- **Demo buttons:** "Point at things" asks for outlines, arrows and a spotlight, which the model now draws with a script; the **Scripts** section of the "Assistant Demos" panel sends ready-made prompts to the chat: a loop over the inventory, a dry run first, a saved script, and a script that asks the model per item.

### Key source files

| File | Purpose |
|---|---|
| `ChatWindow.h/.cpp` | Main widget — chat display, input, stats panel, model bar |
| `ChatWindowScripting.cpp` | Scripting demo: inventory methods, shipped scripts, Scripts window |
| `ModelManagerDialog.h/.cpp` | Model browser and downloader dialog |
| `main.cpp` | Application entry point — selects provider |

---

## FoundryDemo — Console Demo

`examples/FoundryDemo/`

A minimal console application that:

1. Registers two tools: `add` (adds two numbers) and `get_weather` (returns a fixed temperature)
2. Sends a single prompt that exercises both tools in one turn
3. Prints the final response and exits

Useful as a minimal reference for non-GUI integration.

### Running

```bat
set ANTHROPIC_FOUNDRY_API_KEY=your_key_here
set ANTHROPIC_FOUNDRY_BASE_URL=https://your-endpoint.example.com/api/anthropic
installation\bin\FoundryDemo.exe
```

---

## Unit Tests

`unittests/QtLLMTest/`

Pure unit tests — no network calls required. Tests cover:

One `TST_*.h` per area in `unittests/QtLLMTest/tests/`, among them:

- `TST_Tool` — schema serialisation for Claude and OpenAI formats
- `TST_ClaudeProtocol`, `TST_OpenAiProtocol` — request body construction and answers
- `TST_Client` — tool registration, unregistration, history management
- `TST_Provider` — Ollama client construction, `toOpenAiApiObject` schema structure
- `TST_Agent`, `TST_AppInterface`, `TST_HarnessFeatures` — agents, the application interface, the chat widget
- `TST_Scripting`, `TST_ScriptsWidget` — scripting and the Scripts tab (in a build with `QTLLM_SCRIPTING=ON`)

Run via CTest:

```bat
ctest --test-dir build/x64-Debug --output-on-failure
```

Or run `QtLLMTest.exe` directly.

---

## Integration Tests

`unittests/IntegrationTest/`

Live tests that hit real API endpoints. Require environment variables:

```bat
set ANTHROPIC_FOUNDRY_API_KEY=your_key_here
set ANTHROPIC_FOUNDRY_BASE_URL=https://your-endpoint.example.com/api/anthropic
installation\bin\IntegrationTest.exe
```

Tests include a 30-second timeout per request and validate full round-trip tool-use loops.
