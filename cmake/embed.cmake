# Run with cmake -P: writes OUTPUT, a C++ file holding each of FILES (a ; list) as a byte
# array, looked up by file name through asset_names, asset_data and asset_sizes.

string(REPEAT "[0-9a-f]" 64 _line)

set(_names "")
set(_data "")
set(_sizes "")
set(_arrays "")
set(_i 0)
foreach(_file ${FILES})
  get_filename_component(_name "${_file}" NAME)
  file(READ "${_file}" _hex HEX)
  string(LENGTH "${_hex}" _len)
  math(EXPR _size "${_len} / 2")
  # 32 bytes a line, then every byte as 0xNN.
  string(REGEX REPLACE "(${_line})" "\\1\n" _hex "${_hex}")
  string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _hex "${_hex}")
  string(APPEND _arrays "static const unsigned char asset_${_i}[] = {\n${_hex}\n};\n")
  string(APPEND _names "    \"${_name}\",\n")
  string(APPEND _data "    asset_${_i},\n")
  string(APPEND _sizes "    ${_size},\n")
  math(EXPR _i "${_i} + 1")
endforeach()

set(_content "#include <cstddef>\n\n${_arrays}
extern const char* const asset_names[] = {\n${_names}};
extern const unsigned char* const asset_data[] = {\n${_data}};
extern const size_t asset_sizes[] = {\n${_sizes}};
extern const size_t asset_count = ${_i};\n")

# Unchanged content keeps its timestamp, so the big file is not recompiled for nothing.
if(EXISTS "${OUTPUT}")
  file(READ "${OUTPUT}" _old)
  if(_old STREQUAL _content)
    return()
  endif()
endif()
file(WRITE "${OUTPUT}" "${_content}")
