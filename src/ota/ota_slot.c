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
#include "pico/cyw43_arch.h"
#include "autoconf.h"
#if CONFIG_BT_LE
#include "btstack.h"
#include "bt/bt.h"     /* bt_stack_up(): never enter BTstack if it was never built */
#endif
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
    int i = 1, p = 0;
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
        (void)id;
        p++;
    }
    /* NB: this used to also call rom_pick_ab_partition_during_update() here
     * purely to LOG the chosen CYW43 firmware partition -- a heavy, image-
     * verifying ROM routine run an extra time on every boot for a debug line,
     * duplicating the pick the CYW43 driver already does during init. It was
     * removed: it did nothing functional and ran non-essential ROM code in
     * the pre-watchdog boot window where an intermittent lockup was seen. The
     * partition-table listing above is a lightweight read-only ROM query and
     * stays. */
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

static inline uint32_t primask(void) {
    uint32_t v;
    __asm volatile ("mrs %0, primask" : "=r" (v));
    return v;
}

int ota_buy(void) {
    if (!ota_buy_pending()) return 0;
    static uint8_t __attribute__((aligned(4))) workarea[4096];
    watchdog_update();
    /* NOT inside our own flash_safe_execute(): since SDK 2.3.0
     * rom_explicit_buy() is a wrapper that runs the ROM call under
     * flash_safe_execute() itself, and that helper keeps ONE saved interrupt
     * state per core. Nested, the inner enter saves "disabled" over the
     * outer's "enabled", and both exits restore "disabled": PRIMASK stays set
     * for good. The main loop carries on (WiFi, WireGuard, HTTP are polled),
     * but no interrupt is ever taken again -- USB's first of all, so USB-NCM
     * dies with both bulk endpoints complete in hardware and unserviced
     * (fw 1.95 confirm, 2026-10-02: ICSR showed thread mode with IRQ 14
     * pending; root-caused through the probe). The same nesting is what
     * "rom_flash_op() took the USB-NCM link down" was, below. */
    uint64_t t0 = time_us_64();
    int rc = rom_explicit_buy(workarea, sizeof workarea);
    uint32_t took_us = (uint32_t)(time_us_64() - t0);
    if (rc == 0) {
        bought = true;
        rom_get_boot_info(&boot);
        /* The ROM's explicit_buy already erases the other slot's first sector
         * on a version downgrade (RP2350 datasheet 5.1.17), so we do NOT do it
         * again here -- that was a redundant second flash blackout. */
    }
    printf("ota: explicit_buy -> %d in %lu us (tbyb now %02x, interrupts %s)\n",
           rc, (unsigned long)took_us, boot.tbyb_and_update_info,
           primask() ? "STILL MASKED" : "on");
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
 * the interface while the board kept running) -- almost certainly the same
 * nested flash_safe_execute as ota_buy's (SDK 2.3.0's rom_flash_op wraps
 * itself too), which leaves interrupts masked for good. Either way the ROM's
 * own bookkeeping is not needed -- the partition table's permissions only
 * matter to the ROM. Never call a pico_bootrom rom_* helper from inside
 * flash_safe_execute.
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

/*
 * Quiesce the shared CYW43 radio before ANY reboot we issue.
 *
 * rom_reboot() and watchdog_reboot() are WARM resets: they reset the RP2350
 * but NOT the external CYW43, which sits on the SPI/PIO bus with its own power
 * domain. Rebooting while the chip is mid-transfer (or with its IRQ asserted)
 * left it in a state the next cyw43_arch_init() could not recover -- the bus
 * wedged so hard the core could not even be halted over SWD, and only a COLD
 * power-cycle cleared it. The SDK's own OTA example deinits the radio right
 * before its flash-update reboot for exactly this reason; we did not, and it
 * bit intermittently on the update boot. Bring BT down, drop both interfaces,
 * and deinit the driver (which resets/powers down the chip) so the next boot
 * starts from a clean radio.
 */
static void reboot_quiesce(void) {
    /* Cut power to the CYW43 and do NOTHING else.
     *
     * This used to politely tear the radio down -- hci_power_control(OFF),
     * cyw43_arch_disable_ap_mode(), cyw43_arch_disable_sta_mode(),
     * cyw43_arch_deinit() -- and every one of those can block forever. They run
     * chip ioctls and lwIP teardown that bottom out in sleep_until()'s bare
     * __wfe(), which only returns if a timer alarm IRQ fires; mid-teardown of
     * the very async context that owns that machinery, it may not. Caught on
     * the bench twice, both INSIDE this function: once in our own sleep_ms(),
     * once in cyw43_arch_disable_sta_mode -> cyw43_cb_tcpip_deinit ->
     * netif_remove. A hang here is the worst place for one, because the reset
     * is never issued: the device does not reboot, and it is dark on every
     * transport until somebody power-cycles it. That is precisely the "applied
     * an OTA and it never came back" failure.
     *
     * The goal was only ever to leave the chip in a state the next
     * cyw43_arch_init() can recover. Dropping WL_REG_ON powers it down, which
     * is a stronger guarantee than any teardown sequence -- an unpowered chip
     * cannot be left mid-transfer -- and a GPIO write cannot block. The RP2350
     * reset that follows reinitialises the SPI/PIO side anyway.
     */
    if (!cyw43_is_initialized(&cyw43_state)) {
        return;                 /* radio never came up; nothing to power down */
    }
    gpio_init(CYW43_PIN_WL_REG_ON);
    gpio_set_dir(CYW43_PIN_WL_REG_ON, GPIO_OUT);
    gpio_put(CYW43_PIN_WL_REG_ON, 0);
    busy_wait_ms(20);           /* let its rails fall before the reset */
}

void ota_reboot(bool flash_update_other) {
    int me = ota_slot_active();
    reboot_quiesce();
    if (flash_update_other && me >= 0) {
        uint32_t base = XIP_BASE + slot[me ^ 1].start;
        printf("ota: flash-update reboot into slot %c @%08lx\n", "AB"[me ^ 1], (unsigned long)base);
        rom_reboot(REBOOT2_FLAG_REBOOT_TYPE_FLASH_UPDATE, 500, base, 0);
    } else {
        printf("ota: reboot\n");
        watchdog_reboot(0, 0, 100);
    }
    for (;;) { __asm volatile("wfi"); }
}
