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

## Releases

`VERSION` holds the explorer's version, its own series, independent of the Rant release
`CMakeLists.txt` pins. Pushing a tag `v<VERSION>` builds Windows and Linux and attaches a
zip per platform to a GitHub Release. The workflow refuses a tag that does not match
`VERSION`.

```
git tag v0.0.1 && git push origin v0.0.1
```

CI clones Rant, which is public, so no credential is needed. Should it go private again,
set a `RANT_TOKEN` secret that can read `KosmosisDire/Rant` and the workflows pick it up.
