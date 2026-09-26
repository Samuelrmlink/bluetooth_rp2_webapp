# bluetooth_rp2_webapp
#
#   export PICO_SDK_PATH=$HOME/.pico-sdk/sdk/2.3.0
#   export PICO_TOOLCHAIN_PATH=$HOME/.pico-sdk/toolchain/15_2_Rel1
#   cmake -S . -B build -G Ninja && cmake --build build
#
# Build options are NOT declared here -- they live in Kconfig, which is why
# this file stays short. tools/genconfig.py turns Kconfig + configs/*_defconfig
# into build/autoconf.h (included by our sources AND by the third-party
# lwipopts.h / tusb_config.h / btstack_config.h) and build/config.cmake, which
# is what gates the target_sources() below.
cmake_minimum_required(VERSION 3.13)

# SDK 2.3.0 hard-requires picotool 2.3.0; ~/.pico-sdk ships 2.2.0-a4.
set(PICOTOOL_FORCE_FETCH_FROM_GIT ON CACHE BOOL "" FORCE)

set(PICO_BOARD pico2_w CACHE STRING "Board type")
set(PICO_PLATFORM rp2350-arm-s CACHE STRING "Platform")
set(APP_DEFCONFIG "${CMAKE_CURRENT_LIST_DIR}/configs/pico2w_defconfig"
    CACHE FILEPATH "Kconfig defaults used when build/.config does not exist yet")

include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)
set(CMAKE_C_STANDARD 11)
project(btgw C CXX ASM)

# --- Kconfig -------------------------------------------------------------
find_package(Python3 REQUIRED COMPONENTS Interpreter)
execute_process(
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/tools/genconfig.py
            ${CMAKE_CURRENT_LIST_DIR} ${APP_DEFCONFIG} ${CMAKE_BINARY_DIR}
    RESULT_VARIABLE kconf_rc OUTPUT_VARIABLE kconf_out ERROR_VARIABLE kconf_out)
if(NOT kconf_rc EQUAL 0)
    message(FATAL_ERROR "kconfig failed:\n${kconf_out}")
endif()
message(STATUS "${kconf_out}")
include(${CMAKE_BINARY_DIR}/config.cmake)
# Reconfigure when the option definitions or the defaults change.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_LIST_DIR}/Kconfig
    ${CMAKE_CURRENT_LIST_DIR}/src/net/Kconfig
    ${CMAKE_CURRENT_LIST_DIR}/src/bt/Kconfig
    ${CMAKE_CURRENT_LIST_DIR}/src/lua/Kconfig
    ${CMAKE_CURRENT_LIST_DIR}/src/io/Kconfig
    ${APP_DEFCONFIG})

pico_sdk_init()

add_executable(btgw
    src/main.c
    src/api/api.c
    src/api/ops.c
    src/net/http.c
    src/net/ws.c
    src/fs/fs.c
    src/fs/littlefs/lfs.c
    src/fs/littlefs/lfs_util.c
    src/config/config.c
)

target_include_directories(btgw PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/include
    ${CMAKE_CURRENT_LIST_DIR}/src
    ${CMAKE_CURRENT_LIST_DIR}/src/fs
    ${CMAKE_CURRENT_LIST_DIR}/third_party/tiny-AES-c
    ${CMAKE_BINARY_DIR}            # autoconf.h
)

target_link_libraries(btgw
    pico_stdlib
    pico_unique_id
    pico_flash
    hardware_flash
    pico_cyw43_arch_lwip_poll
    pico_lwip
)

if(CONFIG_BT_LE)
    target_sources(btgw PRIVATE src/bt/bt.c src/bt/registry.c src/bt/le/central.c src/bt/le/gattdb.c src/bt/le/bond.c src/bt/le/gattserver.c)
    target_link_libraries(btgw pico_btstack_ble pico_btstack_cyw43 pico_btstack_flash_bank)
endif()
if(CONFIG_BT_CLASSIC)
    target_link_libraries(btgw pico_btstack_classic)
endif()

if(CONFIG_IO_PINS)
    target_sources(btgw PRIVATE src/io/pins.c src/io/io_ops.c)
    target_link_libraries(btgw hardware_gpio)
    if(CONFIG_IO_ADC)
        target_link_libraries(btgw hardware_adc)
    endif()
    if(CONFIG_IO_I2C)
        target_link_libraries(btgw hardware_i2c)
    endif()
    if(CONFIG_IO_SPI)
        target_link_libraries(btgw hardware_spi)
    endif()
    if(CONFIG_IO_UART)
        target_link_libraries(btgw hardware_uart)
    endif()
    if(CONFIG_IO_PWM)
        target_link_libraries(btgw hardware_pwm)
    endif()
endif()

if(CONFIG_PROTO_ANKER)
    target_sources(btgw PRIVATE src/codec/anker_frame.c src/codec/anker_tlv.c)
    if(CONFIG_PROTO_ANKER_CRYPTO)
        # uECC comes in with pico_btstack_ble already; we only add the AES.
        target_sources(btgw PRIVATE
            src/crypto/ghash.c
            src/crypto/anker_crypto.c
            third_party/tiny-AES-c/aes.c)
    endif()
endif()

if(CONFIG_LUA_ENABLE)
    add_subdirectory(third_party/lua)
    target_sources(btgw PRIVATE src/lua/lrt.c src/lua/lbind.c src/lua/lops.c)
    if(CONFIG_PROTO_ANKER)
        target_sources(btgw PRIVATE src/lua/lbind_proto.c)
    endif()
    target_link_libraries(btgw lua)
endif()

if(CONFIG_NET_WIFI)
    target_sources(btgw PRIVATE src/net/wifi.c)
endif()
if(CONFIG_NET_USB_NCM)
    target_sources(btgw PRIVATE src/net/usb_ncm.c src/net/dhcpserver.c src/usb/usb_descriptors.c)
    target_link_libraries(btgw tinyusb_device tinyusb_board)
endif()
if(CONFIG_NET_MDNS)
    target_sources(btgw PRIVATE src/net/mdns.c)
    target_link_libraries(btgw pico_lwip_mdns)
endif()

target_compile_definitions(btgw PRIVATE
    PICO_STDIO_USB_ENABLE_RESET_VIA_VENDOR_INTERFACE=1
    PICO_ENABLE_USB_RESET_VIA_VENDOR_INTERFACE=1
    PICO_USB_RESET_MS_OS_20_DESCRIPTOR_ITF=2
    LFS_NO_DEBUG=1
    LFS_NAME_MAX=64
    CYW43_HOST_NAME=\"${CONFIG_NET_HOSTNAME}\"
)

# The console is RTT over SWD: no UART is wired from the probe, and CDC-ACM is
# off. See tools/debug.sh.
pico_enable_stdio_rtt(btgw 1)
pico_enable_stdio_uart(btgw 0)
pico_enable_stdio_usb(btgw 0)

# Caps FLASH at 2 MiB so the firmware can never silently grow over the
# filesystem. NOT pico_override_flash_size(), which also redefines
# PICO_FLASH_SIZE_BYTES and would then stop hardware_flash writing past it.
pico_add_linker_script_override_path(btgw ${CMAKE_CURRENT_LIST_DIR}/linker
    FILES pico_flash_region.ld)

pico_add_extra_outputs(btgw)
