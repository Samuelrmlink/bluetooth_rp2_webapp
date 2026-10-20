/*
 * Both SD buses -- src/sd/sd_sdio.c (SD mode) and src/sd/sd_spi.c (SPI mode
 * on the shared bus layer) -- through src/sd/sd_card.c, against the same
 * simulated card (sdcard_sim.c). Every scenario runs on both buses.
 *
 *   gcc -DSPIBUS_HOST -DCONFIG_SD_DETECT=22 -Isrc -Itest/host test/host/sd_card_test.c \
 *       src/sd/sd_card.c src/sd/sd_sdio.c src/sd/sd_spi.c src/sd/sd_crc.c src/io/spibus.c \
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
static const char *bus_name = "";
#define CHECK(c, msg) do { checks++; if (!(c)) { failed++; printf("FAIL [%s] %s:%d %s\n", bus_name, __FILE__, __LINE__, msg); } } while (0)

extern void (*sd_host_yield_hook)(void);
extern sdsim_t *sd_host_detect_card;

static sdsim_t card;
static bool spi;

/* The main loop, as far as the SPI driver is concerned: DMA completions are
 * delivered by spibus_poll(). And a second device on the same bus that tries
 * to get a turn whenever the SD code yields -- proving busy waits give the
 * bus up. */
static spidev_t other;
static int other_turns, other_tries;
static uint8_t echo(void *ctx, int ev, uint8_t mosi) { (void)ctx; return ev == FAKE_BYTE ? (uint8_t)~mosi : 0; }
static void loop_pass(void) {
    if (!spi) { return; }
    spibus_poll();
    other_tries++;
    if (spibus_try_claim(&other)) {
        uint8_t tx = 0x3C, rx = 0;
        spiseg_t s = { &tx, &rx, 1 };
        spibus_xfer_blocking(&other, &s, 1);
        if (rx == (uint8_t)~tx) { other_turns++; }
        spibus_release(&other);
    }
}

static void fresh(uint64_t sectors, bool sdhc) {
    sdsim_free(&card);
    sdsim_init(&card, sectors, sdhc);
    for (uint64_t i = 0; i < sectors * 512; i += 4096) { card.img[i] = (uint8_t)(i >> 12); }
    sd_host_detect_card = &card;
    if (spi) { sdsim_attach_spi(&card, 9); } else { sdsim_attach_sdio(&card); }
    sd_card_use(spi ? sd_bus_spi() : sd_bus_sdio());
    memset(sd_card_stats(), 0, sizeof *sd_card_stats());
}

static void pattern(uint8_t *b, uint32_t n, uint32_t seed) {
    for (uint32_t i = 0; i < n; i++) { b[i] = (uint8_t)(i * 31 + seed * 7 + (i >> 9)); }
}

static void scenarios(void) {
    static uint8_t buf[64 * 512], back[64 * 512];

    /* --- SDHC, identification ------------------------------------------- */
    fresh(131072, true);                                 /* 64 MiB */
    card.acmd41_busy = 5;
    CHECK(sd_card_init() == SD_OK, "SDHC identifies (after 5 busy ACMD41 polls)");
    const sd_info_t *in = sd_card_info();
    CHECK(in->valid && in->v2 && in->block_addr, "v2, block addressed");
    CHECK(in->sectors == 131072, "sector count from the CSD");
    CHECK(in->bus_width == (spi ? 1 : 4), "4-bit on SDIO, 1 on SPI");
    CHECK(in->crc, "data CRCs checked (SPI: CMD59 accepted)");
    sd_cid_t cid;
    sd_cid_decode(in->cid, &cid);
    CHECK(in->cid_ok && !strncmp(cid.name, "SIM01", 5) && cid.oem[0] == 'S' && cid.serial == 0x12345678 &&
          cid.year == 2025 && cid.month == 10, "CID decoded");

    /* --- reads and writes ------------------------------------------------ */
    CHECK(sd_card_read(0, back, 1) == SD_OK && back[0] == 0, "read LBA 0");
    CHECK(sd_card_read(8, back, 1) == SD_OK && back[0] == 1, "read LBA 8 (marker byte)");
    CHECK(sd_card_read(131071, back, 1) == SD_OK, "read the last sector");
    CHECK(sd_card_read(131071, back, 2) == SD_EPARAM, "past the end refused before the bus");
    int ok = 1;
    uint32_t sizes[] = { 1, 2, 8, 64 };
    uint64_t starts[] = { 0, 1, 777, 131072 - 64 };
    for (int si = 0; si < 4; si++) {
        for (int st = 0; st < 4; st++) {
            pattern(buf, sizes[si] * 512, (uint32_t)(si * 4 + st));
            if (sd_card_write(starts[st], buf, sizes[si]) != SD_OK) { ok = 0; printf("  write %u@%llu\n", sizes[si], (unsigned long long)starts[st]); }
            if (memcmp(card.img + starts[st] * 512, buf, sizes[si] * 512)) { ok = 0; printf("  image %u@%llu\n", sizes[si], (unsigned long long)starts[st]); }
            memset(back, 0, sizeof back);
            if (sd_card_read(starts[st], back, sizes[si]) != SD_OK || memcmp(back, buf, sizes[si] * 512)) {
                ok = 0; printf("  readback %u@%llu\n", sizes[si], (unsigned long long)starts[st]);
            }
        }
    }
    CHECK(ok, "1/2/8/64-block writes land in the image and read back, at 0, 1, 777 and the end");
    CHECK(sd_card_sync() == SD_OK, "sync");
    CHECK(sd_card_stats()->read_retries == 0 && sd_card_stats()->write_retries == 0, "no retries on a clean card");

    /* --- faults ---------------------------------------------------------- */
    card.crc_read_lba = 100;
    sd_card_stats()->read_retries = 0;
    CHECK(sd_card_read(99, back, 4) == SD_OK && !memcmp(back + 512, card.img + 100 * 512, 512),
          "a CRC error on a read block is retried and the data is right");
    CHECK(sd_card_stats()->read_retries == 1 && sd_card_stats()->crc_errors >= 1, "counted as a CRC retry");

    card.crc_write_lba = 200;
    sd_card_stats()->write_retries = 0;
    pattern(buf, 3 * 512, 99);
    CHECK(sd_card_write(199, buf, 3) == SD_OK && !memcmp(card.img + 199 * 512, buf, 3 * 512),
          "a CRC-status error on a write is retried and lands");
    CHECK(sd_card_stats()->write_retries == 1, "one write retry");

    card.werr_lba = 300;
    pattern(buf, 512, 5);
    CHECK(sd_card_write(300, buf, 1) == SD_OK && !memcmp(card.img + 300 * 512, buf, 512), "a write error is retried");

    if (!spi) {
        /* A card whose busy a host can miss (one look at D0, unclocked): the
         * next write must still wait for the transfer state (CMD13), or the
         * card ignores it as illegal while programming. */
        card.busy_hidden = true;
        card.write_busy_us = 5000;
        pattern(buf, 512, 41);
        uint32_t wr0 = sd_card_stats()->write_retries;
        CHECK(sd_card_write(500, buf, 1) == SD_OK && sd_card_write(501, buf, 1) == SD_OK &&
              !memcmp(card.img + 501 * 512, buf, 512), "back-to-back writes wait out a busy D0 did not show");
        CHECK(sd_card_stats()->write_retries == wr0, "... without needing a retry (the retry hid it)");
        /* A CMD24 the card took but whose answer arrived garbled: the card
         * waits for data; it must be stopped, or every later write is ignored. */
        card.garble_resp_cmd = 24;
        CHECK(sd_card_write(502, buf, 1) == SD_OK, "a garbled CMD24 answer is recovered (retry)");
        pattern(buf, 512, 43);
        CHECK(sd_card_write(503, buf, 1) == SD_OK && sd_card_write(504, buf, 1) == SD_OK &&
              !memcmp(card.img + 504 * 512, buf, 512), "and later writes are not ignored");
        card.garble_resp_cmd = 17;
        CHECK(sd_card_read(503, back, 1) == SD_OK && !memcmp(back, card.img + 503 * 512, 512),
              "a garbled CMD17 answer is recovered too");
        card.busy_hidden = false;
        card.write_busy_us = 300;
    }

    {
        /* Recovery (the hardware case of 2026-10-19: every read failing its
         * CRC until the card was reseated). A wedged card: the read that
         * hits it re-identifies the card and succeeds, at full speed. On SPI
         * this is what CRC being on buys: with it off the bad data went
         * through unseen. */
        uint32_t rec0 = sd_card_stats()->recoveries, full = sd_card_info()->clock_khz;
        card.crc_until_reset = true;
        CHECK(sd_card_read(600, back, 4) == SD_OK && !memcmp(back, card.img + 600 * 512, 2048),
              "a wedged card is re-identified and the same read succeeds");
        CHECK(sd_card_stats()->recoveries == rec0 + 1 && sd_card_info()->clock_khz == full,
              "one recovery, no slowdown");
    }
    if (!spi) {
        uint32_t full = sd_card_info()->clock_khz;
        /* A marginal contact above 10 MHz: recoveries step the clock down
         * until the data comes through, and the data is right. */
        card.marginal_khz = 10000;
        int ok_at = -1;
        for (int i = 0; i < 6 && ok_at < 0; i++) {
            if (sd_card_read(610, back, 2) == SD_OK) { ok_at = i; }
        }
        CHECK(ok_at >= 0 && !memcmp(back, card.img + 610 * 512, 1024), "a marginal contact ends up readable");
        CHECK(sd_card_info()->clock_khz <= 10000 && sd_card_info()->clock_khz >= 3000,
              "... at a lower clock, but not lower than needed");
        pattern(buf, 1024, 77);
        CHECK(sd_card_write(612, buf, 2) == SD_OK && !memcmp(card.img + 612 * 512, buf, 1024), "and writes work there");
        /* Taking the card out puts the speed back. */
        card.marginal_khz = 0;
        card.present = false;
        sd_card_present();
        sdsim_insert(&card);
        CHECK(sd_card_init() == SD_OK && sd_card_info()->clock_khz == full, "a reinserted card gets full speed again");
    }

    card.write_busy_us = 1500000;                       /* a card garbage-collecting */
    other_turns = 0;
    CHECK(sd_card_write(400, buf, 1) == SD_OK && sd_card_sync() == SD_OK, "a 1.5 s write busy is waited out");
    CHECK(sd_card_stats()->busy_max_us >= 1400000, "and recorded as the longest busy");
    if (spi) { CHECK(other_turns > 10, "the other SPI device got the bus during the busy wait"); }
    card.write_busy_us = 3000000;
    CHECK(sd_card_write(401, buf, 1) != SD_OK, "a 3 s busy is a failure");
    CHECK(sd_card_stats()->busy_timeouts >= 1, "counted");
    card.write_busy_us = 300;
    sdsim_advance_us(4000000);

    card.mute_cmd = 17;
    CHECK(sd_card_read(5, back, 1) == SD_ETIMEOUT || sd_card_read(5, back, 1) == SD_EIO, "a command that never answers fails");
    card.mute_cmd = -1;
    CHECK(sd_card_read(5, back, 1) == SD_OK, "and the card works again after");

    /* pulled mid-read, then reinserted */
    card.pull_at_lba = 50;
    CHECK(sd_card_read(48, back, 4) != SD_OK, "pulled mid-read: the read fails");
    CHECK(!sd_card_present() && !sd_card_ready(), "card detect says gone, and the card is forgotten");
    CHECK(sd_card_read(0, back, 1) == SD_ENOCARD, "nothing reaches the bus while it is out");
    sdsim_insert(&card);
    { int r = sd_card_init(); if (r) printf("  reinit %d\n", r); }
    CHECK(sd_card_present() && sd_card_ready() && sd_card_read(8, back, 1) == SD_OK &&
          !memcmp(back, card.img + 8 * 512, 512),
          "reinserted: identified again and readable");

    /* --- SDSC (byte addressing) and v1 ---------------------------------- */
    fresh(65536, false);                                 /* 32 MiB SDSC */
    CHECK(sd_card_init() == SD_OK && !sd_card_info()->block_addr, "SDSC: byte addressed");
    CHECK(sd_card_info()->sectors == 65536, "CSD 1.0 size");
    int n0 = card.ntrace;
    pattern(buf, 2 * 512, 3);
    CHECK(sd_card_write(10, buf, 2) == SD_OK && !memcmp(card.img + 10 * 512, buf, 1024), "SDSC write lands");
    bool byte_arg = false;
    for (int i = n0; i < card.ntrace; i++) { if (card.trace[i].cmd == 25 && card.trace[i].arg == 10 * 512) { byte_arg = true; } }
    CHECK(byte_arg, "CMD25 carried a byte address (10 * 512)");

    fresh(65536, false);
    card.v1 = true;
    CHECK(sd_card_init() == SD_OK && !sd_card_info()->v2, "v1 card (no CMD8 answer) identifies");
    CHECK(sd_card_read(8, back, 1) == SD_OK && back[0] == 1, "and reads");

    if (spi) {
        fresh(131072, true);
        card.refuse_cmd59 = true;
        CHECK(sd_card_init() == SD_OK && !sd_card_info()->crc, "an old card that refuses CMD59 runs without CRC");
        CHECK(sd_card_read(8, back, 1) == SD_OK && back[0] == 1, "and reads");
    }

    fresh(131072, true);
    card.present = false;
    CHECK(sd_card_init() == SD_ENOCARD, "no card: ENOCARD without touching the bus");

    fresh(131072, true);
    card.mute_cmd = 41;
    CHECK(sd_card_init() != SD_OK, "a card that never finishes ACMD41 is not identified");
}

int main(void) {
    fake_reset();
    fake_autocomplete(true);
    sd_host_yield_hook = loop_pass;

    bus_name = "sdio";
    spi = false;
    scenarios();

    bus_name = "spi";
    spi = true;
    /* the W5500's place on SPI1, sharing it */
    spibus_t *b = spibus_init(1, 14, 15, 8, "w5500");
    spibus_dev_init(&other, b, 13, 30000000, 0, "w5500");
    fake_attach(13, echo, NULL);
    scenarios();
    CHECK(fake_bus_conflicts() == 0, "no MISO conflict on the shared bus, ever");
    CHECK(b->users == 2, "the SD card shares the W5500's bus (refcounted)");

    sdsim_free(&card);
    printf("%d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
