# Writes a C++ source holding a file's bytes, for embedding fonts:
#   cmake -DINPUT=<file> -DOUTPUT=<file.cpp> -DSYMBOL=<name> [-DNAMESPACE=<ns>] -P embed_file.cmake
# Defines `const unsigned char <SYMBOL>[]` and `const size_t <SYMBOL>Size`.
if(NOT INPUT OR NOT OUTPUT OR NOT SYMBOL)
  message(FATAL_ERROR "embed_file.cmake needs INPUT, OUTPUT and SYMBOL")
endif()
if(NOT NAMESPACE)
  set(NAMESPACE "recomp::achievements::ui::fonts")
endif()
file(READ "${INPUT}" hex HEX)
string(LENGTH "${hex}" hex_len)
math(EXPR size "${hex_len} / 2")
# 0xAB, per byte, 32 bytes a line.
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
string(REGEX REPLACE "((0x..,){32})" "\\1\n" bytes "${bytes}")
get_filename_component(name "${INPUT}" NAME)
file(WRITE "${OUTPUT}.tmp"
  "// Generated from ${name} by cmake/embed_file.cmake. Do not edit.\n"
  "#include <cstddef>\n"
  "namespace ${NAMESPACE} {\n"
  "extern const unsigned char ${SYMBOL}[];\n"
  "extern const size_t ${SYMBOL}Size;\n"
  "const unsigned char ${SYMBOL}[] = {\n${bytes}\n};\n"
  "const size_t ${SYMBOL}Size = ${size};\n"
  "}  // namespace ${NAMESPACE}\n")
file(COPY_FILE "${OUTPUT}.tmp" "${OUTPUT}" ONLY_IF_DIFFERENT)
file(REMOVE "${OUTPUT}.tmp")
