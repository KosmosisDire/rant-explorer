# The web build: the explorer as wasm in a page, its node a client of the Rant bridge. The
# page, its script and the bridge client land beside the wasm in bin/web, ready to serve.

# The bridge client: the package a bridge release attaches, or a built dist folder, which
# the local preset takes from the bridge checkout beside this one.
set(RANT_CLIENT_VERSION 0.0.2)
set(RANT_CLIENT_DIR "" CACHE PATH "A built rant-client dist folder, empty to fetch the release")
if(RANT_CLIENT_DIR)
  set(WEB_CLIENT "${RANT_CLIENT_DIR}")
else()
  set(WEB_CLIENT "${DEPS}/rant-client-${RANT_CLIENT_VERSION}/package/dist")
  if(NOT EXISTS "${WEB_CLIENT}/rant.js")
    message(STATUS "fetch rant-client ${RANT_CLIENT_VERSION}")
    set(_tgz "${DEPS}/rant-client-${RANT_CLIENT_VERSION}.tgz")
    file(DOWNLOAD
         "https://github.com/KosmosisDire/rant-bridge/releases/download/v${RANT_CLIENT_VERSION}/rant-client-${RANT_CLIENT_VERSION}.tgz"
         "${_tgz}" STATUS _st TLS_VERIFY ON)
    list(GET _st 0 _code)
    if(NOT _code EQUAL 0)
      file(REMOVE "${_tgz}")
      message(FATAL_ERROR "download failed: rant-client
  ${_st}")
    endif()
    file(ARCHIVE_EXTRACT INPUT "${_tgz}" DESTINATION "${DEPS}/rant-client-${RANT_CLIENT_VERSION}")
  endif()
endif()

function(web_target target)
  set(_web "${CMAKE_CURRENT_SOURCE_DIR}/web")
  set(_out "${CMAKE_CURRENT_SOURCE_DIR}/bin/web")
  # The bridge stands in for the node, and the implementation anchor carries a schema
  # registry of its own, since a page opens no node to compile schemas in.
  target_sources(${target} PRIVATE src/bridge.cpp src/rant_web.cpp)
  # A page runs one thread, so Rant compiles its threads out.
  target_compile_definitions(${target} PRIVATE RANT_NO_THREADS)
  set_target_properties(${target} PROPERTIES OUTPUT_NAME rant_explorer SUFFIX ".js"
                        RUNTIME_OUTPUT_DIRECTORY "${_out}")
  target_link_options(${target} PRIVATE
    -sUSE_WEBGL2=1 -sMIN_WEBGL_VERSION=2 -sMAX_WEBGL_VERSION=2
    -sALLOW_MEMORY_GROWTH=1 -sSTACK_SIZE=4MB
    -sMODULARIZE=1 -sEXPORT_ES6=1 -sEXPORT_NAME=createExplorer -sENVIRONMENT=web
    "-sEXPORTED_FUNCTIONS=_main,_malloc,_free"
    "-sEXPORTED_RUNTIME_METHODS=HEAPU8")
  # The page and the client copy on every build, not only when the wasm links, so an edit to
  # the page alone still lands.
  add_custom_target(${target}_page ALL
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${_out}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_web}/index.html" "${_web}/explorer.js"
            "${CMAKE_CURRENT_SOURCE_DIR}/assets/Logo.png" "${_out}/"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${WEB_CLIENT}" "${_out}/rant-client"
    VERBATIM)
endfunction()
