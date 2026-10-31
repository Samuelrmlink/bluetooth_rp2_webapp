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
        "pex_host.c"
        "../../src/ota/smp.c"
        "../../src/ota/cbor.c"
        "../../src/crypto/sha256.c"
        "../../src/fs/littlefs/lfs.c"
        "../../src/fs/littlefs/lfs_util.c"
    INCLUDE_DIRS
        "."
        "../../src"
        "../../src/fs/littlefs"
    REQUIRES
        app_update
        bootloader_support
        esp_wifi
        esp_netif
        esp_event
        nvs_flash
        mbedtls
        esp_http_server
        esp_timer
        spi_flash
        esp_mm
)

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
    LFS_NAME_MAX=64)

include(${CMAKE_CURRENT_LIST_DIR}/../../pex/runtime/pex_runtime.cmake)
pex_runtime_add(${COMPONENT_LIB} NO_PICO_PORT)
pex_add_module(hello
    SOURCES ${CMAKE_CURRENT_LIST_DIR}/../modules/hello/hello.c
    TARGETS c5
    MODEL reloc
    KIND app
    VERSION 0.1.0
    OUTPUT_DIR ${CMAKE_BINARY_DIR}/pex)
add_dependencies(${COMPONENT_LIB} pex_hello_reloc)
