# libomc, linked into every module of the tree.
#   Linux/macOS: one prelinked object whose only globals are omc_* (cmake/build_omc.sh), plus the link libraries omc's
#                Makefile names for this platform.
#   Windows:     omc's libomc.dll (a MinGW-w64 build, see omc's docs/windows.md) through an MSVC import library made
#                from its libomc.def (cmake/build_omc_win.cmake). The DLL ships next to the plugin, which loads it
#                itself (delay-load, see common/omc/OmcLibrary.h).
if(WIN32)
    set(OMC_LIB_DIR "" CACHE PATH
        "Prebuilt libomc: a folder with libomc.dll and libomc.def (omc's release zip lib\\, or install.ps1 -Lib); empty builds it from OMC_DIR")
    set(OMC_BUILD_WITH "" CACHE STRING "How to build libomc.dll from OMC_DIR: native (MinGW-w64 gcc + make on PATH) or wsl; empty picks one")
    set(OMC_DLL "${CMAKE_BINARY_DIR}/libomc.dll")
    set(OMC_IMPLIB "${CMAKE_BINARY_DIR}/omc/omc.lib")
    set(_omc_make "")
    if(OMC_LIB_DIR)
        set(_omc_mode prebuilt)
        set(_omc_from "${OMC_LIB_DIR}")
    else()
        set(_omc_from "${OMC_DIR}")
        find_program(OMC_MAKE NAMES make mingw32-make)
        find_program(OMC_MINGW_GCC NAMES x86_64-w64-mingw32-gcc gcc)
        find_program(OMC_WSL NAMES wsl)
        if(OMC_BUILD_WITH)
            set(_omc_mode ${OMC_BUILD_WITH})
        elseif(OMC_MAKE AND OMC_MINGW_GCC)
            set(_omc_mode native)
        elseif(OMC_WSL)
            set(_omc_mode wsl)
        else()
            message(FATAL_ERROR "libomc.dll needs a MinGW-w64 toolchain: MSYS2 UCRT64 (gcc + make on PATH) or WSL with "
                                "mingw-w64 and make, as omc's docs/windows.md describes. Or pass -DOMC_LIB_DIR=<folder with "
                                "libomc.dll and libomc.def> (omc's release zip lib\\ folder).")
        endif()
        if(_omc_mode STREQUAL "native")
            if(NOT OMC_MAKE OR NOT OMC_MINGW_GCC)
                message(FATAL_ERROR "OMC_BUILD_WITH=native needs make and a MinGW-w64 gcc on PATH")
            endif()
            set(_omc_make "${OMC_MAKE}")
            get_filename_component(_gcc_name "${OMC_MINGW_GCC}" NAME_WE)
            if(_gcc_name STREQUAL "x86_64-w64-mingw32-gcc")
                set(_omc_prefix "x86_64-w64-mingw32-")
            else()
                set(_omc_prefix "")
            endif()
        elseif(_omc_mode STREQUAL "wsl")
            if(NOT OMC_WSL)
                message(FATAL_ERROR "OMC_BUILD_WITH=wsl but wsl.exe is not on PATH")
            endif()
            set(_omc_make "${OMC_WSL}")
            set(_omc_prefix "x86_64-w64-mingw32-")
        else()
            message(FATAL_ERROR "OMC_BUILD_WITH must be native or wsl")
        endif()
    endif()
    add_custom_target(omc_lib_build
        COMMAND "${CMAKE_COMMAND}" -DMODE=${_omc_mode} "-DOMC_DIR=${OMC_DIR}" "-DOMC_LIB_DIR=${OMC_LIB_DIR}"
                "-DMAKE=${_omc_make}" "-DPREFIX=${_omc_prefix}" "-DLIB_EXE=${CMAKE_AR}" "-DOUT_DLL=${OMC_DLL}"
                "-DOUT_LIB=${OMC_IMPLIB}" -P "${CMAKE_SOURCE_DIR}/cmake/build_omc_win.cmake"
        BYPRODUCTS "${OMC_IMPLIB}" "${OMC_DLL}"
        COMMENT "libomc.dll (${_omc_mode}: ${_omc_from})"
        USES_TERMINAL)

    function(rk_link_omc target)
        add_dependencies(${target} omc_lib_build)
        target_link_libraries(${target} PRIVATE "${OMC_IMPLIB}")
        get_target_property(_type ${target} TYPE)
        if(_type STREQUAL "MODULE_LIBRARY")
            # the host loads the plugin from a folder it does not search for dependencies; the plugin loads
            # libomc.dll from its own folder before the first omc_* call (rk::loadOmcLibrary)
            target_link_options(${target} PRIVATE "/DELAYLOAD:libomc.dll")
            target_link_libraries(${target} PRIVATE delayimp)
        endif()
    endfunction()

    # MSVC exports only __declspec(dllexport) symbols: the hosts' entry-point macros (BINARYNINJAPLUGIN,
    # ida_module_data) already limit a plugin to its entry points.
    function(rk_export_only target)
    endfunction()
    return()
endif()

set(OMC_LIB_O "${CMAKE_BINARY_DIR}/omc/omc-lib.o")
# macOS: OpenSSL linked in statically (Homebrew's static libraries), as build_omc.sh builds omc: the plugin must load on
# a Mac without Homebrew
set(_omc_env)
if(APPLE)
    set(_omc_env STATIC_OPENSSL=1)
endif()
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env ${_omc_env} make -s -C "${OMC_DIR}" -f Makefile -f "${CMAKE_SOURCE_DIR}/mk/omc-vars.mk" rios-libs
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
