/*
 * Sharing one SPI bus: an SD card in the middle of a format and the devices
 * that must keep working beside it (docs/spi-bus-sharing.md).
 *
 * Part A pins down the bus layer's fairness on its own: a device refused the
 * bus is in line, the holder that lets go cannot take it straight back, an
 * interrupt-time want counts, wants lapse, and a lone device sees no change.
 *
 * Part B runs a format-shaped SD workload -- 8- and 64-block zero fills, the
 * single-sector FAT writes, 64-block reads -- through src/sd/sd_card.c and
 * src/sd/sd_spi.c on a simulated card, with two more devices on the bus:
 *   eth: W5500-like, a job every 500 us of three small transactions, tried
 *        from every main-loop pass (as w5500_step does);
 *   can: MCP2515-like, an interrupt at irregular times (spibus_want_isr from
 *        the "ISR", possibly while the SD worker is mid-slice), then a burst
 *        of two transactions from the loop.
 * Time is the card simulator's, advanced by every byte at its device's clock
 * (SD 25 MHz, eth 30 MHz, can 10 MHz). Asserted: (a) no request waits longer
 * than the spec bound, (b) every job completes while the format runs, (c)
 * never two chip selects low nor a card driving MISO beside another device,
 * (d) the card holds exactly what was written.
 *
 *   gcc -DSPIBUS_HOST -DCONFIG_SD_DETECT=22 -Isrc -Itest/host test/host/spibus_share_test.c \
 *       src/sd/sd_card.c src/sd/sd_spi.c src/sd/sd_sdio.c src/sd/sd_crc.c src/io/spibus.c \
 *       test/host/fake_spi.c test/host/sdcard_sim.c test/host/sdio_hw_sim.c test/host/sd_port_host.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fake_spi.h"
#include "io/spibus.h"
#include "sd/sd_card.h"
#include "sdcard_sim.h"

static int checks, failed;
#define CHECK(c, msg) do { checks++; if (!(c)) { failed++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, msg); } } while (0)

extern void (*sd_host_yield_hook)(void);
extern sdsim_t *sd_host_detect_card;

static uint8_t echo(void *ctx, int ev, uint8_t mosi) { (void)ctx; return ev == FAKE_BYTE ? (uint8_t)~mosi : 0; }

/* ---------------------------------------------------------------- part A */

static void part_a(void) {
    fake_reset();
    spibus_t *b = spibus_init(0, 2, 3, 4, "a");
    spidev_t x, y, z;
    spibus_dev_init(&x, b, 7, 25000000, 0, "x");
    spibus_dev_init(&y, b, 13, 30000000, 0, "y");
    spibus_dev_init(&z, b, 12, 10000000, 0, "z");
    fake_attach(7, echo, NULL); fake_attach(13, echo, NULL); fake_attach(12, echo, NULL);

    /* A lone device: release and reclaim as before. */
    CHECK(spibus_try_claim(&x), "x claims a free bus");
    CHECK(!spibus_contended(&x), "nobody waits");
    spibus_release(&x);
    CHECK(spibus_try_claim(&x), "with nobody waiting, the releaser reclaims at once");
    CHECK(b->claim_yield == 0, "no fairness refusal without a waiter");

    /* The handoff: y is refused while x holds; x lets go and wants it back. */
    CHECK(!spibus_try_claim(&y), "y is refused while x holds the bus");
    CHECK(spibus_contended(&x), "x sees y waiting");
    spibus_release(&x);
    CHECK(!spibus_try_claim(&x), "x cannot take the bus back while y waits");
    CHECK(b->claim_yield == 1, "the refusal is counted as a yield");
    CHECK(spibus_try_claim(&y), "y gets its turn");
    CHECK(spibus_contended(&y), "x, refused, is now in line behind y");
    spibus_release(&y);
    CHECK(!spibus_try_claim(&y), "...and y cannot take it straight back either");
    CHECK(spibus_try_claim(&x), "x's turn");
    spibus_release(&x);
    CHECK(spibus_try_claim(&y), "with the line empty, y claims");
    spibus_release(&y);

    /* Ticket order: z asks first, then y; y cannot jump the line. */
    CHECK(spibus_try_claim(&x), "x holds");
    CHECK(!spibus_try_claim(&z), "z waits (first)");
    CHECK(!spibus_try_claim(&y), "y waits (second)");
    spibus_release(&x);
    CHECK(!spibus_try_claim(&y), "y may not pass z");
    CHECK(spibus_try_claim(&z), "z first");
    spibus_release(&z);
    CHECK(spibus_try_claim(&y), "then y");
    spibus_release(&y);

    /* An interrupt-time want: no claim attempted yet, but the holder sees it. */
    CHECK(spibus_try_claim(&x), "x holds");
    spibus_want_isr(&z);
    CHECK(spibus_contended(&x), "an interrupt's want makes the bus contended");
    spibus_release(&x);
    CHECK(!spibus_try_claim(&x), "and keeps x from reclaiming");
    CHECK(spibus_try_claim(&z), "z, interrupted, gets the bus");
    spibus_release(&z);
    CHECK(spibus_try_claim(&x), "x, still in line from its refusal, is next");
    spibus_release(&x);

    /* A want that is never followed up lapses. */
    spibus_want(&y);
    CHECK(!spibus_try_claim(&x), "a fresh want is honoured");
    fake_advance_ms(SPIBUS_WANT_TTL_MS + 1);
    /* x also has a ticket now (it was refused); y's is the older but stale. */
    CHECK(spibus_try_claim(&x), "a want not renewed within the TTL lapses");
    CHECK(!spibus_contended(&x), "nothing left waiting");
    spibus_release(&x);
    spibus_want(&y);
    spibus_unwant(&y);
    CHECK(spibus_try_claim(&x), "an unwant withdraws it");
    spibus_release(&x);

    /* A device torn down leaves no ticket behind. */
    CHECK(spibus_try_claim(&x), "x holds");
    CHECK(!spibus_try_claim(&z), "z waits");
    spibus_release(&x);
    spibus_dev_deinit(&z);
    CHECK(spibus_try_claim(&x), "a deinit'ed device's place in line is gone");
    spibus_release(&x);

    CHECK(fake_cs_overlaps() == 0, "never two chip selects low");
    spibus_dev_deinit(&x);
    spibus_dev_deinit(&y);
    spibus_deinit(b);
}

/* ---------------------------------------------------------------- part B */

#define SECTORS   (65536u)            /* a 32 MB card: room for the workload */
#define BOUND_US  1000u               /* the spec's bound at SD 25 MHz (docs/spi-bus-sharing.md) */

static sdsim_t card;
static uint8_t *shadow;               /* what the card should hold */
static spibus_t *bus;

/* Time: bytes on the wire move the card simulator's clock; the bus layer's
 * millisecond clock (wants' TTL) follows it. */
static uint64_t ms_synced_us;
static void sync_clock(void) {
    uint64_t now = sdsim_now_us();
    if (now > ms_synced_us) { spibus_port_delay_us((uint32_t)(now - ms_synced_us)); ms_synced_us = now; }
}
static uint64_t frac_ns;
static void on_bytes(spibus_t *b, uint16_t n) {
    uint32_t baud = fake_baud(b->index);
    if (!baud) { baud = 1000000; }
    frac_ns += (uint64_t)n * 8u * 1000000000u / baud;
    sdsim_advance_us(frac_ns / 1000u);
    frac_ns %= 1000u;
    sync_clock();
}

/* A device beside the card: jobs of `ntx` transactions of `len` bytes. */
typedef struct {
    spidev_t dev;
    const char *name;
    int      ntx, len;
    uint64_t due;          /* request time of the transaction it is waiting for (0: idle) */
    int      left;         /* transactions left in the job */
    uint64_t max_wait, sum_wait;
    uint32_t grants, jobs_started, jobs_done;
    uint32_t over;         /* waits past the bound */
} peer_t;

static peer_t eth, can;
static uint64_t eth_next;              /* eth: its next job's time */
static uint64_t can_next;              /* can: its next interrupt */
static uint32_t lcg = 12345;
static uint32_t rnd(uint32_t n) { lcg = lcg * 1103515245u + 12345u; return (lcg >> 8) % n; }

static void peer_start(peer_t *p, uint64_t at) {
    if (p->left) { return; }           /* still busy with the last one: it coalesces */
    p->left = p->ntx;
    p->due = at;
    p->jobs_started++;
}

static void peer_step(peer_t *p) {
    while (p->left) {
        if (!spibus_try_claim(&p->dev)) { return; }
        uint64_t w = sdsim_now_us() - p->due;
        if (w > p->max_wait) { p->max_wait = w; }
        if (w > BOUND_US) { p->over++; }
        p->sum_wait += w;
        p->grants++;
        uint8_t tx[32], rx[32];
        memset(tx, 0x5A, sizeof tx);
        spiseg_t s = { tx, rx, (uint16_t)p->len };
        spibus_xfer_blocking(&p->dev, &s, 1);
        CHECK(rx[0] == (uint8_t)~0x5A, "a peer's transfer reads its own device");
        spibus_release(&p->dev);
        if (--p->left == 0) { p->jobs_done++; p->due = 0; }
        else { p->due = sdsim_now_us(); }   /* the next transaction asks now */
    }
}

/* Interrupts can come at any time, even mid-slice: checked on every byte
 * burst and every loop pass. */
static void irqs(void) {
    uint64_t now = sdsim_now_us();
    if (now >= can_next) {
        peer_start(&can, can_next);
        spibus_want_isr(&can.dev);
        can_next = now + 300 + rnd(1700);
    }
}

static bool running;
static void loop_pass(void) {
    sync_clock();
    spibus_poll();
    if (!running) { return; }
    irqs();
    uint64_t now = sdsim_now_us();
    if (now >= eth_next) { peer_start(&eth, eth_next); eth_next = now + 500; }
    peer_step(&eth);
    peer_step(&can);
}

static void on_bytes_irq(spibus_t *b, uint16_t n) {
    on_bytes(b, n);
    if (running) { irqs(); }
}

static void peer_init(peer_t *p, const char *name, int cs, uint32_t baud, int ntx, int len) {
    memset(p, 0, sizeof *p);
    p->name = name; p->ntx = ntx; p->len = len;
    spibus_dev_init(&p->dev, bus, cs, baud, 0, name);
    fake_attach(cs, echo, NULL);
}

static void report(const peer_t *p) {
    printf("  %s: %u jobs, %u grants, max wait %llu us, mean %llu us, %u over %u us\n",
           p->name, p->jobs_done, p->grants, (unsigned long long)p->max_wait,
           (unsigned long long)(p->grants ? p->sum_wait / p->grants : 0), p->over, BOUND_US);
}

/* One format-shaped run. busy_us: the card's programming time per block
 * (short = a card that buffers writes, which never makes the driver wait). */
static void part_b(uint32_t busy_us) {
    fake_reset();
    fake_autocomplete(true);
    fake_bytes_hook = on_bytes_irq;
    sd_host_yield_hook = loop_pass;
    running = false;

    sdsim_free(&card);
    sdsim_init(&card, SECTORS, true);
    card.write_busy_us = busy_us;
    sd_host_detect_card = &card;
    free(shadow);
    shadow = calloc(SECTORS, 512);
    memcpy(shadow, card.img, (size_t)SECTORS * 512);

    /* The card alone on SPI0 in the default wiring; here it shares the bus
     * with both peers, as it would on SPI1 beside the W5500. */
    sdsim_attach_spi(&card, 9);          /* sd_spi.c's CS without CONFIG */
    sd_card_use(sd_bus_spi());
    memset(sd_card_stats(), 0, sizeof *sd_card_stats());
    CHECK(sd_card_init() == SD_OK, "the card identifies");
    bus = spibus_get(1);                 /* sd_spi.c's bus without CONFIG: SPI1 */
    CHECK(bus != NULL, "the SD bus is up");
    if (!bus) { return; }
    peer_init(&eth, "eth", 13, 30000000, 3, 24);
    peer_init(&can, "can", 12, 10000000, 2, 14);
    ms_synced_us = sdsim_now_us();
    eth_next = sdsim_now_us() + 100;
    can_next = sdsim_now_us() + 700;
    running = true;
    uint64_t t0 = sdsim_now_us();

    static uint8_t zeros[64 * 512];
    static uint8_t buf[64 * 512];
    int errs = 0;
    /* zero fill in 8-block pieces (f_mkfs's 4 KB work buffer), 1 MB */
    for (uint32_t lba = 4096; lba < 4096 + 2048; lba += 8) {
        errs += sd_card_write(lba, zeros, 8) != SD_OK;
        memset(shadow + (size_t)lba * 512, 0, 8 * 512);
    }
    /* and in 64-block pieces (a big upload, a bench), 1 MB */
    for (uint32_t lba = 8192; lba < 8192 + 2048; lba += 64) {
        errs += sd_card_write(lba, zeros, 64) != SD_OK;
        memset(shadow + (size_t)lba * 512, 0, 64 * 512);
    }
    /* the FAT and directory sectors, one at a time */
    for (int i = 0; i < 200; i++) {
        uint32_t lba = 2048 + (uint32_t)rnd(2048);
        for (int j = 0; j < 512; j++) { buf[j] = (uint8_t)(i * 7 + j); }
        errs += sd_card_write(lba, buf, 1) != SD_OK;
        memcpy(shadow + (size_t)lba * 512, buf, 512);
    }
    /* read it back in 64-block pieces (verify, a GET) */
    for (uint32_t lba = 8192; lba < 8192 + 2048; lba += 64) {
        errs += sd_card_read(lba, buf, 64) != SD_OK;
        errs += memcmp(buf, shadow + (size_t)lba * 512, 64 * 512) != 0;
    }
    errs += sd_card_sync() != SD_OK;
    uint64_t span = sdsim_now_us() - t0;
    running = false;
    /* let the peers finish what they had started */
    for (int i = 0; i < 100 && (eth.left || can.left); i++) {
        spibus_poll();
        peer_step(&eth);
        peer_step(&can);
    }

    printf("  busy %u us/block: workload %llu ms, sd write retries %u, crc errors %u, bus yields %u\n",
           busy_us, (unsigned long long)(span / 1000), sd_card_stats()->write_retries,
           sd_card_stats()->crc_errors, bus->claim_yield);
    report(&eth);
    report(&can);

    CHECK(errs == 0, "(d) every SD operation succeeds and reads back what was written");
    CHECK(memcmp(card.img, shadow, (size_t)SECTORS * 512) == 0, "(d) the card holds exactly what was written");
    CHECK(eth.jobs_done > 100 && eth.jobs_done == eth.jobs_started, "(b) eth's jobs all complete during the format");
    CHECK(can.jobs_done > 20 && can.jobs_done == can.jobs_started, "(b) can's interrupts are all serviced during the format");
    CHECK(eth.max_wait <= BOUND_US, "(a) eth never waits past the bound");
    CHECK(can.max_wait <= BOUND_US, "(a) can never waits past the bound");
    CHECK(fake_cs_overlaps() == 0, "(c) never two chip selects low");
    CHECK(fake_bus_conflicts() == 0, "(c) the card never drives MISO beside another device");

    spibus_dev_deinit(&eth.dev);
    spibus_dev_deinit(&can.dev);
    fake_bytes_hook = NULL;
}

int main(void) {
    part_a();
    part_b(50);        /* a card that buffers: no busy to wait out between blocks */
    part_b(300);       /* a card that programs each block: busy waits release the bus */
    sdsim_free(&card);
    free(shadow);
    printf("%d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
