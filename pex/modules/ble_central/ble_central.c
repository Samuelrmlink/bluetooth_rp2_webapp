/* BLE central. PEX_APP_RUN config: peer, io, mitm, passkey, policy, confirm,
 * forget, hold. Scans (name pex-periph when peer is empty), connects, pairs
 * unless policy is never, then reads service 1111 characteristic 2222.
 *
 * Pairing kinds below are used only when the host header does not define
 * them yet (next values after PEX_BT_FAILED). hold 0 stays connected. */
#include "pex.h"
#include <string.h>

#ifndef PEX_BT_JUST_WORKS
#define PEX_BT_JUST_WORKS 20
#endif
#ifndef PEX_BT_PASSKEY
#define PEX_BT_PASSKEY 21
#endif
#ifndef PEX_BT_CONFIRM
#define PEX_BT_CONFIRM 22
#endif
#ifndef PEX_BT_BONDED
#define PEX_BT_BONDED 23
#endif
#ifndef PEX_BT_PAIR_FAIL
#define PEX_BT_PAIR_FAIL 24
#endif
#ifndef PEX_BT_PASSKEY_DISP
#define PEX_BT_PASSKEY_DISP 25
#endif

enum { FORGET_NONE = 0, FORGET_PEER = 1, FORGET_ALL = 2 };
enum { SCAN_MS = 25000, CONNECT_MS = 15000 };

static const uint8_t ZERO[6];
static pex_bt_dev_t s_devs[8];

static char s_io[24];
static char s_policy[16];
static uint8_t s_want[6];
static uint8_t s_peer[6];
static uint32_t s_passkey, s_t0, s_conn_ms, s_link_ms;
static int s_mitm, s_have_io, s_have_mitm, s_have_policy, s_have_want, s_policy_never;
static int s_confirm_yes, s_forget, s_hold_s;
static int s_failed, s_forgot, s_bonded, s_method, s_have_peer;
static int s_found, s_connecting, s_connected, s_linked, s_ready, s_up;
static int s_pair_sent, s_chr, s_read_sent, s_read_done, s_hold_fired;
static int s_listen, s_task, s_scanning, s_running;

static void copy_str(char *d, size_t cap, const char *s) {
    size_t i = 0;
    if (!cap) { return; }
    while (s[i] && i + 1 < cap) { d[i] = s[i]; i++; }
    d[i] = 0;
}

static int key_present(const char *json, const char *key) {
    return pex_json_find(json, key, NULL) != NULL;
}

static int io_ok(const char *s) {
    return strcmp(s, "no_input_no_output") == 0 || strcmp(s, "display_only") == 0
        || strcmp(s, "display_yes_no") == 0 || strcmp(s, "keyboard_only") == 0
        || strcmp(s, "keyboard_display") == 0;
}

static void fail(const char *why) {
    if (s_failed) { return; }
    s_failed = 1;
    pex_log(PEX_LOG_INFO, "FAIL %s", why);
}

static void note_method(const char *method) {
    if (s_method) { return; }
    s_method = 1;
    pex_log(PEX_LOG_INFO, "PAIR %s", method);
}

static void log_hex(const uint8_t *data, unsigned len) {
    static const char H[] = "0123456789ABCDEF";
    char buf[81];
    unsigned i;
    if (!data) { len = 0; }
    if (len > 40) { len = 40; }
    for (i = 0; i < len; i++) {
        buf[i * 2] = H[data[i] >> 4];
        buf[i * 2 + 1] = H[data[i] & 0x0f];
    }
    buf[len * 2] = 0;
    pex_log(PEX_LOG_INFO, "VALUE %s", buf);
}

static void log_addr(const char *tag, const uint8_t *addr) {
    char txt[18];
    if (!addr) { addr = ZERO; }
    pex_bt_addr_str(addr, txt);
    pex_log(PEX_LOG_INFO, "%s %s", tag, txt);
}

static void take_addr(const uint8_t *addr) {
    if (!addr || memcmp(addr, ZERO, 6) == 0) { return; }
    memcpy(s_peer, addr, 6);
    s_have_peer = 1;
}

static const uint8_t *peer_or(const uint8_t *addr) {
    if (addr && memcmp(addr, ZERO, 6) != 0) { return addr; }
    return s_peer;
}

static int for_us(const uint8_t *addr) {
    if (!addr || memcmp(addr, ZERO, 6) == 0 || !s_have_peer) { return 1; }
    return memcmp(addr, s_peer, 6) == 0;
}

static void do_forget(void) {
    if (s_forgot || s_forget == FORGET_NONE) { return; }
    if (s_forget == FORGET_PEER) {
        if (!s_have_peer) { return; }
        pex_bt_forget(s_peer);
        pex_log(PEX_LOG_INFO, "FORGET peer");
    } else {
        pex_bt_forget(NULL);
        pex_log(PEX_LOG_INFO, "FORGET all");
    }
    s_forgot = 1;
}

static void maybe_read(void) {
    int c, rc;
    if (s_failed || s_read_done || s_read_sent || !s_bonded || !s_ready || !s_have_peer) { return; }
    if (s_chr < 0) {
        c = pex_bt_find(s_peer, "1111", "2222", 0);
        if (c == PEX_ESTATE) { return; }
        if (c < 0) { fail("find"); return; }
        s_chr = c;
    }
    rc = pex_bt_read(s_peer, s_chr);
    if (rc < 0 && rc != PEX_EPENDING) { fail("read"); return; }
    s_read_sent = 1;
}

static void on_bonded(const uint8_t *addr) {
    take_addr(addr);
    if (!s_method && strcmp(s_io, "no_input_no_output") == 0) { note_method("just_works"); }
    if (s_bonded) { return; }
    s_bonded = 1;
    log_addr("BONDED", s_have_peer ? s_peer : addr);
    maybe_read();
}

static void on_event(void *arg, const pex_bt_event_t *e) {
    int rc;
    (void)arg;
    if (!s_running || !for_us(e->addr)) { return; }
    switch (e->kind) {
    case PEX_BT_LINK_UP:
        take_addr(e->addr);
        if (!s_linked) { s_linked = 1; s_link_ms = pex_ms(); }
        if (!s_connected) {
            s_connected = 1;
            s_connecting = 0;
            s_up = 1;
            log_addr("CONNECTED", s_peer);
        }
        if (s_failed || s_pair_sent) { break; }
        s_pair_sent = 1;
        if (s_policy_never) {
            fail("policy");
            pex_bt_disconnect(s_peer);
            break;
        }
        rc = pex_bt_pair(s_peer);
        if (rc == PEX_ESTATE) { fail("policy"); pex_bt_disconnect(s_peer); }
        else if (rc < 0) { fail("pair"); }
        break;
    case PEX_BT_LINK_DOWN:
        s_up = 0;
        s_connected = 0;
        s_ready = 0;
        s_method = 0;
        s_bonded = 0;
        s_pair_sent = 0;
        s_chr = -1;
        s_read_sent = 0;
        break;
    case PEX_BT_READY:
        s_ready = 1;
        maybe_read();
        break;
    case PEX_BT_JUST_WORKS:
        note_method("just_works");
        break;
    case PEX_BT_PASSKEY:
        if (s_failed) { break; }
        take_addr(e->addr);
        note_method("passkey");
        pex_bt_passkey(peer_or(e->addr), s_passkey);
        break;
    case PEX_BT_PASSKEY_DISP:
        take_addr(e->addr);
        note_method("passkey");
        break;
    case PEX_BT_CONFIRM:
        if (s_failed) { break; }
        take_addr(e->addr);
        note_method("confirm");
        pex_bt_confirm(peer_or(e->addr), s_confirm_yes);
        break;
    case PEX_BT_BONDED:
        if (s_failed) { break; }
        on_bonded(e->addr);
        break;
    case PEX_BT_PAIR_FAIL:
        fail("pair");
        break;
    case PEX_BT_READ:
        if (s_failed || s_read_done || !s_read_sent) { break; }
        if (s_chr >= 0 && e->chr != s_chr) { break; }
        if (e->status != 0 && e->status != -1) { fail("read"); break; }
        log_hex(e->data, e->len);
        s_read_done = 1;
        do_forget();
        break;
    case PEX_BT_FAILED:
        if (!s_connected) { fail("connect"); }
        else if (s_read_sent && !s_read_done) { fail("read"); }
        else { fail("link"); }
        break;
    default:
        break;
    }
}

static int end_task(void) {
    s_task = 0;
    return PEX_TASK_DONE;
}

static int tick(void *arg) {
    uint32_t now;
    int n, i, rc;
    (void)arg;
    if (!s_running) { return end_task(); }
    now = pex_ms();
    if (!s_found) {
        if (now - s_t0 > SCAN_MS) {
            fail("scan");
            pex_bt_scan_stop();
            s_scanning = 0;
            return end_task();
        }
        n = pex_bt_devices(s_devs, 8, s_have_want ? NULL : "pex-periph", NULL);
        if (n > 8) { n = 8; }
        for (i = 0; i < n; i++) {
            if (s_have_want && memcmp(s_devs[i].addr, s_want, 6) != 0) { continue; }
            memcpy(s_peer, s_devs[i].addr, 6);
            s_have_peer = 1;
            s_found = 1;
            log_addr("FOUND", s_peer);
            pex_bt_scan_stop();
            s_scanning = 0;
            s_conn_ms = now;
            rc = pex_bt_connect(s_peer);
            if (rc != PEX_OK && rc != PEX_EPENDING) {
                fail("connect");
                return end_task();
            }
            s_connecting = 1;
            break;
        }
    } else if (s_connecting && !s_connected && !s_failed && now - s_conn_ms > CONNECT_MS) {
        fail("connect");
        return end_task();
    }
    if (s_linked && !s_hold_fired && s_hold_s > 0 && now - s_link_ms >= (uint32_t)s_hold_s * 1000u) {
        s_hold_fired = 1;
        if (!s_read_done && !s_failed && !s_bonded) { fail("timeout"); }
        if (s_have_peer) { pex_bt_disconnect(s_peer); }
        return end_task();
    }
    maybe_read();
    return PEX_TASK_AGAIN;
}

static const char *parse(const char *json) {
    copy_str(s_io, sizeof s_io, "no_input_no_output");
    s_policy[0] = 0;
    s_mitm = 0;
    s_have_io = 0;
    s_have_mitm = 0;
    s_have_policy = 0;
    s_have_want = 0;
    s_policy_never = 0;
    s_passkey = 123456;
    s_confirm_yes = 1;
    s_forget = FORGET_NONE;
    s_hold_s = 5;
    if (key_present(json, "peer")) {
        char p[20];
        if (!pex_json_str(json, "peer", p, sizeof p)) { return "config"; }
        if (p[0]) {
            if (pex_bt_addr(p, s_want) != PEX_OK) { return "config"; }
            s_have_want = 1;
        }
    }
    if (key_present(json, "io")) {
        if (!pex_json_str(json, "io", s_io, sizeof s_io) || !io_ok(s_io)) { return "config"; }
        s_have_io = 1;
    }
    if (key_present(json, "mitm")) {
        int64_t v;
        int b;
        if (pex_json_int(json, "mitm", &v)) {
            if (v != 0 && v != 1) { return "config"; }
            s_mitm = (int)v;
        } else if (pex_json_bool(json, "mitm", &b)) {
            s_mitm = b ? 1 : 0;
        } else {
            return "config";
        }
        s_have_mitm = 1;
    }
    if (key_present(json, "passkey")) {
        int64_t v;
        if (!pex_json_int(json, "passkey", &v) || v < 0 || v > 999999) { return "config"; }
        s_passkey = (uint32_t)v;
    }
    if (key_present(json, "policy")) {
        if (!pex_json_str(json, "policy", s_policy, sizeof s_policy)) { return "config"; }
        if (strcmp(s_policy, "never") && strcmp(s_policy, "on_demand") && strcmp(s_policy, "always")) {
            return "config";
        }
        s_have_policy = 1;
        s_policy_never = strcmp(s_policy, "never") == 0;
    }
    if (key_present(json, "confirm")) {
        char c[8];
        if (!pex_json_str(json, "confirm", c, sizeof c)) { return "config"; }
        if (strcmp(c, "yes") == 0) { s_confirm_yes = 1; }
        else if (strcmp(c, "no") == 0) { s_confirm_yes = 0; }
        else { return "config"; }
    }
    if (key_present(json, "forget")) {
        char f[8];
        if (!pex_json_str(json, "forget", f, sizeof f)) { return "config"; }
        if (strcmp(f, "none") == 0) { s_forget = FORGET_NONE; }
        else if (strcmp(f, "peer") == 0) { s_forget = FORGET_PEER; }
        else if (strcmp(f, "all") == 0) { s_forget = FORGET_ALL; }
        else { return "config"; }
    }
    if (key_present(json, "hold")) {
        int64_t v;
        if (!pex_json_int(json, "hold", &v) || v < 0 || v > 86400) { return "config"; }
        s_hold_s = (int)v;
    }
    return NULL;
}

static void stop(void) {
    s_running = 0;
    if (s_task > 0) {
        pex_sched_cancel(s_task);
        s_task = 0;
    }
    if (s_listen > 0) {
        pex_bt_unlisten(s_listen);
        s_listen = 0;
    }
    if (s_scanning) {
        pex_bt_scan_stop();
        s_scanning = 0;
    }
    if ((s_up || s_connecting) && s_have_peer) { pex_bt_disconnect(s_peer); }
    s_up = 0;
    s_connecting = 0;
}

static void clear_session(void) {
    s_failed = 0;
    s_forgot = 0;
    s_bonded = 0;
    s_method = 0;
    s_have_peer = 0;
    s_found = 0;
    s_connecting = 0;
    s_connected = 0;
    s_linked = 0;
    s_ready = 0;
    s_up = 0;
    s_pair_sent = 0;
    s_chr = -1;
    s_read_sent = 0;
    s_read_done = 0;
    s_hold_fired = 0;
    memset(s_peer, 0, sizeof s_peer);
}

static int start(const char *json) {
    const char *err;
    stop();
    clear_session();
    err = parse(json);
    if (err) { fail(err); return PEX_OK; }
    if (s_have_io || s_have_mitm) {
        if (pex_bt_set_io(s_have_io ? s_io : NULL, s_have_mitm ? s_mitm : -1) < 0) {
            fail("io");
            return PEX_OK;
        }
    }
    if (s_have_policy && pex_bt_set_policy(s_policy) < 0) {
        fail("policy");
        return PEX_OK;
    }
    pex_log(PEX_LOG_INFO, "READY role=central");
    pex_log(PEX_LOG_INFO, "IO %s mitm=%d", s_io, s_mitm);
    s_running = 1;
    s_listen = pex_bt_listen(NULL, on_event, NULL);
    if (s_listen <= 0) { fail("listen"); stop(); return PEX_OK; }
    if (pex_bt_scan(0, 1) < 0) { fail("scan"); stop(); return PEX_OK; }
    s_scanning = 1;
    s_t0 = pex_ms();
    s_task = pex_sched_every(200, tick, NULL);
    if (s_task <= 0) { fail("sched"); stop(); return PEX_OK; }
    return PEX_OK;
}

int pex_main(const pex_host_t *host, uint32_t reason, void *arg) {
    (void)host;
    if (reason == PEX_APP_STOP || reason == PEX_DEINIT) {
        stop();
        return PEX_OK;
    }
    if (reason == PEX_APP_RUN) { return start((const char *)arg); }
    return PEX_OK;
}
