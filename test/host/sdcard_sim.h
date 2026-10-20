/* A simulated SD card for host tests: one card model with two front ends --
 * SPI mode on the fake SPI bus (fake_spi.c) and SD mode as the sdio_hw.h
 * wire -- so sd_spi.c and sd_sdio.c are tested against the same card. Its
 * CRCs are computed bit by bit here, independently of src/sd/sd_crc.c.
 * See sdcard_sim.c. */
#ifndef SDCARD_SIM_H
#define SDCARD_SIM_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint8_t  cmd;
    uint32_t arg;
    int      result;          /* 0 answered, -1 ignored/illegal, -2 no response (fault) */
} sdsim_trace_t;

typedef struct sdsim {
    /* the medium */
    uint8_t *img;
    uint64_t sectors;
    bool     sdhc;            /* block addressing (CCS) */
    bool     v1;              /* answers no CMD8 */
    uint8_t  cid[16], csd[16];
    bool     present;
    int      acmd41_busy;     /* ACMD41 polls answered "still powering up" */

    /* state */
    int      state;           /* SD state machine (idle, ready, ident, stby, tran, data, rcv, prg) */
    bool     spi;             /* entered SPI mode (CMD0 with CS low) */
    bool     spi_crc;         /* CMD59 turned CRC checking on (SPI mode) */
    bool     refuse_cmd59;    /* an old card: CMD59 is illegal */
    uint8_t  wcrc[2];         /* SPI write: the CRC16 the host sent */
    bool     app;             /* next command is an ACMD */
    uint16_t rca;
    int      width;           /* 1 or 4 */
    uint32_t blocklen;
    uint32_t status;          /* card status (R1 in SD mode) */
    uint32_t pending_err;     /* error bits reported with the next response */
    int      xfer;            /* 0 none, 1 reading, 2 writing */
    bool     multi;
    uint64_t xfer_lba;
    uint32_t xfer_left;       /* blocks left in an ACMD23/finite transfer (0 = open-ended) */
    uint64_t busy_until_us;

    /* faults (one-shot unless noted) */
    int64_t  crc_read_lba;    /* corrupt the CRC of this block on its next read */
    int64_t  crc_write_lba;   /* answer CRC-error to the next write of this block */
    int64_t  werr_lba;        /* answer write-error to the next write of this block */
    int      mute_cmd;        /* never answer this command (sticky; -1 none) */
    uint32_t write_busy_us;   /* programming time per written block */
    int64_t  pull_at_lba;     /* the card is pulled while this block is read */
    int      garble_resp_cmd; /* corrupt the CRC of this command's next response, but act on it (-1 none) */
    bool     busy_hidden;     /* D0 shows no busy until clocked a while (a real card, bench 2026-10-19) */
    bool     crc_until_reset; /* every block it sends has a bad CRC until CMD0 / power-up (a wedged card) */
    uint32_t marginal_khz;    /* data above this clock arrives corrupted (a marginal contact); 0 = none */

    /* trace */
    sdsim_trace_t trace[4096];
    int      ntrace;
    uint32_t blocks_read, blocks_written;

    /* SPI front end */
    uint8_t  sb[8];  int sbn; /* command bytes */
    uint8_t  out[600]; int outn, outi;   /* queued MISO bytes */
    bool     driving;         /* DO driven (until a clock with CS high) */
    int      wphase;          /* SPI write: 0 wait token, 1 data, 2 crc */
    uint8_t  wbuf[512]; int wn;
    bool     wstop_pending;
} sdsim_t;

void     sdsim_init(sdsim_t *s, uint64_t sectors, bool sdhc);
void     sdsim_free(sdsim_t *s);
void     sdsim_pull(sdsim_t *s);            /* card removed */
void     sdsim_insert(sdsim_t *s);          /* inserted again: back to power-up */

/* The simulated time base, shared with the host sd_port. */
uint64_t sdsim_now_us(void);
void     sdsim_advance_us(uint64_t us);

/* SPI front end: a fake_spi device on chip select `cs`. */
void     sdsim_attach_spi(sdsim_t *s, int cs);

/* SD-mode front end: the card sdio_hw_* (test/host/sdio_hw_sim.c) talks to. */
void     sdsim_attach_sdio(sdsim_t *s);

/* For the SD-mode front end. */
int      sdsim_sd_command(sdsim_t *s, uint8_t cmd, uint32_t arg, uint8_t crc_ok,
                          uint8_t *resp, int *resp_bits);
int      sdsim_sd_read_block(sdsim_t *s, uint8_t *buf, uint64_t *crc);   /* 0 ok, -1 nothing to send */
int      sdsim_sd_write_block(sdsim_t *s, const uint8_t *buf, uint64_t crc, uint8_t *token);
bool     sdsim_busy(sdsim_t *s);

#endif
