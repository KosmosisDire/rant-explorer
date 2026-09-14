# Ramble Explorer

A cross-platform Ramble network explorer. 

## Usage

```
./ramble_explorer [--domain N] [--group IP] [--port N] [--if IP] [--name STR]
```

## Build

Requires CMake, a C compiler, and `git`. On Linux, install the SDL build deps
(e.g. `apt install libgl1-mesa-dev libx11-dev libxext-dev`).

```
cmake -S explore -B explore/build
cmake --build explore/build
```
