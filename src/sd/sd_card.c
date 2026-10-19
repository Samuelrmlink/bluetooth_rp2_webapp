/*
 * The bus-independent half of the SD card: addressing, retries, counters,
 * CSD/CID. The policy is km003c_rp2_webapp's (src/log/km_sd.c), which ran
 * 1 GB transfers on real cards: a failed read is repeated once (always
 * safe), a failed write is repeated once after the card has had its busy
 * time (same data, same sectors: harmless), and a card that is gone is
 * forgotten so the next access identifies it afresh.
 */
#include "sd/sd_card.h"

#include <string.h>

#include "autoconf.h"
#include "sd/sd_port.h"

#ifndef CONFIG_SD_DETECT
#define CONFIG_SD_DETECT -1
#endif
#ifndef CONFIG_SD_DETECT_LEVEL
#define CONFIG_SD_DETECT_LEVEL 0
#endif

/* Keep a worker slice to about this long: FatFs can issue many card
 * operations in one call (a mount's free-space scan, a format's tables). */
#define SLICE_US 2000u

static const sd_bus_t *bus;
static sd_info_t info;
static sd_stats_t stats;
static bool detect_ready;

const char *sd_strerror(int err) {
    switch (err) {
    case SD_OK:       return "ok";
    case SD_ENOCARD:  return "no card";
    case SD_ETIMEOUT: return "timeout";
    case SD_ECRC:     return "crc";
    case SD_EIO:      return "io";
    case SD_EPARAM:   return "param";
    default:          return "?";
    }
}

void sd_card_use(const sd_bus_t *b) {
    bus = b;
    memset(&info, 0, sizeof info);
}

const sd_bus_t *sd_card_bus(void) {
    if (!bus) {
#if defined(CONFIG_SD_BUS_SPI)
        sd_card_use(sd_bus_spi());
#elif defined(CONFIG_SD_BUS_SDIO)
        sd_card_use(sd_bus_sdio());
#endif
    }
    return bus;
}

sd_stats_t *sd_card_stats(void) { return &stats; }
const sd_info_t *sd_card_info(void) { return &info; }
bool sd_card_ready(void) { return info.valid; }

bool sd_card_present(void) {
#if CONFIG_SD_DETECT >= 0
    if (!detect_ready) {
        sd_port_detect_init(CONFIG_SD_DETECT);
        detect_ready = true;
    }
    return sd_port_detect_level(CONFIG_SD_DETECT) == (CONFIG_SD_DETECT_LEVEL != 0);
#else
    (void)detect_ready;
    return true;   /* no detect switch: find out by talking to it */
#endif
}

void sd_card_forget(void) {
    info.valid = false;
}

int sd_card_init(void) {
    const sd_bus_t *b = sd_card_bus();
    if (!b) { return SD_EPARAM; }
    memset(&info, 0, sizeof info);
    if (!sd_card_present()) { return SD_ENOCARD; }
    static const sd_bus_t *hw_ok;           /* the bus whose hardware is up */
    if (hw_ok != b) {
        if (!b->hw_init()) { return SD_EIO; }
        hw_ok = b;
    }
    stats.inits++;
    int r = b->identify(&info);
    if (r != SD_OK) {
        info.valid = false;
        return r;
    }
    info.sectors = sd_csd_sectors(info.csd);
    info.valid = true;
    return SD_OK;
}

static uint32_t addr_of(uint64_t sector) {
    return info.block_addr ? (uint32_t)sector : (uint32_t)(sector * 512u);
}

static bool in_range(uint64_t sector, uint32_t count) {
    return count && (!info.sectors || sector + count <= info.sectors) &&
           (info.block_addr || (sector + count) * 512u <= 0xFFFFFFFFull);
}

int sd_card_read(uint64_t sector, uint8_t *buf, uint32_t count) {
    if (!info.valid) { return SD_ENOCARD; }
    if (!in_range(sector, count)) { return SD_EPARAM; }
    sd_port_maybe_yield(SLICE_US);
    int r = bus->read(addr_of(sector), buf, count);
    if (r != SD_OK && r != SD_ENOCARD && sd_card_present()) {
        stats.read_retries++;              /* a read is always safe to repeat */
        if (r == SD_ECRC) { stats.crc_errors++; }
        r = bus->read(addr_of(sector), buf, count);
    }
    if (r == SD_ECRC) { stats.crc_errors++; }
    if (r == SD_OK) { stats.sectors_read += count; }
    if (r == SD_ENOCARD || !sd_card_present()) { sd_card_forget(); }
    return r;
}

int sd_card_write(uint64_t sector, const uint8_t *buf, uint32_t count) {
    if (!info.valid) { return SD_ENOCARD; }
    if (!in_range(sector, count)) { return SD_EPARAM; }
    sd_port_maybe_yield(SLICE_US);
    int r = bus->write(addr_of(sector), buf, count);
    if (r != SD_OK && r != SD_ENOCARD && sd_card_present()) {
        /* Once more, after the card has had its time: one slow garbage
         * collection should not cost a transfer. */
        stats.write_retries++;
        if (r == SD_ECRC) { stats.crc_errors++; }
        bus->sync();
        r = bus->write(addr_of(sector), buf, count);
    }
    if (r == SD_ECRC) { stats.crc_errors++; }
    if (r == SD_OK) { stats.sectors_written += count; }
    if (r == SD_ENOCARD || !sd_card_present()) { sd_card_forget(); }
    return r;
}

int sd_card_sync(void) {
    if (!info.valid) { return SD_ENOCARD; }
    return bus->sync();
}

/* --- CSD / CID (Physical Layer Simplified Spec 5.2, 5.3) ------------------ */

/* Bits [hi:lo] of a 128-bit big-endian register (bit 127 = MSB of byte 0). */
static uint32_t bits(const uint8_t r[16], int hi, int lo) {
    uint32_t v = 0;
    for (int i = hi; i >= lo; i--) {
        v = (v << 1) | ((r[15 - i / 8] >> (i % 8)) & 1u);
    }
    return v;
}

uint64_t sd_csd_sectors(const uint8_t csd[16]) {
    switch (bits(csd, 127, 126)) {
    case 0: {                                   /* CSD 1.0: SDSC */
        uint32_t c_size = bits(csd, 73, 62);
        uint32_t mult = bits(csd, 49, 47);
        uint32_t bl_len = bits(csd, 83, 80);
        if (bl_len < 9 || bl_len > 11) { return 0; }
        return ((uint64_t)c_size + 1) << (mult + 2 + bl_len - 9);
    }
    case 1:                                     /* CSD 2.0: SDHC/SDXC */
        return ((uint64_t)bits(csd, 69, 48) + 1) * 1024u;
    case 2:                                     /* CSD 3.0: SDUC, 28-bit C_SIZE */
        return ((uint64_t)bits(csd, 75, 48) + 1) * 1024u;
    default:
        return 0;
    }
}

void sd_cid_decode(const uint8_t cid[16], sd_cid_t *o) {
    memset(o, 0, sizeof *o);
    o->mid = cid[0];
    o->oem[0] = (char)cid[1];
    o->oem[1] = (char)cid[2];
    for (int i = 0; i < 5; i++) { o->name[i] = (char)cid[3 + i]; }
    for (int i = 0; i < 2; i++) { if ((uint8_t)o->oem[i] < 0x20 || (uint8_t)o->oem[i] > 0x7E) { o->oem[i] = '?'; } }
    for (int i = 0; i < 5; i++) { if ((uint8_t)o->name[i] < 0x20 || (uint8_t)o->name[i] > 0x7E) { o->name[i] = '?'; } }
    o->rev_major = cid[8] >> 4;
    o->rev_minor = cid[8] & 0x0F;
    o->serial = ((uint32_t)cid[9] << 24) | ((uint32_t)cid[10] << 16) | ((uint32_t)cid[11] << 8) | cid[12];
    uint32_t mdt = bits(cid, 19, 8);
    o->year = (uint16_t)(2000 + (mdt >> 4));
    o->month = (uint8_t)(mdt & 0x0F);
}
