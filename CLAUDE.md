# Claude Project Instructions

## Project Root

The directory containing this file is the project root and the working sandbox.

Claude Code may freely read, edit, create, delete, build, and test project files inside this directory.

## Project Background

QtLLM is built on the KROIA QT_cmake_library_template (v1.6.2). How that
template's CMake layout, USER_SECTION markers, and dependency `.cmake` files
work is documented in `.claude/Knowledge/QT_cmake_library_template.md` — read it
before changing anything under `cmake/` or `dependencies/`.

The original v1.0.0 design proposal is kept at
`documentation/design-history/ProjectDescriptionAndRequirements.md` as a
historical record. It predates the implementation and has drifted from what
shipped — treat `core/inc/` and `documentation/API.md` as authoritative.

## Development Environment

This project may use:

- C++
- Qt
- CMake
- Visual Studio 2022
- MSVC
- Ninja
- MSBuild
- CTest
- Git

Prefer out-of-source builds, for example:

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure