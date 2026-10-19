/*
 * The platform under spibus.c for host tests, and a W5500 on the end of it.
 *
 * The DMA moves the bytes the moment a segment starts (so the simulated chip
 * sees them in order) but reports completion only when the test calls
 * fake_dma_complete(), which is where spibus_dma_done() -- the IRQ path --
 * runs. That is what lets a test check that nothing is delivered before the
 * loop's spibus_poll(), and that a busy bus really excludes other devices.
 *
 * The chip model covers what the MACRAW driver touches: the SPI frame
 * (16-bit address, control byte with block select and R/W, auto-increment),
 * VERSIONR, MR reset, SHAR, buffer sizes, Sn_MR/CR/IR/SR/IMR, the TX and RX
 * pointers, PHYCFGR, the RX ring with its 2-byte length prefix and
 * wrap-around at 16 KB, and the INT line.
 */
#include "fake_spi.h"

#include <stdio.h>
#include <string.h>

/* --- the port ------------------------------------------------------------ */

#define NPINS 32
static int gpio_level[NPINS];
static bool gpio_is_input[NPINS];
static int input_level[NPINS];
static bool reserved[NPINS];
static struct { fake_dev_fn fn; void *ctx; } dev_at[NPINS];
static int dma_starts;
static spibus_t *inflight;
static uint32_t baud_of[2];
static uint8_t mode_of[2];
static uint32_t now_ms;
static bool cs_glitch;
static bool autocomplete;
static uint32_t now_us;
static int bus_conflicts;

void fake_reset(void) {
    for (int i = 0; i < NPINS; i++) {
        gpio_level[i] = -1; gpio_is_input[i] = false; input_level[i] = 1;
        reserved[i] = false; dev_at[i].fn = NULL;
    }
    dma_starts = 0; inflight = NULL; now_ms = 1000; cs_glitch = false; autocomplete = false; now_us = 0;
    bus_conflicts = 0;
}

void fake_autocomplete(bool on) { autocomplete = on; }
void fake_attach(int cs, fake_dev_fn fn, void *ctx) { dev_at[cs].fn = fn; dev_at[cs].ctx = ctx; }
int fake_dma_starts(void) { return dma_starts; }
int fake_bus_conflicts(void) { return bus_conflicts; }
bool fake_dma_pending(void) { return inflight != NULL; }
void fake_dma_complete(void) {
    spibus_t *b = inflight;
    inflight = NULL;
    if (b) { spibus_dma_done(b); }
}
int fake_gpio(int pin) { return gpio_level[pin]; }
void fake_set_input(int pin, bool level) { input_level[pin] = level; }
uint32_t fake_baud(int bus) { return baud_of[bus]; }
uint8_t fake_mode(int bus) { return mode_of[bus]; }
void fake_advance_ms(uint32_t ms) { now_ms += ms; }
uint32_t fake_now(void) { return now_ms; }
bool fake_cs_glitched(void) { return cs_glitch; }
int fake_reserved_count(void) {
    int n = 0;
    for (int i = 0; i < NPINS; i++) { n += reserved[i]; }
    return n;
}

void spibus_port_sem_init(spibus_sem_t *s) { s->permits = 1; }
bool spibus_port_sem_try(spibus_sem_t *s) {
    if (s->permits <= 0) { return false; }
    s->permits--;
    return true;
}
bool spibus_port_sem_take(spibus_sem_t *s, uint32_t t) { (void)t; return spibus_port_sem_try(s); }
void spibus_port_sem_give(spibus_sem_t *s) { if (s->permits < 1) { s->permits++; } }

bool spibus_port_open(spibus_t *b) { b->dma_tx = 0; b->dma_rx = 1; return true; }
void spibus_port_format(spibus_t *b, uint32_t baud, uint8_t mode) {
    baud_of[b->index] = baud;
    mode_of[b->index] = mode;
}

static int low_cs(void) {
    for (int i = 0; i < NPINS; i++) {
        if (!gpio_is_input[i] && gpio_level[i] == 0 && dev_at[i].fn) { return i; }
    }
    return -1;
}

void spibus_port_dma_start(spibus_t *b, const uint8_t *tx, uint8_t *rx, uint16_t len) {
    dma_starts++;
    inflight = b;
    int cs = low_cs();
    for (uint16_t i = 0; i < len; i++) {
        uint8_t out = tx ? tx[i] : b->zero_tx;
        uint8_t in = cs >= 0 ? dev_at[cs].fn(dev_at[cs].ctx, FAKE_BYTE, out) : 0xFF;
        /* Every deselected device sees the clock too. One that still drives
         * MISO (an SD card before its release clock) fights the selected
         * one: the byte is garbage and the conflict is counted. */
        for (int p = 0; p < NPINS; p++) {
            if (p == cs || !dev_at[p].fn || gpio_level[p] == 0) { continue; }
            if (dev_at[p].fn(dev_at[p].ctx, FAKE_IDLE_BYTE, out) && cs >= 0) {
                bus_conflicts++;         /* only a fight if someone is selected */
                in = 0xEE;
            }
        }
        if (rx) { rx[i] = in; }
    }
    if (autocomplete) { fake_dma_complete(); }
}

void spibus_port_release_pin(int pin, const char *owner) { (void)owner; if (pin >= 0) { reserved[pin] = false; } }
void spibus_port_close(spibus_t *b) { (void)b; }
void spibus_port_dma_abort(spibus_t *b) { (void)b; inflight = NULL; }

void spibus_port_gpio_out(int pin, bool level) {
    gpio_is_input[pin] = false;
    spibus_port_gpio_put(pin, level);
}
void spibus_port_gpio_put(int pin, bool level) {
    int was = gpio_level[pin];
    gpio_level[pin] = level;
    if (dev_at[pin].fn && was != (int)level) {
        if (level && inflight) { cs_glitch = true; }
        dev_at[pin].fn(dev_at[pin].ctx, level ? FAKE_CS_HIGH : FAKE_CS_LOW, 0);
    }
}
void spibus_port_gpio_in(int pin, bool pull_up) { (void)pull_up; gpio_is_input[pin] = true; }
bool spibus_port_gpio_get(int pin) { return input_level[pin] != 0; }
void spibus_port_delay_us(uint32_t us) {
    now_us += us;
    while (now_us >= 1000) { now_us -= 1000; now_ms++; }
}
uint32_t spibus_port_now_ms(void) { return now_ms; }

bool spibus_port_reserve_pin(int pin, const char *owner, bool is_spi) {
    (void)owner; (void)is_spi;
    if (pin < 0) { return true; }
    if (reserved[pin]) { return false; }
    reserved[pin] = true;
    return true;
}

bool spibus_port_adhoc(int index, int sck, int mosi, int miso, int cs, uint32_t hz,
                       const uint8_t *tx, uint8_t *rx, size_t len) {
    (void)index; (void)sck; (void)mosi; (void)miso; (void)cs; (void)hz;
    memcpy(rx, tx, len);
    return true;
}

/* --- the W5500 ----------------------------------------------------------- */

#define RXMASK 0x3FFF

static uint16_t rd16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }

static void update_int(w5500_sim_t *s) {
    bool asserted = (s->sreg[0][0x02] & s->sreg[0][0x2C]) && (s->common[0x18] & 1);
    if (s->int_pin >= 0) { fake_set_input(s->int_pin, !asserted); }
}

static void update_phy(w5500_sim_t *s) {
    s->common[0x2E] = (uint8_t)(0xB8 | (s->link ? 0x07 : 0x00));
}

static void chip_reset(w5500_sim_t *s) {
    memset(s->common, 0, sizeof s->common);
    memset(s->sreg, 0, sizeof s->sreg);
    s->common[0x39] = 0x04;
    for (int n = 0; n < 8; n++) { s->sreg[n][0x1E] = 2; s->sreg[n][0x1F] = 2; }
    s->rx_wr = 0;
    update_phy(s);
}

static void sock_cmd(w5500_sim_t *s, uint8_t cmd) {
    uint8_t *r = s->sreg[0];
    switch (cmd) {
    case 0x01:   /* OPEN */
        if ((r[0x00] & 0x0F) == 0x04) { r[0x03] = 0x42; }
        wr16(&r[0x22], 0); wr16(&r[0x24], 0); wr16(&r[0x28], 0); wr16(&r[0x26], 0);
        s->rx_wr = 0;
        break;
    case 0x10:   /* CLOSE */
        r[0x03] = 0;
        break;
    case 0x20: { /* SEND: TX_RD .. TX_WR is one frame */
        uint16_t rd = rd16(&r[0x22]), wr = rd16(&r[0x24]);
        uint16_t n = (uint16_t)(wr - rd);
        if (s->n_sent < 32) {
            for (uint16_t i = 0; i < n && i < 1600; i++) {
                s->sent[s->n_sent][i] = s->txbuf[(uint16_t)(rd + i) & RXMASK];
            }
            s->sent_len[s->n_sent++] = n;
        }
        wr16(&r[0x22], wr);
        if (!s->defer_sendok) { r[0x02] |= 0x10; }
        break;
    }
    case 0x40: { /* RECV: RSR follows the committed RX_RD */
        uint16_t rd = rd16(&r[0x28]);
        wr16(&r[0x26], (uint16_t)(s->rx_wr - rd));
        break;
    }
    }
    update_int(s);
}

static uint8_t sim_byte(void *ctx, int event, uint8_t mosi) {
    w5500_sim_t *s = (w5500_sim_t *)ctx;
    if (event == FAKE_CS_LOW) { s->phase = 0; return 0; }
    if (event != FAKE_BYTE) { return 0; }   /* idle clocks: the W5500 lets go at CS high */
    if (s->phase == 0) { s->addr = (uint16_t)(mosi << 8); s->phase = 1; return 0; }
    if (s->phase == 1) { s->addr |= mosi; s->phase = 2; return 0; }
    if (s->phase == 2) { s->ctrl = mosi; s->phase = 3; return 0; }

    uint8_t bsb = s->ctrl >> 3;
    bool write = (s->ctrl & 0x04) != 0;
    uint16_t a = s->addr++;
    uint8_t *cell = NULL;
    if (bsb == 0) {
        if (a < sizeof s->common) { cell = &s->common[a]; }
    } else {
        int sock = bsb >> 2, kind = bsb & 3;
        if (kind == 1 && a < 0x30) { cell = &s->sreg[sock][a]; }
        else if (kind == 2 && sock == 0) { cell = &s->txbuf[a & RXMASK]; }
        else if (kind == 3 && sock == 0) { cell = &s->rxbuf[a & RXMASK]; }
    }
    if (!cell) { return 0; }
    if (!write) {
        /* TX_FSR is computed. */
        if (bsb == 1 && (a == 0x20 || a == 0x21)) {
            uint16_t fsr = (uint16_t)(16384 - (uint16_t)(rd16(&s->sreg[0][0x24]) - rd16(&s->sreg[0][0x22])));
            return a == 0x20 ? (uint8_t)(fsr >> 8) : (uint8_t)fsr;
        }
        uint8_t v = *cell;
        if (bsb == 1 && a == 0x01) { v = 0; }           /* CR reads back 0 */
        return v;
    }
    if (bsb == 0 && a == 0x00 && (mosi & 0x80)) { chip_reset(s); return 0; }
    if (bsb == 0 && a == 0x39) { return 0; }             /* read-only */
    if (bsb == 1 && a == 0x01) { sock_cmd(s, mosi); return 0; }
    if (bsb == 1 && a == 0x02) { *cell &= (uint8_t)~mosi; update_int(s); return 0; }
    if (bsb == 1 && (a == 0x03 || a == 0x26 || a == 0x27 || a == 0x22 || a == 0x23)) { return 0; }
    *cell = mosi;
    if (bsb == 1 && a == 0x2C) { update_int(s); }
    if (bsb == 0 && a == 0x18) { update_int(s); }
    return 0;
}

void sim_init(w5500_sim_t *s, int cs, int int_pin) {
    memset(s, 0, sizeof *s);
    s->int_pin = int_pin;
    chip_reset(s);
    fake_attach(cs, sim_byte, s);
    if (int_pin >= 0) { fake_set_input(int_pin, true); }
}

uint16_t sim_rx_free(const w5500_sim_t *s) {
    return (uint16_t)(16384 - (uint16_t)(s->rx_wr - rd16(&s->sreg[0][0x28])));
}

bool sim_inject(w5500_sim_t *s, const uint8_t *frame, uint16_t len) {
    if (s->sreg[0][0x03] != 0x42 || !s->link) { s->dropped++; return false; }
    uint16_t need = (uint16_t)(len + 2);
    if (sim_rx_free(s) < need) { s->dropped++; return false; }
    s->rxbuf[s->rx_wr & RXMASK] = (uint8_t)(need >> 8);
    s->rxbuf[(uint16_t)(s->rx_wr + 1) & RXMASK] = (uint8_t)need;
    for (uint16_t i = 0; i < len; i++) {
        s->rxbuf[(uint16_t)(s->rx_wr + 2 + i) & RXMASK] = frame[i];
    }
    s->rx_wr = (uint16_t)(s->rx_wr + need);
    uint8_t *r = s->sreg[0];
    wr16(&r[0x26], (uint16_t)(rd16(&r[0x26]) + need));
    r[0x02] |= 0x04;
    update_int(s);
    return true;
}

void sim_set_link(w5500_sim_t *s, bool up) { s->link = up; update_phy(s); }
void sim_sendok(w5500_sim_t *s) { s->sreg[0][0x02] |= 0x10; update_int(s); }

void sim_corrupt_next(w5500_sim_t *s) {
    uint16_t rd = rd16(&s->sreg[0][0x28]);
    s->rxbuf[rd & RXMASK] = 0xFF;
    s->rxbuf[(uint16_t)(rd + 1) & RXMASK] = 0xFF;
}
