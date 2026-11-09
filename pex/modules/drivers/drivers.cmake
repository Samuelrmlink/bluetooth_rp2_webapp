# Anker and Pinecil PEX drivers.
# Included by the firmware when CONFIG_PEX is on, and by the standalone
# project in this directory:
#   cmake -S pex/modules/drivers -B /tmp/pex-drivers -G Ninja && cmake --build /tmp/pex-drivers
# The .pex files are uploaded to lfs:/pex/. They are not linked into the firmware.
include(${CMAKE_CURRENT_LIST_DIR}/../../sdk/cmake/pex.cmake)

set(_d ${CMAKE_CURRENT_LIST_DIR})
if(NOT PEX_DRIVER_OUT)
    set(PEX_DRIVER_OUT ${CMAKE_BINARY_DIR}/pex-drivers)
endif()

pex_add_module(anker
    SOURCES ${_d}/anker_app.c ${_d}/anker_adv.c ${_d}/anker_wire.c ${_d}/anker_models.c
    TARGETS m33
    MODEL reloc
    KIND app
    LUA
    VERSION 1.1.0
    DESCRIPTION "Anker SOLIX / Prime driver"
    OUTPUT_DIR ${PEX_DRIVER_OUT})

pex_add_module(pinecil
    SOURCES ${_d}/pinecil_app.c ${_d}/pinecil_live.c
    TARGETS m33
    MODEL reloc
    KIND app
    VERSION 1.0.0
    DESCRIPTION "Pinecil V2 driver"
    OUTPUT_DIR ${PEX_DRIVER_OUT})

pex_add_module(ws2812
    SOURCES ${_d}/ws2812_app.c
    TARGETS m33
    MODEL reloc
    KIND app
    VERSION 1.0.0
    DESCRIPTION "WS2812B strip"
    OUTPUT_DIR ${PEX_DRIVER_OUT})
