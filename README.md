# Rant Explorer

A cross platform Rant network explorer.

## Usage

```
./rant_explorer [--domain N] [--group IP] [--port N] [--if IP] [--name STR]
```

## Build

Requires CMake 3.21, a C compiler, and `git` with access to `KosmosisDire/Rant`. On Linux,
install the SDL build deps (e.g. `apt install libgl1-mesa-dev libx11-dev libxext-dev`).

```
cmake --preset windows
cmake --build --preset windows
```

Use `linux` on Linux. Executables land in `bin/`. CPM fetches the Rant release pinned in
`CMakeLists.txt` plus SDL3 and SDL3_ttf. The `windows-local` and `linux-local` presets
build against the `../Rant` working tree instead. Set `CPM_SOURCE_CACHE` to share the
downloads between build trees.

## Demo mesh

```
powershell -ExecutionPolicy Bypass -File .\run_demo.ps1 -Build -Explorer
```
