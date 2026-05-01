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

### Running with Claude

```bat
set ANTHROPIC_FOUNDRY_API_KEY=your_key_here
set ANTHROPIC_FOUNDRY_BASE_URL=https://your-endpoint.example.com/api/anthropic
installation\bin\LibraryExample.exe
```

### Running with Ollama

Edit `examples/LibraryExample/main.cpp` to use the Ollama constructor (already the default):

```cpp
ChatWindow window(QtLLM::Provider::Ollama, "", "http://localhost:11434/api/chat", "llama3.2");
```

Build and run. The application will:
1. Check if Ollama is running
2. Start `ollama serve` in the background if not
3. Populate the model combo box with locally installed models
4. Allow switching models and downloading new ones via **Manage Models…**

### Key source files

| File | Purpose |
|---|---|
| `ChatWindow.h/.cpp` | Main widget — chat display, input, stats panel, model bar |
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

- `TST_Tool` — schema serialisation for Claude and OpenAI formats
- `TST_ClaudeProtocol` — request body construction
- `TST_Client` — tool registration, unregistration, history management
- `TST_Provider` — Ollama client construction, `toOpenAiApiObject` schema structure

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
