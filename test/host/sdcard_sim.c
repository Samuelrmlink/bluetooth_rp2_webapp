/*
 * A simulated SD card (see sdcard_sim.h): enough of the Physical Layer
 * Simplified Spec for both drivers -- the SD-mode state machine and
 * responses (R1/R1b/R2/R3/R6/R7), SPI mode (R1, R3/R7 trailers, data tokens,
 * data response, busy), single and multi-block read/write, ACMD23, CMD12,
 * card status error bits, SDSC byte vs SDHC block addressing, v1 cards,
 * ACMD41 power-up polls, a removable card, write busy in simulated time, and
 * one-shot faults (CRC on a read block, CRC/write error on a written block,
 * a command that never answers, a card pulled mid-read).
 *
 * Every CRC here is bit-serial, independent of src/sd/sd_crc.c.
 */
#include "sdcard_sim.h"

#include <stdlib.h>
#include <string.h>

#include "fake_spi.h"

enum { S_IDLE, S_READY, S_IDENT, S_STBY, S_TRAN, S_DATA, S_RCV, S_PRG };

#define ST_OUT_OF_RANGE   (1u << 31)
#define ST_ADDRESS_ERROR  (1u << 30)
#define ST_COM_CRC_ERROR  (1u << 23)
#define ST_ILLEGAL        (1u << 22)
#define ST_ERROR          (1u << 19)
#define ST_READY_FOR_DATA (1u << 8)
#define ST_APP_CMD        (1u << 5)

/* --- time ----------------------------------------------------------------- */

static uint64_t sim_us = 1000000;
uint64_t sdsim_now_us(void) { return sim_us; }
void sdsim_advance_us(uint64_t us) { sim_us += us; }

/* --- bit-serial CRCs ------------------------------------------------------ */

static uint8_t crc7(const uint8_t *p, int n) {
    uint8_t c = 0;
    for (int i = 0; i < n * 8; i++) {
        int fb = ((c >> 6) & 1) ^ ((p[i / 8] >> (7 - i % 8)) & 1);
        c = (uint8_t)((c << 1) & 0x7F);
        if (fb) { c ^= 0x09; }
    }
    return c;
}

static uint16_t crc16_line(const uint8_t *p, int nbytes, int width, int line) {
    uint16_t c = 0;
    int nbits = nbytes * 8;
    for (int i = 0; i < nbits; i++) {
        int bit;
        if (width == 1) {
            bit = (p[i / 8] >> (7 - i % 8)) & 1;
        } else {
            if (i % 4 != 3 - line) { continue; }          /* this line's bit of the nibble */
            bit = (p[i / 8] >> (7 - i % 8)) & 1;
        }
        int fb = ((c >> 15) & 1) ^ bit;
        c = (uint16_t)(c << 1);
        if (fb) { c ^= 0x1021; }
    }
    return c;
}

/* The CRC as it goes out on the wire: 1-bit -> 16 bits; 4-bit -> 16 nibbles
 * holding one bit of each line's CRC, MSB first (as sd_crc16x4 returns it). */
static uint64_t wire_crc(const uint8_t *p, int width) {
    if (width == 1) { return crc16_line(p, 512, 1, 0); }
    uint16_t l[4];
    for (int i = 0; i < 4; i++) { l[i] = crc16_line(p, 512, 4, i); }
    uint64_t w = 0;
    for (int k = 15; k >= 0; k--) {
        uint8_t nib = 0;
        for (int i = 0; i < 4; i++) { nib |= (uint8_t)(((l[i] >> k) & 1) << i); }
        w = (w << 4) | nib;
    }
    return w;
}

/* --- the card ------------------------------------------------------------- */

static void make_regs(sdsim_t *s) {
    static const uint8_t cid[15] = { 0x03, 'S', 'D', 'S', 'I', 'M', '0', '1', 0x10,
                                     0x12, 0x34, 0x56, 0x78, 0x01, 0x9A };   /* 2025-10 */
    memcpy(s->cid, cid, 15);
    s->cid[15] = (uint8_t)((crc7(s->cid, 15) << 1) | 1);
    memset(s->csd, 0, 16);
    if (s->sdhc) {
        uint32_t c_size = (uint32_t)(s->sectors / 1024 - 1);
        s->csd[0] = 0x40;                       /* CSD 2.0 */
        s->csd[5] = 0x59;                       /* READ_BL_LEN 9 */
        s->csd[7] = (uint8_t)((c_size >> 16) & 0x3F);
        s->csd[8] = (uint8_t)(c_size >> 8);
        s->csd[9] = (uint8_t)c_size;
    } else {
        /* CSD 1.0: sectors = (c_size+1) << (mult+2), BL_LEN 9 */
        uint32_t mult = 7, c_size = (uint32_t)(s->sectors >> (mult + 2)) - 1;
        s->csd[5] = 0x59;
        s->csd[6] = (uint8_t)((c_size >> 10) & 3);
        s->csd[7] = (uint8_t)(c_size >> 2);
        s->csd[8] = (uint8_t)((c_size & 3) << 6);
        s->csd[9] = (uint8_t)((mult >> 1) & 3);
        s->csd[10] = (uint8_t)((mult & 1) << 7);
    }
    s->csd[15] = (uint8_t)((crc7(s->csd, 15) << 1) | 1);
}

static void power_up(sdsim_t *s) {
    s->state = S_IDLE;
    s->spi = false;
    s->app = false;
    s->rca = 0;
    s->width = 1;
    s->blocklen = 512;
    s->pending_err = 0;
    s->xfer = 0;
    s->busy_until_us = 0;
    s->sbn = s->outn = s->outi = 0;
    s->driving = false;
    s->wphase = 0;
}

void sdsim_init(sdsim_t *s, uint64_t sectors, bool sdhc) {
    memset(s, 0, sizeof *s);
    s->sectors = sectors;
    s->sdhc = sdhc;
    s->img = calloc((size_t)sectors, 512);
    s->present = true;
    s->crc_read_lba = s->crc_write_lba = s->werr_lba = s->pull_at_lba = -1;
    s->mute_cmd = -1;
    s->garble_resp_cmd = -1;
    s->write_busy_us = 300;
    make_regs(s);
    power_up(s);
}

void sdsim_free(sdsim_t *s) { free(s->img); s->img = NULL; }
void sdsim_pull(sdsim_t *s) { s->present = false; }
void sdsim_insert(sdsim_t *s) { s->present = true; power_up(s); }
bool sdsim_busy(sdsim_t *s) { return sim_us < s->busy_until_us; }

static void trace(sdsim_t *s, uint8_t cmd, uint32_t arg, int result) {
    if (s->ntrace < (int)(sizeof s->trace / sizeof s->trace[0])) {
        s->trace[s->ntrace++] = (sdsim_trace_t){ cmd, arg, result };
    }
}

static uint32_t status_word(sdsim_t *s, int state) {
    uint32_t st = s->pending_err | ((uint32_t)state << 9);
    if (!sdsim_busy(s)) { st |= ST_READY_FOR_DATA; }
    if (s->app) { st |= ST_APP_CMD; }
    return st;
}

static uint64_t lba_of(sdsim_t *s, uint32_t arg) {
    return s->sdhc ? arg : arg / 512u;
}

/* Settle a finished programming phase. */
static void settle(sdsim_t *s) {
    if (s->state == S_PRG && !sdsim_busy(s)) { s->state = S_TRAN; }
}

/* The SD-mode core. Fills a response; *bits 0 = none. Returns 0, or -1 when
 * the card stays silent. */
enum { RN, R1, R1B, R2C, R2S, R3, R6, R7 };
static int core(sdsim_t *s, uint8_t cmd, uint32_t arg, int *rtype, uint32_t *st_out) {
    settle(s);
    bool app = s->app;
    s->app = false;
    int state = s->state;
    *rtype = RN;
    if (!s->present || (int)cmd == s->mute_cmd) { trace(s, cmd, arg, -2); return -1; }
    uint32_t err = 0;
    if (app) {
        switch (cmd) {
        case 41:
            if (s->acmd41_busy > 0) { s->acmd41_busy--; *rtype = R3; break; }
            s->state = S_READY;
            *rtype = R3;
            break;
        case 6:
            if (state != S_TRAN) { err = ST_ILLEGAL; break; }
            s->width = (arg & 3) == 2 ? 4 : 1;
            *rtype = R1;
            break;
        case 23:
            s->xfer_left = arg & 0x7FFFFF;
            *rtype = R1;
            break;
        default:
            err = ST_ILLEGAL;
        }
    } else {
        switch (cmd) {
        case 0:
            power_up(s);
            trace(s, cmd, arg, 0);
            return 0;
        case 8:
            if (s->v1) { err = ST_ILLEGAL; break; }
            *rtype = R7;
            break;
        case 55:
            s->app = true;
            *rtype = R1;
            break;
        case 2:
            if (state != S_READY) { err = ST_ILLEGAL; break; }
            s->state = S_IDENT;
            *rtype = R2C;
            break;
        case 3:
            if (state != S_IDENT && state != S_STBY) { err = ST_ILLEGAL; break; }
            s->rca = 0x1234;
            s->state = S_STBY;
            *rtype = R6;
            break;
        case 9: case 10:
            if (state != S_STBY || (arg >> 16) != s->rca) { err = ST_ILLEGAL; break; }
            *rtype = cmd == 9 ? R2S : R2C;
            break;
        case 7:
            if ((arg >> 16) == s->rca && state == S_STBY) { s->state = S_TRAN; *rtype = R1B; }
            else if ((arg >> 16) != s->rca) { s->state = S_STBY; trace(s, cmd, arg, 0); return -1; }
            else { err = ST_ILLEGAL; }
            break;
        case 13:
            *rtype = R1;
            break;
        case 16:
            s->blocklen = arg;
            *rtype = R1;
            break;
        case 17: case 18: case 24: case 25: {
            if (state != S_TRAN) { err = ST_ILLEGAL; break; }
            uint64_t lba = lba_of(s, arg);
            *rtype = R1;
            if (!s->sdhc && (arg % 512)) { err = ST_ADDRESS_ERROR; break; }
            if (lba >= s->sectors) { err = ST_OUT_OF_RANGE; break; }
            s->xfer_lba = lba;
            s->multi = cmd == 18 || cmd == 25;
            s->xfer = cmd <= 18 ? 1 : 2;
            s->state = cmd <= 18 ? S_DATA : S_RCV;
            if (!s->multi) { s->xfer_left = 0; }
            break;
        }
        case 12:
            if (state == S_DATA) { s->state = S_TRAN; }
            else if (state == S_RCV) { s->state = sdsim_busy(s) ? S_PRG : S_TRAN; }
            s->xfer = 0;
            s->xfer_left = 0;
            *rtype = R1B;
            break;
        case 58: case 59:
            if (!s->spi) { err = ST_ILLEGAL; break; }
            *rtype = R1;
            break;
        default:
            err = ST_ILLEGAL;
        }
    }
    if (err == ST_ILLEGAL && !s->spi) {
        /* SD mode: an illegal command gets no response; the bit shows up in
         * the next status. */
        s->pending_err |= err;
        trace(s, cmd, arg, -1);
        return -1;
    }
    *st_out = status_word(s, state) | err;
    s->pending_err = 0;
    trace(s, cmd, arg, err ? -1 : 0);
    return 0;
}

/* --- SD-mode front end (sdio_hw_sim.c calls these) ------------------------ */

int sdsim_sd_command(sdsim_t *s, uint8_t cmd, uint32_t arg, uint8_t crc_ok, uint8_t *r, int *bits) {
    *bits = 0;
    if (!crc_ok) {
        s->pending_err |= ST_COM_CRC_ERROR;
        trace(s, cmd, arg, -1);
        return -1;
    }
    int rt;
    uint32_t st = 0;
    if (core(s, cmd, arg, &rt, &st) != 0 || rt == RN) { return -1; }
    switch (rt) {
    case R1: case R1B:
        r[0] = cmd; r[1] = (uint8_t)(st >> 24); r[2] = (uint8_t)(st >> 16); r[3] = (uint8_t)(st >> 8); r[4] = (uint8_t)st;
        r[5] = (uint8_t)((crc7(r, 5) << 1) | 1);
        *bits = 48;
        if (rt == R1B && cmd == 7) { s->busy_until_us = sim_us + 50; }
        break;
    case R2C: case R2S:
        r[0] = 0x3F;
        memcpy(r + 1, rt == R2C ? s->cid : s->csd, 16);
        *bits = 136;
        break;
    case R3: {
        uint32_t ocr = 0x00FF8000u;
        if (s->state == S_READY) { ocr |= 0x80000000u | (s->sdhc && (arg & (1u << 30)) ? 0x40000000u : 0); }
        r[0] = 0x3F; r[1] = (uint8_t)(ocr >> 24); r[2] = (uint8_t)(ocr >> 16); r[3] = (uint8_t)(ocr >> 8); r[4] = (uint8_t)ocr;
        r[5] = 0xFF;
        *bits = 48;
        break;
    }
    case R6: {
        uint16_t sb = (uint16_t)(((st >> 8) & 0xC000) | ((st >> 6) & 0x2000) | (st & 0x1FFF));
        r[0] = cmd; r[1] = (uint8_t)(s->rca >> 8); r[2] = (uint8_t)s->rca; r[3] = (uint8_t)(sb >> 8); r[4] = (uint8_t)sb;
        r[5] = (uint8_t)((crc7(r, 5) << 1) | 1);
        *bits = 48;
        break;
    }
    case R7:
        r[0] = cmd; r[1] = 0; r[2] = 0; r[3] = (uint8_t)((arg >> 8) & 0xF); r[4] = (uint8_t)arg;
        r[5] = (uint8_t)((crc7(r, 5) << 1) | 1);
        *bits = 48;
        break;
    }
    if ((int)cmd == s->garble_resp_cmd && *bits == 48) {   /* the host sees a bad CRC; the card acted */
        r[5] ^= 0x10;
        s->garble_resp_cmd = -1;
    }
    return 0;
}

int sdsim_sd_read_block(sdsim_t *s, uint8_t *buf, uint64_t *crc) {
    if (s->xfer != 1 || !s->present) { return -1; }
    if ((int64_t)s->xfer_lba == s->pull_at_lba) {
        s->pull_at_lba = -1;
        s->present = false;
        return -1;
    }
    if (s->xfer_lba >= s->sectors) { s->pending_err |= ST_OUT_OF_RANGE; s->xfer = 0; s->state = S_TRAN; return -1; }
    memcpy(buf, s->img + s->xfer_lba * 512, 512);
    *crc = wire_crc(buf, s->width);
    if ((int64_t)s->xfer_lba == s->crc_read_lba) {
        s->crc_read_lba = -1;
        *crc ^= 0x10;                                   /* one bit off */
    }
    s->blocks_read++;
    s->xfer_lba++;
    if (!s->multi) { s->xfer = 0; s->state = S_TRAN; }
    return 0;
}

int sdsim_sd_write_block(sdsim_t *s, const uint8_t *buf, uint64_t crc, uint8_t *token) {
    if (s->xfer != 2 || !s->present) { return -1; }
    int64_t lba = (int64_t)s->xfer_lba;
    if (crc != wire_crc(buf, s->width) || lba == s->crc_write_lba) {
        if (lba == s->crc_write_lba) { s->crc_write_lba = -1; }
        *token = 0x5;
        s->xfer = 0;                                    /* the card waits for CMD12 */
        return 0;
    }
    if (lba == s->werr_lba) {
        s->werr_lba = -1;
        *token = 0x6;
        s->pending_err |= ST_ERROR;
        s->xfer = 0;
        return 0;
    }
    memcpy(s->img + lba * 512, buf, 512);
    s->blocks_written++;
    s->xfer_lba++;
    s->busy_until_us = sim_us + s->write_busy_us;
    *token = 0x2;
    if (!s->multi) { s->xfer = 0; s->state = S_PRG; }
    return 0;
}

/* --- SPI front end -------------------------------------------------------- */

static void q(sdsim_t *s, uint8_t b) { if (s->outn < (int)sizeof s->out) { s->out[s->outn++] = b; } }

static void q_block(sdsim_t *s, const uint8_t *data, int n) {
    q(s, 0xFF);
    q(s, 0xFE);
    for (int i = 0; i < n; i++) { q(s, data[i]); }
    uint16_t c = crc16_line(data, n, 1, 0);
    q(s, (uint8_t)(c >> 8));
    q(s, (uint8_t)c);
}

static uint8_t spi_r1(uint32_t st, bool idle) {
    uint8_t r = idle ? 0x01 : 0x00;
    if (st & ST_ILLEGAL) { r |= 0x04; }
    if (st & ST_COM_CRC_ERROR) { r |= 0x08; }
    if (st & ST_ADDRESS_ERROR) { r |= 0x20; }
    if (st & ST_OUT_OF_RANGE) { r |= 0x40; }
    return r;
}

static void spi_command(sdsim_t *s) {
    uint8_t cmd = s->sb[0] & 0x3F;
    uint32_t arg = ((uint32_t)s->sb[1] << 24) | ((uint32_t)s->sb[2] << 16) | ((uint32_t)s->sb[3] << 8) | s->sb[4];
    s->outn = s->outi = 0;
    bool crc_ok = (uint8_t)((crc7(s->sb, 5) << 1) | 1) == s->sb[5];
    if (cmd == 0) {
        if (!crc_ok || !s->present || s->mute_cmd == 0) { trace(s, cmd, arg, -2); return; }
        power_up(s);
        s->spi = true;
        trace(s, cmd, arg, 0);
        q(s, 0xFF); q(s, 0x01);
        return;
    }
    if (!s->spi) { return; }
    if (cmd == 8 && !crc_ok) { q(s, 0xFF); q(s, 0x09); trace(s, cmd, arg, -1); return; }
    if (cmd == 12) {
        /* stop a multi-block read: R1 after the stuff byte, then ready */
        bool app = s->app;
        s->app = false;
        (void)app;
        s->xfer = 0;
        s->state = S_TRAN;
        trace(s, cmd, arg, 0);
        q(s, 0xFF); q(s, 0xFF); q(s, 0x00);
        return;
    }
    bool app = s->app;
    if (app && cmd == 41) {
        /* SPI ACMD41: R1 0x01 until initialised */
        s->app = false;
        if (!s->present || s->mute_cmd == 41) { trace(s, cmd, arg, -2); return; }
        if (s->acmd41_busy > 0) { s->acmd41_busy--; trace(s, cmd, arg, 0); q(s, 0xFF); q(s, 0x01); return; }
        s->state = S_TRAN;               /* SPI mode has no ident/stby: straight to transfer */
        trace(s, cmd, arg, 0);
        q(s, 0xFF); q(s, 0x00);
        return;
    }
    int rt;
    uint32_t st = 0;
    if (cmd == 9 || cmd == 10) {
        /* SPI: CSD/CID come as a data block, in transfer state */
        s->app = false;
        if (!s->present || s->mute_cmd == cmd) { trace(s, cmd, arg, -2); return; }
        trace(s, cmd, arg, 0);
        q(s, 0xFF); q(s, 0x00);
        q_block(s, cmd == 9 ? s->csd : s->cid, 16);
        return;
    }
    if (cmd == 58) {
        s->app = false;
        trace(s, cmd, arg, 0);
        uint32_t ocr = 0x00FF8000u | (s->state == S_TRAN ? 0x80000000u : 0) |
                       (s->state == S_TRAN && s->sdhc ? 0x40000000u : 0);
        q(s, 0xFF); q(s, s->state == S_TRAN ? 0x00 : 0x01);
        q(s, (uint8_t)(ocr >> 24)); q(s, (uint8_t)(ocr >> 16)); q(s, (uint8_t)(ocr >> 8)); q(s, (uint8_t)ocr);
        return;
    }
    if (cmd == 59) { s->app = false; trace(s, cmd, arg, 0); q(s, 0xFF); q(s, 0x00); return; }
    if (core(s, cmd, arg, &rt, &st) != 0) { return; }
    bool idle = s->state == S_IDLE || s->state == S_READY;
    q(s, 0xFF);
    q(s, spi_r1(st, idle));
    if (rt == R7) { q(s, 0x00); q(s, 0x00); q(s, (uint8_t)((arg >> 8) & 0xF)); q(s, (uint8_t)arg); }
    if (st & (ST_ILLEGAL | ST_ADDRESS_ERROR | ST_OUT_OF_RANGE)) { s->xfer = 0; s->state = S_TRAN; return; }
    if (s->xfer == 1) {
        uint8_t blk[512];
        uint64_t crc;
        if (sdsim_sd_read_block(s, blk, &crc) == 0) {
            q_block(s, blk, 512);
            if (crc != wire_crc(blk, 1)) { s->out[s->outn - 1] ^= 1; }   /* the injected CRC fault */
        }
    }
    if (s->xfer == 2) { s->wphase = 0; s->wn = 0; }
}

/* Next MISO byte while the host clocks with CS low; `mosi` is what it sent. */
static uint8_t spi_byte(sdsim_t *s, uint8_t mosi) {
    /* a write in progress: token, data, crc */
    if (s->spi && s->xfer == 2 && s->outi >= s->outn) {
        if (s->wphase == 0) {
            if (mosi == 0xFE || mosi == 0xFC) { s->wphase = 1; s->wn = 0; return 0xFF; }
            if (mosi == 0xFD && s->multi) {
                s->xfer = 0;
                s->state = S_TRAN;
                s->outn = s->outi = 0;
                q(s, 0xFF);                     /* then busy while programming */
                return 0xFF;
            }
            if (sdsim_busy(s)) { return 0x00; }
        } else if (s->wphase == 1) {
            s->wbuf[s->wn++] = mosi;
            if (s->wn == 512) { s->wphase = 2; s->wn = 0; }
            return 0xFF;
        } else {
            if (++s->wn < 2) { return 0xFF; }
            uint8_t token;
            sdsim_sd_write_block(s, s->wbuf, wire_crc(s->wbuf, 1), &token);
            s->wphase = 0;
            s->outn = s->outi = 0;
            q(s, token == 0x2 ? 0x05 : token == 0x5 ? 0x0B : 0x0D);
            if (token != 0x2) { s->xfer = 0; s->state = S_TRAN; }
            else if (!s->multi) { s->xfer = 0; }
            return 0xFF;
        }
    }
    /* collecting a command */
    if (s->sbn == 0 && (mosi & 0xC0) == 0x40 && (s->outi >= s->outn || s->xfer == 1)) {
        s->sb[s->sbn++] = mosi;
        return s->outi < s->outn ? s->out[s->outi++] : 0xFF;
    }
    if (s->sbn > 0) {
        s->sb[s->sbn++] = mosi;
        if (s->sbn == 6) {
            s->sbn = 0;
            spi_command(s);
        }
        return 0xFF;
    }
    if (s->outi < s->outn) { return s->out[s->outi++]; }
    /* a multi-block read keeps sending while clocked */
    if (s->spi && s->xfer == 1 && s->multi) {
        uint8_t blk[512];
        uint64_t crc;
        s->outn = s->outi = 0;
        if (sdsim_sd_read_block(s, blk, &crc) == 0) {
            q_block(s, blk, 512);
            if (crc != wire_crc(blk, 1)) { s->out[s->outn - 1] ^= 1; }
            return s->out[s->outi++];
        }
        return 0xFF;
    }
    /* busy after a write: DO low until programmed */
    if (s->spi && (s->state == S_PRG || (s->state == S_TRAN && sdsim_busy(s)))) {
        settle(s);
        return sdsim_busy(s) ? 0x00 : 0xFF;
    }
    return 0xFF;
}

static uint8_t spi_dev(void *ctx, int ev, uint8_t mosi) {
    sdsim_t *s = ctx;
    if (ev == FAKE_CS_HIGH) { s->sbn = 0; return 0; }
    if (ev == FAKE_CS_LOW) { return 0; }
    if (ev == FAKE_IDLE_BYTE) {
        bool was = s->driving;
        s->driving = false;
        return was;
    }
    s->driving = true;
    if (!s->present) { return 0xFF; }
    return spi_byte(s, mosi);
}

void sdsim_attach_spi(sdsim_t *s, int cs) { fake_attach(cs, spi_dev, s); }
