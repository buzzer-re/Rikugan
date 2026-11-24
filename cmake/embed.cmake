# Every file under skills/ and prompts/ becomes bytes in the plugin (src/common/util/Embedded.h): the single source of
# skills and prompt templates at runtime.
file(GLOB_RECURSE RK_EMBED_FILES CONFIGURE_DEPENDS LIST_DIRECTORIES false
     "${CMAKE_SOURCE_DIR}/skills/*" "${CMAKE_SOURCE_DIR}/prompts/*")
list(SORT RK_EMBED_FILES)
set(RK_EMBEDDED_CPP "${CMAKE_BINARY_DIR}/generated/rk_embedded.cpp")
set(_list_file "${CMAKE_BINARY_DIR}/generated/rk_embedded.list")
string(REPLACE ";" "\n" _list_body "${RK_EMBED_FILES}")
file(WRITE "${_list_file}.tmp" "${_list_body}\n")
file(COPY_FILE "${_list_file}.tmp" "${_list_file}" ONLY_IF_DIFFERENT)
add_custom_command(
    OUTPUT "${RK_EMBEDDED_CPP}"
    COMMAND "${CMAKE_COMMAND}" -DROOT=${CMAKE_SOURCE_DIR} -DLIST=${_list_file} -DOUT=${RK_EMBEDDED_CPP}
            -P "${CMAKE_SOURCE_DIR}/cmake/embed_gen.cmake"
    DEPENDS ${RK_EMBED_FILES} "${_list_file}" "${CMAKE_SOURCE_DIR}/cmake/embed_gen.cmake"
    COMMENT "Embedding skills and prompts")
