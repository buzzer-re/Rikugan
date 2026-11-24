# libomc as one prelinked object whose only globals are omc_* (cmake/build_omc.sh), plus the link libraries
# omc's Makefile names for this platform.
set(OMC_LIB_O "${CMAKE_BINARY_DIR}/omc/omc-lib.o")
execute_process(
    COMMAND make -s -C "${OMC_DIR}" -f Makefile -f "${CMAKE_SOURCE_DIR}/mk/omc-vars.mk" rios-libs
    OUTPUT_VARIABLE OMC_LINK_LIBS OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE _omc_rc)
if(NOT _omc_rc EQUAL 0)
    message(FATAL_ERROR "cannot read omc's link libraries from ${OMC_DIR}/Makefile")
endif()
separate_arguments(OMC_LINK_LIBS UNIX_COMMAND "${OMC_LINK_LIBS}")

# always runs: omc's make is incremental, and the script only re-prelinks when an object changed
add_custom_target(omc_lib_build
    COMMAND "${CMAKE_SOURCE_DIR}/cmake/build_omc.sh" "${OMC_DIR}" "${OMC_LIB_O}" "${CMAKE_SOURCE_DIR}"
    BYPRODUCTS "${OMC_LIB_O}"
    COMMENT "libomc (${OMC_DIR})"
    USES_TERMINAL)

function(rk_link_omc target)
    add_dependencies(${target} omc_lib_build)
    target_link_libraries(${target} PRIVATE "${OMC_LIB_O}" ${OMC_LINK_LIBS})
endfunction()

# Only the named symbols stay dynamic exports of a plugin module.
function(rk_export_only target)
    if(APPLE)
        set(_f "${CMAKE_BINARY_DIR}/${target}.exports")
        set(_body "")
        foreach(_s ${ARGN})
            string(APPEND _body "_${_s}\n")
        endforeach()
        file(WRITE "${_f}" "${_body}")
        target_link_options(${target} PRIVATE "LINKER:-exported_symbols_list,${_f}")
    else()
        set(_f "${CMAKE_BINARY_DIR}/${target}.map")
        set(_body "{ global:\n")
        foreach(_s ${ARGN})
            string(APPEND _body "  ${_s};\n")
        endforeach()
        string(APPEND _body "local: *; };\n")
        file(WRITE "${_f}" "${_body}")
        target_link_options(${target} PRIVATE "LINKER:--version-script=${_f}" "LINKER:-z,relro,-z,now" "LINKER:--no-undefined")
    endif()
endfunction()
