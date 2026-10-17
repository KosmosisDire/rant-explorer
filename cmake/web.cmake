# The web build: the explorer as wasm in a page with Rant's own node, whose sockets a Rant
# bridge lends through Rant's TunnelNet. The page, its script and Rant's net objects land
# beside the wasm in bin/web, ready to serve.

# Rant's net objects: the web package of the Rant release CPM pins, or the compiled ones of the
# checkout a local preset builds against.
if(CPM_rant_SOURCE)
  set(WEB_NET "${CPM_rant_SOURCE}/bindings/web/dist")
  if(NOT EXISTS "${WEB_NET}/tunnel.js")
    message(FATAL_ERROR "no net objects in ${WEB_NET}: build Rant's web preset, or npm run build in its bindings/web")
  endif()
else()
  set(_v "${CPM_PACKAGE_rant_VERSION}")
  set(WEB_NET "${DEPS}/rant-middleware-${_v}/package/dist")
  if(NOT EXISTS "${WEB_NET}/tunnel.js")
    message(STATUS "fetch rant-middleware ${_v}")
    set(_tgz "${DEPS}/rant-middleware-${_v}.tgz")
    file(DOWNLOAD
         "https://github.com/KosmosisDire/Rant/releases/download/v${_v}/rant-middleware-${_v}.tgz"
         "${_tgz}" STATUS _st TLS_VERIFY ON)
    list(GET _st 0 _code)
    if(NOT _code EQUAL 0)
      file(REMOVE "${_tgz}")
      message(FATAL_ERROR "download failed: rant-middleware ${_v}
  ${_st}")
    endif()
    file(ARCHIVE_EXTRACT INPUT "${_tgz}" DESTINATION "${DEPS}/rant-middleware-${_v}")
  endif()
endif()

function(web_target target)
  set(_web "${CMAKE_CURRENT_SOURCE_DIR}/web")
  set(_out "${CMAKE_CURRENT_SOURCE_DIR}/bin/web")
  target_link_libraries(${target} PRIVATE rant::rant_web)
  set_target_properties(${target} PROPERTIES OUTPUT_NAME rant_explorer SUFFIX ".js"
                        RUNTIME_OUTPUT_DIRECTORY "${_out}")
  target_link_options(${target} PRIVATE
    -sUSE_WEBGL2=1 -sMIN_WEBGL_VERSION=2 -sMAX_WEBGL_VERSION=2
    -sALLOW_MEMORY_GROWTH=1 -sSTACK_SIZE=4MB
    -sMODULARIZE=1 -sEXPORT_ES6=1 -sEXPORT_NAME=createExplorer -sENVIRONMENT=web
    "-sEXPORTED_FUNCTIONS=_main,_malloc,_free"
    "-sEXPORTED_RUNTIME_METHODS=HEAPU8")
  # The page and the net objects copy on every build, not only when the wasm links, so an
  # edit to the page alone still lands.
  add_custom_target(${target}_page ALL
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${_out}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_web}/index.html" "${_web}/explorer.js"
            "${CMAKE_CURRENT_SOURCE_DIR}/assets/Logo.png" "${_out}/"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${WEB_NET}" "${_out}/rant"
    VERBATIM)
endfunction()
