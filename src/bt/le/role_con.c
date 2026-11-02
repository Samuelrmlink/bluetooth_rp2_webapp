/*
 * UART console for the role-swap tests. Off unless the build sets
 * BTGW_ROLE_CON. The product image does not include this file.
 *
 * Lines: role, io, policy, passkey, confirm, go, forget, disconnect, restart.
 * Replies match tools/ble_role_e2e.py.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/watchdog.h"
#include "btstack.h"
#include "btstack_crypto.h"
#include "ble/le_device_db.h"

#define KIND_READ 3
#define KIND_UP 16
#define KIND_DOWN 17
#define KIND_READY 18
#define KIND_FAIL 19
#define KIND_JW 20
#define KIND_PK 21
#define KIND_CONFIRM 22
#define KIND_BONDED 23
#define KIND_PFAIL 24
#define KIND_PKDISP 25

#define ROLE_NONE 0
#define ROLE_PERIPH 1
#define ROLE_CENTRAL 2

typedef struct {
    uint8_t addr[6];
    uint8_t kind;
    uint8_t pad;
    int16_t chr;
    int16_t status;
    uint16_t len;
    const uint8_t *data;
} pex_bt_event_t;
typedef void (*pex_bt_fn)(void *arg, const pex_bt_event_t *ev);
typedef struct {
    uint8_t addr[6];
    uint8_t addr_type;
    int8_t rssi;
    char name[31];
    uint8_t connectable, connected, n_uuid16;
    uint16_t uuid16[4];
    uint8_t mfg_len;
    uint8_t mfg[31];
    uint32_t age_ms;
} pex_bt_dev_t;

int pex_bt_listen(const uint8_t *addr, pex_bt_fn fn, void *arg);
int pex_bt_set_io(const char *io_capability, int mitm);
int pex_bt_set_policy(const char *policy);
int pex_bt_passkey(const uint8_t *addr, uint32_t passkey);
int pex_bt_confirm(const uint8_t *addr, int accept);
int pex_bt_forget(const uint8_t *addr);
int pex_bt_scan(uint32_t duration_ms, int active);
void pex_bt_scan_stop(void);
int pex_bt_scanning(void);
int pex_bt_devices(pex_bt_dev_t *out, uint32_t max, const char *name_prefix, const char *uuid128);
int pex_bt_connect(const uint8_t *addr);
int pex_bt_disconnect(const uint8_t *addr);
int pex_bt_find(const uint8_t *addr, const char *svc_uuid, const char *chr_uuid, int instance);
int pex_bt_read(const uint8_t *addr, int chr);
int pex_gatt_begin(void);
int pex_gatt_service(const char *uuid);
int pex_gatt_set(const uint8_t *data, uint16_t len);
int pex_gatt_characteristic(const char *uuid, int properties);
int pex_gatt_commit(void);
int pex_adv_data(const uint8_t *adv, uint16_t adv_len, const uint8_t *scan_rsp, uint16_t scan_len);
int pex_adv_start(uint16_t interval_min, uint16_t interval_max);
int pex_adv_stop(void);
int pex_bt_pair(const uint8_t *addr);

static char s_line[96];
static int s_n;
static int s_role;
static int s_listen;
static char s_io[24] = "no_input_no_output";
static int s_mitm;
static int s_have_io;
static char s_policy[16] = "on_demand";
static int s_have_policy;
static uint32_t s_passkey = 123456;
static int s_confirm_yes = 1;
static int s_go;
static int s_adv_ok;
static int s_scanning;
static int s_found;
static int s_connected;
static int s_read_sent;
static int s_have_peer;
static int s_bonded;
static int s_asked;
static uint8_t s_peer[6];
static btstack_packet_callback_registration_t s_hci;
static int s_hci_on;
/* The simulated controller seeds every chip with the same xorshift, and the
 * two images boot in lockstep, so both build the same P-256 key. BTstack then
 * rejects the peer (CVE-2020-26558) with SMP reason 0x0B. One LE Rand on the
 * peripheral, queued before HCI_STATE_WORKING starts key generation, shifts
 * that chip's stream. */
static btstack_crypto_random_t s_burn_req;
static uint8_t s_burn_buf[8];
static int s_burned;

static void burn_done(void *arg) {
    (void)arg;
    printf("rng burn done\n");
    fflush(stdout);
}

static void burn_rng(void) {
    if (s_burned) return;
    s_burned = 1;
    btstack_crypto_random_generate(&s_burn_req, s_burn_buf, sizeof s_burn_buf, burn_done, NULL);
    printf("rng burn queued\n");
    fflush(stdout);
}

#define LOG_N 32
#define LOG_L 96
#define IN_N 8
static char s_log[LOG_N][LOG_L];
static uint32_t s_log_seq[LOG_N];
static uint32_t s_log_n;
static uint32_t s_boot;
static char s_in[IN_N][LOG_L];
static int s_in_r, s_in_w, s_in_n;

static void log_line(const char *s) {
    if (!s_boot) {
        uint32_t t = time_us_32();
        s_boot = t ? t : 1;
    }
    s_log_n++;
    uint32_t seq = s_log_n;
    snprintf(s_log[(seq - 1) % LOG_N], LOG_L, "%s", s ? s : "");
    s_log_seq[(seq - 1) % LOG_N] = seq;
    fputs(s ? s : "", stdout);
    fputc('\n', stdout);
    fflush(stdout);
}

static void sayf(const char *fmt, ...) {
    char b[LOG_L];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    log_line(b);
}

static void say(const char *s) { log_line(s); }

static void say_addr(const char *tag, const uint8_t *a) {
    sayf("%s %02X:%02X:%02X:%02X:%02X:%02X", tag,
         a[0], a[1], a[2], a[3], a[4], a[5]);
}

static void remember(const uint8_t *a) {
    if (!a) return;
    memcpy(s_peer, a, 6);
    s_have_peer = 1;
}

static void hex_value(const uint8_t *d, unsigned n) {
    static const char H[] = "0123456789ABCDEF";
    char buf[81];
    if (n > 40) n = 40;
    for (unsigned i = 0; i < n; i++) {
        buf[i * 2] = H[d[i] >> 4];
        buf[i * 2 + 1] = H[d[i] & 0x0f];
    }
    buf[n * 2] = 0;
    sayf("VALUE %s", buf);
}

static int bond_known(const uint8_t *addr) {
    if (!addr) return 0;
    int max = le_device_db_max_count();
    for (int i = 0; i < max; i++) {
        int type = -1;
        bd_addr_t a;
        sm_key_t irk;
        le_device_db_info(i, &type, a, irk);
        if (type < 0 || type == BD_ADDR_TYPE_UNKNOWN) continue;
        if (memcmp(a, addr, 6) == 0) return 1;
    }
    return 0;
}

/* bond_set_io clears the SC bit. Numeric comparison needs it, and the
 * display role must show the passkey the console was given. */
static void arm_sm(void) {
    sm_use_fixed_passkey_in_display_role(s_passkey);
    uint8_t req = SM_AUTHREQ_BONDING;
    if (s_mitm) req |= SM_AUTHREQ_MITM_PROTECTION;
    if (!strcmp(s_io, "display_yes_no")) req |= SM_AUTHREQ_SECURE_CONNECTION;
    sm_set_authentication_requirements(req);
}

static void try_read(void) {
    if (s_role != ROLE_CENTRAL || s_read_sent || !s_bonded || !s_have_peer) return;
    int chr = pex_bt_find(s_peer, "1111", "2222", 0);
    if (chr < 0) return;
    if (pex_bt_read(s_peer, chr) < 0) { say("FAIL read"); s_read_sent = 1; return; }
    s_read_sent = 1;
}

static void on_hci(uint8_t type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)channel;
    (void)size;
    if (s_role != ROLE_CENTRAL || type != HCI_EVENT_PACKET) return;
    if (hci_event_packet_get_type(packet) != HCI_EVENT_ENCRYPTION_CHANGE) return;
    uint8_t st = hci_event_encryption_change_get_status(packet);
    uint8_t en = hci_event_encryption_change_get_encryption_enabled(packet);
    if (st == 0 && en) {
        if (!s_asked && !s_bonded) {
            say("bond restored");
            s_bonded = 1;
            try_read();
        }
        return;
    }
    /* The other side dropped its keys. Forget ours and pair again. */
    if (st != 0 && s_have_peer && !s_asked && !s_bonded) {
        pex_bt_forget(s_peer);
        if (!strcmp(s_policy, "never")) { say("FAIL policy"); return; }
        s_asked = 1;
        if (pex_bt_pair(s_peer) < 0) { s_asked = 0; say("FAIL pair"); }
    }
}

static void on_event(void *arg, const pex_bt_event_t *e) {
    (void)arg;
    if (!e) return;
    switch (e->kind) {
    case KIND_UP:
        remember(e->addr);
        s_connected = 1;
        s_bonded = 0;
        s_asked = 0;
        s_read_sent = 0;
        if (s_role != ROLE_CENTRAL) break;
        say_addr("CONNECTED", e->addr);
        if (!strcmp(s_policy, "never")) { say("FAIL policy"); break; }
        if (!bond_known(e->addr)) {
            s_asked = 1;
            if (pex_bt_pair(e->addr) < 0) { s_asked = 0; say("FAIL pair"); }
        } else {
            /* A stored LTK does not start encryption by itself. Ask for it
             * and leave s_asked clear: success is "bond restored", and a
             * missing peer key fails encryption so on_hci can pair again
             * without reporting that failure as FAIL pair. */
            pex_bt_pair(e->addr);
        }
        break;
    case KIND_DOWN:
        /* The link can drop after `go` has already treated the old
         * connection as the new one. Clear that and let the poll retry. */
        s_connected = 0;
        s_found = 0;
        s_scanning = 0;
        s_bonded = 0;
        s_asked = 0;
        s_read_sent = 0;
        if (s_role == ROLE_PERIPH) s_adv_ok = 0;
        break;
    case KIND_READY:
        try_read();
        break;
    case KIND_READ:
        if (s_role != ROLE_CENTRAL || !s_read_sent) break;
        if (e->status != 0 && e->status != -1) { say("FAIL read"); break; }
        if (e->data && e->len) hex_value(e->data, e->len);
        break;
    case KIND_JW:
        s_asked = 1;
        say("PAIR just_works");
        break;
    case KIND_PK:
        s_asked = 1;
        remember(e->addr);
        say("PAIR passkey");
        if (s_have_peer) pex_bt_passkey(s_peer, s_passkey);
        break;
    case KIND_PKDISP:
        s_asked = 1;
        say("PAIR passkey");
        break;
    case KIND_CONFIRM:
        s_asked = 1;
        remember(e->addr);
        say("PAIR confirm");
        if (s_have_peer) pex_bt_confirm(s_peer, s_confirm_yes);
        break;
    case KIND_BONDED:
        s_bonded = 1;
        s_asked = 0;
        if (s_role == ROLE_CENTRAL && e->addr) say_addr("BONDED", e->addr);
        try_read();
        break;
    case KIND_PFAIL:
    case KIND_FAIL:
        s_asked = 0;
        say("FAIL pair");
        break;
    default:
        break;
    }
}

static const uint8_t ADV[] = {
    0x02, 0x01, 0x06,
    0x0B, 0x09, 'p', 'e', 'x', '-', 'p', 'e', 'r', 'i', 'p', 'h',
};

static void apply_cfg(void) {
    if (s_have_io) pex_bt_set_io(s_io, s_mitm);
    arm_sm();
    if (s_have_policy) pex_bt_set_policy(s_policy);
}

static void start_periph(void) {
    static const uint8_t secret[] = { 'S', 'e', 'c', 'r', 'e', 't' };
    apply_cfg();
    /* The controller may still be powering on. A quiet miss is retried. */
    if (pex_gatt_begin() < 0 || pex_gatt_service("1111") < 0 ||
        pex_gatt_set(secret, sizeof secret) < 0 ||
        pex_gatt_characteristic("2222", 0x02 | 0x08) < 0 ||
        pex_gatt_commit() < 0) {
        return;
    }
    if (pex_adv_data(ADV, sizeof ADV, NULL, 0) < 0 || pex_adv_start(0, 0) < 0) {
        return;
    }
    s_adv_ok = 1;
    say("ADV pex-periph");
}

static void start_central(void) {
    apply_cfg();
    if (pex_bt_scan(0, 1) < 0) return;
    s_scanning = 1;
}

static void poll_scan(void) {
    if (!s_scanning || s_found) return;
    pex_bt_dev_t devs[8];
    int n = pex_bt_devices(devs, 8, "pex-periph", NULL);
    for (int i = 0; i < n; i++) {
        if (!devs[i].connectable) continue;
        /* PEX_EPENDING (-21) means the connect was accepted. */
        int rc = pex_bt_connect(devs[i].addr);
        if (rc == -10) return;
        s_found = 1;
        remember(devs[i].addr);
        say_addr("FOUND", devs[i].addr);
        pex_bt_scan_stop();
        s_scanning = 0;
        if (rc < 0 && rc != -21) say("FAIL connect");
        return;
    }
}

static void handle(char *line) {
    char cmd[16];
    if (sscanf(line, "%15s", cmd) != 1) return;
    if (!strcmp(cmd, "role")) {
        char role[16];
        if (sscanf(line, "%*s %15s", role) != 1) return;
        if (!strcmp(role, "peripheral")) s_role = ROLE_PERIPH;
        else if (!strcmp(role, "central")) s_role = ROLE_CENTRAL;
        else return;
        if (s_role == ROLE_PERIPH) burn_rng();
        s_go = 0;
        s_adv_ok = 0;
        s_found = 0;
        s_connected = 0;
        s_read_sent = 0;
        s_bonded = 0;
        s_asked = 0;
        sayf("READY role=%s", s_role == ROLE_PERIPH ? "peripheral" : "central");
    } else if (!strcmp(cmd, "io")) {
        char name[24];
        int mitm = 0;
        if (sscanf(line, "%*s %23s %d", name, &mitm) < 1) return;
        snprintf(s_io, sizeof s_io, "%s", name);
        s_mitm = mitm ? 1 : 0;
        s_have_io = 1;
        if (pex_bt_set_io(s_io, s_mitm) < 0) { say("FAIL io"); return; }
        arm_sm();
        sayf("IO %s mitm=%d", s_io, s_mitm);
    } else if (!strcmp(cmd, "policy")) {
        char name[16];
        if (sscanf(line, "%*s %15s", name) != 1) return;
        snprintf(s_policy, sizeof s_policy, "%s", name);
        s_have_policy = 1;
        if (pex_bt_set_policy(s_policy) < 0) say("FAIL policy");
    } else if (!strcmp(cmd, "passkey")) {
        unsigned v = 0;
        if (sscanf(line, "%*s %u", &v) == 1 && v <= 999999) {
            s_passkey = v;
            arm_sm();
        }
    } else if (!strcmp(cmd, "confirm")) {
        char yn[8];
        if (sscanf(line, "%*s %7s", yn) == 1) s_confirm_yes = strcmp(yn, "no") != 0;
    } else if (!strcmp(cmd, "go")) {
        s_go = 1;
        if (s_role == ROLE_PERIPH && !s_adv_ok) start_periph();
        if (s_role == ROLE_CENTRAL && !s_scanning && !s_found) start_central();
    } else if (!strcmp(cmd, "forget")) {
        char what[8];
        if (sscanf(line, "%*s %7s", what) != 1) return;
        if (!strcmp(what, "all")) {
            pex_bt_forget(NULL);
            say("FORGET all");
        } else if (!strcmp(what, "peer") && s_have_peer) {
            pex_bt_forget(s_peer);
            say("FORGET peer");
        }
    } else if (!strcmp(cmd, "disconnect")) {
        pex_adv_stop();
        s_adv_ok = 0;
        if (s_have_peer) pex_bt_disconnect(s_peer);
        s_connected = 0;
        s_found = 0;
        s_read_sent = 0;
        s_bonded = 0;
        s_asked = 0;
        s_go = 0;
    } else if (!strcmp(cmd, "restart")) {
        fflush(stdout);
        /* Delay 0 sets the trigger bit. A countdown does not elapse inside
         * the harness's wait, so the reboot arrived after the next scene. */
        watchdog_reboot(0, 0, 0);
    }
}

void role_con_submit(const char *line) {
    if (!line) return;
    while (*line == ' ' || *line == '\r' || *line == '\n') line++;
    if (!*line) return;
    if (s_in_n >= IN_N) return;
    snprintf(s_in[s_in_w], LOG_L, "%s", line);
    char *nl = strpbrk(s_in[s_in_w], "\r\n");
    if (nl) *nl = 0;
    s_in_w = (s_in_w + 1) % IN_N;
    s_in_n++;
}

void role_con_console(uint32_t from, char *dst, size_t cap) {
    if (!dst || cap < 8) return;
    if (!s_boot) {
        uint32_t t = time_us_32();
        s_boot = t ? t : 1;
    }
    size_t o = 0;
    int n = snprintf(dst, cap, "boot %u\n", s_boot);
    if (n < 0 || (size_t)n >= cap) { dst[0] = 0; return; }
    o = (size_t)n;
    uint32_t last = s_log_n;
    uint32_t shown = from == 0xffffffffu ? last : from;
    if (from != 0xffffffffu) {
        uint32_t start = 1;
        if (last > LOG_N) start = last - LOG_N + 1;
        if (from + 1 > start) start = from + 1;
        for (uint32_t seq = start; seq <= last && o + 24 < cap; seq++) {
            if (s_log_seq[(seq - 1) % LOG_N] != seq) continue;
            int w = snprintf(dst + o, cap - o - 24, "%s\n", s_log[(seq - 1) % LOG_N]);
            if (w < 0 || (size_t)w >= cap - o - 24) break;
            o += (size_t)w;
            shown = seq;
        }
    }
    snprintf(dst + o, cap - o, "next %u\n", shown);
}

void role_con_restart(void) {
    fflush(stdout);
    watchdog_reboot(0, 0, 0);
}

void role_con_poll(void) {
    if (!s_hci_on) {
        s_hci.callback = &on_hci;
        hci_add_event_handler(&s_hci);
        s_hci_on = 1;
    }
    if (!s_listen) {
        int id = pex_bt_listen(NULL, on_event, NULL);
        if (id > 0) s_listen = id;
    }
    while (s_in_n) {
        char line[LOG_L];
        snprintf(line, sizeof line, "%s", s_in[s_in_r]);
        s_in_r = (s_in_r + 1) % IN_N;
        s_in_n--;
        if (line[0]) handle(line);
    }
    int c;
    while ((c = getchar_timeout_us(0)) != PICO_ERROR_TIMEOUT) {
        if (c == '\r') continue;
        if (c == '\n') {
            s_line[s_n] = 0;
            s_n = 0;
            if (s_line[0]) handle(s_line);
        } else if (s_n < (int)sizeof s_line - 1) {
            s_line[s_n++] = (char)c;
        }
    }
    if (s_go && s_role == ROLE_PERIPH && !s_adv_ok) start_periph();
    if (s_go && s_role == ROLE_CENTRAL && !s_found) {
        if (!s_scanning || !pex_bt_scanning()) start_central();
        poll_scan();
    }
    try_read();
}
