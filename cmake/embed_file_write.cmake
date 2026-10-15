# Writes IN's bytes to OUT as a C++ array named NAME (embed.cmake). A regex
# over a whole font (900,000 hex digits) takes CMake minutes, so the bytes go
# through in lines of 32.
file(READ "${IN}" hex HEX)
string(LENGTH "${hex}" chars)
math(EXPR size "${chars} / 2")
set(lines "")
set(at 0)
while(at LESS chars)
    string(SUBSTRING "${hex}" ${at} 64 line)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," line "${line}")
    string(APPEND lines "${line}\n")
    math(EXPR at "${at} + 64")
endwhile()
get_filename_component(file "${IN}" NAME)
file(WRITE "${OUT}.tmp"
    "// ${file}, embedded by cmake/embed.cmake; don't edit\n"
    "#pragma once\n"
    "#include <cstddef>\n"
    "inline constexpr unsigned char ${NAME}[] = {\n${lines}};\n"
    "inline constexpr std::size_t ${NAME}_size = ${size};\n")
file(COPY_FILE "${OUT}.tmp" "${OUT}" ONLY_IF_DIFFERENT)
file(REMOVE "${OUT}.tmp")
