#include "ota/ota_slot.h"

#include <stdio.h>
#include <string.h>
#include "pico/bootrom.h"
#include "pico/flash.h"
#include "boot/picobin.h"
#include "boot/picoboot_constants.h"
#include "hardware/watchdog.h"
#include "hardware/flash.h"
#include "pico/time.h"
#include "pico/platform.h"

static ota_part_t slot[2];

/*
 * Replaces the SDK's rom_pick_ab_partition_during_update() (linked with
 * --wrap; see CMakeLists.txt). The SDK 2.3.0 version decides "verified" by
 * reading fixed offsets of the ROM's work area that were generated from ONE
 * bootrom's struct layout, and on this board (RP2350 rev A3) the word it
 * reads is 0 while the ROM's own pick has already chosen the partition --
 * so the CYW43 driver's firmware-partition lookup failed with NOT_FOUND
 * and the radio never came up. Measured on the bench: the raw pick said
 * "partition 2", the wrapper said -17, for our running slot A too.
 *
 * This version trusts the ROM's pick (the ROM does not choose an image whose
 * block loop it rejects) and adds the one check that is layout-independent:
 * a picobin block start marker within the picked partition's first sector.
 * It does NOT save/restore the ROM's version-downgrade erase address
 * (another bootram offset of the same provenance); ota_buy() erases a
 * higher-versioned other slot itself after a successful buy, which is the
 * outcome that bookkeeping existed for.
 */
static bool part_location(uint index, uint32_t *start, uint32_t *size);
int __wrap_rom_pick_ab_partition_during_update(uint32_t *workarea_base, uint32_t workarea_size, uint partition_a_num) {
    /* The flash-update window base is passed ONLY when it falls inside the
     * pair being asked about. Measured: on a flash-update boot into slot A,
     * passing slot A's base while asking about the CYW43 firmware partition
     * made the ROM hand back something other than the blob, the driver fed
     * it to the chip, and cyw43_arch_init() waited forever (the wedge path,
     * sem_acquire_block_until) -- every time, only on the update boot. */
    uint32_t flash_update_base = 0;
    if (rom_get_last_boot_type() == BOOT_TYPE_FLASH_UPDATE) {
        boot_info_t bi = {0};
        if (rom_get_boot_info(&bi)) {
            uint32_t base = bi.reboot_params[0] - XIP_BASE;
            uint32_t st, sz;
            /* A itself, or its B partner (the next partition whose link names A). */
            if (part_location(partition_a_num, &st, &sz) && base >= st && base < st + sz) {
                flash_update_base = bi.reboot_params[0];
            } else {
                for (uint p = 0; p < 16; p++) {
                    uint32_t buf[8] = {0};
                    int ret = rom_get_partition_table_info(buf, 8,
                        PT_INFO_PARTITION_LOCATION_AND_FLAGS | PT_INFO_SINGLE_PARTITION | (p << 24));
                    if (ret != 3) break;
                    uint32_t flags = buf[2];
                    bool links_a = ((flags & PICOBIN_PARTITION_FLAGS_LINK_TYPE_BITS) >> PICOBIN_PARTITION_FLAGS_LINK_TYPE_LSB) == PICOBIN_PARTITION_FLAGS_LINK_TYPE_A_PARTITION &&
                                   ((flags & PICOBIN_PARTITION_FLAGS_LINK_VALUE_BITS) >> PICOBIN_PARTITION_FLAGS_LINK_VALUE_LSB) == partition_a_num;
                    if (links_a && part_location(p, &st, &sz) && base >= st && base < st + sz) {
                        flash_update_base = bi.reboot_params[0];
                        break;
                    }
                }
            }
        }
    }
    int rc = rom_pick_ab_partition((uint8_t *)workarea_base, workarea_size, partition_a_num, flash_update_base);
    if (rc < 0) return rc;
    uint32_t start, size;
    if (!part_location((uint)rc, &start, &size)) return BOOTROM_ERROR_NOT_FOUND;
    const uint32_t *w = (const uint32_t *)(XIP_NOCACHE_NOALLOC_NOTRANSLATE_BASE + start);
    for (size_t i = 0; i < 1024; i++) if (w[i] == PICOBIN_BLOCK_MARKER_START) return rc;
    return BOOTROM_ERROR_NOT_FOUND;
}
static bool have_slots;
static boot_info_t boot;
static bool bought;

static bool part_location(uint index, uint32_t *start, uint32_t *size) {
    uint32_t buf[8] = {0};
    int ret = rom_get_partition_table_info(buf, 8,
        PT_INFO_PARTITION_LOCATION_AND_FLAGS | PT_INFO_SINGLE_PARTITION | (index << 24));
    if (ret != 3) return false;
    uint32_t loc = buf[1];
    uint32_t first = (loc & PICOBIN_PARTITION_LOCATION_FIRST_SECTOR_BITS) >> PICOBIN_PARTITION_LOCATION_FIRST_SECTOR_LSB;
    uint32_t last  = (loc & PICOBIN_PARTITION_LOCATION_LAST_SECTOR_BITS) >> PICOBIN_PARTITION_LOCATION_LAST_SECTOR_LSB;
    *start = first * 4096u;
    *size = (last + 1 - first) * 4096u;
    return *size > 0;
}

static bool read_part(int index, ota_part_t *out) {
    return part_location((uint)index, &out->start, &out->size);
}

/* The table as the ROM reads it, once per boot. Cheap, and the only way to
 * see why a partition lookup (ours, or the CYW43 driver's) came back empty. */
static void dump_pt(void) {
    uint32_t buf[(16 * 4) + 1] = {0};
    int ret = rom_get_partition_table_info(buf, sizeof buf / sizeof buf[0],
                                           PT_INFO_PARTITION_LOCATION_AND_FLAGS | PT_INFO_PARTITION_ID);
    if (ret <= 0) { printf("ota: pt query -> %d\n", ret); return; }
    int i = 1, p = 0, wifi = -1;
    while (i < ret) {
        uint32_t loc = buf[i++];
        uint32_t flags = buf[i++];
        uint64_t id = 0;
        bool has_id = flags & PICOBIN_PARTITION_FLAGS_HAS_ID_BITS;
        if (has_id) { id = buf[i++]; id |= (uint64_t)buf[i++] << 32; }
        uint32_t first = (loc & PICOBIN_PARTITION_LOCATION_FIRST_SECTOR_BITS) >> PICOBIN_PARTITION_LOCATION_FIRST_SECTOR_LSB;
        uint32_t last  = (loc & PICOBIN_PARTITION_LOCATION_LAST_SECTOR_BITS) >> PICOBIN_PARTITION_LOCATION_LAST_SECTOR_LSB;
        printf("ota: pt[%d] %06lx..%06lx flags %08lx%s%016llx\n", p, (unsigned long)first * 4096,
               (unsigned long)(last + 1) * 4096 - 1, (unsigned long)flags, has_id ? " id " : "",
               has_id ? (unsigned long long)id : 0ull);
        if (has_id && id == 0x776966696669726dull) wifi = p;
        p++;
    }
    if (wifi >= 0) {
        static uint32_t __attribute__((aligned(4))) work[1024];
        int pick = rom_pick_ab_partition_during_update(work, sizeof work, (uint)wifi);
        printf("ota: chip rev A%d; CYW43 firmware partition %d, pick -> %d\n", rp2350_chip_version(), wifi, pick);
    }
}

void ota_slot_init(void) {
    memset(&boot, 0, sizeof boot);
    rom_get_boot_info(&boot);
    have_slots = read_part(0, &slot[0]) && read_part(1, &slot[1]);
    if (!have_slots) {
        printf("ota: no A/B partition table -- image group disabled (boot type %d)\n", boot.boot_type);
        return;
    }
    dump_pt();
    char v[2][16];
    ota_slot_version_str(0, v[0], sizeof v[0]);
    ota_slot_version_str(1, v[1], sizeof v[1]);
    printf("ota: slot A @%08lx %lu KB v%s, slot B @%08lx %lu KB v%s; booted %d, type %d, tbyb %02x\n",
           (unsigned long)slot[0].start, (unsigned long)slot[0].size / 1024, v[0],
           (unsigned long)slot[1].start, (unsigned long)slot[1].size / 1024, v[1],
           boot.partition, boot.boot_type, boot.tbyb_and_update_info);
}

int ota_slot_active(void) {
    if (!have_slots) return -1;
    return (boot.partition == 0 || boot.partition == 1) ? boot.partition : -1;
}

bool ota_slot_get(int which, ota_part_t *out) {
    if (!have_slots || which < 0 || which > 1) return false;
    *out = slot[which];
    return true;
}

const uint8_t *ota_slot_bytes(int which) {
    if (!have_slots || which < 0 || which > 1) return NULL;
    return (const uint8_t *)(XIP_NOCACHE_NOALLOC_NOTRANSLATE_BASE + slot[which].start);
}

uint8_t ota_boot_type(void) { return boot.boot_type; }
bool ota_buy_pending(void) {
    return (boot.tbyb_and_update_info & BOOT_TBYB_AND_UPDATE_FLAG_BUY_PENDING) != 0 && !bought;
}
bool ota_bought(void) { return bought; }

static void do_buy(void *p) {
    static uint8_t __attribute__((aligned(4))) workarea[4096];
    *(int *)p = rom_explicit_buy(workarea, sizeof workarea);
}

int ota_buy(void) {
    if (!ota_buy_pending()) return 0;
    int rc = -1;
    watchdog_update();
    /* Runs the ROM against flash with XIP off for the duration; the same
     * guard fs.c uses for its writes. */
    int fs = flash_safe_execute(do_buy, &rc, 2000);
    if (fs != PICO_OK) rc = fs;
    if (rc == 0) {
        bought = true;
        rom_get_boot_info(&boot);
        /* A downgrade: the other slot still holds a HIGHER version, which the
         * ROM would prefer on the next normal boot. Erase its first sector so
         * the slot we just proved is the one that boots. (The ROM's own
         * bookkeeping for this is what the wrapped pick function above
         * stopped relying on.) */
        int me = ota_slot_active();
        uint16_t ma, mi, oa, oi;
        if (me >= 0 && ota_slot_version(me, &ma, &mi) && ota_slot_version(me ^ 1, &oa, &oi) &&
            ((oa << 16) | oi) > ((ma << 16) | mi)) {
            printf("ota: other slot is v%u.%u > ours v%u.%u: erasing it after the buy\n", oa, oi, ma, mi);
            ota_slot_erase_sector(me ^ 1, 0);
        }
    }
    printf("ota: explicit_buy -> %d (tbyb now %02x)\n", rc, boot.tbyb_and_update_info);
    return rc;
}

/* Walk the picobin block at the start of the slot for its VERSION item.
 * Block: MARKER_START, items..., a last item (0xff type) and MARKER_END.
 * Every item starts with a header word: type in the low byte, and for the
 * "1-byte size" types (bit 6 set) the size in words in the next byte. */
bool ota_slot_version(int which, uint16_t *major, uint16_t *minor) {
    const uint8_t *b = ota_slot_bytes(which);
    if (!b) return false;
    const uint32_t *w = (const uint32_t *)b;
    /* The block is within the first sector for every image picotool makes. */
    for (size_t i = 0; i < 1024 - 4; i++) {
        if (w[i] != PICOBIN_BLOCK_MARKER_START) continue;
        size_t j = i + 1;
        while (j < 1024 - 2) {
            uint32_t h = w[j];
            uint8_t type = h & 0x7f;
            if ((h & 0xff) == 0xff) break;                       /* last item */
            uint32_t size = (h & 0x40) ? ((h >> 8) & 0xff) : ((h >> 8) & 0xffff);
            if (size == 0) break;
            if (type == (PICOBIN_BLOCK_ITEM_1BS_VERSION & 0x7f) && size >= 2) {
                uint32_t v = w[j + 1];
                *major = (uint16_t)(v >> 16);
                *minor = (uint16_t)v;
                return true;
            }
            j += size;
        }
        return false;
    }
    return false;
}

void ota_slot_version_str(int which, char *dst, size_t cap) {
    uint16_t ma, mi;
    if (ota_slot_version(which, &ma, &mi)) snprintf(dst, cap, "%u.%u.0", ma, mi);
    else snprintf(dst, cap, "0.0.0");
}

/*
 * Erase/program through hardware_flash (RAM-resident, raw storage offsets),
 * the same path fs.c has used for every LittleFS write, inside
 * flash_safe_execute. rom_flash_op() was tried first: the first sector
 * written through it took the USB-NCM link down every time (the host lost
 * the interface while the board kept running), and the ROM's own bookkeeping
 * is not needed -- the partition table's permissions only matter to the ROM.
 * Timings are logged for the first operations so a regression is visible.
 */
typedef struct { bool erase; uint32_t off; const uint8_t *page; } fop_t;
static void do_fop(void *p) {
    fop_t *f = p;
    if (f->erase) flash_range_erase(f->off, 4096);
    else flash_range_program(f->off, f->page, 256);
}
static uint32_t fop_logged;
static int fop(bool erase, uint32_t off, const uint8_t *page) {
    fop_t f = { .erase = erase, .off = off, .page = page };
    uint64_t t0 = time_us_64();
    int rc = flash_safe_execute(do_fop, &f, 2000);
    uint32_t us = (uint32_t)(time_us_64() - t0);
    if (fop_logged < 6 || us > 100000) {
        fop_logged++;
        printf("ota: %s @%06lx took %lu us (rc %d)\n", erase ? "erase  " : "program", (unsigned long)off, (unsigned long)us, rc);
    }
    return rc == PICO_OK ? 0 : rc;
}

int ota_slot_erase_sector(int which, uint32_t off) {
    if (!have_slots || which < 0 || which > 1 || off + 4096 > slot[which].size) return -1;
    return fop(true, slot[which].start + off, NULL);
}

int ota_slot_program_page(int which, uint32_t off, const uint8_t page[256]) {
    if (!have_slots || which < 0 || which > 1 || off + 256 > slot[which].size) return -1;
    return fop(false, slot[which].start + off, page);
}

void ota_reboot(bool flash_update_other) {
    int me = ota_slot_active();
    if (flash_update_other && me >= 0) {
        uint32_t base = XIP_BASE + slot[me ^ 1].start;
        printf("ota: flash-update reboot into slot %c @%08lx\n", "AB"[me ^ 1], (unsigned long)base);
        rom_reboot(REBOOT2_FLAG_REBOOT_TYPE_FLASH_UPDATE, 100, base, 0);
    } else {
        printf("ota: reboot\n");
        watchdog_reboot(0, 0, 50);
    }
    for (;;) { __asm volatile("wfi"); }
}
