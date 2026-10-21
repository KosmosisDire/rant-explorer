# Rant Explorer

A GUI tool to browse and easily visualize anything on a Rant network. Runs native or on the
web through the Rant bridge.

![Topics with live cards for joints, a 3D pose, a camera and a lidar scan](docs/topics.png)

![The mesh of nodes and the names between them](docs/mesh.png)

![A node's status, link, runtime and log](docs/nodes.png)

## Install

```
rant install explorer
```

## Usage

```
rant explore                 # open the explorer
rant run WebExplorer         # serve it as a page on port 6732
```

The page reaches the network through a Rant bridge on the same host (`rant install bridge`,
then `rant enable Bridge`). With a bridge running, https://explore.rantlib.dev works too.

## Build

```
cmake --preset windows       # or linux, macos
cmake --build --preset windows
```

The web page builds with `cmake --preset web` and the emsdk in `EMSDK`. The Linux packages
it needs are listed in `.github/workflows/ci.yml`.
