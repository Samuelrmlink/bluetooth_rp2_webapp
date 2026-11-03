/* WS2812B strip. Idle until the device window turns it on, then one
 * unprotected state machine on the data pin. The window is
 * profile.ws2812.telemetry, same discovery path as Pinecil.
 *
 * The four instruction words are pioasm of ws2812.pio (origin 0).
 * The ledger adds the load offset to each JMP. Pixels are GRB in the
 * top 24 bits. Two section buffers of 256 pixels are enough for any
 * length: DMA shifts the current section into the TX FIFO while the
 * main loop renders the next one. The joined FIFO holds eight pixels
 * after a section ends, which is the time the interrupt has to start
 * the armed section. The last section is not followed by another, so
 * the line stays low and the strip latches. Off asks the firmware to
 * clock zeros, and the pin stays a PIO output until that transfer is
 * idle and the latch time has passed. */
#include "pex.h"

#define WS_SECTION 256
#define WS_MAX 65535
#define WS_FRAME_US 20000u
#define WS_LATCH_US 300u

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
static int s_lit;                  /* pixels the last finished frame addressed */
static int s_pub;
static int s_blank;
static int s_reclaim;
static int s_frame;
static int s_span;                 /* pixels this frame will clock, tail included */
static int s_sent;                 /* pixels already given to DMA this frame */
static int s_free;                 /* section half DMA is not reading */
static int s_color;                /* LED count captured when the frame started */
static int s_fmode, s_fhue, s_fsat, s_fval;
static uint32_t s_fphase;
static uint64_t s_pass_us;
static char s_emit[480];
static char s_status[96];
static uint32_t s_sec[2][WS_SECTION];

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
    if (rc == PEX_EAGAIN) { return "DMA already has a section waiting"; }
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
    if (s_blank) { put_str(p, end, "blanking"); return; }
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

/* LEDs at and after s_color are the tail a shorter count still has to darken. */
static void render_range(uint32_t *dst, int start, int n) {
    int color_n = s_color;
    if (color_n < 1) { color_n = 1; }
    int shift = (int)(s_fphase % 360u);
    int head = (int)((s_fphase / 24u) % (uint32_t)color_n);
    int breathe = (int)((s_fphase / 8u) % 256u);
    if (breathe > 127) { breathe = 255 - breathe; }
    for (int i = 0; i < n; i++) {
        int led = start + i;
        if (led >= color_n) { dst[i] = 0; continue; }
        int h = s_fhue;
        int v = s_fval;
        if (s_fmode == 1) {
            h = s_fhue + (led * 360) / color_n + shift;
        } else if (s_fmode == 2) {
            v = s_fval * breathe / 127;
        } else if (s_fmode == 3) {
            int d = led - head;
            if (d < 0) { d = -d; }
            int wrap = color_n - d;
            if (wrap < d) { d = wrap; }
            if (d == 0) { v = s_fval; }
            else if (d == 1) { v = s_fval / 6; }
            else { v = 0; }
            h = s_fhue + shift;
        }
        uint8_t r, g, b;
        hsv_rgb(h, s_fsat, v, &r, &g, &b);
        dst[i] = ((uint32_t)g << 24) | ((uint32_t)r << 16) | ((uint32_t)b << 8);
    }
}

static int section_n(int sent, int span) {
    int left = span - sent;
    if (left > WS_SECTION) { left = WS_SECTION; }
    if (left < 0) { left = 0; }
    return left;
}

static void wait_us(uint32_t us) {
    uint64_t t0 = pex_us();
    while ((uint32_t)(pex_us() - t0) < us) { }
}

static void drop_handle(void) {
    if (s_handle < 0) { return; }
    pex_pio_enable(s_handle, 0);
    pex_pio_release(s_handle);
    s_handle = -1;
}

static int claim_strip(void);
static void finish_blank(void);

static int start_pass(void) {
    int n, n2, rc;
    if (s_handle < 0 || !s_on || s_blank || s_frame) { return PEX_OK; }
    if (s_speed > 0) { s_phase += (uint32_t)s_speed; }
    s_color = s_count;
    if (s_color < 1) { s_color = 1; }
    s_span = s_lit > s_color ? s_lit : s_color;
    s_sent = 0;
    s_free = 0;
    s_fmode = s_mode;
    s_fhue = s_hue;
    s_fsat = s_sat;
    s_fval = s_val;
    s_fphase = s_phase;
    s_pass_us = pex_us();
    n = section_n(0, s_span);
    render_range(s_sec[0], 0, n);
    rc = pex_pio_dma_tx(s_handle, s_sec[0], (uint32_t)n);
    if (rc < 0) {
        describe("dma failed");
        s_on = 0;
        s_frame = 0;
        drop_handle();
        return rc;
    }
    s_sent = n;
    s_frame = 1;
    if (s_sent >= s_span) { return PEX_OK; }
    n2 = section_n(s_sent, s_span);
    render_range(s_sec[1], s_sent, n2);
    if (pex_pio_dma_arm(s_handle, s_sec[1], (uint32_t)n2) == 0) { s_sent += n2; }
    return PEX_OK;
}

static void feed_more(void) {
    int n;
    if (s_handle < 0 || !s_frame) { return; }
    if (s_sent < s_span) {
        n = section_n(s_sent, s_span);
        render_range(s_sec[s_free], s_sent, n);
        if (pex_pio_dma_arm(s_handle, s_sec[s_free], (uint32_t)n) == 0) {
            s_sent += n;
            s_free ^= 1;
        } else if (pex_pio_dma_idle(s_handle) == 1) {
            s_frame = 0;
            describe("dma failed");
        }
        return;
    }
    if (pex_pio_dma_idle(s_handle) == 1) {
        s_frame = 0;
        s_lit = s_color;
    }
}

static void begin_blank(void) {
    int n = s_count;
    if (s_lit > n) { n = s_lit; }
    if (s_span > n) { n = s_span; }
    if (n < 1) { n = 1; }
    if (n > WS_MAX) { n = WS_MAX; }
    s_frame = 0;
    s_blank = 1;
    describe(0);
    if (s_handle < 0) { s_blank = 0; return; }
    if (pex_pio_dma_zero(s_handle, (uint32_t)n) < 0) {
        drop_handle();
        s_blank = 0;
        s_lit = 0;
        s_span = 0;
        describe("blank failed");
    }
}

static void finish_blank(void) {
    int reclaim;
    if (!s_blank || s_handle < 0) { return; }
    if (pex_pio_dma_idle(s_handle) != 1) { return; }
    wait_us(WS_LATCH_US);
    reclaim = s_reclaim && s_on && s_running;
    drop_handle();
    s_blank = 0;
    s_lit = 0;
    s_span = 0;
    s_frame = 0;
    s_reclaim = 0;
    if (reclaim) {
        int rc = claim_strip();
        if (rc < 0) {
            s_on = 0;
            describe((rc == PEX_EINVAL && ws_div() == 0) ? "clock is unknown" : pio_why(rc));
        } else {
            describe(0);
            start_pass();
        }
    } else {
        s_on = 0;
        describe(0);
    }
    if (s_running) { publish(); }
}

static void on_dma(void *arg) {
    (void)arg;
    if (s_handle < 0) { return; }
    if (s_blank) { finish_blank(); return; }
    if (s_frame) { feed_more(); }
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
    if (rc >= 0) { rc = pex_pio_dma_listen(h, on_dma, 0); }
    if (rc < 0) {
        pex_pio_release(h);
        return rc;
    }
    s_handle = h;
    s_frame = 0;
    s_sent = 0;
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
    s_blank = 0;
    s_reclaim = 0;
    s_frame = 0;
    s_span = 0;
    s_sent = 0;
    s_free = 0;
    s_pass_us = 0;
    describe(0);
}

static int tick(void *arg) {
    (void)arg;
    if (s_blank) {
        finish_blank();
    } else if (s_on && s_handle >= 0 && !s_frame && pex_pio_dma_idle(s_handle) == 1 &&
               pex_us() - s_pass_us >= WS_FRAME_US) {
        start_pass();
    }
    if (++s_pub >= 25) { s_pub = 0; publish(); }
    return PEX_TASK_AGAIN;
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
    if (count < 1 || count > WS_MAX) { return reply_msg(reply, cap, "count is 1-65535", PEX_EINVAL); }
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
        s_on = 0;
        s_reclaim = 0;
        if (s_handle >= 0 && !s_blank) { begin_blank(); }
        else if (!s_blank) { describe(0); }
        publish();
    } else if (s_blank) {
        s_on = 1;
        s_reclaim = 1;
        publish();
    } else if (s_handle < 0 || pin_changed) {
        if (s_handle >= 0) {
            s_on = 1;
            s_reclaim = 1;
            begin_blank();
            publish();
        } else {
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
            start_pass();
        }
    } else {
        s_on = 1;
        describe(0);
        publish();
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
        if (s_handle >= 0) { drop_handle(); }
        defaults();
        s_running = 1;
        s_pub = 0;
        s_task = pex_sched_every(20, tick, 0);
        if (s_task <= 0) { s_running = 0; return s_task < 0 ? s_task : PEX_ENOMEM; }
        publish();
        pex_log(PEX_LOG_INFO, "ws2812: idle, data pin GP%d, strip off until the window turns it on", s_pin);
        return PEX_OK;
    case PEX_APP_STOP:
        s_running = 0;
        s_on = 0;
        s_reclaim = 0;
        if (s_handle >= 0 && !s_blank) { begin_blank(); }
        if (s_task > 0) { pex_sched_cancel(s_task); s_task = 0; }
        pex_bus_emit("profile.ws2812.gone", "{}");
        return PEX_OK;
    case PEX_DEINIT:
        s_running = 0;
        s_on = 0;
        s_blank = 0;
        s_reclaim = 0;
        drop_handle();
        if (s_task > 0) { pex_sched_cancel(s_task); s_task = 0; }
        return PEX_OK;
    default:
        return PEX_OK;
    }
}
