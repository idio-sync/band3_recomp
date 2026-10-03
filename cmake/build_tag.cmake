# band3's build tag: `git describe` of the checkout it's built from, which the
# log's first line shows and a Liveless Rooms server gets as band3's version.
# A build without .git (Nix's, a source zip's) passes -DBAND3_BUILD_TAG=<tag>.
#
# The tag is written each build rather than at configure time, so a commit or
# an edit shows in the next build without a reconfigure. build_tag_write.cmake
# leaves the header alone while the tag is the same, so only src/build_tag.cpp
# (its one includer) recompiles, and only when the tag changes.
set(BAND3_BUILD_TAG "" CACHE STRING
    "build tag to use instead of git describe, for a build without .git")
find_package(Git QUIET)

# Call from the directory that adds src/build_tag.cpp to target.
function(band3_setup_build_tag target source_root)
    set(dir "${CMAKE_CURRENT_BINARY_DIR}/build_tag")
    # the header as a byproduct, so Ninja knows what makes it on a first build
    # and checks whether the step changed it (restat) before recompiling
    add_custom_target(${target}_build_tag
        COMMAND ${CMAKE_COMMAND}
            "-DOUT=${dir}/band3_build_tag.h"
            "-DSRC=${source_root}"
            "-DGIT=${GIT_EXECUTABLE}"
            "-DFALLBACK=${BAND3_BUILD_TAG}"
            -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/build_tag_write.cmake"
        BYPRODUCTS "${dir}/band3_build_tag.h"
        COMMENT "Checking band3's build tag"
        VERBATIM)
    add_dependencies(${target} ${target}_build_tag)
    target_include_directories(${target} PRIVATE "${dir}")
endfunction()
