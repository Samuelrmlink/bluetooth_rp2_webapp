# BLE peripheral app. Built by this directory's standalone project.
# The .pex files are loaded by the host; they are not linked into firmware.
include(${CMAKE_CURRENT_LIST_DIR}/../../sdk/cmake/pex.cmake)

set(_d ${CMAKE_CURRENT_LIST_DIR})
if(NOT PEX_BLE_OUT)
    set(PEX_BLE_OUT ${CMAKE_BINARY_DIR}/pex)
endif()

# c5 reloc accepts R_RISCV_32 only. A switch jump table is ADD32/SUB32.
pex_add_module(ble_periph
    SOURCES ${_d}/ble_periph.c
    TARGETS m33 c5
    MODEL reloc
    KIND app
    VERSION 1.0.0
    DESCRIPTION "BLE peripheral pairing exercise"
    CFLAGS -fno-jump-tables
    OUTPUT_DIR ${PEX_BLE_OUT})
