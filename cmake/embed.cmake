# Run with cmake -P: writes OUTPUT, a C++ file holding each of FILES (a ; list) as a byte
# array, looked up by name through <PREFIX>_names, _data, _sizes and _count. PREFIX defaults
# to asset. A name is the file name, or with BASE its path under BASE. With GZIP each file is
# gzipped first, for a server to send as it is.

if(NOT PREFIX)
  set(PREFIX asset)
endif()

string(REPEAT "[0-9a-f]" 64 _line)
get_filename_component(_scratch "${OUTPUT}" DIRECTORY)

set(_names "")
set(_data "")
set(_sizes "")
set(_arrays "")
set(_i 0)
foreach(_file ${FILES})
  if(BASE)
    file(RELATIVE_PATH _name "${BASE}" "${_file}")
  else()
    get_filename_component(_name "${_file}" NAME)
  endif()
  set(_read "${_file}")
  if(GZIP)
    set(_read "${_scratch}/${PREFIX}_${_i}.gz")
    file(ARCHIVE_CREATE OUTPUT "${_read}" PATHS "${_file}" FORMAT raw COMPRESSION GZip
         COMPRESSION_LEVEL 9)
  endif()
  file(READ "${_read}" _hex HEX)
  if(GZIP)
    # Bytes 4 to 7 of a gzip header are the time it was made. Zeroed, a rebuild is the same.
    string(SUBSTRING "${_hex}" 0 8 _head)
    string(SUBSTRING "${_hex}" 16 -1 _tail)
    set(_hex "${_head}00000000${_tail}")
  endif()
  string(LENGTH "${_hex}" _len)
  math(EXPR _size "${_len} / 2")
  # 32 bytes a line, then every byte as 0xNN.
  string(REGEX REPLACE "(${_line})" "\\1\n" _hex "${_hex}")
  string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _hex "${_hex}")
  string(APPEND _arrays "static const unsigned char ${PREFIX}_${_i}[] = {\n${_hex}\n};\n")
  string(APPEND _names "    \"${_name}\",\n")
  string(APPEND _data "    ${PREFIX}_${_i},\n")
  string(APPEND _sizes "    ${_size},\n")
  math(EXPR _i "${_i} + 1")
endforeach()

# An empty table still needs one entry, since C++ has no empty array.
if(_i EQUAL 0)
  set(_names "    nullptr,\n")
  set(_data "    nullptr,\n")
  set(_sizes "    0,\n")
endif()

set(_content "#include <cstddef>\n\n${_arrays}
extern const char* const ${PREFIX}_names[] = {\n${_names}};
extern const unsigned char* const ${PREFIX}_data[] = {\n${_data}};
extern const size_t ${PREFIX}_sizes[] = {\n${_sizes}};
extern const size_t ${PREFIX}_count = ${_i};\n")

# Unchanged content keeps its timestamp, so the big file is not recompiled for nothing.
if(EXISTS "${OUTPUT}")
  file(READ "${OUTPUT}" _old)
  if(_old STREQUAL _content)
    return()
  endif()
endif()
file(WRITE "${OUTPUT}" "${_content}")
