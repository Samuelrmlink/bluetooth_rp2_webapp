# Differential test firmware for the SD card's SDIO driver (docs/sd.md):
# the same seeded script (sddiff.c) on OUR driver and on carlk3's
# no-OS-FatFS-SD-SDIO-SPI-RPi-Pico, each run in mcusim against the same
# simulated card by tools/sdio_diff.py, which compares what they did.
#
# carlk3's library is fetched at a pinned commit into THIS build directory
# only. Its SDIO core is ZuluSCSI's GPL-3.0 code: it is never committed to
# this MIT tree and never linked into the gateway (tools/run_tests.py
# t_sd_guards). These ELFs are test instruments, not something to ship.
#
#   PICO_SDK_PATH=... cmake -S test/sdio_diff -B build-sdio-diff -G Ninja
#   cmake --build build-sdio-diff
#   -> sddiff_ours_raw.elf sddiff_carlk3_raw.elf sddiff_ours_fs.elf sddiff_carlk3_fs.elf
cmake_minimum_required(VERSION 3.13)
set(PICO_BOARD pico2_w CACHE STRING "Board type")
set(PICO_PLATFORM rp2350-arm-s CACHE STRING "Platform")
include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)
project(sdio_diff C CXX ASM)
pico_sdk_init()

set(BTGW ${CMAKE_CURRENT_LIST_DIR}/../..)
# The pins both drivers get (carlk3's rule CLK = D0 - 2 holds).
set(SDDIFF_DEFS SDDIFF_CLK=2 SDDIFF_CMD=3 SDDIFF_D0=4 SDDIFF_DETECT=9)

include(FetchContent)
FetchContent_Declare(carlk3
    GIT_REPOSITORY https://github.com/carlk3/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico.git
    GIT_TAG d5e453404cdbfaa55ab30d285b6ab0b730e84a05)       # main, 2025-02-10 (v3.6.2 + fixes)
FetchContent_GetProperties(carlk3)
if(NOT carlk3_POPULATED)
    FetchContent_Populate(carlk3)
endif()
add_subdirectory(${carlk3_SOURCE_DIR}/src carlk3)

foreach(scen raw fs)
    # ours: the gateway's driver sources, unchanged; its FatFs for the fs run
    add_executable(sddiff_ours_${scen} sddiff.c be_ours.c port_min.c
        ${BTGW}/src/sd/sd_card.c ${BTGW}/src/sd/sd_sdio.c ${BTGW}/src/sd/sd_crc.c
        ${BTGW}/src/sd/sdio_hw_pico.c ${BTGW}/src/sd/sd_diskio.c
        ${BTGW}/third_party/fatfs/ff.c ${BTGW}/third_party/fatfs/ffunicode.c)
    pico_generate_pio_header(sddiff_ours_${scen} ${BTGW}/src/sd/sdio.pio)
    target_include_directories(sddiff_ours_${scen} PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${BTGW}/src ${BTGW}/third_party/fatfs)
    target_compile_definitions(sddiff_ours_${scen} PRIVATE ${SDDIFF_DEFS} SDDIFF_SCENARIO_${scen}=1 SDDIFF_OURS=1)
    target_link_libraries(sddiff_ours_${scen} pico_stdlib hardware_pio hardware_dma hardware_clocks)
    pico_enable_stdio_rtt(sddiff_ours_${scen} 1)
    pico_enable_stdio_uart(sddiff_ours_${scen} 0)

    # carlk3's, with its own FatFs (both R0.15)
    add_executable(sddiff_carlk3_${scen} sddiff.c be_carlk3.c)
    target_include_directories(sddiff_carlk3_${scen} PRIVATE ${CMAKE_CURRENT_LIST_DIR})
    target_compile_definitions(sddiff_carlk3_${scen} PRIVATE ${SDDIFF_DEFS} SDDIFF_SCENARIO_${scen}=1 SDDIFF_CARLK3=1)
    target_link_libraries(sddiff_carlk3_${scen} pico_stdlib no-OS-FatFS-SD-SDIO-SPI-RPi-Pico)
    pico_enable_stdio_rtt(sddiff_carlk3_${scen} 1)
    pico_enable_stdio_uart(sddiff_carlk3_${scen} 0)
endforeach()
