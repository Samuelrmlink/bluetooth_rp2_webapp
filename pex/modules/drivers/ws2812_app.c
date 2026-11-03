/* WS2812B strip. Idle until the device window turns it on, then one
 * unprotected state machine on the data pin. The window is
 * profile.ws2812.telemetry, same discovery path as Pinecil.
 *
 * The four instruction words are pioasm of ws2812.pio (origin 0).
 * The ledger adds the load offset to each JMP. Pixels are GRB in the
 * top 24 bits: the machine shifts left and autopulls at 24. A joined
 * TX FIFO holds 8 pixels. The line sits low when that FIFO runs dry,
 * and a low of more than about 50 us latches the strip, so the rest
 * of a frame is written here before that gap. Off clocks a frame of
 * zeros and waits for that latch before the pin is released. */
#include "pex.h"

#define WS_MAX 64
#define WS_FRAME_MS 20

/* pioasm 2.3.0, .side_set 1, T1=3 T2=3 T3=4:
 *   0: out x, 1 side 0 [3]
 *   1: jmp !x, 3 side 1 [2]
 *   2: jmp 0 side 1 [2]
 *   3: nop side 0 [2] */
static const uint16_t WS_PROG[4] = { 0x6321, 0x1223, 0x1200, 0xa242 };

static int s_handle = -1;
static int s_task;
static int s_running;
static int s_on;
static int s_pin;
static int s_count;
static int s_mode;                 /* 0 solid, 1 rainbow, 2 breathe, 3 chase */
static int s_hue, s_sat, s_val, s_speed;
static uint32_t s_phase;
static int s_lit;                 /* pixels the last frame addressed */
static int s_pub;
static uint32_t s_pix[WS_MAX];
static char s_emit[480];
static char s_status[96];

static int streq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static const char *mode_name(int m) {
    if (m == 1) { return "rainbow"; }
    if (m == 2) { return "breathe"; }
    if (m == 3) { return "chase"; }
    return "solid";
}

static int mode_of(const char *s) {
    if (streq(s, "solid")) { return 0; }
    if (streq(s, "rainbow")) { return 1; }
    if (streq(s, "breathe")) { return 2; }
    if (streq(s, "chase")) { return 3; }
    return -1;
}

/* SD, W5500, and the radio. The ledger would refuse these too; saying
 * which wire it is saves a trip to the console. */
static const char *pin_reason(int pin) {
    switch (pin) {
    case 2: return "GP2 is the SD clock";
    case 3: return "GP3 is the SD command line";
    case 4: case 5: case 6: case 7: return "GP4-GP7 are the SD data lines";
    case 8: return "GP8 is the W5500 MISO";
    case 9: return "GP9 is the SD card detect";
    case 10: return "GP10 is the W5500 interrupt";
    case 11: return "GP11 is the W5500 reset";
    case 13: return "GP13 is the W5500 chip select";
    case 14: return "GP14 is the W5500 clock";
    case 15: return "GP15 is the W5500 MOSI";
    case 23: case 24: case 25: case 29: return "that pin belongs to the radio";
    default: return 0;
    }
}

static const char *pio_why(int rc) {
    if (rc == PEX_EBUSY) { return "PIO is busy (see the console)"; }
    if (rc == PEX_ENOMEM) { return "PIO has no free state machine"; }
    if (rc == PEX_EINVAL) { return "PIO refused the pin or the program (see the console)"; }
    if (rc == PEX_ENOTSUP) { return "this board has no PIO ledger"; }
    return "PIO claim failed (see the console)";
}

static char *put_str(char *p, char *end, const char *s) {
    while (*s && p + 1 < end) { *p++ = *s++; }
    *p = 0;
    return p;
}

static char *put_u(char *p, char *end, unsigned v) {
    char tmp[10];
    int n = 0;
    if (v == 0) { tmp[n++] = '0'; }
    while (v && n < 10) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
    while (n > 0 && p + 1 < end) { *p++ = tmp[--n]; }
    *p = 0;
    return p;
}

static void describe(const char *err) {
    char *p = s_status;
    char *end = s_status + sizeof s_status;
    if (err && err[0]) { put_str(p, end, err); return; }
    if (s_on && s_handle >= 0) {
        p = put_str(p, end, "driving GP");
        p = put_u(p, end, (unsigned)s_pin);
        p = put_str(p, end, ", ");
        p = put_u(p, end, (unsigned)s_count);
        p = put_str(p, end, " LEDs, ");
        put_str(p, end, mode_name(s_mode));
        return;
    }
    p = put_str(p, end, "off, data on GP");
    put_u(p, end, (unsigned)s_pin);
}

static void publish(void) {
    pex_jw_t w;
    pex_jw_init(&w, s_emit, sizeof s_emit);
    pex_jw_obj(&w, NULL);
    pex_jw_int(&w, "on", s_on);
    pex_jw_int(&w, "pin", s_pin);
    pex_jw_int(&w, "count", s_count);
    pex_jw_str(&w, "mode", mode_name(s_mode));
    pex_jw_int(&w, "hue", s_hue);
    pex_jw_int(&w, "sat", s_sat);
    pex_jw_int(&w, "val", s_val);
    pex_jw_int(&w, "speed", s_speed);
    pex_jw_int(&w, "running", s_handle >= 0 ? 1 : 0);
    pex_jw_str(&w, "status", s_status);
    pex_jw_end(&w);
    if (pex_jw_finish(&w) >= 0) { pex_bus_emit("profile.ws2812.telemetry", s_emit); }
}

static int reply_msg(char *reply, size_t cap, const char *msg, int rc) {
    size_t i = 0;
    if (!reply || cap == 0) { return rc; }
    while (msg[i] && i + 1 < cap) { reply[i] = msg[i]; i++; }
    reply[i] = 0;
    return rc;
}

/* 800 kHz at 10 cycles/bit is an 8 MHz state-machine clock.
 * Divider is 16.8 fixed point: bits 23:8 integer, bits 7:0 fraction. */
static uint32_t ws_div(void) {
    uint32_t hz = pex_board_cpu_hz();
    if (hz < 8000000u) { return 0; }
    uint32_t q = hz / 8000000u;
    uint32_t rem = hz - q * 8000000u;
    uint32_t frac = (rem * 256u) / 8000000u;
    if (q == 0) { q = 1; }
    if (q > 65535u) { q = 65535u; }
    return (q << 8) | (frac & 0xffu);
}

static void hsv_rgb(int h, int s, int v, uint8_t *r, uint8_t *g, uint8_t *b) {
    h %= 360;
    if (h < 0) { h += 360; }
    if (s < 0) { s = 0; }
    if (s > 255) { s = 255; }
    if (v < 0) { v = 0; }
    if (v > 255) { v = 255; }
    int region = h / 60;
    int rem = h - region * 60;
    int p = v * (255 - s) / 255;
    int q = v * (255 - (s * rem) / 60) / 255;
    int t = v * (255 - (s * (60 - rem)) / 60) / 255;
    int rr = p, gg = p, bb = p;
    if (region == 0) { rr = v; gg = t; }
    else if (region == 1) { rr = q; gg = v; }
    else if (region == 2) { gg = v; bb = t; }
    else if (region == 3) { gg = q; bb = v; }
    else if (region == 4) { rr = t; bb = v; }
    else { rr = v; bb = q; }
    *r = (uint8_t)rr;
    *g = (uint8_t)gg;
    *b = (uint8_t)bb;
}

static void render(void) {
    int n = s_count;
    if (n < 1) { n = 1; }
    if (n > WS_MAX) { n = WS_MAX; }
    int shift = (int)(s_phase % 360u);
    int head = n ? (int)((s_phase / 24u) % (uint32_t)n) : 0;
    int breathe = (int)((s_phase / 8u) % 256u);
    if (breathe > 127) { breathe = 255 - breathe; }
    for (int i = 0; i < n; i++) {
        int h = s_hue;
        int v = s_val;
        if (s_mode == 1) {
            h = s_hue + (i * 360) / n + shift;
        } else if (s_mode == 2) {
            v = s_val * breathe / 127;
        } else if (s_mode == 3) {
            int d = i - head;
            if (d < 0) { d = -d; }
            int wrap = n - d;
            if (wrap < d) { d = wrap; }
            if (d == 0) { v = s_val; }
            else if (d == 1) { v = s_val / 6; }
            else { v = 0; }
            h = s_hue + shift;
        }
        uint8_t r, g, b;
        hsv_rgb(h, s_sat, v, &r, &g, &b);
        s_pix[i] = ((uint32_t)g << 24) | ((uint32_t)r << 16) | ((uint32_t)b << 8);
    }
}

/* One pixel is 24 bits at 800 kHz, 30 us. The joined FIFO is 8 deep. */
#define WS_FIFO 8
#define WS_LATCH_US 300u
#define WS_DRAIN_US 5000u

static void wait_us(uint32_t us) {
    uint64_t t0 = pex_us();
    while ((uint32_t)(pex_us() - t0) < us) { }
}

static int fifo_room(void) {
    uint32_t tx = 0;
    if (s_handle < 0) { return -1; }
    if (pex_pio_levels(s_handle, &tx, 0) < 0) { return -1; }
    if (tx >= (uint32_t)WS_FIFO) { return 0; }
    return (int)(WS_FIFO - tx);
}

/* Block until every queued word has been pulled, or the machine is stuck. */
static int wait_empty(void) {
    uint64_t t0 = pex_us();
    for (;;) {
        int room = fifo_room();
        if (room < 0) { return -1; }
        if (room >= WS_FIFO) { return 0; }
        if ((uint32_t)(pex_us() - t0) > WS_DRAIN_US) { return -1; }
    }
}

/* Shift s_pix[0..n) out as one frame. A low gap in the middle would
 * latch early, so a full FIFO is waited out here, for one pixel time. */
static int send_frame(int n) {
    int i = 0;
    int stalls = 0;
    if (n < 1 || n > WS_MAX) { return -1; }
    if (wait_empty() < 0) { return -1; }
    wait_us(WS_LATCH_US);
    while (i < n) {
        int room = fifo_room();
        int wrote = 0;
        if (room < 0) { return -1; }
        while (i < n && room > 0) {
            int rc = pex_pio_put(s_handle, s_pix[i]);
            if (rc == PEX_EAGAIN) { break; }
            if (rc < 0) { return -1; }
            i++;
            room--;
            wrote = 1;
        }
        if (wrote) { stalls = 0; continue; }
        wait_us(20);
        if (++stalls > 16) { return -1; }
    }
    return 0;
}

/* Zeros for every LED this claim has addressed, then hold the line low
 * until the strip latches. Releasing the pin before that leaves the
 * last colors lit. */
static void blank_strip(void) {
    int n = s_count;
    if (s_lit > n) { n = s_lit; }
    if (n < 1) { n = 1; }
    if (n > WS_MAX) { n = WS_MAX; }
    for (int i = 0; i < n; i++) { s_pix[i] = 0; }
    if (send_frame(n) == 0 && wait_empty() == 0) { wait_us(WS_LATCH_US); }
    s_lit = 0;
}

static void release_strip(void) {
    if (s_handle < 0) { return; }
    blank_strip();
    pex_pio_enable(s_handle, 0);
    pex_pio_release(s_handle);
    s_handle = -1;
    s_lit = 0;
}

static void start_frame(void) {
    int n, span;
    if (!s_on || s_handle < 0) { return; }
    if (s_speed > 0) { s_phase += (uint32_t)s_speed; }
    n = s_count;
    if (n < 1) { n = 1; }
    if (n > WS_MAX) { n = WS_MAX; }
    span = s_lit > n ? s_lit : n;
    render();
    for (int i = n; i < span; i++) { s_pix[i] = 0; }
    if (send_frame(span) == 0) { s_lit = n; }
}

static int tick(void *arg) {
    (void)arg;
    if (s_on) { start_frame(); }
    if (++s_pub >= 25) { s_pub = 0; publish(); }
    return PEX_TASK_AGAIN;
}

static int claim_strip(void) {
    if (pex_has(15) == 0) { return PEX_ENOTSUP; }
    uint32_t div = ws_div();
    if (div == 0) { return PEX_EINVAL; }
    uint8_t pin = (uint8_t)s_pin;
    int h = pex_pio_load(&pin, 1, WS_PROG, 4);
    if (h < 0) { return h; }
    pex_pio_cfg_t c;
    c.out_base = 0;
    c.out_count = 0;
    c.set_base = 0;
    c.set_count = 0;
    c.sideset_base = pin;
    c.sideset_count = 1;
    c.sideset_opt = 0;
    c.shift_right = 0;
    c.autopush = 0;
    c.autopull = 1;
    c.push_thresh = 0;
    c.pull_thresh = 24;
    c.fifo_join = PEX_PIO_JOIN_TX;
    c.in_base = -1;
    c.jmp_pin = -1;
    c.clkdiv = div;
    int rc = pex_pio_init(h, &c);
    if (rc >= 0) { rc = pex_pio_enable(h, 1); }
    if (rc < 0) {
        pex_pio_release(h);
        return rc;
    }
    s_handle = h;
    s_lit = 0;
    return PEX_OK;
}

static void defaults(void) {
    s_on = 0;
    s_pin = 22;
    s_count = 8;
    s_mode = 0;
    s_hue = 0;
    s_sat = 255;
    s_val = 80;
    s_speed = 24;
    s_phase = 0;
    s_handle = -1;
    s_lit = 0;
    describe(0);
}

static int op_set(void *arg, const char *params, char *reply, size_t cap) {
    (void)arg;
    if (!s_running) { return reply_msg(reply, cap, "module is stopped", PEX_ESTATE); }
    int pin = s_pin, count = s_count, hue = s_hue, sat = s_sat, val = s_val;
    int speed = s_speed, mode = s_mode, on = s_on;
    int64_t v = 0;
    int b = 0;
    if (pex_json_int(params, "pin", &v)) { pin = (int)v; }
    if (pex_json_int(params, "count", &v)) { count = (int)v; }
    if (pex_json_int(params, "hue", &v)) { hue = (int)v; }
    if (pex_json_int(params, "sat", &v)) { sat = (int)v; }
    if (pex_json_int(params, "val", &v)) { val = (int)v; }
    if (pex_json_int(params, "speed", &v)) { speed = (int)v; }
    if (pex_json_bool(params, "on", &b)) { on = b ? 1 : 0; }
    else if (pex_json_int(params, "on", &v)) { on = v ? 1 : 0; }
    char mode_s[16];
    if (pex_json_str(params, "mode", mode_s, sizeof mode_s)) {
        int m = mode_of(mode_s);
        if (m < 0) { return reply_msg(reply, cap, "mode is solid, rainbow, breathe, or chase", PEX_EINVAL); }
        mode = m;
    }
    if (pin < 0 || pin > 29) { return reply_msg(reply, cap, "pin is 0-29 on this board", PEX_EINVAL); }
    const char *blocked = pin_reason(pin);
    if (blocked) { return reply_msg(reply, cap, blocked, PEX_EINVAL); }
    if (count < 1 || count > WS_MAX) { return reply_msg(reply, cap, "count is 1-64", PEX_EINVAL); }
    if (hue < 0 || hue > 359) { return reply_msg(reply, cap, "hue is 0-359", PEX_EINVAL); }
    if (sat < 0 || sat > 255 || val < 0 || val > 255) {
        return reply_msg(reply, cap, "saturation and brightness are 0-255", PEX_EINVAL);
    }
    if (speed < 0 || speed > 100) { return reply_msg(reply, cap, "speed is 0-100", PEX_EINVAL); }

    int pin_changed = pin != s_pin;
    s_pin = pin;
    s_count = count;
    s_hue = hue;
    s_sat = sat;
    s_val = val;
    s_speed = speed;
    s_mode = mode;

    if (!on) {
        release_strip();
        s_on = 0;
        describe(0);
        publish();
    } else if (s_handle < 0 || pin_changed) {
        release_strip();
        int rc = claim_strip();
        if (rc < 0) {
            s_on = 0;
            const char *why = (rc == PEX_EINVAL && ws_div() == 0) ? "clock is unknown" : pio_why(rc);
            describe(why);
            publish();
            return reply_msg(reply, cap, why, rc);
        }
        s_on = 1;
        describe(0);
        pex_log(PEX_LOG_INFO, "ws2812: driving GP%d, %d LEDs", s_pin, s_count);
        publish();
        start_frame();
    } else {
        s_on = 1;
        describe(0);
        publish();
        start_frame();
    }

    pex_jw_t w;
    pex_jw_init(&w, reply, cap);
    pex_jw_obj(&w, NULL);
    pex_jw_bool(&w, "ok", 1);
    pex_jw_int(&w, "on", s_on);
    pex_jw_int(&w, "pin", s_pin);
    pex_jw_int(&w, "count", s_count);
    pex_jw_str(&w, "mode", mode_name(s_mode));
    pex_jw_end(&w);
    return pex_jw_finish(&w);
}

int pex_main(const pex_host_t *host, uint32_t reason, void *arg) {
    (void)host;
    (void)arg;
    switch (reason) {
    case PEX_INIT:
        return pex_bus_op("ws2812.set", op_set, 0);
    case PEX_APP_RUN:
        release_strip();
        defaults();
        s_running = 1;
        s_pub = 0;
        s_task = pex_sched_every(WS_FRAME_MS, tick, 0);
        if (s_task <= 0) { s_running = 0; return s_task < 0 ? s_task : PEX_ENOMEM; }
        publish();
        pex_log(PEX_LOG_INFO, "ws2812: idle, data pin GP%d, strip off until the window turns it on", s_pin);
        return PEX_OK;
    case PEX_APP_STOP:
        s_running = 0;
        s_on = 0;
        release_strip();
        if (s_task > 0) { pex_sched_cancel(s_task); s_task = 0; }
        pex_bus_emit("profile.ws2812.gone", "{}");
        return PEX_OK;
    case PEX_DEINIT:
        s_running = 0;
        s_on = 0;
        release_strip();
        if (s_task > 0) { pex_sched_cancel(s_task); s_task = 0; }
        return PEX_OK;
    default:
        return PEX_OK;
    }
}
