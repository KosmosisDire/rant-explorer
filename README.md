# Rant Explorer

The Rant network explorer, written in C++ over the Rant C++ wrapper, with RmlUi laying
out the UI from RML and RCSS.

## Usage

```
./rant_explorer [--domain N] [--group IP] [--port N] [--if IP] [--name STR]
./rant_explorer --web-server [--port N] [--bind IP] [--log-file PATH]
```

`--web-server` opens no window. It serves the explorer as a web page, by default on port
8090 of every address, for any browser on the network. The page reaches the mesh through a
Rant bridge, which it looks for on port 7480 of the same host. Its plug button goes back
to the form to pick another bridge or domain. `rant explore web enable` runs the server as
a service.

A card's right click menu saves it as a PNG or records it as an MP4 or a GIF. On Linux the
save dialog needs `xdg-desktop-portal` or `zenity`.

`F12` toggles the RmlUi debugger and `F5` reloads the assets. A page leaves both to the
browser, with every other key it has no use for. `RANT_UI_ZOOM` scales the
whole UI. The exe carries its `assets/` embedded, so a copy runs on its own. An exe built
here reads the RML and RCSS from this tree's `assets/` instead, so an edit shows in the
running window. `RANT_UI_ASSETS` points at any other directory.

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

Use `linux` on Linux and `macos` on macOS. The executable lands in `bin/`. CPM fetches the
Rant release pinned in `CMakeLists.txt` plus FreeType, SDL3, RmlUi, the video decoders
(openh264, dav1d, libde265) and what records a card (the openh264 encoder, minimp4,
msf_gif), all built static from source. libde265 is LGPL 3. The
`-local` presets build against the `../rant` working tree instead. Set `CPM_SOURCE_CACHE`
to share the downloads between build trees.

The web page is its own build, with the emsdk in `EMSDK`. It lands in `bin/web`. A desktop
build embeds it for `--web-server` when `EXPLORER_WEB_DIR` points there:

```
cmake --preset web
cmake --build --preset web
cmake --preset windows -DEXPLORER_WEB_DIR=bin/web
cmake --build --preset windows
```

## Release

`.github/workflows/ci.yml` builds the page, then Windows, Linux and macOS with it embedded,
on every push. To release, bump `VERSION` and push the tag `v<VERSION>`: the workflow
attaches the exe of each platform and a zip of the page.

## Status

The Topics, Nodes and Mesh tabs: discovery, the node's meta and log, the topic tree with
subscriptions and each name's rate as its nodes report it, the value tree with editing,
publishing and calls, a visual card per field, and a graph of the nodes and the names
between them with their traffic.
