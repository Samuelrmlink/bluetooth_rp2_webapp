# PEX module SDK -- CMake side (spec docs/pex-format.md section 11).
#
#   include(${PEX_ROOT}/sdk/cmake/pex.cmake)
#   pex_add_module(hello
#       SOURCES hello.c
#       TARGETS m33 m0p          # one .pex per target: hello.m33.pex, hello.m0p.pex
#       MODEL   reloc|pic        # A: GCC, relocated at load; B: clang ROPI+RWPI (default: pic)
#       [KIND lib|app|lua] [LUA] [VERSION 1.2.3] [DESCRIPTION "..."]
#       [FLAGS reentrant,unload-on-exit,needs-sig,ram-only]
#       [OUTPUT_DIR dir] [SIGN [KEY pem] [KEY_ID n]] [CFLAGS ...] [INCLUDES ...])
#
# Modules are compiled with custom commands that call the cross compilers
# directly, so they build inside ANY CMake project (a pico-sdk firmware, or a
# plain host project) without touching its toolchain. Results:
#   target  pex_<name>[_<model>]      builds all of the module's .pex files
#   var     <name>_PEX_FILES          (parent scope) the .pex paths
#
# Tools (cache variables): PEX_GCC (arm-none-eabi-gcc, model A + libgcc),
# PEX_CLANG and PEX_LLD (model B), Python3 for pexlink.
cmake_minimum_required(VERSION 3.13)

get_filename_component(PEX_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(PEX_ROOT "${PEX_ROOT}" CACHE INTERNAL "")

if(NOT PEX_GCC)
    if(DEFINED ENV{PICO_TOOLCHAIN_PATH})
        set(_pg "$ENV{PICO_TOOLCHAIN_PATH}/bin/arm-none-eabi-gcc")
    elseif(PICO_TOOLCHAIN_PATH)
        set(_pg "${PICO_TOOLCHAIN_PATH}/bin/arm-none-eabi-gcc")
    else()
        set(_pg "$ENV{HOME}/.pico-sdk/toolchain/15_2_Rel1/bin/arm-none-eabi-gcc")
    endif()
    set(PEX_GCC "${_pg}" CACHE FILEPATH "arm-none-eabi-gcc for PEX model A")
endif()
find_program(PEX_CLANG NAMES clang PATHS /usr/bin NO_DEFAULT_PATH)
find_program(PEX_CLANG NAMES clang)
find_program(PEX_LLD NAMES ld.lld PATHS /usr/bin NO_DEFAULT_PATH)
find_program(PEX_LLD NAMES ld.lld)
if(NOT Python3_EXECUTABLE)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
endif()

set(PEX_GEN "${CMAKE_BINARY_DIR}/pex_gen" CACHE INTERNAL "")
set(PEX_API_JSON "${PEX_ROOT}/host_api.json" CACHE INTERNAL "")
set(PEX_GEN_TOOL "${PEX_ROOT}/tools/gen_host_api.py" CACHE INTERNAL "")

# CPU flags per target; model A (gcc) and model B (clang) spell them differently.
set(_PEX_GCC_m33  -mthumb -mcpu=cortex-m33 -march=armv8-m.main+fp+dsp -mfloat-abi=softfp)
set(_PEX_GCC_m0p  -mthumb -mcpu=cortex-m0plus -mfloat-abi=soft)
set(_PEX_GCC_c5   -march=rv32imac_zicsr_zifencei -mabi=ilp32 -mcmodel=medany -mno-relax -msmall-data-limit=0)
if(NOT PEX_GCC_c5)
    file(GLOB _pex_c5_candidates "$ENV{HOME}/.espressif/tools/riscv32-esp-elf/*/riscv32-esp-elf/bin/riscv32-esp-elf-gcc")
    if(_pex_c5_candidates)
        list(SORT _pex_c5_candidates)
        list(GET _pex_c5_candidates -1 _pex_c5_gcc)
        set(PEX_GCC_c5 "${_pex_c5_gcc}" CACHE FILEPATH "riscv32-esp-elf-gcc for the c5 PEX target")
    endif()
endif()
set(_PEX_CLANG_m33 --target=thumbv8m.main-none-eabi -mcpu=cortex-m33 -mfloat-abi=softfp -mfpu=fpv5-sp-d16)
set(_PEX_CLANG_m0p --target=thumbv6m-none-eabi -mcpu=cortex-m0plus -mfloat-abi=soft)
set(_PEX_COMMON -Os -g -std=gnu11 -ffreestanding -ffunction-sections -fdata-sections -fno-common
    -fno-exceptions -fno-unwind-tables -fno-asynchronous-unwind-tables -fstack-usage
    -Wall -Wextra -Wno-unused-parameter)

# The SDK header from host_api.json (once per build tree).
function(_pex_gen_header)
    if(TARGET pex_api_header)
        return()
    endif()
    add_custom_command(OUTPUT ${PEX_GEN}/include/pex_api.h
        COMMAND ${Python3_EXECUTABLE} ${PEX_GEN_TOOL} header ${PEX_API_JSON} ${PEX_GEN}/include/pex_api.h
        DEPENDS ${PEX_API_JSON} ${PEX_GEN_TOOL} VERBATIM)
    add_custom_target(pex_api_header DEPENDS ${PEX_GEN}/include/pex_api.h)
endfunction()

# libgcc for a target (division, 64-bit and soft-float helpers): pure code, so
# it links into either model. Asked of gcc once, at configure time.
function(_pex_cc t outvar)
    if(t STREQUAL "c5")
        if(NOT PEX_GCC_c5)
            message(FATAL_ERROR "pex: target c5 needs riscv32-esp-elf-gcc (PEX_GCC_c5)")
        endif()
        set(${outvar} ${PEX_GCC_c5} ${_PEX_GCC_c5} PARENT_SCOPE)
    else()
        set(${outvar} ${PEX_GCC} ${_PEX_GCC_${t}} PARENT_SCOPE)
    endif()
endfunction()

function(_pex_libgcc t outvar)
    _pex_cc(${t} _cc)
    execute_process(COMMAND ${_cc} -print-libgcc-file-name
        OUTPUT_VARIABLE _lg OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE rc)
    if(NOT rc EQUAL 0 OR NOT EXISTS "${_lg}")
        message(FATAL_ERROR "pex: cannot find libgcc for ${t}")
    endif()
    set(${outvar} "${_lg}" PARENT_SCOPE)
endfunction()

# The import veneers for (target, model), once per build tree.
function(_pex_stubs t m outvar)
    set(_s ${PEX_GEN}/pex_imports_${t}_${m}.S)
    set(_o ${PEX_GEN}/pex_imports_${t}_${m}.o)
    if(NOT TARGET pex_stubs_${t}_${m})
        add_custom_command(OUTPUT ${_s}
            COMMAND ${Python3_EXECUTABLE} ${PEX_GEN_TOOL} stubs ${PEX_API_JSON} ${t} ${m} ${_s}
            DEPENDS ${PEX_API_JSON} ${PEX_GEN_TOOL} VERBATIM)
        if(m STREQUAL "pic")
            set(_cc ${PEX_CLANG} ${_PEX_CLANG_${t}})
        else()
            _pex_cc(${t} _cc)
        endif()
        add_custom_command(OUTPUT ${_o} COMMAND ${_cc} -c ${_s} -o ${_o} DEPENDS ${_s} VERBATIM)
        add_custom_target(pex_stubs_${t}_${m} DEPENDS ${_o})
    endif()
    set(${outvar} ${_o} PARENT_SCOPE)
endfunction()

function(pex_add_module name)
    cmake_parse_arguments(P "SIGN;LUA" "MODEL;KIND;VERSION;DESCRIPTION;FLAGS;OUTPUT_DIR;KEY;KEY_ID;TARGET_NAME"
        "SOURCES;TARGETS;CFLAGS;INCLUDES" ${ARGN})
    if(NOT P_SOURCES)
        message(FATAL_ERROR "pex_add_module(${name}): SOURCES missing")
    endif()
    if(NOT P_TARGETS)
        set(P_TARGETS m33)
    endif()
    if(NOT P_MODEL)
        set(P_MODEL pic)
    endif()
    if(NOT P_MODEL MATCHES "^(pic|reloc)$")
        message(FATAL_ERROR "pex_add_module(${name}): MODEL must be pic or reloc")
    endif()
    if(NOT P_KIND)
        set(P_KIND lib)
    endif()
    if(NOT P_VERSION)
        set(P_VERSION 0.1.0)
    endif()
    if(NOT P_OUTPUT_DIR)
        set(P_OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/pex)
    endif()
    if(NOT P_TARGET_NAME)
        set(P_TARGET_NAME pex_${name}_${P_MODEL})
    endif()
    if(P_MODEL STREQUAL "pic" AND (NOT PEX_CLANG OR NOT PEX_LLD))
        message(FATAL_ERROR "pex: MODEL pic needs clang and ld.lld (PEX_CLANG/PEX_LLD)")
    endif()
    _pex_gen_header()

    set(_work ${CMAKE_CURRENT_BINARY_DIR}/pex_obj/${name}_${P_MODEL})
    set(_outs)
    foreach(t ${P_TARGETS})
        _pex_stubs(${t} ${P_MODEL} _stub)
        _pex_libgcc(${t} _libgcc)
        set(_incs -I${PEX_ROOT}/sdk/include -I${PEX_GEN}/include)
        foreach(i ${P_INCLUDES})
            list(APPEND _incs -I${i})
        endforeach()
        if(t STREQUAL "c5" AND P_MODEL STREQUAL "pic")
            message(FATAL_ERROR "pex_add_module(${name}): c5 is MODEL reloc only")
        endif()
        if(P_MODEL STREQUAL "pic")
            set(_cc ${PEX_CLANG} ${_PEX_CLANG_${t}} -fropi -frwpi)
        else()
            _pex_cc(${t} _cc)
        endif()
        set(_objs)
        set(_sus)
        foreach(src ${P_SOURCES})
            get_filename_component(_abs ${src} ABSOLUTE)
            get_filename_component(_b ${src} NAME_WE)
            set(_o ${_work}/${t}/${_b}.o)
            add_custom_command(OUTPUT ${_o}
                COMMAND ${CMAKE_COMMAND} -E make_directory ${_work}/${t}
                COMMAND ${_cc} ${_PEX_COMMON} ${P_CFLAGS} ${_incs} -DPEX_TARGET_${t}=1 -MD -MF ${_o}.d -c ${_abs} -o ${_o}
                DEPENDS ${_abs} ${PEX_GEN}/include/pex_api.h ${PEX_ROOT}/sdk/include/pex.h
                DEPFILE ${_o}.d
                VERBATIM)
            list(APPEND _objs ${_o})
            list(APPEND _sus --su ${_work}/${t}/${_b}.su)
        endforeach()
        set(_elf ${_work}/${t}/${name}.elf)
        set(_ld ${PEX_ROOT}/sdk/ld/pex_${t}_${P_MODEL}.ld)
        if(P_MODEL STREQUAL "pic")
            set(_link ${PEX_LLD} -T ${_ld} -L ${PEX_ROOT}/sdk/ld --emit-relocs --gc-sections -e pex_main
                ${_objs} ${_stub} ${_libgcc} -o ${_elf})
        else()
            _pex_cc(${t} _ldcc)
            set(_link ${_ldcc} -nostdlib -nostartfiles -T ${_ld} -L ${PEX_ROOT}/sdk/ld
                -Wl,--emit-relocs -Wl,--gc-sections -Wl,-e,pex_main ${_objs} ${_stub} -lgcc -o ${_elf})
        endif()
        add_custom_command(OUTPUT ${_elf} COMMAND ${_link}
            DEPENDS ${_objs} ${_stub} ${_ld} ${PEX_ROOT}/sdk/ld/pex_${P_MODEL}.ld
                    ${PEX_ROOT}/sdk/ld/pex_common_tail.ld VERBATIM)
        set(_pex ${P_OUTPUT_DIR}/${name}.${t}.pex)
        set(_args link ${_elf} -o ${_pex} --target ${t} --model ${P_MODEL} --api ${PEX_API_JSON}
            --name ${name} --version ${P_VERSION} --kind ${P_KIND} ${_sus})
        if(P_LUA)
            list(APPEND _args --lua)
        endif()
        if(P_DESCRIPTION)
            list(APPEND _args --desc ${P_DESCRIPTION})
        endif()
        if(P_FLAGS)
            list(APPEND _args --flags ${P_FLAGS})
        endif()
        if(P_SIGN)
            list(APPEND _args --sign)
            if(P_KEY)
                list(APPEND _args --key ${P_KEY})
            endif()
            if(P_KEY_ID)
                list(APPEND _args --key-id ${P_KEY_ID})
            endif()
        endif()
        add_custom_command(OUTPUT ${_pex}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${P_OUTPUT_DIR}
            COMMAND ${Python3_EXECUTABLE} ${PEX_ROOT}/tools/pexlink.py ${_args}
            DEPENDS ${_elf} ${PEX_ROOT}/tools/pexlink.py ${PEX_API_JSON} VERBATIM)
        list(APPEND _outs ${_pex})
    endforeach()
    add_custom_target(${P_TARGET_NAME} ALL DEPENDS ${_outs})
    add_dependencies(${P_TARGET_NAME} pex_api_header)
    set(${name}_PEX_FILES ${_outs} PARENT_SCOPE)
endfunction()
