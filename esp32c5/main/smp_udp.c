/*
 * MCUmgr over UDP port 1337. One datagram in, one datagram out. The handler
 * may write flash; this task is the only caller, so the writes are serial.
 */
#include "ota/smp.h"

#include <string.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

bool img_mgmt_test_armed(void);
const esp_partition_t *img_mgmt_test_partition(void);
bool smp_reset_requested(void);

static const char *TAG = "smp";

static void smp_task(void *arg) {
    (void)arg;
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) {
        ESP_LOGE(TAG, "socket failed");
        vTaskDelete(NULL);
        return;
    }
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(1337),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
        ESP_LOGE(TAG, "bind 1337 failed");
        close(fd);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "listening on udp :1337");
    uint8_t rx[SMP_BUF], tx[SMP_BUF];
    for (;;) {
        struct sockaddr_in from;
        socklen_t flen = sizeof from;
        int n = recvfrom(fd, rx, sizeof rx, 0, (struct sockaddr *)&from, &flen);
        if (n <= 0) continue;
        size_t out = smp_process(rx, (size_t)n, tx, sizeof tx);
        if (out) sendto(fd, tx, out, 0, (struct sockaddr *)&from, flen);
        if (smp_reset_requested()) {
            vTaskDelay(pdMS_TO_TICKS(150));
            if (img_mgmt_test_armed()) {
                const esp_partition_t *p = img_mgmt_test_partition();
                if (!p || esp_ota_set_boot_partition(p) != ESP_OK) {
                    ESP_LOGE(TAG, "could not arm the other slot; staying up");
                    continue;
                }
                ESP_LOGI(TAG, "otadata set to slot %c", p->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0 ? 'A' : 'B');
            }
            esp_restart();
        }
    }
}

void smp_udp_start(void) {
    xTaskCreate(smp_task, "smp", 8192, NULL, 5, NULL);
}
