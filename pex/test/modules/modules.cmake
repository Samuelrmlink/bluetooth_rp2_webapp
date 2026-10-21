# The PEX test modules, built for both targets in both code models:
#   ${PEX_TEST_OUT}/<model>/<name>.<target>.pex
# Included by the test hosts (pex/test/host) and by the standalone
# pex/test/modules/CMakeLists.txt. PEX_TEST_MODULE_FILES lists every output.
include(${CMAKE_CURRENT_LIST_DIR}/../../sdk/cmake/pex.cmake)
if(NOT PEX_TEST_OUT)
    set(PEX_TEST_OUT ${CMAKE_BINARY_DIR}/pexmods)
endif()
set(_m ${CMAKE_CURRENT_LIST_DIR})
set(PEX_TEST_MODULE_FILES)
# PEX_TEST_SIGN: sign every module (pexlink --sign: the OTA key handling of
# tools/ota_sign.py; PEX_TEST_KEY overrides the key file).
set(_sign)
if(PEX_TEST_SIGN)
    set(_sign SIGN)
    if(PEX_TEST_KEY)
        list(APPEND _sign KEY ${PEX_TEST_KEY})
    endif()
endif()
foreach(model reloc pic)
    set(_o ${PEX_TEST_OUT}/${model})
    set(_reent)
    if(model STREQUAL "pic")
        set(_reent FLAGS reentrant)
    endif()
    pex_add_module(hello   SOURCES ${_m}/hello.c   TARGETS m33 m0p MODEL ${model} KIND lua VERSION 1.0.0
                   DESCRIPTION "hello world, also a Lua module" OUTPUT_DIR ${_o} ${_sign})
    list(APPEND PEX_TEST_MODULE_FILES ${hello_PEX_FILES})
    pex_add_module(arith   SOURCES ${_m}/arith.c   TARGETS m33 m0p MODEL ${model} VERSION 1.0.0 OUTPUT_DIR ${_o} ${_sign})
    list(APPEND PEX_TEST_MODULE_FILES ${arith_PEX_FILES})
    pex_add_module(counter SOURCES ${_m}/counter.c TARGETS m33 m0p MODEL ${model} VERSION 1.0.0 OUTPUT_DIR ${_o} ${_reent} ${_sign})
    list(APPEND PEX_TEST_MODULE_FILES ${counter_PEX_FILES})
    pex_add_module(imports SOURCES ${_m}/imports.c TARGETS m33 m0p MODEL ${model} VERSION 1.0.0 OUTPUT_DIR ${_o} ${_sign})
    list(APPEND PEX_TEST_MODULE_FILES ${imports_PEX_FILES})
    pex_add_module(needboard SOURCES ${_m}/needboard.c TARGETS m33 m0p MODEL ${model} VERSION 1.0.0 OUTPUT_DIR ${_o} ${_sign})
    list(APPEND PEX_TEST_MODULE_FILES ${needboard_PEX_FILES})
    pex_add_module(app     SOURCES ${_m}/app.c     TARGETS m33 m0p MODEL ${model} KIND app VERSION 1.0.0 OUTPUT_DIR ${_o} ${_sign})
    list(APPEND PEX_TEST_MODULE_FILES ${app_PEX_FILES})
    pex_add_module(memtest SOURCES ${_m}/memtest.c TARGETS m33 m0p MODEL ${model} VERSION 1.0.0 OUTPUT_DIR ${_o} ${_sign})
    list(APPEND PEX_TEST_MODULE_FILES ${memtest_PEX_FILES})
    pex_add_module(crash   SOURCES ${_m}/crash.c   TARGETS m33 m0p MODEL ${model} VERSION 1.0.0 OUTPUT_DIR ${_o} ${_sign})
    list(APPEND PEX_TEST_MODULE_FILES ${crash_PEX_FILES})
    pex_add_module(big     SOURCES ${_m}/big.c     TARGETS m33 m0p MODEL ${model} VERSION 1.0.0 OUTPUT_DIR ${_o} ${_sign})
    list(APPEND PEX_TEST_MODULE_FILES ${big_PEX_FILES})
endforeach()

# The gateway's test modules (bt/fs/net/bus/sys groups): M33, model B.
# PEX_TEST_GW: ON to build them (needs nothing but the SDK). crashapp and
# hangapp go to their own folder: they exist for the simulator's fault tests
# and are never part of the set uploaded to a real board.
if(PEX_TEST_GW)
    foreach(g gwbus gwfs gwnet gwbt)
        pex_add_module(${g} SOURCES ${_m}/gw/${g}.c TARGETS m33 MODEL pic KIND app VERSION 1.0.0
                       OUTPUT_DIR ${PEX_TEST_OUT}/gw ${_sign})
        list(APPEND PEX_TEST_MODULE_FILES ${${g}_PEX_FILES})
    endforeach()
    foreach(g crashapp hangapp)
        pex_add_module(${g} SOURCES ${_m}/gw/${g}.c TARGETS m33 MODEL pic KIND app VERSION 1.0.0
                       OUTPUT_DIR ${PEX_TEST_OUT}/gw-faults ${_sign})
        list(APPEND PEX_TEST_MODULE_FILES ${${g}_PEX_FILES})
    endforeach()
endif()
