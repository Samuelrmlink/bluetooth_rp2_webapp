/* BLE peripheral. PEX_APP_RUN config: io, mitm, passkey, policy, confirm,
 * forget, addr. Advertises complete local name pex-periph and service 1111
 * characteristic 2222 (read|write).
 *
 * pex_gatt_set stores the six bytes "Secret" for characteristic 2222.
 * Pairing kinds below match the host. The ifndefs cover a header that
 * has not been regenerated yet. */
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
#ifndef PEX_BT_LOCAL_WRITE
#define PEX_BT_LOCAL_WRITE 26
#endif

/* ATT Read | ATT Write. */
enum { CHR_PROPS = 0x02 | 0x08 };
enum { FORGET_NONE = 0, FORGET_PEER = 1, FORGET_ALL = 2 };

/* Flags: LE General Discoverable | BR/EDR not supported. Then the name. */
static const uint8_t ADV[] = {
    0x02, 0x01, 0x06,
    0x0B, 0x09, 'p', 'e', 'x', '-', 'p', 'e', 'r', 'i', 'p', 'h',
};

static const uint8_t ZERO[6];

static char s_io[24];
static char s_policy[16];
static uint8_t s_local[6];
static uint8_t s_peer[6];
static uint32_t s_passkey;
static int s_mitm, s_have_io, s_have_mitm, s_have_policy, s_have_local;
static int s_confirm_yes, s_forget;
static int s_failed, s_forgot, s_bonded, s_method, s_valued, s_up, s_have_peer;
static int s_listen, s_adv, s_running;

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

static void on_bonded(const uint8_t *addr) {
    take_addr(addr);
    if (!s_method && strcmp(s_io, "no_input_no_output") == 0) { note_method("just_works"); }
    if (s_bonded) { return; }
    s_bonded = 1;
    log_addr("BONDED", s_have_peer ? s_peer : addr);
    do_forget();
}

static void on_value(const pex_bt_event_t *e) {
    if (s_valued || s_failed || !e->data || e->len == 0) { return; }
    s_valued = 1;
    log_hex(e->data, e->len);
}

static void on_event(void *arg, const pex_bt_event_t *e) {
    (void)arg;
    if (!s_running || s_failed || !for_us(e->addr)) { return; }
    switch (e->kind) {
    case PEX_BT_LINK_UP:
        take_addr(e->addr);
        s_up = 1;
        break;
    case PEX_BT_LINK_DOWN:
        s_up = 0;
        s_method = 0;
        s_bonded = 0;
        s_valued = 0;
        break;
    case PEX_BT_JUST_WORKS:
        note_method("just_works");
        break;
    case PEX_BT_PASSKEY:
        take_addr(e->addr);
        note_method("passkey");
        pex_bt_passkey(peer_or(e->addr), s_passkey);
        break;
    case PEX_BT_PASSKEY_DISP:
        take_addr(e->addr);
        note_method("passkey");
        break;
    case PEX_BT_CONFIRM:
        take_addr(e->addr);
        note_method("confirm");
        pex_bt_confirm(peer_or(e->addr), s_confirm_yes);
        break;
    case PEX_BT_BONDED:
        on_bonded(e->addr);
        break;
    case PEX_BT_PAIR_FAIL:
        fail("pair");
        break;
    case PEX_BT_LOCAL_WRITE:
    case PEX_BT_WRITE:
        on_value(e);
        break;
    case PEX_BT_FAILED:
        if (s_up) { fail("link"); }
        break;
    default:
        break;
    }
}

static const char *parse(const char *json) {
    copy_str(s_io, sizeof s_io, "no_input_no_output");
    s_policy[0] = 0;
    s_mitm = 0;
    s_have_io = 0;
    s_have_mitm = 0;
    s_have_policy = 0;
    s_have_local = 0;
    s_passkey = 123456;
    s_confirm_yes = 1;
    s_forget = FORGET_NONE;
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
    if (key_present(json, "addr")) {
        char a[20];
        if (!pex_json_str(json, "addr", a, sizeof a) || pex_bt_addr(a, s_local) != PEX_OK) { return "config"; }
        s_have_local = 1;
    }
    return NULL;
}

static void stop(void) {
    s_running = 0;
    if (s_listen > 0) {
        pex_bt_unlisten(s_listen);
        s_listen = 0;
    }
    if (s_adv) {
        pex_adv_stop();
        s_adv = 0;
    }
    if (s_up && s_have_peer) { pex_bt_disconnect(s_peer); }
    s_up = 0;
}

static void clear_session(void) {
    s_failed = 0;
    s_forgot = 0;
    s_bonded = 0;
    s_method = 0;
    s_valued = 0;
    s_up = 0;
    s_have_peer = 0;
    memset(s_peer, 0, sizeof s_peer);
}

static int gatt_up(void) {
    if (pex_gatt_begin() < 0) { return -1; }
    if (pex_gatt_service("1111") < 0) { return -1; }
    static const uint8_t secret[] = { 'S', 'e', 'c', 'r', 'e', 't' };
    if (pex_gatt_set(secret, sizeof secret) < 0) { return -1; }
    if (pex_gatt_characteristic("2222", CHR_PROPS) < 0) { return -1; }
    if (pex_gatt_commit() < 0) { return -1; }
    return 0;
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
    if (s_have_local) {
        int rc = pex_bt_set_addr(s_local, 1);
        if (rc == PEX_ENOTSUP) { pex_log(PEX_LOG_INFO, "set_addr ENOTSUP"); }
        else if (rc < 0) { fail("addr"); return PEX_OK; }
    }
    pex_log(PEX_LOG_INFO, "READY role=peripheral");
    pex_log(PEX_LOG_INFO, "IO %s mitm=%d", s_io, s_mitm);
    s_running = 1;
    s_listen = pex_bt_listen(NULL, on_event, NULL);
    if (s_listen <= 0) { fail("listen"); stop(); return PEX_OK; }
    if (gatt_up() < 0) { fail("gatt"); stop(); return PEX_OK; }
    if (pex_adv_data(ADV, (uint16_t)sizeof ADV, NULL, 0) < 0 || pex_adv_start(0, 0) < 0) {
        fail("adv");
        stop();
        return PEX_OK;
    }
    s_adv = 1;
    pex_log(PEX_LOG_INFO, "ADV pex-periph");
    return PEX_OK;
}

int pex_main(const pex_host_t *host, uint32_t reason, void *arg) {
    (void)host;
    if (reason == PEX_APP_STOP || reason == PEX_DEINIT) {
        stop();
        return PEX_OK;
    }
    if (reason == PEX_APP_RUN) { return start(arg); }
    return PEX_OK;
}
