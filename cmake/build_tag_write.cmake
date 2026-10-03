# Writes band3's build tag header (see build_tag.cmake):
#
#   cmake -DOUT=<header> -DSRC=<checkout> [-DGIT=<git>] [-DFALLBACK=<tag>]
#         -P build_tag_write.cmake
#
# The tag is FALLBACK when set, else `git describe` of SRC, else "unknown".
# A header that already says it is left untouched, so nothing recompiles.
if(FALLBACK)
    set(tag "${FALLBACK}")
else()
    set(tag "")
    if(GIT)
        execute_process(
            COMMAND "${GIT}" describe --tags --always --dirty --abbrev=9
            WORKING_DIRECTORY "${SRC}"
            OUTPUT_VARIABLE tag
            RESULT_VARIABLE result
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET)
        if(NOT result EQUAL 0)
            set(tag "")
        endif()
    endif()
    if(tag STREQUAL "")
        set(tag "unknown")
    endif()
endif()

# as a C string literal
string(REPLACE "\\" "\\\\" literal "${tag}")
string(REPLACE "\"" "\\\"" literal "${literal}")
set(contents "// written by cmake/build_tag_write.cmake each build\n#define BAND3_BUILD_TAG \"${literal}\"\n")

if(EXISTS "${OUT}")
    file(READ "${OUT}" old)
    if(old STREQUAL contents)
        return()
    endif()
endif()
file(WRITE "${OUT}" "${contents}")
message(STATUS "band3 build tag: ${tag}")
