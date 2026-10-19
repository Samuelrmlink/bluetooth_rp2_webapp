#ifndef BTGW_SD_CARD_H
#define BTGW_SD_CARD_H
/*
 * The SD card, independent of the bus that reaches it.
 *
 * Two buses implement sd_bus_t: SDIO 4-bit on PIO (sd_sdio.c) and SPI mode
 * through the shared SPI bus layer (sd_spi.c). Each knows its own
 * identification sequence and framing; what is common lives here -- the
 * result codes, CSD/CID decoding, addressing (byte-addressed SDSC vs
 * block-addressed SDHC/SDXC), the retry policy and the counters -- and
 * sd_diskio.c puts FatFs's disk_* on top.
 *
 * Every call that touches the card runs in the storage worker (sd_worker.h):
 * a wait yields to the main loop through sd_port_yield(), never blocks it.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Results. The same classes are what the differential tests compare. */
enum {
    SD_OK       = 0,
    SD_ENOCARD  = -1,   /* card detect says none, or nothing answers at all */
    SD_ETIMEOUT = -2,   /* no response, no data token, or busy past its bound */
    SD_ECRC     = -3,   /* a CRC mismatch, ours or reported by the card */
    SD_EIO      = -4,   /* the card answered with an error, or refused the data */
    SD_EPARAM   = -5,
};
const char *sd_strerror(int err);

typedef struct {
    bool     valid;          /* identified and in transfer state */
    bool     v2;             /* answered CMD8: physical layer 2.00 or later */
    bool     block_addr;     /* SDHC/SDXC: block addresses; SDSC: byte addresses */
    uint8_t  bus_width;      /* 1 or 4 (SDIO); 1 for SPI */
    uint32_t clock_khz;      /* data clock in use */
    uint32_t ocr;
    uint16_t rca;            /* SDIO only */
    uint64_t sectors;        /* 512-byte sectors, from the CSD */
    uint8_t  csd[16];
    uint8_t  cid[16];
    bool     cid_ok;
} sd_info_t;

/* A bus. addr is a block number or a byte offset as info->block_addr says;
 * sd_card.c does that arithmetic, the bus only frames it. */
typedef struct sd_bus {
    const char *name;                                 /* "sdio" or "spi" */
    bool (*hw_init)(void);                            /* pins, PIO/SPI; once */
    int  (*identify)(sd_info_t *info);                /* power-up -> transfer state, full clock */
    int  (*read)(uint32_t addr, uint8_t *buf, uint32_t count);
    int  (*write)(uint32_t addr, const uint8_t *buf, uint32_t count);
    int  (*sync)(void);                               /* wait out programming */
} sd_bus_t;

const sd_bus_t *sd_bus_sdio(void);
const sd_bus_t *sd_bus_spi(void);

typedef struct {
    uint32_t busy_max_us;    /* longest write busy seen */
    uint32_t busy_timeouts;
    uint32_t read_retries;
    uint32_t write_retries;
    uint32_t crc_errors;
    uint32_t dma_blocks;
    uint32_t sectors_read;
    uint32_t sectors_written;
    uint32_t inits;
} sd_stats_t;

/* The bus this build uses (CONFIG_SD_BUS_*); tests may pick another. */
void              sd_card_use(const sd_bus_t *bus);
const sd_bus_t   *sd_card_bus(void);

bool  sd_card_present(void);                /* card detect (true when there is none) */
int   sd_card_init(void);                   /* identify; SD_OK leaves it ready */
void  sd_card_forget(void);                 /* card gone: next access re-identifies */
bool  sd_card_ready(void);
const sd_info_t *sd_card_info(void);
int   sd_card_read(uint64_t sector, uint8_t *buf, uint32_t count);
int   sd_card_write(uint64_t sector, const uint8_t *buf, uint32_t count);
int   sd_card_sync(void);
sd_stats_t *sd_card_stats(void);            /* the buses add to it too */

/* CSD -> 512-byte sector count (CSD v1 and v2; 0 for one we cannot read). */
uint64_t sd_csd_sectors(const uint8_t csd[16]);
/* CID fields, for the status JSON. */
typedef struct {
    uint8_t  mid;
    char     oem[3];
    char     name[6];
    uint8_t  rev_major, rev_minor;
    uint32_t serial;
    uint16_t year;
    uint8_t  month;
} sd_cid_t;
void sd_cid_decode(const uint8_t cid[16], sd_cid_t *out);

/* Bounds shared by both buses (SD spec 4.6.2, plus room for cheap cards). */
#define SD_INIT_TIMEOUT_US   1000000u   /* ACMD41 until ready */
#define SD_READ_TIMEOUT_US    200000u   /* command to first data (spec: 100 ms) */
#define SD_BUSY_TIMEOUT_US   2000000u   /* write busy (spec 250/500 ms; cards exceed it) */
#define SD_SPIN_US               200u   /* spin this long before yielding to the loop */

#endif
