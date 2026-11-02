/*
 * HTTP role console for the ESP32-C5. Same lines as src/bt/le/role_con.c.
 * It calls the PEX bt group only. NimBLE stays inside ble_pex.c.
 * Compiled into every C5 image; the calls are empty when CONFIG_C5_BLE is off.
 */
#include "role_hw.h"

#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"

#if !CONFIG_C5_BLE

void role_hw_poll(void) {}
void role_hw_note(const char *line) { (void)line; }
void role_hw_submit(const char *line) { (void)line; }
void role_hw_console(uint32_t from, char *dst, size_t cap) {
    (void)from;
    if (dst && cap) dst[0] = 0;
}
void role_hw_restart(void) {}

#else

#include <stdarg.h>

#include <fcntl.h>
#include <unistd.h>

#include "esp_app_desc.h"
#include "esp_random.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "pex.h"

#define ROLE_NONE 0
#define ROLE_PERIPH 1
#define ROLE_CENTRAL 2

#define LOG_N 32
#define LOG_L 96
#define IN_N 8

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static char s_log[LOG_N][LOG_L];
static uint32_t s_log_seq[LOG_N];
static uint32_t s_log_n;
static uint32_t s_boot;
static char s_in[IN_N][LOG_L];
static int s_in_r, s_in_w, s_in_n;

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

/* The portMUX disables interrupts. libc and the RNG take locks, so the
 * critical section only copies bytes. */
static void copy_str(char *dst, size_t cap, const char *s) {
    size_t i = 0;
    if (!s) s = "";
    while (s[i] && i + 1 < cap) {
        dst[i] = s[i];
        i++;
    }
    dst[i] = 0;
}

static void log_line(const char *s) {
    char tmp[LOG_L];
    copy_str(tmp, sizeof tmp, s);
    taskENTER_CRITICAL(&s_mux);
    if (!s_boot) s_boot = 1;
    s_log_n++;
    uint32_t seq = s_log_n;
    memcpy(s_log[(seq - 1) % LOG_N], tmp, LOG_L);
    s_log_seq[(seq - 1) % LOG_N] = seq;
    taskEXIT_CRITICAL(&s_mux);
    printf("%s\n", tmp);
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

void role_hw_note(const char *line) { say(line ? line : ""); }

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

static void try_read(void) {
    if (s_role != ROLE_CENTRAL || s_read_sent || !s_bonded || !s_have_peer) return;
    int chr = pex_bt_find(s_peer, "1111", "2222", 0);
    if (chr < 0) return;
    if (pex_bt_read(s_peer, chr) < 0) { say("FAIL read"); s_read_sent = 1; return; }
    s_read_sent = 1;
}

static void on_event(void *arg, const pex_bt_event_t *e) {
    (void)arg;
    if (!e) return;
    switch (e->kind) {
    case PEX_BT_LINK_UP:
        remember(e->addr);
        s_connected = 1;
        s_bonded = 0;
        s_asked = 0;
        s_read_sent = 0;
        if (s_role != ROLE_CENTRAL) break;
        say_addr("CONNECTED", e->addr);
        if (!strcmp(s_policy, "never")) { say("FAIL policy"); break; }
        /* ble_pex emits BONDED with no PAIR when a stored key encrypts,
         * and pairs again when the peer has dropped its key. */
        if (pex_bt_pair(e->addr) < 0) say("FAIL pair");
        break;
    case PEX_BT_LINK_DOWN:
        s_connected = 0;
        s_found = 0;
        s_scanning = 0;
        s_bonded = 0;
        s_asked = 0;
        s_read_sent = 0;
        if (s_role == ROLE_PERIPH) s_adv_ok = 0;
        break;
    case PEX_BT_READY:
        try_read();
        break;
    case PEX_BT_READ:
        if (s_role != ROLE_CENTRAL || !s_read_sent) break;
        if (e->status != 0 && e->status != -1) { say("FAIL read"); break; }
        if (e->data && e->len) hex_value(e->data, e->len);
        break;
    case PEX_BT_JUST_WORKS:
        s_asked = 1;
        say("PAIR just_works");
        break;
    case PEX_BT_PASSKEY:
        s_asked = 1;
        remember(e->addr);
        say("PAIR passkey");
        if (s_have_peer) pex_bt_passkey(s_peer, s_passkey);
        break;
    case PEX_BT_PASSKEY_DISP:
        /* NimBLE does not use a static passkey. The console injects it. */
        s_asked = 1;
        remember(e->addr);
        say("PAIR passkey");
        if (s_have_peer) pex_bt_passkey(s_peer, s_passkey);
        break;
    case PEX_BT_CONFIRM:
        s_asked = 1;
        remember(e->addr);
        say("PAIR confirm");
        if (s_have_peer) pex_bt_confirm(s_peer, s_confirm_yes);
        break;
    case PEX_BT_BONDED:
        s_bonded = 1;
        s_asked = 0;
        if (s_role == ROLE_CENTRAL) say_addr("BONDED", e->addr);
        try_read();
        break;
    case PEX_BT_PAIR_FAIL:
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
    if (s_have_policy) pex_bt_set_policy(s_policy);
}

static void start_periph(void) {
    static const uint8_t secret[] = { 'S', 'e', 'c', 'r', 'e', 't' };
    apply_cfg();
    if (pex_gatt_begin() < 0 || pex_gatt_service("1111") < 0 ||
        pex_gatt_set(secret, sizeof secret) < 0 ||
        pex_gatt_characteristic("2222", 0x02 | 0x08) < 0 ||
        pex_gatt_commit() < 0) {
        return;
    }
    if (pex_adv_data(ADV, sizeof ADV, NULL, 0) < 0 || pex_adv_start(0, 0) < 0) return;
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
        int rc = pex_bt_connect(devs[i].addr);
        if (rc == PEX_EBUSY) return;
        s_found = 1;
        remember(devs[i].addr);
        say_addr("FOUND", devs[i].addr);
        pex_bt_scan_stop();
        s_scanning = 0;
        if (rc < 0 && rc != PEX_EPENDING) say("FAIL connect");
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
        sayf("IO %s mitm=%d", s_io, s_mitm);
    } else if (!strcmp(cmd, "policy")) {
        char name[16];
        if (sscanf(line, "%*s %15s", name) != 1) return;
        snprintf(s_policy, sizeof s_policy, "%s", name);
        s_have_policy = 1;
        if (pex_bt_set_policy(s_policy) < 0) say("FAIL policy");
    } else if (!strcmp(cmd, "passkey")) {
        unsigned v = 0;
        if (sscanf(line, "%*s %u", &v) == 1 && v <= 999999) s_passkey = v;
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
        esp_restart();
    }
}

void role_hw_submit(const char *line) {
    char tmp[LOG_L];
    if (!line) return;
    while (*line == ' ' || *line == '\r' || *line == '\n') line++;
    if (!*line) return;
    copy_str(tmp, sizeof tmp, line);
    char *nl = strpbrk(tmp, "\r\n");
    if (nl) *nl = 0;
    if (!tmp[0]) return;
    taskENTER_CRITICAL(&s_mux);
    if (s_in_n < IN_N) {
        memcpy(s_in[s_in_w], tmp, LOG_L);
        s_in_w = (s_in_w + 1) % IN_N;
        s_in_n++;
    }
    taskEXIT_CRITICAL(&s_mux);
}

void role_hw_console(uint32_t from, char *dst, size_t cap) {
    /* Static so the HTTP task does not put a 3 KB copy on its 8 KB stack. */
    static char snap[LOG_N][LOG_L];
    static uint32_t snap_seq[LOG_N];
    uint32_t boot, last;
    if (!dst || cap < 8) return;
    taskENTER_CRITICAL(&s_mux);
    if (!s_boot) s_boot = 1;
    boot = s_boot;
    last = s_log_n;
    memcpy(snap, s_log, sizeof snap);
    memcpy(snap_seq, s_log_seq, sizeof snap_seq);
    taskEXIT_CRITICAL(&s_mux);
    size_t o = 0;
    int n = snprintf(dst, cap, "boot %u\n", (unsigned)boot);
    if (n < 0 || (size_t)n >= cap) { dst[0] = 0; return; }
    o = (size_t)n;
    uint32_t shown = from == 0xffffffffu ? last : from;
    if (from != 0xffffffffu) {
        uint32_t start = 1;
        if (last > LOG_N) start = last - LOG_N + 1;
        if (from + 1 > start) start = from + 1;
        for (uint32_t seq = start; seq <= last && o + 24 < cap; seq++) {
            if (snap_seq[(seq - 1) % LOG_N] != seq) continue;
            int w = snprintf(dst + o, cap - o - 24, "%s\n", snap[(seq - 1) % LOG_N]);
            if (w < 0 || (size_t)w >= cap - o - 24) break;
            o += (size_t)w;
            shown = seq;
        }
    }
    snprintf(dst + o, cap - o, "next %u\n", (unsigned)shown);
}

void role_hw_restart(void) { esp_restart(); }

/* The sim harness writes role commands to the console UART. The console
 * reads that FIFO directly; the UART driver object is not installed, so
 * uart_read_bytes only logs an error. stdin is that same FIFO. */
static void uart_lines(void) {
    static int noblock;
    static char line[LOG_L];
    static int n;
    char b;
    if (!noblock) {
        int fl = fcntl(STDIN_FILENO, F_GETFL, 0);
        if (fl >= 0) fcntl(STDIN_FILENO, F_SETFL, fl | O_NONBLOCK);
        noblock = 1;
    }
    for (;;) {
        ssize_t r = read(STDIN_FILENO, &b, 1);
        if (r <= 0) break;
        if (b == '\r') continue;
        if (b == '\n') {
            line[n] = 0;
            n = 0;
            if (line[0]) handle(line);
        } else if (n < (int)sizeof line - 1) {
            line[n++] = (char)b;
        }
    }
}

void role_hw_poll(void) {
    static int announced;
    if (!announced) {
        announced = 1;
        sayf("POLL up %s", esp_app_get_description()->version);
    }
    if (!s_listen) {
        int id = pex_bt_listen(NULL, on_event, NULL);
        if (id > 0) s_listen = id;
    }
    for (;;) {
        char line[LOG_L];
        int have = 0;
        taskENTER_CRITICAL(&s_mux);
        if (s_in_n) {
            memcpy(line, s_in[s_in_r], LOG_L);
            s_in_r = (s_in_r + 1) % IN_N;
            s_in_n--;
            have = 1;
        }
        taskEXIT_CRITICAL(&s_mux);
        if (!have) break;
        if (line[0]) handle(line);
    }
    uart_lines();
    if (s_go && s_role == ROLE_PERIPH && !s_adv_ok) start_periph();
    if (s_go && s_role == ROLE_CENTRAL && !s_found) {
        if (!s_scanning || !pex_bt_scanning()) start_central();
        poll_scan();
    }
    try_read();
}

#endif
