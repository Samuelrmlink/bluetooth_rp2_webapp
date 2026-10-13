/*
 * src/net/w5500.c against a simulated W5500 (fake_spi.c), through the real
 * src/io/spibus.c.
 *
 * Covers: probe (absent chip touches nothing), MACRAW open, link up/down,
 * frames in (single, back-to-back, chained into two buffers, wrapping the
 * 16 KB ring, ring full), no buffer -> dropped but the ring advances, frames
 * out (one SEND at a time, SENDOK collected, link down drops), a corrupt
 * length -> socket reopened and traffic continues, and a bus held by another
 * device -> no progress until it is released.
 *
 *   gcc -DSPIBUS_HOST -Isrc -Itest/host test/host/w5500_test.c src/net/w5500.c \
 *       src/io/spibus.c test/host/fake_spi.c
 */
#include <stdio.h>
#include <string.h>

#include "net/w5500.h"
#include "fake_spi.h"

static int checks, failed;
#define CHECK(c, msg) do { checks++; if (!(c)) { failed++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, msg); } } while (0)

#define CS 13
#define INT 10

/* --- a stand-in for the lwIP glue ---------------------------------------- */
static uint8_t rx_store[64][1600];
static uint16_t rx_len[64];
static int n_rx, rx_aborted;
static bool rx_no_mem, rx_chain;
static uint8_t cur[1600];
static uint16_t cur_len;

static int g_rx_begin(void *ctx, uint16_t len, w5500_buf_t *b, int max) {
    (void)ctx; (void)max;
    if (rx_no_mem) { return 0; }
    cur_len = len;
    if (rx_chain && len > 100) {
        b[0] = (w5500_buf_t){ cur, 100 };
        b[1] = (w5500_buf_t){ cur + 100, (uint16_t)(len - 100) };
        return 2;
    }
    b[0] = (w5500_buf_t){ cur, len };
    return 1;
}
static void g_rx_end(void *ctx, bool ok) {
    (void)ctx;
    if (!ok) { rx_aborted++; return; }
    if (n_rx < 64) { memcpy(rx_store[n_rx], cur, cur_len); rx_len[n_rx] = cur_len; }
    n_rx++;
}

static uint8_t txq[8][1600];
static uint16_t txq_len[8];
static int tx_head, tx_tail, tx_ok, tx_fail;
static int g_tx_peek(void *ctx, w5500_buf_t *b, int max) {
    (void)ctx; (void)max;
    if (tx_head == tx_tail) { return 0; }
    int i = tx_head % 8;
    /* two pieces, as a pbuf chain would be */
    uint16_t n = txq_len[i], h = n > 14 ? 14 : n;
    b[0] = (w5500_buf_t){ txq[i], h };
    if (n == h) { return 1; }
    b[1] = (w5500_buf_t){ txq[i] + h, (uint16_t)(n - h) };
    return 2;
}
static void g_tx_done(void *ctx, bool ok) { (void)ctx; tx_head++; if (ok) { tx_ok++; } else { tx_fail++; } }
static void queue_tx(const uint8_t *f, uint16_t n) { memcpy(txq[tx_tail % 8], f, n); txq_len[tx_tail % 8] = n; tx_tail++; }

static int link_calls, link_speed;
static bool link_up, link_full;
static void g_link(void *ctx, bool up, int speed, bool full) {
    (void)ctx; link_calls++; link_up = up; link_speed = speed; link_full = full;
}

static const w5500_ops_t ops = { g_rx_begin, g_rx_end, g_tx_peek, g_tx_done, g_link };

/* Drive the loop the way eth_poll() does: per pass, up to 8 rounds of
 * {DMA completes, callbacks, step} while the machine makes progress. */
static void run(w5500_t *w, int passes) {
    for (int i = 0; i < passes; i++) {
        for (int k = 0; k < 8; k++) {
            while (fake_dma_pending()) { fake_dma_complete(); }
            spibus_poll();
            if (!w5500_step(w, fake_now())) { break; }
        }
        fake_advance_ms(1);
    }
}

static void mkframe(uint8_t *f, uint16_t n, uint8_t seed) {
    for (uint16_t i = 0; i < n; i++) { f[i] = (uint8_t)(seed + i * 7); }
}

static w5500_sim_t sim;

int main(void) {
    const uint8_t mac[6] = { 0x02, 0x11, 0x22, 0x33, 0x44, 0x55 };
    w5500_t w;

    /* --- absent chip ----------------------------------------------------- */
    fake_reset();
    fake_autocomplete(true);
    spibus_t *bus = spibus_init(1, 14, 15, 8, "w5500");
    CHECK(!w5500_init(&w, bus, CS, 30000000, INT, 11, mac, true, &ops, NULL), "no chip -> false");
    CHECK(!w.present, "not present");
    CHECK(!spibus_holds(&w.dev), "bus released after a failed probe");
    CHECK(!w5500_step(&w, fake_now()), "an absent chip never steps");

    /* --- present --------------------------------------------------------- */
    fake_reset();
    fake_autocomplete(true);
    bus = spibus_init(1, 14, 15, 8, "w5500");
    sim_init(&sim, CS, INT);
    CHECK(w5500_init(&w, bus, CS, 30000000, INT, 11, mac, true, &ops, NULL), "chip found");
    CHECK(sim.sreg[0][0x03] == 0x42, "socket 0 open in MACRAW");
    CHECK(sim.sreg[0][0x00] == 0x94, "MACRAW + MIP6B + MFEN, multicast not blocked");
    CHECK(memcmp(&sim.common[0x09], mac, 6) == 0, "MAC written to SHAR");
    CHECK(sim.sreg[0][0x1E] == 16 && sim.sreg[0][0x1F] == 16 && sim.sreg[1][0x1E] == 0 && sim.sreg[7][0x1F] == 0,
          "socket 0 has all 16 KB, the rest none");
    CHECK(sim.sreg[0][0x2C] == 0x04 && sim.common[0x18] == 0x01, "INT on socket 0 RECV");
    CHECK(fake_gpio(11) == 1, "RST released high");
    fake_autocomplete(false);

    /* --- link ------------------------------------------------------------ */
    run(&w, 600);
    CHECK(link_calls == 0, "no link yet, no callback");
    sim_set_link(&sim, true);
    run(&w, 600);
    CHECK(link_calls == 1 && link_up && link_speed == 100 && link_full, "link up 100/full reported once");

    /* --- one frame in ---------------------------------------------------- */
    uint8_t f[1600];
    mkframe(f, 60, 1);
    sim_inject(&sim, f, 60);
    CHECK(!spibus_port_gpio_get(INT), "INT asserted by the frame");
    run(&w, 20);
    CHECK(n_rx == 1 && rx_len[0] == 60 && memcmp(rx_store[0], f, 60) == 0, "frame delivered intact");
    CHECK(sim.sreg[0][0x02] == 0, "Sn_IR cleared, INT released");
    CHECK(sim_rx_free(&sim) == 16384, "ring fully consumed");

    /* --- back-to-back ---------------------------------------------------- */
    for (int i = 0; i < 3; i++) { mkframe(f, (uint16_t)(100 + i), (uint8_t)(10 + i)); sim_inject(&sim, f, (uint16_t)(100 + i)); }
    run(&w, 40);
    CHECK(n_rx == 4, "three queued frames all delivered");
    mkframe(f, 102, 12);
    CHECK(rx_len[3] == 102 && memcmp(rx_store[3], f, 102) == 0, "in order");

    /* --- wrap-around + chained buffers ------------------------------------ */
    rx_chain = true;
    int before = n_rx, sent_ok = 0, bad = 0;
    uint32_t total = 0;
    for (int i = 0; i < 40; i++) {
        uint16_t n = (uint16_t)(600 + (i * 37) % 900);
        mkframe(f, n, (uint8_t)i);
        if (sim_inject(&sim, f, n)) { sent_ok++; total += n + 2u; }
        run(&w, 12);
        int last = (n_rx - 1) % 64;
        if (rx_len[last] != n || memcmp(rx_store[last], f, n) != 0) { bad++; }
    }
    CHECK(sent_ok == 40 && n_rx - before == 40, "40 frames across the 16 KB wrap, none lost");
    CHECK(total > 2 * 16384, "the ring wrapped (twice)");
    CHECK(bad == 0, "every frame intact, including the ones split by the wrap");
    /* the last one, through two buffers, intact */
    { uint16_t n = (uint16_t)(600 + (39 * 37) % 900); mkframe(f, n, 39);
      CHECK(rx_len[(n_rx - 1) % 64] == n && memcmp(rx_store[(n_rx - 1) % 64], f, n) == 0, "chained, wrapped frame intact"); }
    rx_chain = false;

    /* --- ring full ------------------------------------------------------- */
    int dropped0 = sim.dropped, got = 0;
    for (int i = 0; i < 20; i++) { mkframe(f, 1500, (uint8_t)i); if (sim_inject(&sim, f, 1500)) { got++; } }
    CHECK(sim.dropped > dropped0, "the chip dropped what did not fit");
    before = n_rx;
    run(&w, 200);
    CHECK(n_rx - before == got, "everything that fitted was delivered");

    /* --- no buffer: dropped, ring still advances --------------------------- */
    rx_no_mem = true;
    mkframe(f, 80, 3);
    sim_inject(&sim, f, 80);
    before = n_rx;
    run(&w, 20);
    CHECK(n_rx == before && w.rx_dropped == 1, "no pbuf -> dropped, counted");
    CHECK(sim_rx_free(&sim) == 16384, "and the ring advanced past it");
    rx_no_mem = false;

    /* --- frames out ------------------------------------------------------ */
    uint8_t t1[200], t2[90];
    mkframe(t1, 200, 50); mkframe(t2, 90, 60);
    sim.defer_sendok = true;
    queue_tx(t1, 200);
    queue_tx(t2, 90);
    run(&w, 20);
    CHECK(sim.n_sent == 1 && sim.sent_len[0] == 200 && memcmp(sim.sent[0], t1, 200) == 0, "first frame sent intact");
    CHECK(w.send_busy, "waiting for SENDOK");
    run(&w, 20);
    CHECK(sim.n_sent == 1, "second frame held until SENDOK");
    sim_sendok(&sim);
    sim.defer_sendok = false;
    run(&w, 20);
    CHECK(sim.n_sent == 2 && sim.sent_len[1] == 90 && memcmp(sim.sent[1], t2, 90) == 0, "then the second");
    CHECK(tx_ok == 2 && tx_fail == 0, "both reported done");

    /* --- link down drops queued frames ------------------------------------ */
    sim_set_link(&sim, false);
    run(&w, 600);
    CHECK(!link_up && link_calls == 2, "link down reported");
    queue_tx(t1, 200);
    run(&w, 5);
    CHECK(tx_fail == 1 && sim.n_sent == 2, "nothing goes out on a dead link");
    sim_set_link(&sim, true);
    run(&w, 600);

    /* --- corrupt length -> reopen ---------------------------------------- */
    mkframe(f, 70, 9);
    sim_inject(&sim, f, 70);
    sim_corrupt_next(&sim);
    before = n_rx;
    run(&w, 30);
    CHECK(w.resyncs == 1, "bad length reopens the socket");
    CHECK(sim.sreg[0][0x03] == 0x42, "socket back in MACRAW");
    mkframe(f, 64, 4);
    sim_inject(&sim, f, 64);
    run(&w, 20);
    CHECK(n_rx == before + 1 && memcmp(rx_store[(n_rx - 1) % 64], f, 64) == 0, "traffic resumes after the resync");

    /* --- someone else holds the bus ---------------------------------------- */
    spidev_t other;
    spibus_dev_init(&other, bus, 9, 1000000, 0, "other");
    CHECK(spibus_try_claim(&other), "other device takes the bus");
    mkframe(f, 64, 5);
    sim_inject(&sim, f, 64);
    before = n_rx;
    uint32_t busy0 = w.bus_busy;
    run(&w, 20);
    CHECK(n_rx == before && w.bus_busy > busy0, "no progress while the bus is held, and it is counted");
    spibus_release(&other);
    run(&w, 20);
    CHECK(n_rx == before + 1, "then the frame arrives");

    /* --- SENDOK that never comes -> timeout + reopen ------------------------ */
    sim.defer_sendok = true;
    queue_tx(t2, 90);
    run(&w, 150);
    CHECK(w.tx_timeouts == 1 && !w.send_busy, "a lost SENDOK times out and the socket is reopened");
    sim.defer_sendok = false;
    queue_tx(t1, 200);
    int s0 = sim.n_sent;
    run(&w, 20);
    CHECK(sim.n_sent == s0 + 1, "and sending resumes");

    printf("%d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
