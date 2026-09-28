/*
 * ota.status -- the human-readable side of the OTA machinery, for the web
 * UI and wsapi. Uploads themselves go over SMP (tools/ota_release.py and
 * smpmgr), not through here.
 */
#include <stdio.h>
#include "api/ops.h"
#include "ota/ota_slot.h"
#include "ota/smp.h"

bool img_mgmt_test_armed(void);
bool img_mgmt_upload_active(uint32_t *off, uint32_t *len);
int img_mgmt_slot_check(int which);
bool img_mgmt_slot_file_hash(int which, uint8_t out[32]);

static char buf[512];

static void op_ota_status(const char *params, api_reply_fn reply, void *ctx, long id) {
    (void)params;
    int me = ota_slot_active();
    char va[16], vb[16];
    ota_slot_version_str(0, va, sizeof va);
    ota_slot_version_str(1, vb, sizeof vb);
    uint32_t off = 0, len = 0;
    bool up = img_mgmt_upload_active(&off, &len);
    int other = me < 0 ? -1 : me ^ 1;
    char other_sha[65] = "";
    uint8_t fh[32];
    if (other >= 0 && img_mgmt_slot_file_hash(other, fh)) {
        for (int i = 0; i < 32; i++) snprintf(other_sha + i * 2, 3, "%02x", fh[i]);
    }
    int chk = other < 0 ? -1 : img_mgmt_slot_check(other);
    snprintf(buf, sizeof buf,
             "{\"slot\":%d,\"active\":\"%c\",\"version\":\"%s\",\"other_version\":\"%s\","
             "\"boot_type\":%u,\"buy_pending\":%s,\"bought\":%s,\"test_armed\":%s,"
             "\"upload\":{\"active\":%s,\"off\":%lu,\"len\":%lu},"
             "\"other_image_check\":%d,\"other_matches_signed\":%s,"
             "\"other_file_sha256\":\"%s\",\"smp_port\":1337}",
             me, me < 0 ? '-' : "AB"[me], me == 1 ? vb : va, me == 1 ? va : vb,
             ota_boot_type(), ota_buy_pending() ? "true" : "false", ota_bought() ? "true" : "false",
             img_mgmt_test_armed() ? "true" : "false", up ? "true" : "false",
             (unsigned long)off, (unsigned long)len, chk,
             chk == 0 ? "true" : "false", other_sha);
    api_reply_ok(reply, ctx, id, buf);
}

static const op_def_t ota_ops[] = {
    { "ota.status", op_ota_status },
    { NULL, NULL }
};

const op_module_t op_module_ota = { .ns = "ota", .ops = ota_ops, .topics = NULL, .init = NULL };
