# cmake -DMODE=native|wsl|prebuilt -DOMC_DIR=<omc source> -DOMC_LIB_DIR=<prebuilt folder> -DMAKE=<make or wsl.exe>
#       -DPREFIX=<MinGW tool prefix> -DLIB_EXE=<MSVC lib.exe> -DOUT_DLL=<libomc.dll> -DOUT_LIB=<omc.lib> -P build_omc_win.cmake
# Windows half of cmake/omc.cmake: builds libomc.dll with omc's own Makefile (incremental) unless a prebuilt one is
# given, copies it into the build tree and makes the MSVC import library from its libomc.def.
cmake_host_system_information(RESULT _jobs QUERY NUMBER_OF_LOGICAL_CORES)
if(MODE STREQUAL "prebuilt")
    set(_src "${OMC_LIB_DIR}")
elseif(MODE STREQUAL "native")
    # MSYS2 / a GitHub runner's Git Bash: omc's documented native build (make WINDOWS=1 MINGW= HOSTCC=gcc)
    execute_process(COMMAND "${MAKE}" -s -j${_jobs} -C "${OMC_DIR}" WINDOWS=1 "MINGW=${PREFIX}" HOSTCC=gcc
                            build/windows/libomc.dll RESULT_VARIABLE _rc)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "building libomc.dll in ${OMC_DIR} failed (make WINDOWS=1)")
    endif()
    set(_src "${OMC_DIR}/build/windows")
elseif(MODE STREQUAL "wsl")
    # omc's cross build from WSL (apt: mingw-w64 make gcc)
    execute_process(COMMAND "${MAKE}" -e wslpath -a "${OMC_DIR}" OUTPUT_VARIABLE _wsl_dir
                    OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE _rc)
    if(NOT _rc EQUAL 0 OR _wsl_dir STREQUAL "")
        message(FATAL_ERROR "wslpath could not translate ${OMC_DIR}")
    endif()
    execute_process(COMMAND "${MAKE}" -e make -s -j${_jobs} -C "${_wsl_dir}" WINDOWS=1 build/windows/libomc.dll
                    RESULT_VARIABLE _rc)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "building libomc.dll in ${OMC_DIR} through WSL failed (make WINDOWS=1); WSL needs "
                            "the mingw-w64, make and gcc packages")
    endif()
    set(_src "${OMC_DIR}/build/windows")
else()
    message(FATAL_ERROR "build_omc_win.cmake: unknown MODE '${MODE}'")
endif()

foreach(_f libomc.dll libomc.def)
    if(NOT EXISTS "${_src}/${_f}")
        message(FATAL_ERROR "${_src}/${_f} is missing")
    endif()
endforeach()

if(NOT EXISTS "${OUT_LIB}" OR "${_src}/libomc.def" IS_NEWER_THAN "${OUT_LIB}" OR "${_src}/libomc.dll" IS_NEWER_THAN "${OUT_LIB}")
    get_filename_component(_out_dir "${OUT_LIB}" DIRECTORY)
    file(MAKE_DIRECTORY "${_out_dir}")
    execute_process(COMMAND "${LIB_EXE}" /nologo "/def:${_src}/libomc.def" /machine:x64 "/out:${OUT_LIB}"
                    RESULT_VARIABLE _rc)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "lib.exe could not make ${OUT_LIB} from ${_src}/libomc.def")
    endif()
endif()
file(COPY_FILE "${_src}/libomc.dll" "${OUT_DLL}" ONLY_IF_DIFFERENT)
