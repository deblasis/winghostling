# bin2header.cmake — Convert a binary file into a C header with a byte array.
#
# Usage (called by CMake at build time):
#   cmake -DINPUT=<file> -DOUTPUT=<file> -DARRAY_NAME=<name> -P bin2header.cmake

file(READ "${INPUT}" hex HEX)
string(LENGTH "${hex}" hex_len)

set(output "// Auto-generated from ${INPUT} — do not edit.\n")
string(APPEND output "static const unsigned char ${ARRAY_NAME}[] = {\n    ")

math(EXPR last_byte "${hex_len} / 2 - 1")
set(col 0)
set(i 0)
while(i LESS hex_len)
  math(EXPR next "${i} + 2")
  string(SUBSTRING "${hex}" ${i} 2 byte)
  math(EXPR byte_idx "${i} / 2")
  if(byte_idx LESS last_byte)
    string(APPEND output "0x${byte}, ")
  else()
    string(APPEND output "0x${byte}")
  endif()
  math(EXPR col "${col} + 1")
  if(col EQUAL 12)
    string(APPEND output "\n    ")
    set(col 0)
  endif()
  set(i ${next})
endwhile()

string(APPEND output "\n};\n")

file(WRITE "${OUTPUT}" "${output}")
