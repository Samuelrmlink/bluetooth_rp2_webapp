/*
 * MCUmgr OS group for the C5 image: echo, reset, params, os info, and the
 * bootloader name. Reset is deferred until the UDP reply has been sent.
 */
#include "ota/smp.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_ota_ops.h"

bool img_mgmt_test_armed(void);

static bool reset_req;

static int slot_of_running(void) {
    const esp_partition_t *p = esp_ota_get_running_partition();
    if (!p) return -1;
    if (p->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0) return 0;
    if (p->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_1) return 1;
    return -1;
}

static int h_echo(const smp_req_t *req, cbor_enc_t *e) {
    char d[256] = "";
    cbor_get_tstr(req->payload, req->plen, "d", d, sizeof d);
    cbor_kv_tstr(e, "r", d);
    return 0;
}

static int h_reset(const smp_req_t *req, cbor_enc_t *e) {
    (void)req;
    (void)e;
    reset_req = true;
    return 0;
}

static int h_params(const smp_req_t *req, cbor_enc_t *e) {
    (void)req;
    cbor_kv_uint(e, "buf_size", SMP_BUF);
    cbor_kv_uint(e, "buf_count", 1);
    return 0;
}

static int h_info(const smp_req_t *req, cbor_enc_t *e) {
    (void)req;
    const esp_app_desc_t *d = esp_app_get_description();
    int me = slot_of_running();
    char out[96];
    snprintf(out, sizeof out, "btgw %s esp32c5 slot %c",
             d && d->version[0] ? d->version : "0.0.0",
             me >= 0 ? "AB"[me] : '-');
    cbor_kv_tstr(e, "output", out);
    return 0;
}

static int h_bootloader_info(const smp_req_t *req, cbor_enc_t *e) {
    char q[16] = "";
    cbor_get_tstr(req->payload, req->plen, "query", q, sizeof q);
    if (q[0] == 0) {
        cbor_kv_tstr(e, "bootloader", "ESP-IDF");
    } else if (strcmp(q, "mode") == 0) {
        /* Same number the RP2350 image reports: a test boot that reverts
         * unless the image is confirmed. */
        cbor_kv_int(e, "mode", 5);
        cbor_kv_bool(e, "no-downgrade", true);
    } else {
        return MGMT_ERR_EINVAL;
    }
    return 0;
}

bool smp_reset_requested(void) { return reset_req; }

const smp_cmd_t smp_os_cmds[] = {
    { SMP_GROUP_OS, 0, true,  h_echo },
    { SMP_GROUP_OS, 5, true,  h_reset },
    { SMP_GROUP_OS, 6, false, h_params },
    { SMP_GROUP_OS, 7, false, h_info },
    { SMP_GROUP_OS, 8, false, h_bootloader_info },
    { 0, 0, false, NULL },
};
