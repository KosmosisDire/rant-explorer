# Rant Explorer (C++)

The Rant network explorer, rewritten in C++ over the Rant C++ wrapper, with RmlUi laying
out the UI from RML and RCSS. It replaces `../explorer-rant`, which is the C original over
Clay and SDL3.

## Usage

```
./rant_explorer [--domain N] [--group IP] [--port N] [--if IP] [--name STR]
```

`F12` toggles the RmlUi debugger and `F5` reloads the assets. `RANT_UI_ZOOM` scales the
whole UI. An exe built here reads the RML and RCSS from this tree's `assets/`, so an edit
shows in the running window. A copied exe reads the `assets/` beside it, and
`RANT_UI_ASSETS` points at any other directory.

## Build

Requires CMake 3.21, a C++17 compiler, and `git` with access to `KosmosisDire/Rant`. On
Linux, install the SDL build deps, X11 and Wayland both, so one build runs on either:

```
apt install build-essential cmake ninja-build git pkg-config libgl1-mesa-dev \
  libegl1-mesa-dev libx11-dev libxext-dev libxrandr-dev libxi-dev libxcursor-dev \
  libxfixes-dev libxkbcommon-dev libxss-dev libxtst-dev libwayland-dev wayland-protocols
```

```
cmake --preset windows
cmake --build --preset windows
```

Use `linux` on Linux and `macos` on macOS. Executables land in `bin/`, with `assets/`
copied beside them. CPM fetches the Rant release pinned in `CMakeLists.txt` plus FreeType,
SDL3, RmlUi and the video decoders (openh264, dav1d, libde265), all built static from
source. libde265 is LGPL 3. The `-local` presets build against the `../rant` working tree
instead. Set `CPM_SOURCE_CACHE` to share the downloads between build trees.

## Release

`.github/workflows/ci.yml` builds Windows, Linux and macOS on every push. To release, bump
`VERSION` and push the tag `v<VERSION>`: the workflow attaches one zip per platform, the
exe with its `assets/`. The macOS build is unsigned, so clear the quarantine flag with
`xattr -dr com.apple.quarantine rant-explorer` before the first run.

## Status

The Nodes tab and the Topics tab: discovery, the node's meta and log, the topic tree with
subscriptions, the value tree with editing, publishing and calls, and a visual card per
field.
