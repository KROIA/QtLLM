# AI_Knowledge — QT cmake library template

> **Purpose of this file**: Single-page cheatsheet for AI assistants. Read this once and you have enough context about the project to be productive without re-scanning the tree. Last updated against template version **1.6.1** (see `CMakeLists.txt` line 8).

---

## 1. What this project is

A **Visual Studio / CMake / Qt C++ library template** (KROIA template, v1.6.1). Cloning + filling in placeholders gives you a ready-to-build C++ library with:

- Three build profiles per library: **shared**, **static**, **static + profiling** (the third only when `easy_profiler` dep is present).
- Builtin **dependency manager** built on `FetchContent` with three-tier resolution: local folder → cached download → git clone.
- Auto Qt5/Qt6 detection (`QT_INSTALL_BASE` defaults to `C:/Qt`).
- Optional integrations auto-activated by presence of dep `.cmake` files: **easy_profiler** (profiling macros) and **Logger** (singleton log instance).
- Example app folder + UnitTest folder, each auto-built per executable.
- `LibraryInfo` runtime metadata class (version/author/license/build type) with optional `QWidget` info display.
- Designed to be **upgradeable in-place** by an external code generator (see §6).

The **expected workflow** is to use the GUI tool [CmakeLibCreator](https://github.com/KROIA/CmakeLibCreator) to *create* a new library from this template or to *upgrade* an existing one. Manual editing is supported but error-prone.

Repo: `https://github.com/KROIA/QT_cmake_library_template`. Author: Alex Krieg <alexkrieg@gmx.ch>.

---

## 2. Directory layout

```
.
├── CMakeLists.txt          ← root: contains the <AUTO_REPLACED> settings
├── CMakePresets.json       ← Ninja x64 Debug/Release/+Profile presets
├── CMakeSettings.json      ← VS-specific equivalent of presets
├── build.bat               ← convenience: VS17 2022 x64 Debug+Release+install
├── cmake/
│   ├── utility.cmake       ← GLOB_FILES, smartDeclare, downloadStandardLibrary,
│   │                         downloadExternalLibrary, windeployqt, set_if_not_defined
│   ├── dependencies.cmake  ← scans dependencies/*.cmake, honors order.cmake
│   ├── ExampleMaster.cmake ← exampleMaster() — used by every example/unitTest
│   ├── QtLocator.cmake     ← finds Qt installation (QT_PATH, QT_PACKAGE_NAME=Qt5/Qt6)
│   └── Config.cmake.in     ← package config template (LibraryNameTargets.cmake)
├── core/                   ← THE LIBRARY
│   ├── CMakeLists.txt      ← creates ${LIBRARY_NAME}_shared / _static / _static_profile
│   ├── inc/                ← public headers (recursive glob *.h *.inl)
│   │   ├── LibraryName.h        ← single public header consumers include
│   │   ├── LibraryName_base.h   ← internal base (include in all your .h)
│   │   ├── LibraryName_global.h ← __declspec(dllimport/export), warning→error pragmas
│   │   ├── LibraryName_debug.h  ← console + profiling macros + Logger singleton
│   │   ├── LibraryName_info.h   ← LibraryInfo class (version, author, QWidget)
│   │   └── LibraryName_meta.h.in← @PROJECT_VERSION_*@ → configure_file → _meta.h
│   ├── src/                ← *.cpp (recursive glob)
│   └── resources/icons/    ← .qrc files (AUTORCC ON)
├── dependencies/
│   ├── order.cmake         ← getOrder() defines load order
│   ├── DependencyTemplate.cmake ← copy-this template
│   ├── Logger.cmake        ← optional dep; presence enables Logger code paths
│   ├── easy_profiler.cmake ← optional dep; presence enables 3rd build profile
│   └── (your .cmake files) ← drop new deps here
├── Examples/
│   ├── CMakeLists.txt      ← auto-add_subdirectory every child folder
│   └── LibraryExample/     ← copy this folder to make new examples
│       ├── CMakeLists.txt  ← calls exampleMaster(...)
│       ├── main.cpp
│       ├── AppIcon.ico, AppIcon.rc
└── unitTests/
    ├── CMakeLists.txt      ← like Examples/, plus include(UnitTest.cmake)
    ├── UnitTest.cmake      ← downloads KROIA/UnitTest as dep
    └── ExampleTest/        ← test executable; tests/TST_*.h files
```

The folder is **named after the library** when generated (e.g. `MyLibrary/`).

---

## 3. The placeholder system (REPLACE-ON-GENERATE)

The codegen tool [CmakeLibCreator](https://github.com/KROIA/CmakeLibCreator) replaces the following **literal string placeholders** throughout filenames *and* file contents. Order matters — `LibraryName` is a substring of `LibraryNamespace`, etc., so the tool processes them in this order:

| Placeholder (template) | Field set in tool          | Example value      |
| ---------------------- | -------------------------- | ------------------ |
| `LibraryNamespace`     | Namespace Name             | `Log`              |
| `LIBRARY_NAME_API`     | Export macro (+`_API` impl)| `LOGGER_API`       |
| `LibraryName`          | Library Name               | `Logger`           |
| `LIBRARY_NAME_LIB`     | LIB define                 | `LOGGER_LIB`       |
| `LIBRARY_NAME_SHORT`   | Short macro prefix         | `LOG`              |

(See `CmakeLibCreator/core/src/ProjectSettings.cpp` lines 7-13 for the canonical list.)

**Filename replacement** also applies: `LibraryName_base.h` → `Logger_base.h`, etc.

**Special root `CMakeLists.txt` lines** marked `# <AUTO_REPLACED>` are parsed and rewritten by the tool — keep the exact comment marker on the same line as the `set(...)`. Editable fields:

- `LIBRARY_NAME` (also drives `project(${LIBRARY_NAME})`)
- `LIBRARY_VERSION` (`"X.Y.Z"`, drives `LibraryName_VERSION_*` defines via `_meta.h.in`)
- `LIB_DEFINE`, `LIB_PROFILE_DEFINE`
- `QT_ENABLE`, `QT_DEPLOY`, `QT_MODULES` list
- `DEBUG_POSTFIX_STR` (`"-d"`), `STATIC_POSTFIX_STR` (`"-s"`), `PROFILING_POSTFIX_STR` (`"-p"`)
- `CMAKE_CXX_STANDARD`, `CMAKE_CXX_STANDARD_REQUIRED`
- `COMPILE_EXAMPLES`, `COMPILE_UNITTESTS`

Output binary names use those postfixes:
- `Logger.dll` / `Logger-d.dll` (shared release/debug)
- `Logger-s.lib` / `Logger-s-d.lib` (static)
- `Logger-s-p.lib` / `Logger-s-p-d.lib` (static + profiling)

---

## 4. USER_SECTION markers (DO-NOT-DELETE-ON-UPGRADE)

The template upgrade flow needs to preserve user code while replacing surrounding template code. Every template file is studded with numbered marker pairs:

```cmake
## USER_SECTION_START 1

## USER_SECTION_END
```

```cpp
/// USER_SECTION_START 1

/// USER_SECTION_END
```

> **Rules** (these are load-bearing — violating them breaks the upgrade tool):
> 1. **CMake** uses `## USER_SECTION_*`, **C++** uses `/// USER_SECTION_*` (triple-slash).
> 2. **Numbers identify the slot.** Do **not** renumber, add, or remove sections.
> 3. Numbers are NOT in source order — gaps and out-of-order numbers are intentional.
> 4. Put **all your custom code inside** these blocks. Anything outside them will be **overwritten** by the upgrade tool.
> 5. To "delete" template code, **comment it out using a USER_SECTION wrapper** rather than removing it. Pattern:
>    ```cpp
>    /// USER_SECTION_START 4
>    /*
>    /// USER_SECTION_END
>    ... template code ...
>    /// USER_SECTION_START 5
>    */
>    /// USER_SECTION_END
>    ```
> 6. **`///` (triple-slash) is reserved for `USER_SECTION` markers in C++ files.** Use `//` for any other comment. (User memory rule.)

---

## 5. Build system internals

### 5.1 Three build profiles in one configure (`core/CMakeLists.txt`)

```
PROFILES = shared static [static_profile]   # 3rd only if EASY_PROFILER_IS_AVAILABLE
```

For each profile, `core/CMakeLists.txt` creates a target named `${LIBRARY_NAME}_${profile}` with these compile defines:

| Profile          | Type   | Compile defs added                               | Output postfix     |
| ---------------- | ------ | ------------------------------------------------ | ------------------ |
| `shared`         | SHARED | `LIB_DEFINE BUILD_SHARED`                        | (none, e.g. `Logger`)|
| `static`         | STATIC | `LIB_DEFINE BUILD_STATIC`                        | `-s`               |
| `static_profile` | STATIC | `LIB_DEFINE BUILD_STATIC LIB_PROFILE_DEFINE`     | `-s-p`             |

Plus when Qt is on: `QT_ENABLED`, and `QT_WIDGETS_ENABLED` if `Widgets ∈ QT_MODULES`.

`AUTOMOC` and `AUTORCC` are set per-target. `.ui` files are **not** picked up by AUTOUIC — they are run through `qt_wrap_internal_ui` and the generated `ui_*.h` lands in `CMAKE_CURRENT_BINARY_DIR` (which is on the include path via `$<BUILD_INTERFACE:>`).

### 5.2 Dependency loading (`cmake/dependencies.cmake` + `cmake/utility.cmake`)

`dependencies.cmake` globs every `*.cmake` in `dependencies/` (skipping `order.cmake`). It then reorders them per `getOrder()` from `order.cmake` and `include()`s each one in order. Each dep `.cmake` defines a `dep(...)` function and immediately invokes it.

Two helper macros (in `cmake/utility.cmake`):

- **`downloadStandardLibrary()`** — for libs **built with this template**. Auto-discovers `<src>/core/inc`, links `${LIB_NAME}_shared` / `_static` / `_static_profile` per profile, falls back from `_static_profile` to `_static` if absent.
- **`downloadExternalLibrary()`** — for libs **not** built with this template. You set `SHARED_LIB_DEPENDENCY`, `STATIC_LIB_DEPENDENCY`, `STATIC_PROFILE_LIB_DEPENDENCY` (CMake target names to link). Auto-discovers `<src>/include` if present.

Both honor the **three-tier resolution**:
1. `USE_LOCAL_DEPENDENCIES=ON` + folder `<LOCAL_DEPENDENCIES_PATH>/<LIB_NAME>/` exists → use as `SOURCE_DIR`.
2. Else if `<FETCHCONTENT_BASE_DIR>/<lib_name>-src/` exists → reuse cached source (no network).
3. Else → `FetchContent_Declare(GIT_REPOSITORY GIT_TAG)` and download.

`LOCAL_DEPENDENCIES_PATH` resolves relative to `CMAKE_SOURCE_DIR` (project root), not the build dir. Setting `USE_LOCAL_DEPENDENCIES=ON -DLOCAL_DEPENDENCIES_PATH="../"` and arranging sibling folders is the recommended local-dev workflow.

`FETCHCONTENT_BASE_DIR` is forced to `${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/dependencies/cache`. A sentinel file `${CMAKE_BINARY_DIR}/.deps_cache_stamp` lets "Delete Cache and Reconfigure" wipe the dep cache; plain reconfigure reuses it (`FETCHCONTENT_UPDATES_DISCONNECTED ON`).

`DEPENDENCY_NAME_MACRO` is a CACHE STRING that accumulates *availability macros* (e.g. `LOGGER_LIBRARY_AVAILABLE`, `EASY_PROFILER_LIBRARY_AVAILABLE`). It's reset only at the top-level project; child projects loaded via FetchContent append. The library reads these macros to enable optional code paths (e.g. `#if LOGGER_LIBRARY_AVAILABLE == 1 #include "Logger.h" #endif`).

If `LIB_MACRO_NAME` is not set in a dep `.cmake`, it's **auto-generated** as `<LIB_NAME_UPPER_WITH_DASHES_TO_UNDERSCORES>_LIBRARY_AVAILABLE`. e.g. `imgui-sfml` → `IMGUI_SFML_LIBRARY_AVAILABLE`.

### 5.3 Templates for new dep `.cmake` files

```cmake
## description: Tooltip text shown in CmakeLibCreator
function(dep LIBRARY_MACRO_NAME SHARED_LIB STATIC_LIB STATIC_PROFILE_LIB INCLUDE_PATHS)
    set(LIB_NAME       Logger)
    set(LIB_MACRO_NAME LOGGER_LIBRARY_AVAILABLE)   # optional; auto-generated otherwise
    set(GIT_REPO       https://github.com/KROIA/Logger.git)
    set(GIT_TAG        main)
    set(NO_EXAMPLES    True)
    set(NO_UNITTESTS   True)
    set(ADDITIONAL_INCLUDE_PATHS )
    downloadStandardLibrary()    # or downloadExternalLibrary() for non-template libs
endfunction()
dep(DEPENDENCY_NAME_MACRO
    DEPENDENCIES_FOR_SHARED_LIB
    DEPENDENCIES_FOR_STATIC_LIB
    DEPENDENCIES_FOR_STATIC_PROFILE_LIB
    DEPENDENCIES_INCLUDE_PATHS)
```

`NO_EXAMPLES` / `NO_UNITTESTS` set `${LIB_NAME}_NO_EXAMPLES` / `${LIB_NAME}_NO_UNITTESTS`, which the dep's own root `CMakeLists.txt` honors to skip its own examples/unittests subdirs.

### 5.4 Examples / UnitTests (`cmake/ExampleMaster.cmake`)

Each `Examples/*/CMakeLists.txt` and `unitTests/*/CMakeLists.txt` ends with a single call to `exampleMaster(...)`. The function:
- Sets `PROJECT_NAME` from the **folder name** (so renaming the folder renames the executable).
- Appends `_profile` suffix to the project name when the profile build is selected.
- Globs `*.h *.cpp *.c *.inl *.ui *.qrc` recursively.
- Links the parent library's `_static` (or `_static_profile`) target — examples never link the shared lib.
- Adds `BUILD_STATIC` define + every `${DEPENDENCY_NAME_MACRO}` so consumer code can `#ifdef LOGGER_LIBRARY_AVAILABLE` etc.
- Calls `windeployqt` (twice — once into `INSTALL_BIN_PATH`, once into `CMAKE_RUNTIME_OUTPUT_DIRECTORY`) when `QT_ENABLE && QT_DEPLOY`.

> ⚠️ Don't change the first 2 args to `exampleMaster`: they're fixed as `${LIBRARY_NAME} ${LIB_PROFILE_DEFINE}`.

UnitTests additionally `include(UnitTest.cmake)` once at the `unitTests/CMakeLists.txt` level, which fetches [KROIA/UnitTest](https://github.com/KROIA/UnitTest) and exposes the `UnitTest_static` target. Each test's `CMakeLists.txt` does `list(APPEND ADDITIONAL_LIBRARIES UnitTest_static)` before calling `exampleMaster`. Tests inherit the macros `TEST_CLASS`, `ADD_TEST`, `TEST_FUNCTION`, `TEST_START`, `TEST_ASSERT[_M]`, `TEST_FAIL`, `TEST_COMPARE`, `TEST_INSTANTIATE` from the UnitTest library.

### 5.5 Qt detection (`cmake/QtLocator.cmake`)

Honors these CMake variables (settable from preset/cmd line):
- `QT_INSTALL_BASE` (default `C:/Qt`)
- `QT_MAJOR_VERSION` (default `5`)
- `QT_VERSION` (`"autoFind"` picks newest matching MAJOR)
- `QT_COMPILER` (`"autoFind"` picks newest `msvcXXXX*` folder)

Sets `QT_PATH`, `QT_PACKAGE_NAME = Qt${QT_MAJOR_VERSION}`. Provides version-agnostic wrappers `qt_wrap_internal_cpp`, `qt_wrap_internal_ui`, `qt_add_internal_resources`. easy_profiler forces Qt5 because it's incompatible with Qt6 (see `dependencies/easy_profiler.cmake`).

### 5.6 Install layout

When this project is the **top-level** CMake project, headers from `core/inc/` are copied to `<INSTALL_PREFIX>/include/<LIBRARY_NAME>/`, all three lib targets are installed to `<INSTALL_PREFIX>/lib/`, executables to `<INSTALL_PREFIX>/bin/`, and the generated `<LIBRARY_NAME>_meta.h` is installed beside the public headers. When loaded as a sub-project via `FetchContent`, install commands are skipped — the parent owns its own install.

---

## 6. The codegen tool: CmakeLibCreator

Path on this system: `C:\Users\KRIA\Documents\Visual Studio 2022\Projects\CmakeLibCreator`. Built from this same template (it's a Qt application).

Three operations:
1. **Create** — Download latest template files from a configurable Git repo (default = KROIA/QT_cmake_library_template), apply the placeholder replacements (§3) + filename renames, and write the result to a chosen folder. The output folder name **becomes the library name** (e.g. `…/Libraries/Logger`).
2. **Edit** — Open an existing library, change settings (Qt modules, dependencies, postfixes…), save back. Touches only the lines flagged `<AUTO_REPLACED>` and the dependency `.cmake` files in `dependencies/`.
3. **Upgrade** — Re-download latest template files, then for each template file: extract the user's `USER_SECTION_*` content, copy the new template skeleton over the user's file, and re-inject the user content into the matching numbered slots. **Always run on a clean git working tree** so you can `git restore` if the parser misbehaves.

Available dep `.cmake` files surfaced in the tool's UI come from the `dependencies` branch of the template repo: `https://github.com/KROIA/QT_cmake_library_template/tree/dependencies`.

---

## 7. C++ public API surface

Always inside `namespace LibraryNamespace { ... }`. All exported types must be tagged `LIBRARY_NAME_API`.

### 7.1 `LibraryInfo` (in `LibraryName_info.h`)
Non-instantiable. Compile-time constants populated from `_meta.h` (auto-generated from `LIBRARY_VERSION`):

```cpp
struct Version { int major, minor, patch; /* full ordering operators, .toString() = "MM.mm.pppp" */ };
static constexpr Version version{ versionMajor, versionMinor, versionPatch };
static constexpr const char* name, author, email, website, license;
static constexpr const char* compiler, compilerVersion, compilationDate, compilationTime, buildTypeStr;
enum class BuildType { debug, release }; static constexpr BuildType buildType;

static void printInfo();
static void printInfo(std::ostream&);
static std::string getInfoStr();
static QWidget* createInfoWidget(QWidget* parent = nullptr, bool disableHyperlink = false);
//   ↑ returns nullptr unless QT_ENABLED && QT_WIDGETS_ENABLED at compile time.
```

Edit `author/email/website/license` directly in `LibraryName_info.h`. **Do not** edit version constants — change `LIBRARY_VERSION` in root `CMakeLists.txt` and reconfigure.

### 7.2 `Profiler` (in `LibraryName_debug.h`)
```cpp
class LIBRARY_NAME_API Profiler {
    static void start();
    static void stop();                              // writes "profile.prof"
    static void stop(const char* profilerOutputFile);
};
```
No-op except in the `static_profile` build target.

### 7.3 `Logger` (in `LibraryName_debug.h`, conditional on `LOGGER_LIBRARY_AVAILABLE`)
Singleton wrapping a `Log::LogObject`. All static API:
```cpp
setEnabled / isEnabled / setName / getName / setColor / getColor / getCreationDateTime
getID / setParentID / getParentID
log(Message) / log(string [,Level [,Color]])
logTrace / logDebug / logInfo / logWarning / logError / logCustom
```
For per-class loggers, use `Log::LogObject custom("name"); custom.setParentID(Logger::getParentID());`.

### 7.4 Macro toolbox (in `LibraryName_debug.h`)

**Console (Debug-only, compile to nothing in Release):**
- `LIBRARY_NAME_SHORT_CONSOLE(msg)` → `std::cout << msg;`
- `LIBRARY_NAME_SHORT_CONSOLE_FUNCTION(msg)` → prepends `__PRETTY_FUNCTION__`
- `LIBRARY_NAME_SHORT_CONSOLE_STREAM` (= `std::cout`)
- `LIBRARY_NAME_SHORT_DEBUG` (defined only in Debug)

**Profiling (real only when `LIBRARY_NAME_SHORT_PROFILING` is defined):**
- `..._PROFILING_BLOCK(text, colorStage)`, `..._PROFILING_BLOCK_C(text, color)`
- `..._PROFILING_NONSCOPED_BLOCK[…]` + `..._PROFILING_END_BLOCK`
- `..._PROFILING_FUNCTION[_C](colorStage|color)`
- `..._PROFILING_THREAD(name)`
- `..._PROFILING_VALUE(name, scalar)`, `..._PROFILING_TEXT(name, str)`

**Color stages:** `LIBRARY_NAME_SHORT_COLOR_STAGE_1..14` map to easy_profiler color suffixes (`50, 100, 200, ..., 900, A100, A200, A400, A700`). Wrap with a per-section colorbase (default `Cyan`) to get distinct visual sections — see `LIBRARY_NAME_SHORT_GENERAL_PROFILING_*` definitions and copy the block into USER_SECTION 3 of `LibraryName_debug.h` to make a custom section (e.g. `MY_LIB_TASK_PROFILING_*` with `Red`).

**Misc:**
- `LIBRARY_NAME_API` — DLL import/export switch (or empty in static).
- `LIBRARY_NAME_SHORT_UNUSED(x)` — `(void)x;` to silence W4100.
- `TimePoint` — typedef'd to `std::chrono::steady_clock::time_point` on MSVC, `system_clock::time_point` elsewhere.
- `__PRETTY_FUNCTION__` — defined to `__FUNCSIG__` on MSVC.

### 7.5 Promoted MSVC warnings → errors (in `LibraryName_global.h`, only when `LIBRARY_NAME_LIB` is defined)

`C4715, C4700, C4244, C4100, C4018, C4996, C4456, C4065, C4189, C4172`. To disable, comment out the block via the surrounding `USER_SECTION 4 / 5` (use the comment-wrap trick from §4).

---

## 8. How to write your own library code

```cpp
// MyClass.h — every library header starts like this:
#pragma once
#include "LibraryName_base.h"      // pulls global, debug, info

namespace LibraryNamespace
{
    class LIBRARY_NAME_API MyClass        // <- tag every exported class
    {
    public:
        MyClass();
        void printMessage();
    };
}
```

```cpp
// MyClass.cpp
#include "MyClass.h"
namespace LibraryNamespace {
    MyClass::MyClass() {}
    void MyClass::printMessage() { Logger::logInfo("Hello from MyClass"); }
}
```

Then **expose the header** by editing `core/inc/LibraryName.h`:
```cpp
/// USER_SECTION_START 2
#include "MyClass.h"
/// USER_SECTION_END
```

> **When you create new files in VS, decline its prompt to add them to the source list.** CMake globs the folder; manual additions break the glob.
> After adding files, **reconfigure** CMake (otherwise globs aren't re-evaluated).
> Header subfolders inside `core/inc/` are fine — globs are recursive (`GLOB_RECURSE` with `CONFIGURE_DEPENDS`).

---

## 9. Build & run

### CLI (Visual Studio 2022 generator):
```bash
./build.bat                     # builds Debug + Release into build/, installs to installation/
```

### CMake presets (`CMakePresets.json`, Ninja):
```bash
cmake --preset x64-Debug        # also: x64-Release, x64-Debug-Profile, x64-Release-Profile
cmake --build --preset x64-Debug
```
Profile presets define `LIBRARY_NAME_SHORT_PROFILING=1` — **note** when manually creating presets, this define name has to be regenerated to match `LIB_PROFILE_DEFINE` from your library (e.g. `LOG_PROFILING` for the Logger library). `CMakeSettings.json` mirrors the same configurations for VS.

### Visual Studio:
Open the folder in VS 2022 → CMake project loads automatically → pick a configuration → **Build → Install <LibraryName>** to populate `installation/` and trigger `windeployqt`.

### Common gotchas
| Symptom | Fix |
| --- | --- |
| `Could not find package Qt5` / `Qt5Config.cmake` missing | Set `QT_INSTALL_BASE` to your Qt root, or install Qt to `C:\Qt`. |
| `Can't find QT installation. Path: C:/Qt/5.1.1 does not exist` | The exact `QT_VERSION` in CMakeLists isn't installed → install it or set to `"autoFind"`. |
| Missing `Qt5Core.dll` etc. at runtime | Run **Build → "Install"** (triggers `windeployqt`). |
| FetchContent download fails sporadically | Delete CMake cache and reconfigure 1-2 times. |
| VS won't reconfigure / weird state | Close VS, delete `.vs/` and `build/`, reopen. |
| New file not picked up | Reconfigure CMake. Globs use `CONFIGURE_DEPENDS` but VS doesn't always notice. |

---

## 10. Reference — every file at a glance

### CMake helpers (`cmake/utility.cmake`)
| Symbol | Purpose |
| --- | --- |
| `GLOB_FILES(var ext)` | `FILE(GLOB_RECURSE var CONFIGURE_DEPENDS <ext>)` |
| `set_if_not_defined(var val)` | Idempotent set, used with cache vars |
| `windeployqt(targetName outputPath)` | Wraps `windeployqt.exe` with stable flags |
| `copyLibraryHeaders(root dest folder)` | `install(DIRECTORY ...)` for `*.h *.hpp` |
| `smartDeclare(LIB_NAME GIT_REPO GIT_TAG)` | Plain `FetchContent_Declare` with local/cache/git fallback (no MakeAvailable) |
| `downloadStandardLibrary()` | Full pipeline for template-built deps; sets `${LIB_NAME}_*` profile lists |
| `downloadExternalLibrary()` | Full pipeline for non-template deps; explicit `*_LIB_DEPENDENCY` target names |

Cache options exposed:
- `USE_LOCAL_DEPENDENCIES` (BOOL, default OFF)
- `LOCAL_DEPENDENCIES_PATH` (PATH, may be relative to source root)
- `FETCHCONTENT_UPDATES_DISCONNECTED` (forced ON in top-level)

### Variables produced by `downloadStandardLibrary` / `downloadExternalLibrary`
Accumulated and propagated to PARENT_SCOPE:
- `DEPENDENCY_NAME_MACRO` — `;`-separated availability macros
- `DEPENDENCIES_FOR_SHARED_LIB` / `..._STATIC_LIB` / `..._STATIC_PROFILE_LIB`
- `DEPENDENCIES_INCLUDE_PATHS`

### Variables consumed by `core/CMakeLists.txt`
- `LIBRARY_NAME`, `LIBRARY_VERSION`, `LIB_DEFINE`, `LIB_PROFILE_DEFINE`
- `QT_ENABLE`, `QT_DEPLOY`, `QT_MODULES`
- `DEBUG_POSTFIX_STR`, `STATIC_POSTFIX_STR`, `PROFILING_POSTFIX_STR`
- `EASY_PROFILER_IS_AVAILABLE` (set by `easy_profiler.cmake`)
- `USER_SPECIFIC_DEFINES` (private to library), `USER_SPECIFIC_GLOBAL_DEFINES` (added via `add_compile_definitions` for the whole tree)
- `LibraryName_NO_EXAMPLES`, `LibraryName_NO_UNITTESTS` (set by parent project to skip subdirs)

### Files generated at configure time
| Source | Generated path | Trigger |
| --- | --- | --- |
| `core/inc/LibraryName_meta.h.in` | `<build>/core/LibraryName_meta.h` | always (configure_file @ONLY) |
| `cmake/Config.cmake.in` | `<LIBRARY_NAME>Config.cmake` | install path package config |
| `*.ui` | `<build>/core/ui_*.h` | via `qt_wrap_internal_ui` |

---

## 11. Template-related conventions to remember when editing as an AI

1. **Don't remove or renumber `USER_SECTION` markers.** Place new content inside them. Use the comment-wrap pattern to "delete" template code.
2. **Don't remove `# <AUTO_REPLACED>` comments** — they mark CMake lines the codegen tool rewrites.
3. **Use `//` for normal C++ comments. `///` is reserved for `USER_SECTION` markers** (project convention; see memory file).
4. **Don't manually edit `LibraryName_meta.h`** (only the `.in` template).
5. New library files go into `core/inc/` and `core/src/`. **Don't add them to CMake manually** — globs handle them. Reconfigure after creating.
6. Every public type needs `LIBRARY_NAME_API`. Every public header should `#include "LibraryName_base.h"` (or `LibraryName_global.h` if you don't need debug/info).
7. Public consumers `#include "LibraryName.h"` (single header). Add new public headers via USER_SECTION 2 in `LibraryName.h`.
8. Examples and unit tests link the `_static` (or `_static_profile`) target — never the shared one.
9. Adding a dependency = drop a `.cmake` file in `dependencies/` (and optionally update `order.cmake`), then **delete CMake cache + reconfigure** so it downloads.
10. The "build" folder and "installation" folder are ignored in git and are always safe to delete.
11. The template is **upgrade-aware**: template files will be replaced wholesale by the codegen tool, and only `USER_SECTION_*` content survives. Anything you want to persist across upgrades belongs inside a USER_SECTION.

---

## 12. Useful external links

- Template repo: https://github.com/KROIA/QT_cmake_library_template
- CmakeLibCreator (codegen tool): https://github.com/KROIA/CmakeLibCreator
- KROIA/Logger dependency: https://github.com/KROIA/Logger
- KROIA/UnitTest dependency: https://github.com/KROIA/UnitTest
- easy_profiler: https://github.com/yse/easy_profiler
- Dependency `.cmake` files registry: https://github.com/KROIA/QT_cmake_library_template/tree/dependencies
