/*
 * SD card in SPI mode, on the shared SPI bus layer (src/io/spibus.c).
 *
 * The framing is km003c_rp2_webapp's (src/log/km_sd.c, bench-proven):
 * CMD0 -> CMD8 -> ACMD41 (HCS) -> CMD58 (CCS) -> CMD16 for SDSC -> CMD9/CMD10,
 * then CMD17/18 + CMD12 reads and CMD24/25 (+ ACMD23) writes, a stuck card
 * recovered with a stop token and a raw CMD12 before CMD0 is retried.
 *
 * What changes is the bus: it may be shared (the W5500 on SPI1), so
 *  - every transaction claims the bus semaphore, and ends with
 *    spibus_release_trailing(): CS high plus one clock, or the card would
 *    keep driving MISO into the next device's read;
 *  - a busy wait releases the bus between polls (a card keeps programming
 *    with CS high; SD spec 7.2.4), so a slow write never starves the W5500;
 *  - data blocks move by the bus layer's DMA while the worker yields.
 * The claim is held across a read's wait for its data token (the card is
 * mid-command then; at most SD_READ_TIMEOUT_US, typically under 1 ms).
 *
 * Fair to the other device (docs/spi-bus-sharing.md): at every safe point
 * -- between commands, between the blocks of a multi-block transfer, during
 * a card's busy -- a device waiting for the bus (spibus_contended) gets it
 * before the next block: a multi-block write lets go while the card
 * programs, a multi-block read is stopped (CMD12) and resumed with a new
 * CMD18. So a format's long zero-fill holds the bus one block at a time.
 *
 * CRC is ON (CMD59 right after CMD0): every command carries its CRC7, every
 * block written its CRC16, and every block read -- CSD and CID included --
 * is checked against its CRC16, as on SDIO. km003c's driver left it off; with
 * it off a bad contact returns wrong data silently and recovery never sees an
 * error. A card that refuses CMD59 (only the oldest do) runs without it.
 */
#include "sd/sd_card.h"

#include <string.h>

#include "autoconf.h"
#include "io/spibus.h"
#include "sd/sd_crc.h"
#include "sd/sd_port.h"

#ifndef CONFIG_SD_SPI_BUS
#define CONFIG_SD_SPI_BUS 1
#define CONFIG_SD_SPI_SCK 14
#define CONFIG_SD_SPI_MOSI 15
#define CONFIG_SD_SPI_MISO 8
#define CONFIG_SD_SPI_CS 9
#endif
#ifndef CONFIG_SD_SPI_KHZ
#define CONFIG_SD_SPI_KHZ 25000
#endif

#define ACMD(n) (0x80 | (n))

static spidev_t dev;
static bool block_addr;
static bool crc_on;              /* CMD59 accepted: data CRC16s are checked and sent */

/* --- bus access ---------------------------------------------------------- */

static void claim(void) {
    while (!spibus_try_claim(&dev)) { sd_port_yield(); }
}

static void release(void) {
    spibus_release_trailing(&dev, 1);
}

/* Give the main loop its turn with the bus let go (CS high, the trailing
 * clock sent): the bus may be shared, and a turn spent holding it is a turn
 * the other device cannot have. Only where the card is between commands. */
static void breathe(void) {
    release();
    sd_port_yield();
    claim();
}

/* The same in the middle of a transfer, where the card stays selected: CS
 * low again once the bus is back (a card ignores CS while it programs). */
static void breathe_selected(void) {
    breathe();
    spibus_port_gpio_put(dev.cs, false);
}

/* A safe point: someone else is waiting, or this slice is spent. */
static bool give_way(void) {
    return spibus_contended(&dev) || sd_port_slice_spent();
}

/* Short exchanges: a blocking transfer of a few bytes (microseconds). */
static void xfer(const uint8_t *tx, uint8_t *rx, uint16_t n) {
    spiseg_t s = { tx, rx, n };
    spibus_xfer_blocking(&dev, &s, 1);
}

static uint8_t xchg(uint8_t b) {
    uint8_t r = 0xFF;
    xfer(&b, &r, 1);
    return r;
}

/* A data block: DMA in the background, the worker yields meanwhile. */
static volatile bool blk_done;
static void blk_cb(void *ctx, bool ok) { (void)ctx; (void)ok; blk_done = true; }
static bool block(const uint8_t *tx, uint8_t *rx, uint16_t n) {
    spiseg_t s = { tx, rx, n };
    blk_done = false;
    if (!spibus_xfer_async(&dev, &s, 1, blk_cb, NULL)) { return false; }
    uint64_t t0 = sd_port_now_us();
    while (!blk_done) {
        if (sd_port_now_us() - t0 > 50000u) { return false; }   /* 512 B take ~165 us */
        sd_port_yield();                                       /* spibus_poll() runs in the loop */
    }
    sd_card_stats()->dma_blocks++;
    return true;
}

/* Wait for DO = 0xFF (not busy). The bus is released between polls once the
 * first SD_SPIN_US have passed; CS is low again before each poll. */
static bool wait_ready(uint32_t timeout_us) {
    sd_stats_t *st = sd_card_stats();
    uint64_t t0 = sd_port_now_us();
    for (;;) {
        if (xchg(0xFF) == 0xFF) {
            uint32_t dt = (uint32_t)(sd_port_now_us() - t0);
            if (dt > st->busy_max_us) { st->busy_max_us = dt; }
            return true;
        }
        uint64_t dt = sd_port_now_us() - t0;
        if (dt >= timeout_us) { break; }
        if (dt > SD_SPIN_US || spibus_contended(&dev)) {
            breathe_selected();              /* the other device's turn */
        }
    }
    if (timeout_us > st->busy_max_us) { st->busy_max_us = timeout_us; }
    st->busy_timeouts++;
    return false;
}

static void deselect(void) {
    spibus_port_gpio_put(dev.cs, true);
    xchg(0xFF);                              /* the clock that frees DO */
}

/* CS low and the card ready for a command. */
static bool select_card(void) {
    spibus_port_gpio_put(dev.cs, false);
    xchg(0xFF);
    if (wait_ready(SD_BUSY_TIMEOUT_US)) { return true; }
    deselect();
    return false;
}

/* Send a command (claim held); R1, or 0xFF for no answer. */
static uint8_t send_cmd(uint8_t cmd, uint32_t arg) {
    if (cmd & 0x80) {                        /* ACMD = CMD55 first */
        cmd &= 0x7F;
        uint8_t r = send_cmd(55, 0);
        if (r > 1) { return r; }
    }
    if (cmd != 12) {
        deselect();
        /* Commands at 400 kHz are ~0.4 ms of blocking bytes each:
         * identification would run a dozen back to back. */
        if (give_way()) { breathe(); }
        if (!select_card()) { return 0xFF; }
    }
    uint8_t b[6] = { (uint8_t)(0x40 | cmd), (uint8_t)(arg >> 24), (uint8_t)(arg >> 16),
                     (uint8_t)(arg >> 8), (uint8_t)arg, 0 };
    b[5] = (uint8_t)((sd_crc7(b, 5) << 1) | 1u);   /* every command: CRC is on (CMD59) */
    xfer(b, NULL, 6);
    if (cmd == 12) { xchg(0xFF); }           /* stuff byte */
    uint8_t r;
    int n = 10;
    do { r = xchg(0xFF); } while ((r & 0x80) && --n);
    return r;
}

static int rx_block(uint8_t *buf, uint16_t len) {
    uint64_t t0 = sd_port_now_us();
    uint8_t tok;
    do {
        sd_port_wait_step(t0);
        tok = xchg(0xFF);
    } while (tok == 0xFF && sd_port_now_us() - t0 < SD_READ_TIMEOUT_US);
    if (tok == 0xFF) { return SD_ETIMEOUT; }
    if (tok != 0xFE) { return SD_EIO; }      /* a data error token */
    if (len >= 64 ? !block(NULL, buf, len) : (xfer(NULL, buf, len), false)) { return SD_EIO; }
    uint8_t c[2];
    xfer(NULL, c, 2);                        /* the block's CRC16 */
    if (crc_on && (uint16_t)(c[0] << 8 | c[1]) != sd_crc16(buf, len)) { return SD_ECRC; }
    return SD_OK;
}

static int tx_block(const uint8_t *buf, uint8_t token) {
    if (!wait_ready(SD_BUSY_TIMEOUT_US)) { return SD_ETIMEOUT; }
    xchg(token);
    if (token == 0xFD) { return SD_OK; }     /* stop-tran: no data */
    if (!block(buf, NULL, 512)) { return SD_EIO; }
    uint16_t crc = sd_crc16(buf, 512);
    uint8_t c[2] = { (uint8_t)(crc >> 8), (uint8_t)crc };
    xfer(c, NULL, 2);
    uint8_t resp = xchg(0xFF) & 0x1F;
    if (resp == 0x05) { return SD_OK; }      /* accepted */
    return resp == 0x0B ? SD_ECRC : SD_EIO;
}

/* A card left mid-transfer by a board reset ignores CMD0: end a write with a
 * stop token, a read with a raw CMD12, wait out busy (km_sd.c, bench
 * 2026-10-17). */
static void recover_card(void) {
    static const uint8_t ff[20] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    static const uint8_t cmd12[6] = { 0x40 | 12, 0, 0, 0, 0, 0x61 };
    spiseg_t idle = { ff, NULL, 20 };
    spibus_xfer_blocking_cs(&dev, &idle, 1, 1u);
    spibus_port_gpio_put(dev.cs, false);
    xchg(0xFD);
    xchg(0xFF);
    wait_ready(600000);
    sd_port_yield();
    xfer(cmd12, NULL, 6);
    xfer(ff, NULL, 16);
    wait_ready(600000);
    spibus_xfer_blocking_cs(&dev, &idle, 1, 1u);
}

static bool go_idle(void) {
    static const uint8_t cmd0[6] = { 0x40, 0, 0, 0, 0, 0x95 };
    for (int attempt = 0; attempt < 10; attempt++) {
        spibus_port_gpio_put(dev.cs, false);
        xchg(0xFF);
        xfer(cmd0, NULL, 6);
        uint8_t r = 0xFF;
        for (int n = 0; n < 10 && (r & 0x80); n++) { r = xchg(0xFF); }
        deselect();
        if (r == 0x01) { return true; }
        /* At 400 kHz each try is ~0.4 ms of blocking bytes; ten of them
         * (and recovery) in one slice made a 12 ms worker slice in mcusim
         * with no card answering. The card is mid-nothing here: yield,
         * with the bus let go. */
        breathe();
    }
    return false;
}

/* --- sd_bus_t ----------------------------------------------------------- */

static bool hw_init(void) {
    spibus_t *b = spibus_init(CONFIG_SD_SPI_BUS, CONFIG_SD_SPI_SCK, CONFIG_SD_SPI_MOSI,
                              CONFIG_SD_SPI_MISO, "sd");
    if (!b) { return false; }
    if (!spibus_dev_init(&dev, b, CONFIG_SD_SPI_CS, 400000, 0, "sd")) { return false; }
    dev.fill = 0xFF;
    return true;
}

static void set_baud(uint32_t hz) {
    dev.baud = hz;                           /* applied by the next claim */
}

/* Between ACMD41 polls: the card powers up for up to a second, and a
 * shared bus must not be held all that time. */
static void acmd41_pause(uint64_t t0) {
    deselect();
    if (sd_port_now_us() - t0 > SD_SPIN_US || sd_port_slice_spent()) { breathe(); }
}

static int identify(sd_info_t *info) {
    set_baud(400000);
    claim();
    static const uint8_t ff[10] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    spiseg_t power = { ff, NULL, 10 };
    spibus_xfer_blocking_cs(&dev, &power, 1, 1u);   /* >= 74 clocks with CS high */

    int result = SD_ENOCARD;
    bool idle = go_idle();
    if (!idle) {
        recover_card();
        idle = go_idle();
    }
    block_addr = false;
    crc_on = false;
    if (idle) {
        crc_on = send_cmd(59, 1) == 0x01;    /* CRC on; a card that refuses runs without */
        uint64_t t0 = sd_port_now_us();
        if (send_cmd(8, 0x1AA) == 1) {                     /* v2 */
            uint8_t r7[4];
            xfer(NULL, r7, 4);
            info->v2 = true;
            if (r7[2] == 0x01 && r7[3] == 0xAA) {
                while (sd_port_now_us() - t0 < SD_INIT_TIMEOUT_US && send_cmd(ACMD(41), 1u << 30)) {
                    acmd41_pause(t0);
                }
                result = SD_ETIMEOUT;
                if (sd_port_now_us() - t0 < SD_INIT_TIMEOUT_US && send_cmd(58, 0) == 0) {
                    uint8_t ocr[4];
                    xfer(NULL, ocr, 4);
                    info->ocr = ((uint32_t)ocr[0] << 24) | ((uint32_t)ocr[1] << 16) |
                                ((uint32_t)ocr[2] << 8) | ocr[3];
                    block_addr = (ocr[0] & 0x40) != 0;
                    result = SD_OK;
                }
            } else {
                result = SD_EIO;
            }
        } else {                                           /* v1: byte addressed */
            while (sd_port_now_us() - t0 < SD_INIT_TIMEOUT_US && send_cmd(ACMD(41), 0)) {
                acmd41_pause(t0);
            }
            result = SD_ETIMEOUT;
            if (sd_port_now_us() - t0 < SD_INIT_TIMEOUT_US && send_cmd(16, 512) == 0) { result = SD_OK; }
        }
    }
    if (result == SD_OK) {
        if (send_cmd(9, 0) != 0 || rx_block(info->csd, 16) != SD_OK) { result = SD_EIO; }
        info->cid_ok = send_cmd(10, 0) == 0 && rx_block(info->cid, 16) == SD_OK;
    }
    deselect();
    release();
    info->block_addr = block_addr;
    info->crc = crc_on;
    info->bus_width = 1;
    uint32_t lim = sd_card_clock_limit_khz();      /* lowered by recovery */
    uint32_t khz = lim && lim < CONFIG_SD_SPI_KHZ ? lim : CONFIG_SD_SPI_KHZ;
    info->clock_khz = khz;
    if (result == SD_OK) { set_baud(khz * 1000u); }
    return result;
}

static int sdspi_read(uint32_t addr, uint8_t *buf, uint32_t count) {
    claim();
    int e;
    if (count == 1) {
        e = send_cmd(17, addr) == 0 ? rx_block(buf, 512) : SD_EIO;
    } else {
        e = send_cmd(18, addr) == 0 ? SD_OK : SD_EIO;
        while (e == SD_OK && count--) {
            e = rx_block(buf, 512);
            buf += 512;
            addr += block_addr ? 1u : 512u;
            if (e == SD_OK && count && spibus_contended(&dev)) {
                /* Another device waits: stop here and resume after its
                 * turn (the card streams blocks for as long as CS is low). */
                send_cmd(12, 0);
                deselect();
                breathe();
                e = send_cmd(18, addr) == 0 ? SD_OK : SD_EIO;
            }
        }
        send_cmd(12, 0);
    }
    deselect();
    release();
    return e;
}

static int sdspi_write(uint32_t addr, const uint8_t *buf, uint32_t count) {
    claim();
    int e;
    if (count == 1) {
        e = send_cmd(24, addr) == 0 ? tx_block(buf, 0xFE) : SD_EIO;
    } else {
        send_cmd(ACMD(23), count);           /* pre-erase hint; ignored if unsupported */
        e = send_cmd(25, addr) == 0 ? SD_OK : SD_EIO;
        while (e == SD_OK && count--) {
            e = tx_block(buf, 0xFC);
            buf += 512;
            /* The block is in; the card programs it with or without CS.
             * Another device waiting gets the bus now, not 63 blocks on. */
            if (e == SD_OK && count && spibus_contended(&dev)) { breathe_selected(); }
        }
        int s = tx_block(NULL, 0xFD);
        if (e == SD_OK) { e = s; }
    }
    /* Done means programmed, as on SDIO: wait the busy out (the bus is
     * released between polls). */
    if (e == SD_OK && !wait_ready(SD_BUSY_TIMEOUT_US)) { e = SD_ETIMEOUT; }
    deselect();
    release();
    return e;
}

static int sdspi_sync(void) {
    claim();
    bool ok = select_card();
    deselect();
    release();
    return ok ? SD_OK : SD_ETIMEOUT;
}

static const sd_bus_t bus = {
    .name = "spi",
    .hw_init = hw_init,
    .identify = identify,
    .read = sdspi_read,
    .write = sdspi_write,
    .sync = sdspi_sync,
};

const sd_bus_t *sd_bus_spi(void) { return &bus; }
