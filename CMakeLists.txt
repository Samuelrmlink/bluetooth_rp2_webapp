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
    ${CMAKE_CURRENT_LIST_DIR}/src/ota/Kconfig
    ${CMAKE_CURRENT_LIST_DIR}/src/sd/Kconfig
    ${CMAKE_CURRENT_LIST_DIR}/src/pex/Kconfig
    ${APP_DEFCONFIG})

pico_sdk_init()

add_executable(btgw
    src/main.c
    src/api/api.c
    src/api/json_escape.c
    src/net/cyw43_wait.c
    src/api/ops.c
    src/net/http.c
    src/net/ws.c
    src/fs/fs.c
    src/fs/fs_migrate.c
    src/ota/crc32.c
    src/util/inflate.c
    src/fs/littlefs/lfs.c
    src/fs/littlefs/lfs_util.c
    src/config/config.c
    src/util/crumb.c
    src/util/libc_slim.c
    src/util/stackmark.c
)

# Runs once per board (the pre-PEX filesystem move, fs_migrate.h): size over speed.
set_property(SOURCE src/fs/fs_migrate.c APPEND PROPERTY COMPILE_OPTIONS -Os)

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

# The cyw43 driver blocks inside cyw43_await_background_or_timeout_us() waiting
# for an ioctl response, and this firmware's single loop is the only thing
# pumping TinyUSB -- so a radio that stops answering takes USB down with it,
# which is exactly how the board was found unreachable on both interfaces at
# once. src/net/cyw43_wait.c services USB before delegating to the real wait,
# and records how long the driver waited. See that file for why tud_task() is
# safe to call there and usb_ncm_poll() is not.
target_link_options(btgw PRIVATE
    -Wl,--wrap=cyw43_await_background_or_timeout_us)

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

# The SPI bus layer: the only code that touches an SPI block (spi.xfer and
# every SPI driver go through it). See src/io/spibus.h.
if(CONFIG_SPI_BUS)
    target_sources(btgw PRIVATE src/io/spibus.c src/io/spibus_pico.c)
    target_link_libraries(btgw hardware_spi hardware_dma hardware_irq pico_sync)
endif()

if(CONFIG_NET_ETH_W5500)
    target_sources(btgw PRIVATE src/net/w5500.c src/net/eth_w5500.c src/net/eth_cfg.c src/net/eth_ops.c)
endif()
# The SD card (src/sd, FatFs). SDIO adds the PIO programs; SPI goes through
# the bus layer above (CONFIG_SD_BUS_SPI selects CONFIG_SPI_BUS).
if(CONFIG_SD)
    target_sources(btgw PRIVATE
        src/sd/sd_card.c src/sd/sd_crc.c src/sd/sd_diskio.c src/sd/sd_store.c
        src/sd/sd_api.c src/sd/sd_core.c src/sd/sd_ops.c src/sd/sd_worker_coro.c
        src/sd/sd_port_pico.c src/sd/sd_jobs.c
        third_party/fatfs/ff.c third_party/fatfs/ffunicode.c)
    target_include_directories(btgw PRIVATE third_party/fatfs)
    if(CONFIG_SD_BUS_SDIO)
        target_sources(btgw PRIVATE src/sd/sd_sdio.c src/sd/sdio_hw_pico.c)
        pico_generate_pio_header(btgw ${CMAKE_CURRENT_LIST_DIR}/src/sd/sdio.pio)
        target_link_libraries(btgw hardware_pio hardware_dma hardware_clocks)
    endif()
    if(CONFIG_SD_BUS_SPI)
        target_sources(btgw PRIVATE src/sd/sd_spi.c)
    endif()
endif()
# The per-netif DHCP server: USB-NCM, the SoftAP and Ethernet's server mode.
if(CONFIG_NET_USB_NCM OR CONFIG_NET_WIFI OR CONFIG_NET_ETH_W5500)
    target_sources(btgw PRIVATE src/net/dhcpserver.c)
endif()
if(CONFIG_NET_UPLINK)
    target_sources(btgw PRIVATE src/net/uplink.c src/net/uplink_core.c)
endif()
if(CONFIG_NET_FORWARD)
    target_sources(btgw PRIVATE src/net/fwd.c src/net/fwd_core.c)
endif()

if(CONFIG_PROTO_ANKER)
    target_sources(btgw PRIVATE src/codec/anker_frame.c src/codec/anker_tlv.c)
    if(CONFIG_PROTO_ANKER_CRYPTO)
        # uECC comes in with pico_btstack_ble already; we only add the AES.
        target_sources(btgw PRIVATE
            src/crypto/ghash.c
            src/crypto/anker_crypto.c
            src/crypto/sha256.c
            third_party/tiny-AES-c/aes.c)
    endif()
endif()

if(CONFIG_LUA_ENABLE)
    add_subdirectory(third_party/lua)
    target_sources(btgw PRIVATE src/lua/lrt.c src/lua/lheap.c src/lua/lwait.c src/lua/ljson.c src/lua/lbind.c src/lua/lops.c
        src/lua/lfile.c src/lua/lfile_path.c)
    # The file module is bound by flash and card speed, not by its own code.
    set_property(SOURCE src/lua/lfile.c src/lua/lfile_path.c APPEND PROPERTY COMPILE_OPTIONS -Os)
    if(CONFIG_PROTO_ANKER)
        target_sources(btgw PRIVATE src/lua/lbind_proto.c)
    endif()
    target_link_libraries(btgw lua)
endif()

if(CONFIG_NET_WIFI)
    target_sources(btgw PRIVATE src/net/wifi.c src/net/wifi_ops.c)
endif()
# Wall-clock time (sys.time, TLS dates). sntp_time.c is always built: with
# SNTP off it just reports "no time".
target_sources(btgw PRIVATE src/net/sntp_time.c)
if(CONFIG_NET_SNTP)
    target_link_libraries(btgw pico_lwip_sntp)
endif()
# The Lua HTTP client (src/net/httpc.c, src/lua/lhttp.c) and, for https://,
# mbedTLS configured by include/mbedtls_config.h. mbedTLS is compiled -Os:
# it is the largest thing added, the firmware slot is 864 KiB, and none of
# it is on a hot path a script can feel (a handshake is dominated by the
# elliptic-curve and RSA arithmetic, which -Os barely slows).
if(CONFIG_NET_HTTP_CLIENT)
    target_sources(btgw PRIVATE src/net/httpc.c src/net/httpc_parse.c src/lua/lhttp.c)
    # Not on any hot path either: the client moves at most a few hundred KB/s.
    set_property(SOURCE src/net/httpc.c src/net/httpc_parse.c src/lua/lhttp.c
                 APPEND PROPERTY COMPILE_OPTIONS -Os)
    if(CONFIG_NET_HTTP_TLS)
        target_link_libraries(btgw pico_mbedtls)
        foreach(_lib crypto x509 tls)
            get_target_property(_srcs pico_mbedtls_${_lib} INTERFACE_SOURCES)
            set_property(SOURCE ${_srcs} APPEND PROPERTY COMPILE_OPTIONS -Os)
        endforeach()
    endif()
endif()
if(CONFIG_NET_WIREGUARD)
    # Vendored from smartalock/wireguard-lwip; see third_party/wireguard-lwip/
    # VENDORED.txt for what was taken and what was left behind.
    target_sources(btgw PRIVATE
        src/net/wg.c
        src/net/wg_ops.c
        third_party/wireguard-lwip/src/wireguard.c
        third_party/wireguard-lwip/src/wireguardif.c
        third_party/wireguard-lwip/src/crypto.c
        third_party/wireguard-lwip/src/crypto/refc/blake2s.c
        third_party/wireguard-lwip/src/crypto/refc/chacha20.c
        third_party/wireguard-lwip/src/crypto/refc/chacha20poly1305.c
        third_party/wireguard-lwip/src/crypto/refc/poly1305-donna.c
        third_party/wireguard-lwip/src/crypto/refc/x25519.c)
    target_include_directories(btgw PRIVATE third_party/wireguard-lwip/src)
    target_link_libraries(btgw pico_rand)
    # wireguardif.h opens with "lwip/arch.h", and lwIP's arch.h includes
    # arch/cc.h WITHOUT first including lwipopts.h. The pico_lwip port
    # typedefs sys_prot_t only "#if NO_SYS", so on that path it is not
    # defined, cc.h's include guard then blocks the later correct include,
    # and lwip/sys.h fails on a type nobody declared. Our own sources never
    # hit it because they include lwip/netif.h (which does pull in opt.h)
    # first. Forcing opt.h ahead of everything fixes it for the vendored
    # translation units without editing them -- see VENDORED.txt, which says
    # they carry no local changes, and should go on being able to say so.
    set_source_files_properties(
        third_party/wireguard-lwip/src/wireguard.c
        third_party/wireguard-lwip/src/wireguardif.c
        PROPERTIES COMPILE_OPTIONS "-include;lwip/opt.h")
endif()

if(CONFIG_NET_USB_NCM)
    target_sources(btgw PRIVATE src/net/usb_ncm.c src/usb/usb_descriptors.c)
    target_link_libraries(btgw tinyusb_device tinyusb_board)
endif()
if(CONFIG_OTA)
    # OTA: A/B firmware slots picked by the RP2350 boot ROM, driven over
    # MCUmgr/SMP on UDP 1337. See src/ota/ and partitions.json.
    target_sources(btgw PRIVATE
        src/ota/cbor.c src/ota/smp.c src/ota/smp_udp.c src/ota/ota_slot.c
        src/ota/img_mgmt.c src/ota/os_mgmt.c src/ota/fs_mgmt.c src/ota/stat_mgmt.c
        src/ota/ota_ops.c)
    if(NOT CONFIG_PROTO_ANKER_CRYPTO)
        target_sources(btgw PRIVATE src/crypto/sha256.c)
    endif()
    # micro-ecc for the signature check. pico_btstack_ble carries it already;
    # without BLE it is compiled in here.
    target_include_directories(btgw PRIVATE ${PICO_SDK_PATH}/lib/btstack/3rd-party/micro-ecc)
    if(NOT CONFIG_BT_LE)
        target_sources(btgw PRIVATE ${PICO_SDK_PATH}/lib/btstack/3rd-party/micro-ecc/uECC.c)
    endif()
    target_link_libraries(btgw pico_flash)
    # See __wrap_rom_pick_ab_partition_during_update in src/ota/ota_slot.c: the
    # SDK's version reads ROM work-area offsets that do not hold on this
    # silicon, and the CYW43 driver's firmware-partition lookup goes through it.
    target_link_options(btgw PRIVATE -Wl,--wrap=rom_pick_ab_partition_during_update)
    # BTstack's micro-ecc compiles ECDSA (uECC_verify) only behind this.
    # (both spellings: uECC.c:528 guards vli_numBits behind a typo.)
    target_compile_definitions(btgw PRIVATE ENABLE_MICRO_ECC_ECDSA=1 ENABLE_MICO_ECC_ECDSA=1)

    # The WiFi blob lives in its own partition (id 0x776966696669726d), never
    # OTA'd; this also builds btgw_wifi_firmware.uf2 to provision it once.
    pico_use_wifi_firmware_partition(btgw NO_EMBEDDED_PT)

    # Version: the ROM picks the A/B slot with the higher VERSION item, so
    # every OTA build must carry a larger one. MINOR is the commit count
    # unless overridden (cmake -DBTGW_VERSION_MINOR=n) for uncommitted work.
    set(BTGW_VERSION_MAJOR 1)
    if(NOT BTGW_VERSION_MINOR)
        execute_process(COMMAND git rev-list --count HEAD
            WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
            OUTPUT_VARIABLE BTGW_VERSION_MINOR OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        if(NOT BTGW_VERSION_MINOR)
            set(BTGW_VERSION_MINOR 0)
        endif()
    endif()
    message(STATUS "btgw image version ${BTGW_VERSION_MAJOR}.${BTGW_VERSION_MINOR}")
    pico_set_binary_version(btgw MAJOR ${BTGW_VERSION_MAJOR} MINOR ${BTGW_VERSION_MINOR})
    pico_hash_binary(btgw)
    # Try-before-you-buy: the image boots ONLY via a flash-update reboot and
    # reverts unless bought (ota_tick). OFF for the first image provisioned
    # over SWD, which the ROM boots normally.
    option(BTGW_TBYB "Build a try-before-you-buy image (OTA releases)" ON)
    if(BTGW_TBYB)
        target_compile_definitions(btgw PRIVATE PICO_CRT0_IMAGE_TYPE_TBYB=1)
    endif()
    target_compile_definitions(btgw PRIVATE BTGW_VERSION_STR=\"${BTGW_VERSION_MAJOR}.${BTGW_VERSION_MINOR}\")
endif()

# PEX native modules (docs/pex-format.md): the standalone runtime in pex/,
# the gateway's groups and ops in src/pex/, pexlua when there is Lua.
if(CONFIG_PEX)
    include(${CMAKE_CURRENT_LIST_DIR}/pex/runtime/pex_runtime.cmake)
    pex_runtime_add(btgw GROUPS sys board bus fs net bt NO_PICO_PORT SIG_UECC)
    target_sources(btgw PRIVATE ${CMAKE_CURRENT_LIST_DIR}/pex/runtime/port/pico/pex_port_pico.c
        src/pex/pex_gw.c src/pex/pex_gw_sys.c src/pex/pex_gw_bus.c src/pex/pex_gw_fs.c src/pex/pex_gw_xip.c)
    if(CONFIG_NET_HTTP_CLIENT)
        target_sources(btgw PRIVATE src/pex/pex_gw_net.c)
    endif()
    if(CONFIG_BT_LE)
        target_sources(btgw PRIVATE src/pex/pex_gw_bt.c)
    endif()
    if(NOT CONFIG_LUA_ENABLE)
        target_sources(btgw PRIVATE src/lua/lfile_path.c)
    endif()
    if(CONFIG_PEX_LUA)
        include(${CMAKE_CURRENT_LIST_DIR}/pex/lua/pexlua.cmake)
        pexlua_add(btgw)
        target_sources(btgw PRIVATE src/pex/pex_gw_lua.c)
    endif()
    # micro-ecc: the OTA key checks module signatures (spec 10).
    target_include_directories(btgw PRIVATE ${PICO_SDK_PATH}/lib/btstack/3rd-party/micro-ecc)
    set_property(SOURCE src/pex/pex_gw.c src/pex/pex_gw_sys.c src/pex/pex_gw_bus.c src/pex/pex_gw_fs.c src/pex/pex_gw_xip.c
        src/pex/pex_gw_net.c src/pex/pex_gw_bt.c src/pex/pex_gw_lua.c APPEND PROPERTY COMPILE_OPTIONS -Os)
    # Paying for PEX (~38 KB) in the 864 KiB slot, measured on the SD build
    # (2026-10-21; it had ~8 KB left): code that is not on a hot path goes
    # -Os. Lua's COMPILE-time half -- code generator, parser, lexer, (un)dump
    # -- and its cold libraries (-31 KB): scripts run from cached bytecode, and
    # the interpreter core (lvm, ldo, lgc, ltable, lapi, lstring, lobject, ltm)
    # and the string library stay -O3. The HTTP server (-6 KB) is bound by TCP
    # and flash, not by the CPU.
    if(CONFIG_LUA_ENABLE)
        foreach(_l lcode lparser llex ldump lundump lauxlib lbaselib lmathlib ltablib lutf8lib lcorolib ldebug)
            set_property(SOURCE ${CMAKE_CURRENT_LIST_DIR}/third_party/lua/${_l}.c TARGET_DIRECTORY lua
                APPEND PROPERTY COMPILE_OPTIONS -Os)
        endforeach()
    endif()
    set_property(SOURCE src/net/http.c APPEND PROPERTY COMPILE_OPTIONS -Os)
    # XIP (0.7) took the SD build past its slot: with native modules doing
    # the work and Lua a launcher, the firmware's Lua glue (bindings, the
    # runtime, file/HTTP/JSON bridges; the interpreter core above and the
    # allocator, lheap.c, every Lua allocation goes through, stay -O3)
    # goes -Os, as do the cold paths -- OTA/SMP (bound by the link and the
    # flash), the one-time LittleFS migration, littlefs itself (bound by
    # flash erase/program).
    file(GLOB _lua_glue ${CMAKE_CURRENT_LIST_DIR}/src/lua/*.c)
    list(FILTER _lua_glue EXCLUDE REGEX "/lheap\\.c$")
    file(GLOB _ota_srcs ${CMAKE_CURRENT_LIST_DIR}/src/ota/*.c)
    set_property(SOURCE ${_lua_glue} ${_ota_srcs} src/fs/fs_migrate.c src/fs/littlefs/lfs.c
        APPEND PROPERTY COMPILE_OPTIONS -Os)
endif()

if(CONFIG_NET_MDNS)
    # lwIP's responder, vendored with one hook for the firmware's own record
    # groups (third_party/lwip-mdns/VENDORED.txt) -- in place of the SDK's
    # pico_lwip_mdns, which compiles the same three files unmodified.
    target_sources(btgw PRIVATE src/net/mdns.c src/net/mdns_records.c
        third_party/lwip-mdns/mdns.c
        third_party/lwip-mdns/mdns_out.c
        third_party/lwip-mdns/mdns_domain.c)
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

# Caps FLASH at one A/B slot (864 KiB, partitions.json) so the firmware can
# never silently grow over the other slot or the filesystem. NOT pico_override_flash_size(), which also redefines
# PICO_FLASH_SIZE_BYTES and would then stop hardware_flash writing past it.
pico_add_linker_script_override_path(btgw ${CMAKE_CURRENT_LIST_DIR}/linker
    FILES pico_flash_region.ld)

# Flash, measured: the SD build's slot is 864 KiB and HTTPS (mbedTLS) took
# most of what was left. BTstack (~105 KB at -O3) and FatFs (~27 KB) go -Os:
# both are protocol/bookkeeping code bound by the radio and the card, not by
# the CPU (the card worker yields every ~2 ms regardless, and BTstack's hot
# loop is waiting on HCI). Measured on the SD build: ~45 KB.
foreach(_lib pico_btstack_base pico_btstack_ble pico_btstack_cyw43 pico_btstack_flash_bank)
    if(TARGET ${_lib})
        get_target_property(_srcs ${_lib} INTERFACE_SOURCES)
        # ...except its crypto: micro-ecc is also Lua's crypto.ecdh (a P-256
        # handshake step is ~41 ms already) and rijndael its AES.
        list(FILTER _srcs EXCLUDE REGEX "micro-ecc|rijndael")
        if(_srcs)
            set_property(SOURCE ${_srcs} APPEND PROPERTY COMPILE_OPTIONS -Os)
        endif()
    endif()
endforeach()
if(CONFIG_SD)
    set_property(SOURCE third_party/fatfs/ff.c third_party/fatfs/ffunicode.c APPEND PROPERTY COMPILE_OPTIONS -Os)
endif()

pico_add_extra_outputs(btgw)
