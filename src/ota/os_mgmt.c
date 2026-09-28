/*
 * MCUmgr OS group (0): echo, taskstat, reset, mcumgr params, os info,
 * bootloader info.
 *
 * "Tasks" here are Lua scripts -- the scheduler in src/lua/lrt.c is this
 * firmware's task system -- plus a `main` pseudo-task whose context-switch
 * count is the number of loop passes.
 */
#include "ota/smp.h"
#include "ota/ota_slot.h"
#include "autoconf.h"

#include <stdio.h>
#include <string.h>
#include "pico/time.h"
#if CONFIG_LUA_ENABLE
#include "lua/lrt.h"
#endif

bool img_mgmt_test_armed(void);

static uint32_t reset_at_ms;      /* 0 = no reset pending */
static uint32_t passes;

static int h_echo(const smp_req_t *req, cbor_enc_t *e) {
    char d[256] = "";
    cbor_get_tstr(req->payload, req->plen, "d", d, sizeof d);
    cbor_kv_tstr(e, "r", d);
    return 0;
}

static int h_taskstat(const smp_req_t *req, cbor_enc_t *e) {
    (void)req;
    cbor_tstr(e, "tasks");
    cbor_map_open(e);
    cbor_tstr(e, "main");
    cbor_map_open(e);
    cbor_kv_uint(e, "prio", 0);
    cbor_kv_uint(e, "tid", 0);
    cbor_kv_uint(e, "state", 1);
    cbor_kv_uint(e, "cswcnt", passes);
    cbor_kv_uint(e, "runtime", to_ms_since_boot(get_absolute_time()));
    cbor_close(e);
#if CONFIG_LUA_ENABLE
    for (int i = 0; ; i++) {
        lrt_task_info_t t;
        int r = lrt_task_info(i, &t);
        if (r < 0) break;
        if (r == 0) continue;
        cbor_tstr(e, t.name);
        cbor_map_open(e);
        cbor_kv_uint(e, "prio", 1);
        cbor_kv_uint(e, "tid", (uint64_t)i + 1);
        cbor_kv_uint(e, "state", t.state);
        cbor_kv_uint(e, "cswcnt", t.resumes);
        cbor_kv_uint(e, "runtime", t.runtime_ms);
        cbor_close(e);
    }
#endif
    cbor_close(e);
    return 0;
}

static int h_reset(const smp_req_t *req, cbor_enc_t *e) {
    (void)req; (void)e;
    /* Deferred so the reply gets out first. */
    reset_at_ms = to_ms_since_boot(get_absolute_time()) + 150;
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
    char v[16] = "0.0.0";
    int me = ota_slot_active();
    if (me >= 0) ota_slot_version_str(me, v, sizeof v);
    char out[96];
    snprintf(out, sizeof out, "btgw %s rp2350 slot %c", v, me >= 0 ? "AB"[me] : '-');
    cbor_kv_tstr(e, "output", out);
    return 0;
}

static int h_bootloader_info(const smp_req_t *req, cbor_enc_t *e) {
    char q[16] = "";
    cbor_get_tstr(req->payload, req->plen, "query", q, sizeof q);
    if (q[0] == 0) {
        cbor_kv_tstr(e, "bootloader", "RP2350 ROM");
    } else if (strcmp(q, "mode") == 0) {
        /* 5 = direct XIP with revert: the honest MCUboot analogue of the
         * ROM's A/B pick plus try-before-you-buy. */
        cbor_kv_int(e, "mode", 5);
        cbor_kv_bool(e, "no-downgrade", false);
    } else {
        return MGMT_ERR_EINVAL;
    }
    return 0;
}

void smp_os_tick(uint32_t now) {
    passes++;
    if (reset_at_ms && (int32_t)(now - reset_at_ms) >= 0) {
        ota_reboot(img_mgmt_test_armed());
    }
}

const smp_cmd_t smp_os_cmds[] = {
    { SMP_GROUP_OS, 0, true,  h_echo },
    { SMP_GROUP_OS, 2, false, h_taskstat },
    { SMP_GROUP_OS, 5, true,  h_reset },
    { SMP_GROUP_OS, 6, false, h_params },
    { SMP_GROUP_OS, 7, false, h_info },
    { SMP_GROUP_OS, 8, false, h_bootloader_info },
    { 0, 0, false, NULL },
};
