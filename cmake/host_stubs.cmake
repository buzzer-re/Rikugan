# Building without the hosts installed (-DRIKUGAN_SDK_ONLY=ON): CI and the release packages.
#
# A plugin links the host's own libraries. Without the host, the build links stand-ins that carry the real libraries'
# names (ELF soname, Mach-O install name, Windows DLL name), so the plugin binds to the host's copies when it loads,
# exactly like one built against an installed host:
#   Binary Ninja's core       the API's own stubs (BN_ALLOW_STUBS), generated from binaryninjacore.h
#   Binary Ninja's UI         made from what the plugin uses (below)
#   IDA                       the SDK's libida stubs (Linux, macOS) and import libraries (Windows)
#   IDA's Qt (namespace QT)   Linux, macOS: made from what the plugin uses; Windows: the SDK's import libraries
#
# "Made from what the plugin uses": a probe of the plugin is linked from the same objects with the host's symbols left
# unresolved. cmake/host_stubs.py reads what is left (the linker's report, or the probe's symbol table on macOS) and
# writes the stand-ins' sources; the plugin itself is then linked against them with nothing left unresolved.

find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(RK_HOST_STUBS_PY "${CMAKE_SOURCE_DIR}/cmake/host_stubs.py")
if(MSVC)
    set(RK_BIN_FORMAT pe)
elseif(APPLE)
    set(RK_BIN_FORMAT macho)
else()
    set(RK_BIN_FORMAT elf)
endif()

# rk_host_probe(<module> KIND binja|ida OBJECTS <objects...> LIBS <libraries...> [QT_LIBS <Core> <Gui> <Widgets>])
# Links <module>_probe from OBJECTS and LIBS with unresolved symbols allowed, and the harvest step after it. Defines
# RK_HOST_STUBS_DIR (in the caller) where the harvest writes the stand-ins' sources.
function(rk_host_probe module)
    cmake_parse_arguments(A "" "KIND" "OBJECTS;LIBS;QT_LIBS" ${ARGN})
    set(_dir "${CMAKE_BINARY_DIR}/host_stubs/${A_KIND}")
    set(_probe ${module}_probe)
    add_library(${_probe} MODULE ${A_OBJECTS})
    target_link_libraries(${_probe} PRIVATE ${A_LIBS})
    rk_link_omc(${_probe})
    set_target_properties(${_probe} PROPERTIES
        RULE_LAUNCH_LINK "\"${Python3_EXECUTABLE}\" \"${RK_HOST_STUBS_PY}\" capture \"${_dir}/probe.log\""
        LIBRARY_OUTPUT_DIRECTORY "${_dir}")
    if(MSVC)
        target_link_options(${_probe} PRIVATE /FORCE:UNRESOLVED)
    elseif(APPLE)
        target_link_options(${_probe} PRIVATE "LINKER:-undefined,dynamic_lookup")
    else()
        # every undefined symbol is reported (by its mangled name), and the probe is still written
        target_link_options(${_probe} PRIVATE "LINKER:-z,defs" "LINKER:--warn-unresolved-symbols" "LINKER:--no-demangle")
    endif()

    if(A_KIND STREQUAL "binja")
        if(RK_BIN_FORMAT STREQUAL "pe")
            set(_outs "${_dir}/binaryninjaui.def")
        else()
            set(_outs "${_dir}/binaryninjaui.c")
        endif()
    else()
        set(_outs "${_dir}/Qt6Core.c" "${_dir}/Qt6Gui.c" "${_dir}/Qt6Widgets.c")
    endif()
    set(_qt_args)
    if(A_QT_LIBS)
        list(GET A_QT_LIBS 0 _core)
        list(GET A_QT_LIBS 1 _gui)
        list(GET A_QT_LIBS 2 _widgets)
        set(_qt_args --qt "Core=${_core}" --qt "Gui=${_gui}" --qt "Widgets=${_widgets}")
        find_program(RK_CXXFILT NAMES c++filt llvm-cxxfilt REQUIRED)
        list(APPEND _qt_args --cxxfilt "${RK_CXXFILT}")
    endif()
    add_custom_command(OUTPUT ${_outs}
        COMMAND "${Python3_EXECUTABLE}" "${RK_HOST_STUBS_PY}" harvest --kind ${A_KIND} --format ${RK_BIN_FORMAT}
                --log "${_dir}/probe.log" --probe "$<TARGET_FILE:${_probe}>" --out "${_dir}" --nm "${CMAKE_NM}"
                ${_qt_args}
        DEPENDS ${_probe} "${RK_HOST_STUBS_PY}"
        COMMENT "Stand-ins for the ${A_KIND} host's libraries"
        VERBATIM)
    set(RK_HOST_STUBS_DIR "${_dir}" PARENT_SCOPE)
endfunction()

# rk_host_stub_library(<target> SOURCE <file> NAME <file name> [INSTALL_NAME <Mach-O id>] [VERSIONS <compat> <current>])
# A shared stand-in (Linux, macOS): ELF soname = NAME; Mach-O install name = INSTALL_NAME.
function(rk_host_stub_library target)
    cmake_parse_arguments(A "" "SOURCE;NAME;INSTALL_NAME" "VERSIONS" ${ARGN})
    add_library(${target} SHARED "${A_SOURCE}")
    set_target_properties(${target} PROPERTIES PREFIX "" SUFFIX "" OUTPUT_NAME "${A_NAME}" C_VISIBILITY_PRESET default
                                               LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/host_stubs/lib/${target}")
    if(APPLE)
        get_filename_component(_id_dir "${A_INSTALL_NAME}" DIRECTORY)
        set_target_properties(${target} PROPERTIES INSTALL_NAME_DIR "${_id_dir}" BUILD_WITH_INSTALL_NAME_DIR ON)
        if(A_VERSIONS)
            list(GET A_VERSIONS 0 _compat)
            list(GET A_VERSIONS 1 _current)
            set_target_properties(${target} PROPERTIES MACHO_COMPATIBILITY_VERSION ${_compat} MACHO_CURRENT_VERSION ${_current})
        endif()
    endif()  # ELF: CMake makes the output name the soname
endfunction()
