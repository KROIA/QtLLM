# Building QtLLM

## Prerequisites

| Dependency | Version | Where to get it |
|---|---|---|
| Qt | 5.15.x | [qt.io/download](https://www.qt.io/download) — select MSVC 2019 64-bit |
| OpenSSL | 1.1.1x | [slproweb.com](https://slproweb.com/products/Win32OpenSSL.html) — *Win64 OpenSSL v1.1.1w Light* |
| CMake | 3.20+ | Bundled with Visual Studio 2022 |
| MSVC | 2019 / 2022 | Visual Studio with "Desktop development with C++" workload |

> **OpenSSL version matters.** Qt 5.15 requires OpenSSL **1.1.x** at runtime for HTTPS.
> OpenSSL 3.x (`libssl-3-x64.dll`) is not compatible and will cause TLS errors.
> After installing OpenSSL 1.1.1w, delete `build/` and reconfigure — the build system
> locates and copies the DLLs automatically.

---

## Building from Visual Studio 2022

1. Open Visual Studio 2022
2. **File → Open → Folder…** — select the project root
3. Visual Studio detects `CMakePresets.json` and shows the configuration picker
4. Select **x64-Debug** or **x64-Release**
5. **Build → Build All** (or press `Ctrl+Shift+B`)
6. **Build → Install QtLLM** to copy outputs to `installation/`

This is the recommended workflow. The VS environment automatically sets up MSVC
include and library paths, so all targets build cleanly.

---

## Building from the Command Line

Open a **Developer Command Prompt for VS 2022** (important — sets MSVC environment):

```bat
:: Configure
cmake --preset x64-Debug

:: Build
cmake --build --preset x64-Debug

:: Install
cmake --install build/x64-Debug
```

A convenience script that builds both configurations and installs:

```bat
build.bat
```

Outputs land in `installation/bin/` and `installation/lib/`.

---

## CMake Presets

| Preset | Config | Output |
|---|---|---|
| `x64-Debug` | Debug, Ninja | `build/x64-Debug/` |
| `x64-Release` | Release, Ninja | `build/x64-Release/` |

---

## Project Targets

| Target | Type | Description |
|---|---|---|
| `QtLLM_shared` | DLL | Shared library (`QtLLM-d.dll` / `QtLLM.dll`) |
| `QtLLM_static` | LIB | Static library |
| `LibraryExample` | EXE | Qt Widgets chat window demo |
| `FoundryDemo` | EXE | Console demo with tool use |
| `QtLLMTest` | EXE | Unit tests (no network) |
| `IntegrationTest` | EXE | Live API tests |

---

## Linking Against the Library

After installing, add to your `CMakeLists.txt`:

```cmake
find_package(QtLLM REQUIRED)
target_link_libraries(MyApp PRIVATE QtLLM::QtLLM)
```

Or link the DLL/LIB directly and add `installation/include` to your include path.
Include the single umbrella header:

```cpp
#include <QtLLM.h>
```
