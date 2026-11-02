/*
 * ESP32-C5 image. Station credentials come from cfg/wifi.json on the littlefs
 * partition, the same file the RP2350 store writes. An empty store does not
 * join. A pending OTA image stays pending until image state-write --confirm,
 * which does not reboot.
 */
#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "cfg_wifi.h"
#include "eth_c5.h"
#include "fs_lfs.h"
#include "http_api.h"
#include "mdns_adv.h"
#include "pex_host.h"
#include "role_hw.h"
#include "wifi_sta.h"

#include "lwip/opt.h"
#include "lwip/tcpip.h"

#if !LWIP_TCPIP_CORE_LOCKING
#error "CONFIG_LWIP_TCPIP_CORE_LOCKING is required: eth_poll calls lwIP off the tcpip thread"
#endif

void smp_udp_start(void);

static const char *TAG = "btgw";

static void note_poll(const char *what) {
    char msg[80];
    snprintf(msg, sizeof msg, "%s %s free=%u large=%u", what,
             esp_app_get_description()->version,
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    role_hw_note(msg);
}

static void eth_task(void *arg) {
    (void)arg;
    for (;;) {
        LOCK_TCPIP_CORE();
        eth_poll();
        UNLOCK_TCPIP_CORE();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static void poll_task(void *arg) {
    (void)arg;
    for (;;) {
        pex_host_poll();
        role_hw_poll();
        cfg_tick();
        mdns_adv_poll();
        wifi_dhcp_poll();
        http_api_poll();
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void app_main(void) {
    esp_err_t nvs = nvs_flash_init();
    if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs);

    const esp_partition_t *me = esp_ota_get_running_partition();
    esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
    if (me) esp_ota_get_state_partition(me, &st);
    ESP_LOGI(TAG, "slot %s version %s state %d",
             me ? me->label : "?", esp_app_get_description()->version, (int)st);
    if (st == ESP_OTA_IMG_PENDING_VERIFY)
        ESP_LOGW(TAG, "image is unconfirmed; a reboot before state-write --confirm rolls back");

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    if (fs_mount() != 0) ESP_LOGW(TAG, "littlefs did not mount");
    cfg_load();
    LOCK_TCPIP_CORE();
    bool eth_up = eth_init();
    UNLOCK_TCPIP_CORE();
    if (eth_up && xTaskCreate(eth_task, "eth", 12288, NULL, 4, NULL) != pdPASS) {
        ESP_LOGW(TAG, "ethernet poll task was not created");
    }
    pex_host_init();
    smp_udp_start();
    wifi_start_from_cfg();
    http_api_start();
    mdns_adv_start();
    /* 1.0.11 had 6524 bytes free and a 6144-byte hole, so an 8192 stack
     * cannot be created. 4096 fits that hole and the TCB. */
    note_poll("HEAP");
    if (xTaskCreate(poll_task, "poll", 4096, NULL, 4, NULL) == pdPASS)
        note_poll("POLL created");
    else
        note_poll("POLL absent");
    ESP_LOGI(TAG, "wifi mode %s, %d saved network(s)", cfg_mode(), cfg_net_count());
}
