/* gwbt: the bt group against a peer advertising a 128-bit service: scan, find
 * by UUID, connect, wait for GATT, read a characteristic (default the Device
 * Information model string, 180a/2a24), subscribe a notify/indicate
 * characteristic of the service, write one byte to it with completion, and
 * note whether the peer sent it back (mcusim's generic peer with echo = true
 * does), then disconnect.
 * APP_RUN {"uuid": service, "chr": characteristic, "rsvc": .., "rchr": ..,
 * "timeout": ms}; op pextest.bt reports. */
#include "pex.h"

static char uuid[40], chr_uuid[40], rsvc[40] = "180a", rchr[40] = "2a24";
static uint8_t addr[6];
static int state, listen_id, ctrl = -1, model_chr = -1, mode;
static uint32_t t0, timeout = 25000;
static char res[176] = "pending", model[32], name[32];
static int got_echo, wrote_ok, mtu;
static const uint8_t ping[1] = { 0x5a };

static void finish(const char *how) {
    pex_snprintf(res, sizeof res, "%s name=%s model=%s mtu=%d echo=%d write_done=%d", how, name, model, mtu,
                 got_echo, wrote_ok);
    if (state >= 2) { pex_bt_disconnect(addr); }
    pex_bt_unlisten(listen_id);
    state = 9;
    pex_app_exit(0);
}

static void on_event(void *arg, const pex_bt_event_t *e) {
    if (state == 9) { return; }
    switch (e->kind) {
    case PEX_BT_READY:
        mtu = pex_bt_mtu(addr);
        model_chr = pex_bt_find(addr, rsvc, rchr, 0);
        ctrl = pex_bt_find(addr, uuid, chr_uuid, 0);
        if (model_chr < 0 || ctrl < 0) { finish("fail:find"); return; }
        {
            int props = pex_bt_props(addr, ctrl);
            mode = (props & 0x20) ? 2 : (props & 0x10) ? 1 : 0;      /* indicate, notify */
        }
        pex_bt_read(addr, model_chr);
        state = 3;
        break;
    case PEX_BT_READ:
        if (e->chr == model_chr && state == 3) {
            size_t n = e->len < sizeof model - 1 ? e->len : sizeof model - 1;
            memcpy(model, e->data, n);
            model[n] = 0;
            if (mode && pex_bt_subscribe(addr, ctrl, mode) < 0) { finish("fail:subscribe"); return; }
            if (pex_bt_write(addr, ctrl, ping, 1, 1, 1) < 0) { finish("fail:write"); return; }
            state = 4;
        }
        break;
    case PEX_BT_NOTIFY:
    case PEX_BT_INDICATE:
        if (e->chr == ctrl && e->len == 1 && e->data[0] == ping[0]) { got_echo = 1; }
        if (wrote_ok) { finish(wrote_ok == 1 ? "ok" : "fail:write status"); }
        break;
    case PEX_BT_WRITE:
        wrote_ok = e->status == 0 ? 1 : -e->status;
        if (wrote_ok != 1 || got_echo || !mode) { finish(wrote_ok == 1 ? "ok" : "fail:write status"); }
        else { t0 = pex_ms() - timeout + 3000; }                  /* an echo, if any, within 3 s */
        break;
    case PEX_BT_FAILED:
        finish("fail:link");
        break;
    }
}

static int tick(void *arg) {
    if (state == 9) { return PEX_TASK_DONE; }
    if (pex_ms() - t0 > timeout) { finish(wrote_ok == 1 ? "ok" : "fail:timeout"); return PEX_TASK_DONE; }
    if (state == 0) {
        pex_bt_dev_t d[4];
        int n = pex_bt_devices(d, 4, 0, uuid);
        if (n > 0) {
            memcpy(addr, d[0].addr, 6);
            pex_snprintf(name, sizeof name, "%s", d[0].name);
            pex_bt_scan_stop();
            listen_id = pex_bt_listen(addr, on_event, 0);
            int rc = pex_bt_connect(addr);
            if (rc != PEX_EPENDING && rc != PEX_OK) { finish("fail:connect"); return PEX_TASK_DONE; }
            state = 2;
        }
    }
    return PEX_TASK_AGAIN;
}

static int op_bt(void *arg, const char *params, char *reply, size_t cap) {
    return pex_snprintf(reply, cap, "{\"result\":\"%s\",\"state\":%d}", res, state);
}

int pex_main(const pex_host_t *host, uint32_t reason, void *arg) {
    if (reason == PEX_INIT) { return pex_bus_op("pextest.bt", op_bt, 0); }
    if (reason == PEX_APP_RUN) {
        pex_json_str(arg, "uuid", uuid, sizeof uuid);
        pex_json_str(arg, "chr", chr_uuid, sizeof chr_uuid);
        pex_json_str(arg, "rsvc", rsvc, sizeof rsvc);
        pex_json_str(arg, "rchr", rchr, sizeof rchr);
        if (!uuid[0] || !chr_uuid[0]) { pex_snprintf(res, sizeof res, "fail:need uuid and chr"); state = 9; return PEX_OK; }
        int64_t t;
        if (pex_json_int(arg, "timeout", &t)) { timeout = (uint32_t)t; }
        state = 0;
        t0 = pex_ms();
        pex_bt_scan(0, 1);
        pex_sched_every(200, tick, 0);
    }
    if (reason == PEX_APP_STOP && state != 9) { finish("stopped"); }
    return PEX_OK;
}
