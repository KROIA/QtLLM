# Building QtLLM

## Prerequisites

| Dependency | Version | Where to get it |
|---|---|---|
| Qt | 5.15.x (default) or 6.x | [qt.io/download](https://www.qt.io/download) — select MSVC 2019 64-bit for Qt 5, MSVC 2022 64-bit for Qt 6. Qt 6 is chosen with `-DQT_MAJOR_VERSION=6` at configure; verified with 6.11 |
| OpenSSL | 1.1.1 for Qt 5, 3.x for Qt 6 | Run-time DLLs only, for https. The Qt Maintenance Tool installs them (Developer and Designer Tools > OpenSSL Toolkit), or [slproweb.com](https://slproweb.com/products/Win32OpenSSL.html) |
| CMake | 3.20+ | Bundled with Visual Studio 2022 |
| MSVC | 2019 / 2022 | Visual Studio with "Desktop development with C++" workload |

### OpenSSL

QtLLM does not call OpenSSL itself. Qt loads it at run time for https, and without it every request to Claude or another https server fails ("TLS initialization failed"); Ollama over http is not affected. Which OpenSSL depends on the Qt version:

| Qt | OpenSSL | Files |
|---|---|---|
| 5.15 | 1.1.1 | `libssl-1_1-x64.dll`, `libcrypto-1_1-x64.dll` |
| 6 | 3.x | `libssl-3-x64.dll`, `libcrypto-3-x64.dll` |

The build copies the pair next to the programs: into the build output folder and into `installation/bin`. It does that for the project being built, so an application that has QtLLM as a dependency gets them next to its own programs as well. Configure prints where it took them from:

```
-- QtLLM: OpenSSL for Qt 5 https from C:/Program Files/Git/mingw64/bin
```

They are looked for in this order: `QTLLM_OPENSSL_DIR`, Qt's OpenSSL toolkit (`C:/Qt/Tools/OpenSSL` for 1.1.1, `C:/Qt/Tools/OpenSSLv3` for 3.x), the Shining Light packages under Program Files, Git for Windows, `PATH`. When none has the pair, configure warns and copies nothing; the programs then use https only where OpenSSL happens to be on `PATH`.

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

## Options

| Option | Default | Effect |
|---|---|---|
| `QTLLM_SCRIPTING` | `ON` | Scripting ([Scripting.md](Scripting.md)). Fetches the [ScriptHost](https://github.com/KROIA/ScriptHost) library, which needs the Qt module **Qml**. `OFF` builds without both: there is no `QtLLM::Scripting` and `QTLLM_HAS_SCRIPTING` is 0 |
| `QTLLM_DEPLOY_OPENSSL` | `ON` | Copies the OpenSSL DLLs Qt needs for https next to the programs (see [OpenSSL](#openssl)). `OFF` for an application that ships OpenSSL its own way |
| `QTLLM_OPENSSL_DIR` | empty | The folder that holds the two OpenSSL DLLs, when they should not be looked for |

```bat
cmake --preset x64-Debug -DQTLLM_SCRIPTING=OFF
```

The option lives in `dependencies/ScriptHost.cmake`. To build against a local checkout of ScriptHost instead of the repository:

```bat
cmake --preset x64-Debug -DFETCHCONTENT_SOURCE_DIR_SCRIPTHOST="C:/path/to/ScriptHost"
```

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
