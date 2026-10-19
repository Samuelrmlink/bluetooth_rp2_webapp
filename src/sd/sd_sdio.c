/*
 * SD card over SDIO (SD mode, 1- or 4-bit): the protocol. Written from the
 * SD Physical Layer Simplified Specification (Part 1, v9.10): commands,
 * responses (R1/R1b/R2/R3/R6/R7), the identification sequence, block
 * read/write, CRC checks. The wire is sdio_hw.h -- PIO on the board, a
 * simulated card in host tests.
 *
 * Identification (4.2):   80 clocks at 400 kHz, 1-bit
 *   CMD0                    -> idle
 *   CMD8  0x1AA       R7    -> v2 card if the pattern echoes (no answer: v1)
 *   ACMD41 HCS|3.3V   R3    -> repeated until OCR bit 31 (powered up), 1 s;
 *                              OCR bit 30 (CCS) = block addressing
 *   CMD2              R2    -> CID
 *   CMD3              R6    -> RCA
 *   CMD9  RCA         R2    -> CSD
 *   CMD7  RCA         R1b   -> transfer state
 *   ACMD6 4-bit       R1    -> 4 data lines (stays 1-bit if refused)
 *   CMD16 512         R1    -> SDSC only
 *   then the data clock goes to CONFIG_SD_SDIO_KHZ.
 *
 * Data: CMD17/18 read (CMD12 ends a multi-block read), CMD24/25 write
 * (ACMD23 pre-erase hint, CMD12 ends it), each block's CRC16s checked here,
 * busy waited out on D0.
 */
#include "sd/sd_card.h"

#include <string.h>

#include "autoconf.h"
#include "sd/sd_crc.h"
#include "sd/sd_port.h"
#include "sd/sdio_hw.h"

#ifndef CONFIG_SD_SDIO_CLK
#define CONFIG_SD_SDIO_CLK 16
#define CONFIG_SD_SDIO_CMD 17
#define CONFIG_SD_SDIO_D0  18
#define CONFIG_SD_SDIO_PIO -1
#endif
#ifndef CONFIG_SD_SDIO_KHZ
#define CONFIG_SD_SDIO_KHZ 25000
#endif

#define R_NONE 0
#define R_48   48
#define R_136  136

/* Card status bits that mean the command failed (4.10.1). COM_CRC_ERROR is
 * told apart: it is our CRC the card did not like. */
#define ST_COM_CRC_ERROR  (1u << 23)
#define ST_ERRORS         0xFDF98008u
#define ST_READY_FOR_DATA (1u << 8)
#define ST_STATE(s)       (((s) >> 9) & 0xFu)
#define STATE_TRAN 4u
#define STATE_PRG  7u

static sd_info_t *cur;            /* the card being identified / used */
static uint8_t    width = 1;
static uint32_t   last_status;    /* the latest R1 card status */

static void pack_cmd(uint8_t out[6], uint8_t cmd, uint32_t arg) {
    out[0] = (uint8_t)(0x40 | cmd);
    out[1] = (uint8_t)(arg >> 24);
    out[2] = (uint8_t)(arg >> 16);
    out[3] = (uint8_t)(arg >> 8);
    out[4] = (uint8_t)arg;
    out[5] = (uint8_t)((sd_crc7(out, 5) << 1) | 1u);
}

/* One command. rtype: 'n' none, '1' R1/R1b, '2' R2, '3' R3, '6' R6, '7' R7.
 * For R1-style responses the card status lands in last_status and its error
 * bits become the result. */
static int command(uint8_t cmd, uint32_t arg, char rtype, uint8_t *resp) {
    uint8_t c[6], r[17];
    pack_cmd(c, cmd, arg);
    int bits = rtype == 'n' ? R_NONE : rtype == '2' ? R_136 : R_48;
    int e = sdio_hw_command(c, bits, r);
    if (e != SD_OK || rtype == 'n') { return e; }
    if (resp) { memcpy(resp, r, bits == R_136 ? 17 : 6); }
    if (rtype == '2') {
        /* R2: 0x3F, then the register with its own CRC7 in the last byte. */
        if ((r[0] & 0xC0) != 0 || (sd_crc7(r + 1, 15) << 1 | 1u) != r[16]) { return SD_ECRC; }
        return SD_OK;
    }
    if (rtype == '3') {
        return (r[0] & 0xC0) == 0 ? SD_OK : SD_EIO;   /* R3 carries no CRC (all ones) */
    }
    if ((r[0] & 0x3F) != cmd || (uint8_t)(sd_crc7(r, 5) << 1 | 1u) != r[5]) { return SD_ECRC; }
    if (rtype == '1') {
        last_status = ((uint32_t)r[1] << 24) | ((uint32_t)r[2] << 16) | ((uint32_t)r[3] << 8) | r[4];
        if (last_status & ST_COM_CRC_ERROR) { return SD_ECRC; }
        if (last_status & ST_ERRORS) { return SD_EIO; }
    }
    return SD_OK;
}

static int app_command(uint8_t acmd, uint32_t arg, char rtype, uint8_t *resp) {
    int e = command(55, (uint32_t)(cur ? cur->rca : 0) << 16, '1', NULL);
    if (e != SD_OK) { return e; }
    return command(acmd, arg, rtype, resp);
}

static int wait_busy(uint32_t timeout_us) {
    sd_stats_t *st = sd_card_stats();
    uint64_t t0 = sd_port_now_us();
    int e = sdio_hw_wait_busy(timeout_us);
    uint32_t dt = (uint32_t)(sd_port_now_us() - t0);
    if (e != SD_OK) {
        st->busy_timeouts++;
        dt = timeout_us;
    }
    if (dt > st->busy_max_us) { st->busy_max_us = dt; }
    return e;
}

static bool hw_init(void) {
    static const sdio_pins_t pins = { CONFIG_SD_SDIO_CLK, CONFIG_SD_SDIO_CMD, CONFIG_SD_SDIO_D0,
                                      CONFIG_SD_SDIO_PIO };
    return sdio_hw_init(&pins);
}

static int identify(sd_info_t *info) {
    cur = info;
    info->rca = 0;
    width = 1;
    sdio_hw_set_width(1);
    sdio_hw_set_clock_khz(400);
    sdio_hw_idle_clocks(80);                         /* >= 74 after power-up */

    command(0, 0, 'n', NULL);
    uint8_t r[17];
    int e = command(8, 0x1AA, '7', r);
    if (e == SD_OK) {
        if (r[3] != 0x01 || r[4] != 0xAA) { return SD_EIO; }   /* voltage not accepted */
        info->v2 = true;
    } else if (e == SD_ETIMEOUT) {
        info->v2 = false;                                       /* v1 card (or none) */
        command(0, 0, 'n', NULL);
    } else {
        return e;
    }

    uint64_t t0 = sd_port_now_us();
    uint32_t ocr = 0;
    for (;;) {
        e = app_command(41, (info->v2 ? (1u << 30) : 0) | 0x00FF8000u, '3', r);
        if (e != SD_OK) { return e == SD_ETIMEOUT ? SD_ENOCARD : e; }
        ocr = ((uint32_t)r[1] << 24) | ((uint32_t)r[2] << 16) | ((uint32_t)r[3] << 8) | r[4];
        if (ocr & (1u << 31)) { break; }                        /* powered up */
        if (sd_port_now_us() - t0 > SD_INIT_TIMEOUT_US) { return SD_ETIMEOUT; }
        sd_port_wait_step(t0);
    }
    info->ocr = ocr;
    info->block_addr = info->v2 && (ocr & (1u << 30));

    if ((e = command(2, 0, '2', r)) != SD_OK) { return e; }
    memcpy(info->cid, r + 1, 16);
    info->cid_ok = true;
    if ((e = command(3, 0, '6', r)) != SD_OK) { return e; }
    info->rca = (uint16_t)((r[1] << 8) | r[2]);
    if ((e = command(9, (uint32_t)info->rca << 16, '2', r)) != SD_OK) { return e; }
    memcpy(info->csd, r + 1, 16);
    if ((e = command(7, (uint32_t)info->rca << 16, '1', NULL)) != SD_OK) { return e; }
    if ((e = wait_busy(SD_BUSY_TIMEOUT_US)) != SD_OK) { return e; }

    if (app_command(6, 2, '1', NULL) == SD_OK) {               /* 4-bit bus */
        width = 4;
        sdio_hw_set_width(4);
    }
    if (!info->block_addr && (e = command(16, 512, '1', NULL)) != SD_OK) { return e; }

    info->bus_width = width;
    info->clock_khz = CONFIG_SD_SDIO_KHZ;
    sdio_hw_set_clock_khz(CONFIG_SD_SDIO_KHZ);
    return SD_OK;
}

static uint64_t block_crc(const uint8_t *buf) {
    return width == 4 ? sd_crc16x4(buf, 512) : sd_crc16(buf, 512);
}

/* Stop a multi-block transfer (or whatever the card was left doing). */
static void stop_transmission(void) {
    sdio_hw_abort();
    command(12, 0, '1', NULL);
    wait_busy(SD_BUSY_TIMEOUT_US);
}

static int sdio_read(uint32_t addr, uint8_t *buf, uint32_t count) {
    bool multi = count > 1;
    sdio_hw_read_arm(buf, 512);                     /* before the command: see sdio_hw.h */
    int e = command(multi ? 18 : 17, addr, '1', NULL);
    if (e != SD_OK) {
        sdio_hw_abort();
        return e;
    }
    for (uint32_t i = 0; i < count; i++) {
        if (i) { sdio_hw_read_arm(buf, 512); }
        uint64_t crc = 0;
        e = sdio_hw_read_wait(&crc, SD_READ_TIMEOUT_US);
        if (e == SD_OK && crc != block_crc(buf)) { e = SD_ECRC; }
        if (e != SD_OK) { break; }
        buf += 512;
    }
    if (multi || e != SD_OK) { stop_transmission(); }
    return e;
}

static int sdio_write(uint32_t addr, const uint8_t *buf, uint32_t count) {
    bool multi = count > 1;
    if (multi) { app_command(23, count, '1', NULL); }   /* pre-erase hint; may be refused */
    int e = command(multi ? 25 : 24, addr, '1', NULL);
    if (e != SD_OK) { return e; }
    for (uint32_t i = 0; i < count && e == SD_OK; i++) {
        uint8_t token = 0;
        e = sdio_hw_write_block(buf, 512, block_crc(buf), &token);
        if (e == SD_OK) {
            if (token == 0x5) { e = SD_ECRC; }           /* 101: CRC error */
            else if (token != 0x2) { e = SD_EIO; }       /* 110: write error */
        }
        if (e == SD_OK) { e = wait_busy(SD_BUSY_TIMEOUT_US); }
        buf += 512;
    }
    if (multi || e != SD_OK) { stop_transmission(); }
    return e;
}

static int sdio_sync(void) {
    int e = wait_busy(SD_BUSY_TIMEOUT_US);
    if (e != SD_OK) { return e; }
    uint64_t t0 = sd_port_now_us();
    for (;;) {
        e = command(13, (uint32_t)(cur ? cur->rca : 0) << 16, '1', NULL);
        if (e != SD_OK) { return e; }
        if ((last_status & ST_READY_FOR_DATA) && ST_STATE(last_status) == STATE_TRAN) { return SD_OK; }
        if (sd_port_now_us() - t0 > SD_BUSY_TIMEOUT_US) { return SD_ETIMEOUT; }
        sd_port_wait_step(t0);
    }
}

static const sd_bus_t bus = {
    .name = "sdio",
    .hw_init = hw_init,
    .identify = identify,
    .read = sdio_read,
    .write = sdio_write,
    .sync = sdio_sync,
};

const sd_bus_t *sd_bus_sdio(void) { return &bus; }
