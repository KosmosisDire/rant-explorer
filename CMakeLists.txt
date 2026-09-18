# The explorer: Clay layout plus SDL3. CPM fetches Rant, SDL3 and SDL3_ttf, and clay.h,
# nanosvg and stb are single headers downloaded at configure (spec/explorer.md in the Rant repo).

cmake_minimum_required(VERSION 3.19)

# The one version. The release workflow refuses a tag that does not match it.
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" RANT_EXPLORER_VERSION LIMIT_COUNT 1)
project(rant_explorer VERSION ${RANT_EXPLORER_VERSION} LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Release first so a bare multi config build is Release. Every config lands in bin/.
if(CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_CONFIGURATION_TYPES "Release;Debug;RelWithDebInfo;MinSizeRel"
      CACHE STRING "Build configs (Release first = the default)" FORCE)
endif()
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/bin")
foreach(_cfg ${CMAKE_CONFIGURATION_TYPES})
  string(TOUPPER "${_cfg}" _CFG)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${_CFG} "${CMAKE_CURRENT_SOURCE_DIR}/bin")
endforeach()

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/get_cpm.cmake")

# The explorer speaks this release's announce wire. The local presets set CPM_rant_SOURCE
# to ../Rant to build against the working tree instead.
CPMAddPackage(NAME rant
  GITHUB_REPOSITORY KosmosisDire/Rant
  VERSION 0.0.15
  EXCLUDE_FROM_ALL YES)

# Static CRT: a copied exe must run with no VC++ redistributable. Before SDL3, so SDL3 and
# vendored freetype build against the same runtime.
include("${rant_SOURCE_DIR}/tools/static_runtime.cmake")

set(DEPS "${CMAKE_BINARY_DIR}/deps")
file(MAKE_DIRECTORY "${DEPS}/include")

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

# stb decodes and shrinks Logo.png, the top bar logo and the window icon on every platform.
set(STB_RAW "https://raw.githubusercontent.com/nothings/stb/master")
fetch("${STB_RAW}/stb_image.h"         "${DEPS}/include/stb_image.h")
fetch("${STB_RAW}/stb_image_resize2.h" "${DEPS}/include/stb_image_resize2.h")

# Logo.png becomes a byte array header. Written only on change so an unchanged logo rebuilds nothing.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/Logo.png")
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/Logo.png" _logo_hex HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _logo_hex "${_logo_hex}")
set(_logo_h "/* generated from Logo.png by CMakeLists.txt */\nstatic const unsigned char UI_LOGO_PNG[] = {${_logo_hex}};\n")
set(_logo_old "")
if(EXISTS "${DEPS}/include/logo_png.h")
  file(READ "${DEPS}/include/logo_png.h" _logo_old)
endif()
if(NOT _logo_old STREQUAL _logo_h)
  file(WRITE "${DEPS}/include/logo_png.h" "${_logo_h}")
endif()

# Fonts are the OS system fonts (system-ui / ui-monospace), loaded by path at
# runtime to match the mockup per platform. Nothing to fetch.

# SDL3 + SDL_ttf, built from source, static (same result on Windows/Linux, no packages).
CPMAddPackage(NAME SDL3
  URL https://github.com/libsdl-org/SDL/releases/download/release-3.2.16/SDL3-3.2.16.tar.gz
  VERSION 3.2.16
  EXCLUDE_FROM_ALL YES
  OPTIONS "BUILD_SHARED_LIBS OFF" "SDL_SHARED OFF" "SDL_STATIC ON" "SDL_TEST OFF"
          "SDL_TEST_LIBRARY OFF" "SDL_EXAMPLES OFF")

# SDL_ttf vendors FreeType from its git submodule, which the tarball omits. HarfBuzz and
# PlutoSVG off keep it lean, so only external/freetype is pulled.
CPMAddPackage(NAME SDL3_ttf
  GITHUB_REPOSITORY libsdl-org/SDL_ttf
  GIT_TAG release-3.2.2
  VERSION 3.2.2
  GIT_SUBMODULES external/freetype
  EXCLUDE_FROM_ALL YES
  OPTIONS "BUILD_SHARED_LIBS OFF" "SDLTTF_VENDORED ON" "SDLTTF_HARFBUZZ OFF"
          "SDLTTF_PLUTOSVG OFF" "SDLTTF_SAMPLES OFF")

# Two TUs: discovery_explorer.c is the SDL3/Clay UI, net_capture.c owns Rant + winsock.
# Each defines its own implementation flavour, so they link the headers, never rant_host.
add_executable(rant_explorer discovery_explorer.c net_capture.c)
target_include_directories(rant_explorer PRIVATE
  "${DEPS}/include" "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(rant_explorer PRIVATE SDL3::SDL3 SDL3_ttf::SDL3_ttf
                      rant::rant rant::rant_platform)
if(WIN32)
  target_sources(rant_explorer PRIVATE rant_explorer.rc)   # the exe file icon, Logo.ico
else()
  target_link_libraries(rant_explorer PRIVATE m)
endif()

# demo_scene: the sample mesh node run_demo.ps1 launches. Built here so it speaks the same
# announce wire as the explorer, since a stale copy never joins.
add_executable(demo_scene demo_scene.c)
target_include_directories(demo_scene PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(demo_scene PRIVATE rant::rant rant::rant_platform)
