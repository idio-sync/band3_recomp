# Files compiled into band3 as byte arrays: the launcher's fonts
# (src/ThirdParty/montserrat), which the release zip doesn't carry beside the
# executable. Each becomes a header in the build folder,
#   inline constexpr unsigned char <name>[] = {...};
# made again when its file changes (embed_file_write.cmake).

# Call from the directory that adds the target's sources.
function(band3_embed_file target input name)
    set(dir "${CMAKE_CURRENT_BINARY_DIR}/embedded")
    set(out "${dir}/${name}.h")
    add_custom_command(
        OUTPUT "${out}"
        COMMAND ${CMAKE_COMMAND} "-DIN=${input}" "-DOUT=${out}" "-DNAME=${name}"
            -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/embed_file_write.cmake"
        DEPENDS "${input}" "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/embed_file_write.cmake"
        COMMENT "Embedding ${name}"
        VERBATIM)
    # a source of the target, so it's made before what includes it compiles
    target_sources(${target} PRIVATE "${out}")
    target_include_directories(${target} PRIVATE "${dir}")
endfunction()
