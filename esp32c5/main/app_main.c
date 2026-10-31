/*
 * ESP32-C5 image. Station credentials come from cfg/wifi.json on the littlefs
 * partition, the same file the RP2350 store writes. An empty store does not
 * join. A pending OTA image stays pending until image state-write --confirm,
 * which does not reboot.
 */
#include <string.h>

#include "esp_log.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_ota_ops.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "cfg_wifi.h"
#include "fs_lfs.h"
#include "http_api.h"
#include "mdns_adv.h"
#include "pex_host.h"
#include "wifi_sta.h"

void smp_udp_start(void);

static const char *TAG = "btgw";

static void poll_task(void *arg) {
    (void)arg;
    for (;;) {
        pex_host_poll();
        cfg_tick();
        mdns_adv_poll();
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
    pex_host_init();
    smp_udp_start();
    wifi_start_from_cfg();
    http_api_start();
    mdns_adv_start();
    xTaskCreate(poll_task, "poll", 8192, NULL, 4, NULL);
    ESP_LOGI(TAG, "wifi mode %s, %d saved network(s)", cfg_mode(), cfg_net_count());
}
