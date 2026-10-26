/* Pinecil V2 driver. No crypto and no new host API: scan by name, connect,
 * read identity once, then poll the 56-byte bulk live characteristic.
 * lua/profiles/pinecil.lua is the behavior spec. The setpoint service uuid
 * and the setpoint characteristic uuid differ by one nibble; both are copied
 * from that file. */
#include "pex.h"
#include "pinecil_live.h"
#include <string.h>

/* Advertised when the name is missing. Same UUID as the scanner manifest. */
#define SVC_LIVE "d85ef000-168e-4a71-aa55-33e27f9bc533"
#define SVC_BULK "9eae1000-9d0d-48c5-aa55-33e27f9bc533"
#define CHR_LIVE "9eae1001-9d0d-48c5-aa55-33e27f9bc533"
#define CHR_BUILD "9eae1003-9d0d-48c5-aa55-33e27f9bc533"
#define CHR_SERIAL "9eae1004-9d0d-48c5-aa55-33e27f9bc533"
#define SVC_SET "f6d80000-5a10-4eba-aa55-33e27f9bc533"
/* Settings index 0, u16 LE. Confirmed: the live setpoint reads back the same
 * value. Index 1 (f6d70001, sleep temp) is unconfirmed in the profile and
 * is not opened or written. */
#define CHR_SETPT "f6d70000-5a10-4eba-aa55-33e27f9bc533"

enum { ST_OFF = 0, ST_HUNT, ST_IDLE, ST_CONN, ST_DISC, ST_READ, ST_GAP, ST_BACKOFF };
enum { RD_BUILD = 0, RD_SERIAL, RD_LIVE };
enum { PH_IDLE = 0, PH_WAIT = 1 };

static pex_bt_dev_t s_devs[8];
static uint8_t s_rx[80];
static char s_emit[1536];
static char s_build[32], s_serial[16], s_addr_str[18];
static uint8_t s_addr[6];
static uint16_t s_rx_len;
static int s_live = -1, s_build_chr = -1, s_serial_chr = -1, s_setpt = -1;
static int s_listen = -1, s_task, s_running, s_stop;
static uint8_t s_st, s_sub, s_phase, s_saw, s_got;
static uint8_t s_ev_up, s_ev_down, s_ev_fail, s_ev_ready;
static int s_want;
static uint32_t s_t0, s_hunt_t0, s_deadline, s_poll_ms, s_max_tip, s_busy_t0, s_last_look;
static uint8_t s_busy;

static void backoff(const char *why) {
    pex_bt_disconnect(s_addr);
    s_st = ST_BACKOFF;
    s_t0 = pex_ms();
    s_saw = 0;
    s_phase = PH_IDLE;
    s_got = 0;
    pex_log(PEX_LOG_WARN, "pinecil: %s", why);
}

static void take_text(char *d, size_t cap) {
    size_t n = s_rx_len;
    size_t m = 0;
    if (n > cap - 1) { n = cap - 1; }
    for (; m < n; m++) {
        unsigned char c = s_rx[m];
        if (c < 0x20 || c > 0x7e) { break; }
    }
    memcpy(d, s_rx, m);
    d[m] = 0;
}

static void take_serial(void) {
    static const char H[] = "0123456789ABCDEF";
    s_serial[0] = 0;
    if (s_rx_len < 6) { return; }
    for (int i = 0; i < 6; i++) {
        s_serial[i * 2] = H[s_rx[i] >> 4];
        s_serial[i * 2 + 1] = H[s_rx[i] & 0xf];
    }
    s_serial[12] = 0;
}

static void publish(void) {
    pinecil_live_t t;
    if (!pinecil_decode(s_rx, s_rx_len, &t)) {
        pex_log(PEX_LOG_WARN, "pinecil: short live read (%u B)", (unsigned)s_rx_len);
        return;
    }
    if (t.max_tip_c) { s_max_tip = t.max_tip_c; }
    pex_jw_t w;
    pex_jw_init(&w, s_emit, sizeof s_emit);
    pex_jw_obj(&w, NULL);
    pex_jw_str(&w, "addr", s_addr_str);
    if (s_build[0]) { pex_jw_str(&w, "build", s_build); }
    if (s_serial[0]) { pex_jw_str(&w, "serial", s_serial); }
    pex_jw_int(&w, "tip_c", t.tip_c);
    pex_jw_int(&w, "setpoint_c", t.setpoint_c);
    pex_jw_num(&w, "input_v", t.input_v);
    pex_jw_num(&w, "handle_c", t.handle_c);
    pex_jw_int(&w, "power_pct", t.power_pct);
    {
        const char *ps = pinecil_power_name(t.power_source);
        if (ps) { pex_jw_str(&w, "power_source", ps); }
        else { pex_jw_int(&w, "power_source", t.power_source); }
    }
    pex_jw_num(&w, "tip_ohms", t.tip_ohms);
    pex_jw_num(&w, "uptime_s", t.uptime_s);
    pex_jw_num(&w, "last_move_s", t.last_move_s);
    pex_jw_int(&w, "max_tip_c", t.max_tip_c);
    pex_jw_int(&w, "tip_uv", t.tip_uv);
    pex_jw_int(&w, "hall_uT", t.hall_uT);
    {
        const char *mn = pinecil_mode_name(t.mode);
        if (mn) { pex_jw_str(&w, "mode", mn); }
        else { pex_jw_int(&w, "mode", t.mode); }
    }
    pex_jw_num(&w, "watts", t.watts);
    pex_jw_end(&w);
    if (pex_jw_finish(&w) >= 0) { pex_bus_emit("profile.pinecil.telemetry", s_emit); }
}

static int find_chr(int *slot, const char *svc, const char *chr) {
    if (*slot >= 0) { return 1; }
    int c = pex_bt_find(s_addr, svc, chr, 0);
    if (c >= 0) { *slot = c; return 1; }
    return 0;
}

static int find_all(void) {
    int ok = 1;
    ok &= find_chr(&s_live, SVC_BULK, CHR_LIVE);
    ok &= find_chr(&s_build_chr, SVC_BULK, CHR_BUILD);
    ok &= find_chr(&s_serial_chr, SVC_BULK, CHR_SERIAL);
    ok &= find_chr(&s_setpt, SVC_SET, CHR_SETPT);
    return ok;
}

static int chr_for(uint8_t sub) {
    if (sub == RD_BUILD) { return s_build_chr; }
    if (sub == RD_SERIAL) { return s_serial_chr; }
    return s_live;
}

static uint32_t timeout_for(uint8_t sub) { return sub == RD_LIVE ? 2500u : 2000u; }

static void arm_read(void) {
    s_phase = PH_IDLE;
    s_got = 0;
    s_busy = 0;
    s_want = chr_for(s_sub);
}

static void after_read(void) {
    if (s_sub == RD_BUILD) {
        take_text(s_build, sizeof s_build);
        s_sub = RD_SERIAL;
        arm_read();
        return;
    }
    if (s_sub == RD_SERIAL) {
        take_serial();
        pex_log(PEX_LOG_INFO, "pinecil: %s build %s", s_addr_str, s_build[0] ? s_build : "?");
        s_sub = RD_LIVE;
        arm_read();
        return;
    }
    if (s_rx_len < 56) {
        pex_log(PEX_LOG_WARN, "pinecil: short live read (%u B)", (unsigned)s_rx_len);
    } else {
        publish();
    }
    s_st = ST_GAP;
    s_t0 = pex_ms();
    s_phase = PH_IDLE;
}

static void read_failed(const char *why) {
    pex_log(PEX_LOG_WARN, "pinecil: %s", why);
    s_rx_len = 0;
    after_read();
}

static void service_read(void) {
    if (s_phase == PH_WAIT) {
        if (s_got) {
            s_got = 0;
            s_phase = PH_IDLE;
            after_read();
            return;
        }
        if ((int32_t)(pex_ms() - s_deadline) >= 0) { read_failed("read timed out"); }
        return;
    }
    s_got = 0;
    s_deadline = pex_ms() + timeout_for(s_sub);
    s_phase = PH_WAIT;
    int rc = pex_bt_read(s_addr, s_want);
    if (rc == PEX_EBUSY) {
        s_phase = PH_IDLE;
        if (!s_busy) { s_busy = 1; s_busy_t0 = pex_ms(); }
        if (pex_ms() - s_busy_t0 > 2000u) { read_failed("read refused"); }
        return;
    }
    s_busy = 0;
    if (rc < 0) {
        s_phase = PH_IDLE;
        read_failed("read refused");
    }
}

static int link_dead(void) {
    int ready = 0;
    if (s_ev_fail) { return 1; }
    if (s_ev_down) { return 1; }
    return pex_bt_state(s_addr, &ready) == PEX_BT_DISCONNECTED;
}

static void start_connect(void) {
    pex_bt_scan_stop();
    s_ev_up = s_ev_down = s_ev_fail = s_ev_ready = 0;
    s_saw = 0;
    s_live = s_build_chr = s_serial_chr = s_setpt = -1;
    s_build[0] = s_serial[0] = 0;
    s_max_tip = 0;
    int rc = pex_bt_connect(s_addr);
    if (rc < 0 && rc != PEX_EPENDING) {
        backoff("connect failed");
        return;
    }
    if (rc == PEX_OK) { s_saw = 1; }
    s_st = ST_CONN;
    s_t0 = pex_ms();
    pex_log(PEX_LOG_INFO, "pinecil: connecting to %s", s_addr_str);
}

static int hunt_look(void) {
    int n = pex_bt_devices(s_devs, 8, "Pinecil", SVC_LIVE);
    if (n <= 0) { return 0; }
    if (n > 8) { n = 8; }
    memcpy(s_addr, s_devs[0].addr, 6);
    pex_bt_addr_str(s_addr, s_addr_str);
    return 1;
}

static void tick_body(void) {
    if (s_st == ST_HUNT) {
        if (pex_ms() - s_hunt_t0 >= 34000u) {
            s_st = ST_IDLE;
            s_t0 = pex_ms();
            return;
        }
        if (pex_ms() - s_last_look >= 500u) {
            s_last_look = pex_ms();
            if (hunt_look()) { start_connect(); }
        }
        return;
    }
    if (s_st == ST_IDLE) {
        if (pex_ms() - s_t0 >= 11000u) {
            s_st = ST_HUNT;
            s_hunt_t0 = pex_ms();
            s_last_look = 0;
        }
        return;
    }
    if (s_st == ST_BACKOFF) {
        if (pex_ms() - s_t0 >= 5000u) {
            s_st = ST_HUNT;
            s_hunt_t0 = pex_ms();
            s_last_look = 0;
        }
        return;
    }
    if (s_st == ST_CONN) {
        if (s_ev_up) { s_saw = 1; s_ev_up = 0; }
        if (s_ev_fail || (s_ev_down && s_saw)) { backoff("link dropped"); return; }
        s_ev_down = 0;
        int ready = 0;
        if (pex_bt_state(s_addr, &ready) == PEX_BT_CONNECTED) { s_saw = 1; }
        if (s_saw) {
            s_st = ST_DISC;
            s_t0 = pex_ms();
            pex_log(PEX_LOG_INFO, "pinecil: link up");
            return;
        }
        if (pex_ms() - s_t0 > 12000u) { backoff("link never came up"); }
        return;
    }
    if (s_st == ST_DISC) {
        int ready = 0;
        int stt = pex_bt_state(s_addr, &ready);
        if (s_ev_ready) { ready = 1; s_ev_ready = 0; }
        if (s_ev_fail || s_ev_down || stt == PEX_BT_DISCONNECTED) {
            backoff("link dropped before discovery finished");
            return;
        }
        if (!ready || !find_all()) {
            if (pex_ms() - s_t0 > 15000u) { backoff("discovery never finished"); }
            return;
        }
        s_st = ST_READ;
        s_sub = RD_BUILD;
        arm_read();
        return;
    }
    if (s_st == ST_READ) {
        if (link_dead()) { backoff("link dropped"); return; }
        s_ev_down = 0;
        s_ev_fail = 0;
        service_read();
        return;
    }
    if (s_st == ST_GAP) {
        if (link_dead()) { backoff("link dropped"); return; }
        s_ev_down = 0;
        s_ev_fail = 0;
        if (pex_ms() - s_t0 >= s_poll_ms) {
            s_st = ST_READ;
            s_sub = RD_LIVE;
            arm_read();
        }
    }
}

static int tick(void *arg) {
    (void)arg;
    if (s_stop) { return PEX_TASK_DONE; }
    tick_body();
    if (s_st == ST_HUNT) {
        if (!pex_bt_scanning()) { pex_bt_scan(0, 1); }
    } else if (pex_bt_scanning()) {
        pex_bt_scan_stop();
    }
    return PEX_TASK_AGAIN;
}

static void on_bt(void *arg, const pex_bt_event_t *ev) {
    (void)arg;
    if (!ev || s_st == ST_OFF || s_st == ST_HUNT || s_st == ST_IDLE || s_st == ST_BACKOFF) { return; }
    if (memcmp(ev->addr, s_addr, 6) != 0) { return; }
    if (ev->kind == PEX_BT_LINK_UP) { s_ev_up = 1; s_saw = 1; return; }
    if (ev->kind == PEX_BT_LINK_DOWN) { s_ev_down = 1; return; }
    if (ev->kind == PEX_BT_FAILED) { s_ev_fail = 1; return; }
    if (ev->kind == PEX_BT_READY) { s_ev_ready = 1; return; }
    if (ev->kind != PEX_BT_READ || ev->chr != s_want || s_phase != PH_WAIT) { return; }
    uint16_t n = ev->len;
    if (n > sizeof s_rx) { n = sizeof s_rx; }
    if (n && ev->data) { memcpy(s_rx, ev->data, n); }
    s_rx_len = n;
    s_got = 1;
}

static int64_t clamp_temp(const char *params) {
    int64_t c = 0;
    if (params) { pex_json_int(params, "celsius", &c); }
    if (c < 10) { c = 10; }
    uint32_t mx = s_max_tip ? s_max_tip : 450u;
    if (c > (int64_t)mx) { c = (int64_t)mx; }
    return c;
}

static int reply_set(char *reply, size_t cap, int ok, int64_t c) {
    pex_jw_t w;
    pex_jw_init(&w, reply, cap);
    pex_jw_obj(&w, NULL);
    pex_jw_bool(&w, "ok", ok);
    pex_jw_int(&w, "celsius", c);
    pex_jw_end(&w);
    int n = pex_jw_finish(&w);
    return n < 0 ? PEX_ENOMEM : n;
}

static int op_set_temp(void *arg, const char *params, char *reply, size_t cap) {
    (void)arg;
    int64_t c = clamp_temp(params);
    int linked = s_running && s_setpt >= 0 && (s_st == ST_READ || s_st == ST_GAP);
    if (!linked) { return reply_set(reply, cap, 0, c); }
    uint8_t b[2] = { (uint8_t)c, (uint8_t)((uint32_t)c >> 8) };
    int rc = pex_bt_write(s_addr, s_setpt, b, 2, 1, 0);
    return reply_set(reply, cap, rc >= 0, c);
}

static void reset_hunt(void) {
    s_st = ST_HUNT;
    s_hunt_t0 = pex_ms();
    s_last_look = 0;
    s_phase = PH_IDLE;
    s_got = 0;
    s_saw = 0;
    s_live = s_build_chr = s_serial_chr = s_setpt = -1;
    s_want = -1;
    s_build[0] = s_serial[0] = 0;
    s_max_tip = 0;
}

int pex_main(const pex_host_t *host, uint32_t reason, void *arg) {
    (void)host;
    switch (reason) {
    case PEX_INIT:
        pex_bus_op("pinecil.set_temp", op_set_temp, NULL);
        return PEX_OK;
    case PEX_APP_RUN: {
        s_poll_ms = 1000;
        if (arg) {
            int64_t p = 0;
            if (pex_json_int(arg, "poll_ms", &p) && p > 0 && p < 600000) { s_poll_ms = (uint32_t)p; }
        }
        s_stop = 0;
        s_running = 1;
        reset_hunt();
        s_listen = pex_bt_listen(NULL, on_bt, NULL);
        if (s_listen < 0) { s_running = 0; return s_listen; }
        s_task = pex_sched_every(50, tick, NULL);
        if (s_task <= 0) { s_running = 0; return s_task < 0 ? s_task : PEX_ENOMEM; }
        pex_log(PEX_LOG_INFO, "pinecil: looking for an iron");
        return PEX_OK;
    }
    case PEX_APP_STOP:
        s_stop = 1;
        s_running = 0;
        if (s_st != ST_OFF && s_st != ST_HUNT && s_st != ST_IDLE && s_st != ST_BACKOFF) {
            pex_bt_disconnect(s_addr);
        }
        s_st = ST_OFF;
        if (s_listen >= 0) { pex_bt_unlisten(s_listen); s_listen = -1; }
        pex_bt_scan_stop();
        return PEX_OK;
    case PEX_DEINIT:
        s_running = 0;
        s_stop = 1;
        return PEX_OK;
    default:
        return PEX_OK;
    }
}
