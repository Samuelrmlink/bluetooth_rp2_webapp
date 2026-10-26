/*
 * MCUmgr stat group (2): named groups of uint32 counters.
 *
 * `stat list` names them, `stat <group>` dumps one. Everything is a plain
 * uint32 (the protocol allows nothing else), so a client gets the same
 * numbers sys.info and lua.stats show, without the JSON.
 */
#include "ota/smp.h"
#include "ota/ota_slot.h"
#include "autoconf.h"

#include <stdio.h>
#include <string.h>
#include "pico/time.h"
#include "hardware/watchdog.h"
#include "lwip/stats.h"
#include "net/cyw43_wait.h"
#if CONFIG_LUA_ENABLE
#include "lua/lrt.h"
#endif
#if CONFIG_BT_LE
#include "bt/bt.h"
#include "bt/le/central.h"
#endif
#if CONFIG_NET_USB_NCM
#include "net/usb_ncm.h"
#endif

bool img_mgmt_upload_active(uint32_t *off, uint32_t *len);

typedef void (*fill_fn)(cbor_enc_t *e);

static void proto(cbor_enc_t *e, const struct stats_proto *p) {
    cbor_kv_uint(e, "xmit", p->xmit);   cbor_kv_uint(e, "recv", p->recv);
    cbor_kv_uint(e, "fw", p->fw);       cbor_kv_uint(e, "drop", p->drop);
    cbor_kv_uint(e, "chkerr", p->chkerr); cbor_kv_uint(e, "lenerr", p->lenerr);
    cbor_kv_uint(e, "memerr", p->memerr); cbor_kv_uint(e, "rterr", p->rterr);
    cbor_kv_uint(e, "proterr", p->proterr); cbor_kv_uint(e, "opterr", p->opterr);
    cbor_kv_uint(e, "err", p->err);     cbor_kv_uint(e, "cachehit", p->cachehit);
}
static void f_ip(cbor_enc_t *e)  { proto(e, &lwip_stats.ip); }
static void f_udp(cbor_enc_t *e) { proto(e, &lwip_stats.udp); }
static void f_tcp(cbor_enc_t *e) { proto(e, &lwip_stats.tcp); }
static void f_mem(cbor_enc_t *e) {
    cbor_kv_uint(e, "avail", lwip_stats.mem.avail); cbor_kv_uint(e, "used", lwip_stats.mem.used);
    cbor_kv_uint(e, "max", lwip_stats.mem.max);     cbor_kv_uint(e, "err", lwip_stats.mem.err);
    uint32_t used = 0, max = 0, err = 0;
    for (int i = 0; i < MEMP_MAX; i++) {
        used += lwip_stats.memp[i]->used; max += lwip_stats.memp[i]->max; err += lwip_stats.memp[i]->err;
    }
    cbor_kv_uint(e, "pool_used", used); cbor_kv_uint(e, "pool_max", max); cbor_kv_uint(e, "pool_err", err);
}
static void f_radio(cbor_enc_t *e) {
    cbor_kv_uint(e, "wait_max_us", cyw43_wait_max_us());
    cbor_kv_uint(e, "wait_max_at_ms", cyw43_wait_max_at_ms());
    cbor_kv_uint(e, "wait_usb_max_us", cyw43_wait_usb_max_us());
    cbor_kv_uint(e, "wait_total_ms", (uint32_t)(cyw43_wait_total_us() / 1000));
    cbor_kv_uint(e, "wait_calls", cyw43_wait_calls());
}
static void f_sys(cbor_enc_t *e) {
    cbor_kv_uint(e, "uptime_s", to_ms_since_boot(get_absolute_time()) / 1000);
    cbor_kv_uint(e, "watchdog_reboot", watchdog_caused_reboot() ? 1 : 0);
    cbor_kv_uint(e, "boot_type", ota_boot_type());
    cbor_kv_uint(e, "slot", ota_slot_active() < 0 ? 0xffffffffu : (uint32_t)ota_slot_active());
    cbor_kv_uint(e, "buy_pending", ota_buy_pending() ? 1 : 0);
    uint32_t off, len;
    bool up = img_mgmt_upload_active(&off, &len);
    cbor_kv_uint(e, "upload_off", up ? off : 0);
    cbor_kv_uint(e, "upload_len", up ? len : 0);
}
#if CONFIG_LUA_ENABLE
static void f_lua(cbor_enc_t *e) {
    lrt_stats_t s; lrt_stats(&s);
    cbor_kv_uint(e, "arena_bytes", s.arena_bytes); cbor_kv_uint(e, "used", s.used);
    cbor_kv_uint(e, "peak", s.peak);               cbor_kv_uint(e, "hook_kills", s.hook_kills);
    cbor_kv_uint(e, "slice_overruns", s.slice_overruns); cbor_kv_uint(e, "scripts_running", s.running);
}
#endif
#if CONFIG_BT_LE
static void f_bt(cbor_enc_t *e) {
    cbor_kv_uint(e, "unhandled_events", bt_unhandled_count());
    cbor_kv_uint(e, "notify_dropped", le_notify_dropped());
    cbor_kv_uint(e, "radio_busy", bt_radio_busy() ? 1 : 0);
}
#endif
#if CONFIG_NET_USB_NCM
static void f_ncm(cbor_enc_t *e) {
    usb_ncm_stats_t s; usb_ncm_stats(&s);
    cbor_kv_uint(e, "rx_ok", s.rx_ok); cbor_kv_uint(e, "rx_drop", s.rx_drop);
    cbor_kv_uint(e, "tx_ok", s.tx_ok); cbor_kv_uint(e, "tx_drop", s.tx_drop);
    cbor_kv_uint(e, "recoveries", s.recoveries);
}
#endif

static const struct { const char *name; fill_fn fn; } groups[] = {
    { "sys", f_sys }, { "radio", f_radio },
    { "lwip_ip", f_ip }, { "lwip_udp", f_udp }, { "lwip_tcp", f_tcp }, { "lwip_mem", f_mem },
#if CONFIG_LUA_ENABLE
    { "lua", f_lua },
#endif
#if CONFIG_BT_LE
    { "bt", f_bt },
#endif
#if CONFIG_NET_USB_NCM
    { "ncm", f_ncm },
#endif
};

static int h_show(const smp_req_t *req, cbor_enc_t *e) {
    char name[32];
    if (!cbor_get_tstr(req->payload, req->plen, "name", name, sizeof name)) return MGMT_ERR_EINVAL;
    for (size_t i = 0; i < sizeof groups / sizeof groups[0]; i++) {
        if (strcmp(groups[i].name, name) == 0) {
            cbor_kv_tstr(e, "name", name);
            cbor_tstr(e, "fields");
            cbor_map_open(e);
            groups[i].fn(e);
            cbor_close(e);
            return 0;
        }
    }
    return SMP_GERR(2);   /* STAT_MGMT_ERR_INVALID_GROUP */
}

static int h_list(const smp_req_t *req, cbor_enc_t *e) {
    (void)req;
    cbor_tstr(e, "stat_list");
    cbor_array_open(e);
    for (size_t i = 0; i < sizeof groups / sizeof groups[0]; i++) cbor_tstr(e, groups[i].name);
    cbor_close(e);
    return 0;
}

const smp_cmd_t smp_stat_cmds[] = {
    { SMP_GROUP_STAT, 0, false, h_show },
    { SMP_GROUP_STAT, 1, false, h_list },
    { 0, 0, false, NULL },
};
