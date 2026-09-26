cmake_minimum_required(VERSION 3.13)

# Official Raspberry Pi Pico 2 W (RP2350A). Target of this project.
set(PICO_BOARD pico2_w CACHE STRING "Board type")
set(PICO_PLATFORM rp2350-arm-s CACHE STRING "Platform")

# SDK 2.3.0 hard-requires picotool 2.3.0; ~/.pico-sdk only has 2.2.0-a4, so let
# CMake fetch and build the matching one. Same workaround as km003c and pm220.
set(PICOTOOL_FORCE_FETCH_FROM_GIT ON CACHE BOOL "" FORCE)

include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)
project(bringup C CXX ASM)
set(CMAKE_C_STANDARD 11)
pico_sdk_init()

add_executable(bringup main.c)
target_link_libraries(bringup pico_stdlib)

# No UART is wired from the probe to the target, so the console rides SWD via
# RTT. USB CDC is on ONLY as the cable diagnostic for this bring-up app; the
# real firmware keeps CDC-ACM off for now.
pico_enable_stdio_rtt(bringup 1)
pico_enable_stdio_usb(bringup 1)
pico_enable_stdio_uart(bringup 0)

pico_add_extra_outputs(bringup)
