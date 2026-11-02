# One radio role per image. The Thread role has no stack yet, so selecting
# it fails here instead of linking a board that claims both.
if(CONFIG_C5_RADIO_THREAD)
    message(FATAL_ERROR "CONFIG_C5_RADIO_THREAD is specified and not built")
endif()
if(CONFIG_ESP_WIFI_ENABLED AND CONFIG_IEEE802154_ENABLED)
    message(FATAL_ERROR "ESP32-C5 radio is Wi-Fi or 802.15.4, not both")
endif()
if(CONFIG_IEEE802154_ENABLED)
    message(FATAL_ERROR "ESP32-C5 image does not enable 802.15.4")
endif()
# Wi-Fi + BLE uses the IDF software coexistence NimBLE expects. Anything
# else (coexistence with no BLE, or a Thread image) still fails configure.
if(CONFIG_ESP_COEX_SW_COEXIST_ENABLE AND NOT CONFIG_C5_BLE)
    message(FATAL_ERROR "ESP32-C5 software coexistence is off: one radio role per image")
endif()

set(c5_reqs
    app_update
    bootloader_support
    esp_wifi
    esp_netif
    esp_event
    nvs_flash
    mbedtls
    esp_http_server
    spi_flash
    esp_mm
    esp_driver_spi
    esp_driver_gpio
    esp_timer
    esp_hw_support
    lwip
)
if(CONFIG_C5_BLE)
    list(APPEND c5_reqs bt esp_driver_uart)
endif()

idf_component_register(
    SRCS
        "app_main.c"
        "smp_udp.c"
        "img_mgmt.c"
        "os_mgmt.c"
        "groups_stub.c"
        "fs_lfs.c"
        "cfg_wifi.c"
        "wifi_sta.c"
        "http_api.c"
        "mdns_adv.c"
        "peer_fwd.c"
        "../../src/net/peer_fwd_msg.c"
        "pex_host.c"
        "ble_pex.c"
        "role_hw.c"
        "spibus_c5.c"
        "eth_c5.c"
        "../../src/io/spibus.c"
        "../../src/net/w5500.c"
        "../../src/net/eth_cfg.c"
        "../../src/net/gw_core.c"
        "../../src/net/dhcpserver.c"
        "../../src/ota/smp.c"
        "../../src/ota/cbor.c"
        "../../src/crypto/sha256.c"
        "../../src/net/ws_link.c"
        "../../src/fs/littlefs/lfs.c"
        "../../src/fs/littlefs/lfs_util.c"
    INCLUDE_DIRS
        "."
        "../../src"
        "../../src/fs/littlefs"
    REQUIRES ${c5_reqs}
)

# Requirement expansion runs before Kconfig, so CONFIG_C5_BLE is invisible
# there and cannot add bt to REQUIRES. The normal pass can. NimBLE include
# dirs exist only when the host choice is NimBLE, which select cannot set.
if(CONFIG_C5_BLE AND NOT CMAKE_BUILD_EARLY_EXPANSION)
    if(NOT CONFIG_BT_NIMBLE_ENABLED)
        message(FATAL_ERROR "CONFIG_C5_BLE requires CONFIG_BT_NIMBLE_ENABLED (the BT host choice ignores select)")
    endif()
    idf_component_optional_requires(PUBLIC bt)
    idf_component_optional_requires(PUBLIC esp_driver_uart)
endif()

# wpa_supplicant already exports hmac_sha256. This image never calls the
# copy in sha256.c; the Pico build is unchanged.
set_source_files_properties(../../src/crypto/sha256.c PROPERTIES
    COMPILE_DEFINITIONS "hmac_sha256=btgw_hmac_sha256")

# Same on-disk name length the RP2350 volume uses. The host packer
# (tools/lfs_pack.c) must pass the same LFS_NAME_MAX.
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    LFS_NO_DEBUG=1
    LFS_NO_WARN=1
    LFS_NO_ERROR=1
    LFS_NAME_MAX=64
    SPIBUS_C5=1)

include(${CMAKE_CURRENT_LIST_DIR}/../../pex/runtime/pex_runtime.cmake)
pex_runtime_add(${COMPONENT_LIB} NO_PICO_PORT GROUPS bt)
pex_add_module(hello
    SOURCES ${CMAKE_CURRENT_LIST_DIR}/../modules/hello/hello.c
    TARGETS c5
    MODEL reloc
    KIND app
    VERSION 0.1.0
    OUTPUT_DIR ${CMAKE_BINARY_DIR}/pex)
add_dependencies(${COMPONENT_LIB} pex_hello_reloc)
