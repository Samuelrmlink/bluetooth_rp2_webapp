# The PEX runtime for a firmware build (spec docs/pex-format.md).
#
#   include(<repo>/pex/runtime/pex_runtime.cmake)
#   pex_runtime_add(myfw                 # an executable target
#       [GROUPS board ...]               # extra API groups whose tables to compile
#                                        # (the host implements their functions)
#       [SIG_UECC]                       # signature check with micro-ecc
#       [NO_PICO_PORT])                  # bring your own port/pex_port.h functions
#
# The core (loader, trampoline, mem/sched/core/log/pex groups) needs nothing
# but C11 and the target's assembler: no Lua, no gateway code. The tables of
# the groups come from pex/host_api.json at build time.
include(${CMAKE_CURRENT_LIST_DIR}/../sdk/cmake/pex.cmake)

function(pex_group_table group outvar)
    set(_c ${PEX_GEN}/pex_host_${group}.c)
    add_custom_command(OUTPUT ${_c}
        COMMAND ${Python3_EXECUTABLE} ${PEX_GEN_TOOL} table ${PEX_API_JSON} ${group} ${_c}
        DEPENDS ${PEX_API_JSON} ${PEX_GEN_TOOL} VERBATIM)
    set(${outvar} ${_c} PARENT_SCOPE)
endfunction()

function(pex_runtime_add target)
    cmake_parse_arguments(R "SIG_UECC;NO_PICO_PORT" "" "GROUPS" ${ARGN})
    _pex_gen_header()
    set(_r ${PEX_ROOT}/runtime)
    set(_srcs ${_r}/pex_core.c ${_r}/pex_load.c ${_r}/pex_mem.c ${_r}/pex_sched.c ${_r}/pex_hash.c ${_r}/pex_xip.c
        ${_r}/arch/pex_tramp.S ${_r}/groups/pex_json.c)
    foreach(g core log mem sched pex json ${R_GROUPS})
        pex_group_table(${g} _t)
        list(APPEND _srcs ${_t})
    endforeach()
    if(NOT R_NO_PICO_PORT)
        list(APPEND _srcs ${_r}/port/pico/pex_port_pico.c)
        target_link_libraries(${target} hardware_sync hardware_timer hardware_irq)
    endif()
    if(R_SIG_UECC)
        list(APPEND _srcs ${_r}/port/pico/pex_sig_uecc.c)
    endif()
    target_sources(${target} PRIVATE ${_srcs})
    # Loading and bookkeeping are cold paths: size over speed (module code runs
    # at whatever its own build chose; the trampoline is assembly).
    set_property(SOURCE ${_srcs} APPEND PROPERTY COMPILE_OPTIONS -Os)
    target_include_directories(${target} PRIVATE ${_r} ${PEX_ROOT}/sdk/include ${PEX_GEN}/include)
    add_dependencies(${target} pex_api_header)
endfunction()
