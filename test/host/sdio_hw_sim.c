/*
 * sdio_hw.h for host tests: the SDIO wire as the simulated card sees it
 * (sdcard_sim.c). Commands are checked bit by bit (CRC7, start/end bits)
 * the way a card would, a host/card bus-width mismatch garbles data exactly
 * as it would on the wire, and time passes in clocks.
 */
#include "sd/sdio_hw.h"

#include <string.h>

#include "sd/sd_card.h"
#include "sdcard_sim.h"

static sdsim_t *card;
static uint32_t khz = 400;
static int width = 1;
static uint8_t *armed_buf;
static uint32_t armed_blocks;
static uint32_t dma_blocks;
int sdio_sim_commands;         /* for tests */

void sdsim_attach_sdio(sdsim_t *s) { card = s; armed_buf = NULL; khz = 400; width = 1; }

static void clocks(uint32_t n) {
    uint64_t us = (uint64_t)n * 1000u / (khz ? khz : 1);
    sdsim_advance_us(us ? us : 1);
}

static uint8_t crc7(const uint8_t *p, int n) {
    uint8_t c = 0;
    for (int i = 0; i < n * 8; i++) {
        int fb = ((c >> 6) & 1) ^ ((p[i / 8] >> (7 - i % 8)) & 1);
        c = (uint8_t)((c << 1) & 0x7F);
        if (fb) { c ^= 0x09; }
    }
    return c;
}

bool sdio_hw_init(const sdio_pins_t *pins) {
    /* the board's constraints: four consecutive data lines, CLK/CMD apart */
    if (!pins || pins->d0 < 0 || pins->clk == pins->cmd ||
        (pins->clk >= pins->d0 && pins->clk <= pins->d0 + 3) ||
        (pins->cmd >= pins->d0 && pins->cmd <= pins->d0 + 3)) { return false; }
    return card != NULL;
}
void sdio_hw_set_clock_khz(uint32_t k) { khz = k; }
void sdio_hw_set_width(int w) { width = w; }
uint32_t sdio_hw_clock_khz(void) { return khz; }
void sdio_hw_idle_clocks(uint32_t n) { clocks(n); }

int sdio_hw_command(const uint8_t c[6], int resp_bits, uint8_t *resp) {
    sdio_sim_commands++;
    clocks(48);
    if ((c[0] & 0xC0) != 0x40 || !(c[5] & 1)) { clocks(64); return SD_ETIMEOUT; }
    uint8_t crc_ok = (uint8_t)((crc7(c, 5) << 1) | 1) == c[5];
    uint32_t arg = ((uint32_t)c[1] << 24) | ((uint32_t)c[2] << 16) | ((uint32_t)c[3] << 8) | c[4];
    uint8_t r[17];
    int bits = 0;
    int rc = sdsim_sd_command(card, c[0] & 0x3F, arg, crc_ok, r, &bits);
    if (resp_bits == 0) { clocks(8); return SD_OK; }
    if (rc != 0 || bits == 0) { clocks(64); return SD_ETIMEOUT; }
    if (bits != resp_bits) { return SD_EIO; }          /* the driver expected another type */
    clocks((uint32_t)bits + 2);
    memcpy(resp, r, bits == 136 ? 17 : 6);
    return SD_OK;
}

void sdio_hw_read_arm(uint8_t *buf, uint32_t blocks) { armed_buf = buf; armed_blocks = blocks; }

int sdio_hw_read_wait(uint64_t *crcs, uint32_t timeout_us) {
    if (!armed_buf || !armed_blocks || armed_blocks > SDIO_MAX_BLOCKS) { return SD_EPARAM; }
    for (uint32_t i = 0; i < armed_blocks; i++) {
        uint8_t blk[512];
        uint64_t c = 0;
        if (sdsim_sd_read_block(card, blk, &c) != 0) {
            sdsim_advance_us(timeout_us);
            armed_buf = NULL;
            return SD_ETIMEOUT;
        }
        if (card->width != width) {                     /* the host samples the wrong lines */
            for (int k = 0; k < 512; k++) { blk[k] ^= 0x5A; }
        }
        if (card->marginal_khz && khz > card->marginal_khz) { blk[7] ^= 0x04; }   /* a bit lost on the wire */
        memcpy(armed_buf + 512 * i, blk, 512);
        crcs[i] = c;
        clocks(1024 + 16 + 2);
        dma_blocks++;
    }
    armed_buf = NULL;
    return SD_OK;
}

int sdio_hw_write_block(const uint8_t *buf, uint32_t len, uint64_t crc, uint8_t *token) {
    if (len != 512) { return SD_EPARAM; }
    uint8_t blk[512];
    memcpy(blk, buf, 512);
    if (card->width != width) { for (int i = 0; i < 512; i++) { blk[i] ^= 0x5A; } }
    if (card->marginal_khz && khz > card->marginal_khz) { blk[7] ^= 0x04; }
    clocks(width == 4 ? 1024 + 16 + 2 : 4096 + 16 + 2);
    if (sdsim_sd_write_block(card, blk, crc, token) != 0) { return SD_ETIMEOUT; }
    dma_blocks++;
    return SD_OK;
}

int sdio_hw_wait_busy(uint32_t timeout_us) {
    uint64_t t0 = sdsim_now_us();
    /* A card that has not been clocked since its CRC token shows D0 high:
     * a host that believes one look goes on while it is still programming. */
    if (card->busy_hidden) { return SD_OK; }
    while (sdsim_busy(card)) {
        if (sdsim_now_us() - t0 >= timeout_us) { return SD_ETIMEOUT; }
        sdsim_advance_us(10);
    }
    return SD_OK;
}

void sdio_hw_abort(void) { armed_buf = NULL; }
uint32_t sdio_hw_dma_blocks(void) { return dma_blocks; }
