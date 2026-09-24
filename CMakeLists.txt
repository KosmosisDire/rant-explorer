# The C++ explorer: RmlUi (RML and RCSS) over SDL3 and OpenGL 3, driving Rant through the
# C++ wrapper. CPM fetches Rant, FreeType, SDL3 and RmlUi, all built static from source.

cmake_minimum_required(VERSION 3.21)

# The one version. The release workflow refuses a tag that does not match it.
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" RANT_EXPLORER_VERSION LIMIT_COUNT 1)
project(rant_explorer VERSION ${RANT_EXPLORER_VERSION} LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

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
# to ../rant to build against the working tree instead.
CPMAddPackage(NAME rant
  GITHUB_REPOSITORY KosmosisDire/Rant
  VERSION 0.0.16
  EXCLUDE_FROM_ALL YES)

# Static CRT: a copied exe must run with no VC++ redistributable. Before every dependency,
# so they all build against the same runtime.
include("${rant_SOURCE_DIR}/tools/static_runtime.cmake")

# FreeType is RmlUi's font engine. Its CMake spells Freetype::Freetype as an export name
# only, so a subdirectory build has to make the alias by hand.
CPMAddPackage(NAME freetype
  GITHUB_REPOSITORY freetype/freetype
  GIT_TAG VER-2-14-3
  VERSION 2.14.3
  EXCLUDE_FROM_ALL YES
  OPTIONS "FT_DISABLE_ZLIB ON" "FT_DISABLE_BZIP2 ON" "FT_DISABLE_PNG ON"
          "FT_DISABLE_HARFBUZZ ON" "FT_DISABLE_BROTLI ON")
add_library(Freetype::Freetype ALIAS freetype)

# RmlUi looks for a system FreeType first, which on Linux would shadow the one above and
# link a shared library into an otherwise static exe.
set(CMAKE_DISABLE_FIND_PACKAGE_Freetype ON)

CPMAddPackage(NAME SDL3
  URL https://github.com/libsdl-org/SDL/releases/download/release-3.4.16/SDL3-3.4.16.tar.gz
  VERSION 3.4.16
  EXCLUDE_FROM_ALL YES
  OPTIONS "BUILD_SHARED_LIBS OFF" "SDL_SHARED OFF" "SDL_STATIC ON" "SDL_TEST OFF"
          "SDL_TEST_LIBRARY OFF" "SDL_EXAMPLES OFF")

# Three patches. Colour transitions mix premultiplied, as CSS does: without it a fade between
# a translucent and an opaque colour flashes the translucent one at full strength. A data-for
# view updates before the other views on its element, which otherwise tie with it and can
# read an index the loop just dropped. Corner radii fit the border box, as in CSS, so a
# thick border keeps a round element round. The patch step runs on every reconfigure, so it
# restores the files before applying.
CPMAddPackage(NAME RmlUi
  GITHUB_REPOSITORY mikke89/RmlUi
  GIT_TAG 6.3
  VERSION 6.3
  EXCLUDE_FROM_ALL YES
  PATCH_COMMAND git checkout -- Source/Core/ElementAnimation.cpp Source/Core/DataViewDefault.cpp
                                Source/Core/GeometryBackgroundBorder.cpp
        COMMAND git apply "${CMAKE_CURRENT_SOURCE_DIR}/cmake/rmlui-premultiplied-fade.patch"
        COMMAND git apply "${CMAKE_CURRENT_SOURCE_DIR}/cmake/rmlui-for-view-first.patch"
        COMMAND git apply "${CMAKE_CURRENT_SOURCE_DIR}/cmake/rmlui-round-border.patch"
  OPTIONS "BUILD_SHARED_LIBS OFF" "RMLUI_SAMPLES OFF" "RMLUI_FONT_ENGINE freetype")

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/video.cmake")

# stb_image decodes Logo.png: RmlUi's OpenGL 3 renderer only reads TGA, and SDL needs raw
# pixels for the window icon. stb_image_write encodes a copied image as PNG.
set(DEPS "${CMAKE_BINARY_DIR}/deps")
file(MAKE_DIRECTORY "${DEPS}/include")
foreach(_stb stb_image.h stb_image_write.h)
  if(NOT EXISTS "${DEPS}/include/${_stb}")
    message(STATUS "fetch ${_stb}")
    file(DOWNLOAD "https://raw.githubusercontent.com/nothings/stb/master/${_stb}"
         "${DEPS}/include/${_stb}" STATUS _st TLS_VERIFY ON)
    list(GET _st 0 _code)
    if(NOT _code EQUAL 0)
      file(REMOVE "${DEPS}/include/${_stb}")
      message(FATAL_ERROR "download failed: ${_stb}
  ${_st}")
    endif()
  endif()
endforeach()

# Lucide as an icon font: one glyph per icon, drawn by the same font engine as the text.
# The font and its name to codepoint table land in assets/ beside Logo.png, so the exe
# embeds them with the rest. The zip name holds the version, so a bump refetches.
set(LUCIDE_VERSION 1.47.0)
set(LUCIDE_ZIP "${DEPS}/lucide-font-${LUCIDE_VERSION}.zip")
set(ASSETS "${CMAKE_CURRENT_SOURCE_DIR}/assets")
if(NOT EXISTS "${LUCIDE_ZIP}" OR NOT EXISTS "${ASSETS}/lucide.ttf"
   OR NOT EXISTS "${ASSETS}/lucide-codepoints.json")
  message(STATUS "fetch lucide font ${LUCIDE_VERSION}")
  file(DOWNLOAD
       "https://github.com/lucide-icons/lucide/releases/download/${LUCIDE_VERSION}/lucide-font-${LUCIDE_VERSION}.zip"
       "${LUCIDE_ZIP}" STATUS _st TLS_VERIFY ON)
  list(GET _st 0 _code)
  if(NOT _code EQUAL 0)
    file(REMOVE "${LUCIDE_ZIP}")
    message(FATAL_ERROR "download failed: lucide font
  ${_st}")
  endif()
  file(ARCHIVE_EXTRACT INPUT "${LUCIDE_ZIP}" DESTINATION "${DEPS}/lucide"
       PATTERNS lucide-font/lucide.ttf lucide-font/codepoints.json)
  file(COPY_FILE "${DEPS}/lucide/lucide-font/lucide.ttf" "${ASSETS}/lucide.ttf")
  file(COPY_FILE "${DEPS}/lucide/lucide-font/codepoints.json" "${ASSETS}/lucide-codepoints.json")
endif()

# RmlUi ships its backends as reference sources for an app to compile in, not as a library:
# the targets in Backends/CMakeLists.txt exist only when the samples are built. We own
# main(), so we take the platform and renderer halves and leave RmlUi_Backend_SDL_GL3.cpp,
# which owns its own window and loop.
set(RMLUI_BACKENDS "${RmlUi_SOURCE_DIR}/Backends")

# Every file in assets/ as a byte array, so a release is the exe alone. Regenerated when an
# asset changes, a new asset needs a reconfigure.
file(GLOB ASSET_FILES CONFIGURE_DEPENDS "${ASSETS}/*")
set(ASSETS_EMBEDDED "${CMAKE_BINARY_DIR}/assets_embedded.cpp")
string(REPLACE ";" "$<SEMICOLON>" _asset_list "${ASSET_FILES}")
add_custom_command(OUTPUT "${ASSETS_EMBEDDED}"
  COMMAND "${CMAKE_COMMAND}" "-DOUTPUT=${ASSETS_EMBEDDED}" "-DFILES=${_asset_list}"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed.cmake"
  DEPENDS ${ASSET_FILES} "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed.cmake"
  COMMENT "embedding assets/"
  VERBATIM)

add_executable(rant_explorer
  src/main.cpp
  src/assets.cpp
  src/capture.cpp
  src/clipboard.cpp
  src/decorators.cpp
  src/editor.cpp
  src/format.cpp
  src/frame.cpp
  src/icons.cpp
  src/image.cpp
  src/reload.cpp
  src/tiling.cpp
  src/tree.cpp
  src/values.cpp
  src/video.cpp
  src/viz.cpp
  "${ASSETS_EMBEDDED}"
  "${rant_SOURCE_DIR}/dist/rant.cpp"              # the one RANT_IMPLEMENTATION anchor
  "${RMLUI_BACKENDS}/RmlUi_Platform_SDL.cpp"
  "${RMLUI_BACKENDS}/RmlUi_Renderer_GL3.cpp")

target_include_directories(rant_explorer PRIVATE "${RMLUI_BACKENDS}" "${DEPS}/include" src)

# The assets this tree was built from. A build here reads them directly so an edited RCSS
# reloads in the running window. A copied exe elsewhere will not find the path and uses
# the embedded copy.
target_compile_definitions(rant_explorer PRIVATE
  RANT_UI_SOURCE_ASSETS="${CMAKE_CURRENT_SOURCE_DIR}/assets")

# The SDL platform half compiles against either SDL major version and picks by this.
target_compile_definitions(rant_explorer PRIVATE RMLUI_SDL_VERSION_MAJOR=3)

target_link_libraries(rant_explorer PRIVATE
  RmlUi::Core RmlUi::Debugger SDL3::SDL3 rant::rant rant::rant_platform
  openh264_decoder dav1d de265)

if(WIN32)
  target_sources(rant_explorer PRIVATE rant_explorer.rc)   # the exe file icon, Logo.ico
  target_link_libraries(rant_explorer PRIVATE dwmapi)       # frame.cpp asks for rounded corners
else()
  # The OpenGL 3 renderer loads libGL through dlopen.
  target_link_libraries(rant_explorer PRIVATE ${CMAKE_DL_LIBS})
endif()

