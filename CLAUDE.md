# Claude Project Instructions

## Project Root

The directory containing this file is the project root and the working sandbox.

Claude Code may freely read, edit, create, delete, build, and test project files inside this directory.

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