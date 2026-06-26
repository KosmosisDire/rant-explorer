# DART Explorer: cross-platform (Windows + Linux) build, Clay layout + SDL3.
#
# Nothing third-party is vendored. CMake fetches SDL3 + SDL3_ttf (built from
# source via FetchContent, FreeType vendored by SDL_ttf) plus the single-file
# Clay header, all pinned to immutable tags. The Clay->SDL3 renderer is vendored
# in ui_render.h. The DART single-header comes from ../dist.
#
# Builds two ways:
#   standalone:  cmake -S explore -B explore/build && cmake --build explore/build
#   from root:   cmake -S . -B build -DDART_BUILD_EXPLORER=ON && cmake --build build
# From the root it is an add_subdirectory of the top build (so its fetches land in
# the same build tree and dart_explorer drops in the repo root with the rest).

cmake_minimum_required(VERSION 3.19)
project(dart_explorer C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Standalone vs. pulled in by the root DART build. Standalone we own the output
# dir and define a minimal `dart` interface pointing at ../dist; under the root
# we inherit its output dir (repo root) and reuse its `dart` target, which
# carries the dist include and regenerates the single-headers as a dependency.
if(NOT TARGET dart)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
  get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
  add_library(dart INTERFACE)
  target_include_directories(dart INTERFACE "${REPO_ROOT}/dist")
endif()
set(DEPS "${CMAKE_BINARY_DIR}/deps")
file(MAKE_DIRECTORY "${DEPS}/include")

include(FetchContent)

# ------------------------------------------------------------------ single files
function(fetch url dest)
  if(NOT EXISTS "${dest}")
    message(STATUS "fetch ${dest}")
    file(DOWNLOAD "${url}" "${dest}" STATUS st TLS_VERIFY ON)
    list(GET st 0 code)
    if(NOT code EQUAL 0)
      file(REMOVE "${dest}")
      message(FATAL_ERROR "download failed: ${url}\n  ${st}")
    endif()
  endif()
endfunction()

# Clay layout header (pinned to a release tag). The SDL3 renderer is vendored in
# ui_render.h (we use only its rounded-rect + arc fills), so nothing else to fetch.
set(CLAY_RAW "https://raw.githubusercontent.com/nicbarker/clay/v0.14")
fetch("${CLAY_RAW}/clay.h" "${DEPS}/include/clay.h")

# nanosvg: single-header SVG parser + rasterizer. Lucide icons (SVG) are rasterized
# to RGBA textures at load and drawn via Clay's image path (tinted by theme color).
# zlib-licensed; SDL3_ttf's own SVG (PlutoSVG) is off and SDL3 has no native SVG.
set(NSVG_RAW "https://raw.githubusercontent.com/memononen/nanosvg/master/src")
fetch("${NSVG_RAW}/nanosvg.h"     "${DEPS}/include/nanosvg.h")
fetch("${NSVG_RAW}/nanosvgrast.h" "${DEPS}/include/nanosvgrast.h")

# Fonts are the OS system fonts (system-ui / ui-monospace), loaded by path at
# runtime to match the mockup per platform. Nothing to fetch.

# ----------------------------------------------------------------- SDL3 + SDL_ttf
# Built from source so the result is identical on Windows and Linux and needs no
# system packages. Static so the binary runs straight from the build dir.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(SDL_SHARED   OFF CACHE BOOL "" FORCE)
set(SDL_STATIC   ON  CACHE BOOL "" FORCE)
set(SDL_TEST         OFF CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES     OFF CACHE BOOL "" FORCE)
FetchContent_Declare(SDL3
  URL https://github.com/libsdl-org/SDL/releases/download/release-3.2.16/SDL3-3.2.16.tar.gz)
FetchContent_MakeAvailable(SDL3)

# SDL_ttf vendors FreeType from its own submodule (the release tarball omits it,
# so fetch via git and pull just external/freetype). HarfBuzz/PlutoSVG off keeps
# the build lean (no complex-script shaping or SVG glyphs), so no other submodule
# is needed.
set(SDLTTF_VENDORED ON  CACHE BOOL "" FORCE)
set(SDLTTF_HARFBUZZ OFF CACHE BOOL "" FORCE)
set(SDLTTF_PLUTOSVG OFF CACHE BOOL "" FORCE)
set(SDLTTF_SAMPLES  OFF CACHE BOOL "" FORCE)
FetchContent_Declare(SDL3_ttf
  GIT_REPOSITORY https://github.com/libsdl-org/SDL_ttf.git
  GIT_TAG release-3.2.2
  GIT_SUBMODULES external/freetype)
FetchContent_MakeAvailable(SDL3_ttf)

# ------------------------------------------------------------------------ target
# Two TUs on purpose: discovery_explorer.c is the SDL3/Clay UI; net_capture.c
# owns DART + winsock. SDL coexists with windows.h, but keeping the split is
# zero-cost and isolates the DART/winsock side.
add_executable(dart_explorer discovery_explorer.c net_capture.c)
target_include_directories(dart_explorer PRIVATE
  "${DEPS}/include" "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(dart_explorer PRIVATE SDL3::SDL3 SDL3_ttf::SDL3_ttf dart)

if(WIN32)
  target_link_libraries(dart_explorer PRIVATE ws2_32 bcrypt winmm)
else()
  target_link_libraries(dart_explorer PRIVATE m)
endif()

# regenerate dist/ before building when the root build owns that target
if(TARGET dist)
  add_dependencies(dart_explorer dist)
endif()
