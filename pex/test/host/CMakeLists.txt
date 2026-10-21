# PEX test host: the runtime alone (no Lua, no gateway), in a minimal pico-sdk
# firmware. One source, two chips:
#   cmake -S pex/test/host -B build-pext-m33 -G Ninja -DPICO_PLATFORM=rp2350-arm-s -DPICO_BOARD=pico2
#   cmake -S pex/test/host -B build-pext-m0p -G Ninja -DPICO_PLATFORM=rp2040 -DPICO_BOARD=pico
# pex/test/run_sim.py does both and runs them in mcusim.
cmake_minimum_required(VERSION 3.20)
if(NOT DEFINED ENV{PICO_SDK_PATH})
    set(ENV{PICO_SDK_PATH} $ENV{HOME}/.pico-sdk/sdk/2.3.0)
endif()
if(NOT DEFINED ENV{PICO_TOOLCHAIN_PATH})
    set(ENV{PICO_TOOLCHAIN_PATH} $ENV{HOME}/.pico-sdk/toolchain/15_2_Rel1)
endif()
set(PICOTOOL_FORCE_FETCH_FROM_GIT ON CACHE BOOL "" FORCE)
include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)
project(pex_test C CXX ASM)
set(CMAKE_C_STANDARD 11)
pico_sdk_init()

include(${CMAKE_CURRENT_LIST_DIR}/../../runtime/pex_runtime.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../modules/modules.cmake)

set(_b ${CMAKE_BINARY_DIR}/pex_bundle.bin)
add_custom_command(OUTPUT ${_b} ${CMAKE_BINARY_DIR}/pex_testkey.h
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/mkbundle.py --out ${_b}
            --keyhdr ${CMAKE_BINARY_DIR}/pex_testkey.h --key ${CMAKE_BINARY_DIR}/pex_test_key.pem
            --mods ${PEX_TEST_OUT} --copy-to ${CMAKE_BINARY_DIR}/pexfiles
    DEPENDS ${PEX_TEST_MODULE_FILES} ${CMAKE_CURRENT_LIST_DIR}/mkbundle.py ${PEX_ROOT}/tools/pexlink.py VERBATIM)
add_custom_target(pex_bundle DEPENDS ${_b})
file(WRITE ${CMAKE_BINARY_DIR}/bundle.S
    ".section .rodata.pex_bundle,\"a\"\n.balign 8\n.global pex_bundle\npex_bundle:\n.incbin \"${_b}\"\n")
set_source_files_properties(${CMAKE_BINARY_DIR}/bundle.S PROPERTIES OBJECT_DEPENDS ${_b})

add_executable(pex_test main.c ${CMAKE_BINARY_DIR}/bundle.S
    $ENV{PICO_SDK_PATH}/lib/btstack/3rd-party/micro-ecc/uECC.c)
add_dependencies(pex_test pex_bundle)
target_include_directories(pex_test PRIVATE ${CMAKE_BINARY_DIR} $ENV{PICO_SDK_PATH}/lib/btstack/3rd-party/micro-ecc)
target_compile_definitions(pex_test PRIVATE ENABLE_MICRO_ECC_ECDSA=1 ENABLE_MICO_ECC_ECDSA=1 uECC_CURVE=uECC_secp256r1)
pex_runtime_add(pex_test GROUPS board SIG_UECC)
target_link_libraries(pex_test pico_stdlib pico_unique_id hardware_clocks)
pico_enable_stdio_uart(pex_test 1)
pico_enable_stdio_usb(pex_test 0)
pico_add_extra_outputs(pex_test)
