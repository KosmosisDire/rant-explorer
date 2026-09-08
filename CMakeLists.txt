# The explorer: Clay layout plus SDL3, with SDL3, SDL3_ttf and clay.h fetched at pinned
# tags and DART from ../dist. Standalone or from the root (spec/explorer.md).

cmake_minimum_required(VERSION 3.19)
project(dart_explorer C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Static CRT (see the module): a copied exe must run with no VC++ redistributable.
# Before FetchContent, so SDL3 and vendored freetype build against the same runtime.
include("${CMAKE_CURRENT_SOURCE_DIR}/../tools/static_runtime.cmake")

# Standalone: own the output dir and define a minimal dart target over ../dist. Under the
# root build the dart target already exists.
if(NOT TARGET dart)
  if(CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_CONFIGURATION_TYPES "Release;Debug;RelWithDebInfo;MinSizeRel"
        CACHE STRING "Build configs (Release first = the default)" FORCE)
  endif()
  get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
  # land binaries in bin/ like the root build, every config shares it (docs/building.md)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${REPO_ROOT}/bin")
  foreach(_cfg ${CMAKE_CONFIGURATION_TYPES})
    string(TOUPPER "${_cfg}" _CFG)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${_CFG} "${REPO_ROOT}/bin")
  endforeach()
  add_library(dart INTERFACE)
  target_include_directories(dart INTERFACE "${REPO_ROOT}/dist")
endif()
set(DEPS "${CMAKE_BINARY_DIR}/deps")
file(MAKE_DIRECTORY "${DEPS}/include")

include(FetchContent)

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

# Clay v0.14 hard codes its scroll container table to 10 entries and every clip container
# counts, so a form overflows it (spec/explorer.md). Bump the cap, idempotent.
file(READ "${DEPS}/include/clay.h" _clay_src)
string(REPLACE
  "context->scrollContainerDatas = Clay__ScrollContainerDataInternalArray_Allocate_Arena(10, arena)"
  "context->scrollContainerDatas = Clay__ScrollContainerDataInternalArray_Allocate_Arena(256, arena)"
  _clay_patched "${_clay_src}")
if(NOT _clay_patched STREQUAL _clay_src)
  file(WRITE "${DEPS}/include/clay.h" "${_clay_patched}")
  message(STATUS "patched clay.h: scroll-container capacity 10 -> 256")
endif()

# Clay_UpdateScrollContainers dereferences a null clipConfig for a transient clip that
# vanished this frame. Guard it, since a non clip element cannot scroll.
file(READ "${DEPS}/include/clay.h" _clay_src)
string(REPLACE
  "bool canScrollVertically = clipConfig->vertical"
  "bool canScrollVertically = clipConfig && clipConfig->vertical"
  _clay_patched "${_clay_src}")
string(REPLACE
  "bool canScrollHorizontally = clipConfig->horizontal"
  "bool canScrollHorizontally = clipConfig && clipConfig->horizontal"
  _clay_patched "${_clay_patched}")
if(NOT _clay_patched STREQUAL _clay_src)
  file(WRITE "${DEPS}/include/clay.h" "${_clay_patched}")
  message(STATUS "patched clay.h: NULL-guard clipConfig in Clay_UpdateScrollContainers")
endif()

# nanosvg: rasterizes the Lucide SVG icons to RGBA textures (SDL3 has no native SVG).
set(NSVG_RAW "https://raw.githubusercontent.com/memononen/nanosvg/master/src")
fetch("${NSVG_RAW}/nanosvg.h"     "${DEPS}/include/nanosvg.h")
fetch("${NSVG_RAW}/nanosvgrast.h" "${DEPS}/include/nanosvgrast.h")

# Fonts are the OS system fonts (system-ui / ui-monospace), loaded by path at
# runtime to match the mockup per platform. Nothing to fetch.

# SDL3 + SDL_ttf, built from source, static (same result on Windows/Linux, no packages).
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(SDL_SHARED   OFF CACHE BOOL "" FORCE)
set(SDL_STATIC   ON  CACHE BOOL "" FORCE)
set(SDL_TEST         OFF CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES     OFF CACHE BOOL "" FORCE)
FetchContent_Declare(SDL3
  URL https://github.com/libsdl-org/SDL/releases/download/release-3.2.16/SDL3-3.2.16.tar.gz)
FetchContent_MakeAvailable(SDL3)

# SDL_ttf vendors FreeType from its git submodule, which the tarball omits. HarfBuzz and
# PlutoSVG off keep it lean, so only external/freetype is pulled.
set(SDLTTF_VENDORED ON  CACHE BOOL "" FORCE)
set(SDLTTF_HARFBUZZ OFF CACHE BOOL "" FORCE)
set(SDLTTF_PLUTOSVG OFF CACHE BOOL "" FORCE)
set(SDLTTF_SAMPLES  OFF CACHE BOOL "" FORCE)
FetchContent_Declare(SDL3_ttf
  GIT_REPOSITORY https://github.com/libsdl-org/SDL_ttf.git
  GIT_TAG release-3.2.2
  GIT_SUBMODULES external/freetype)
FetchContent_MakeAvailable(SDL3_ttf)

# Two TUs: discovery_explorer.c is the SDL3/Clay UI, net_capture.c owns DART + winsock.
add_executable(dart_explorer discovery_explorer.c net_capture.c)
target_include_directories(dart_explorer PRIVATE
  "${DEPS}/include" "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(dart_explorer PRIVATE SDL3::SDL3 SDL3_ttf::SDL3_ttf dart)

if(WIN32)
  target_link_libraries(dart_explorer PRIVATE ws2_32 bcrypt winmm)
else()
  target_link_libraries(dart_explorer PRIVATE m)
endif()

# demo_scene: the sample mesh node run_demo.ps1 launches. Built here so it stays in
# lockstep with dist/, since a stale copy speaks an old announce wire and never joins.
add_executable(demo_scene demo_scene.c)
target_include_directories(demo_scene PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(demo_scene PRIVATE dart)
if(WIN32)
  target_link_libraries(demo_scene PRIVATE ws2_32 bcrypt winmm)
elseif(NOT APPLE)
  target_link_libraries(demo_scene PRIVATE rt)   # shm_open (the SHM fast path is on by default)
endif()

# regenerate dist/ before building when the root build owns that target
if(TARGET dist)
  add_dependencies(dart_explorer dist)
  add_dependencies(demo_scene dist)
endif()
