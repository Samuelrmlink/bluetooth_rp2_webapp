/* The next 4 MB map, and the pure decisions the mover is allowed to make.
 *
 * This image does not switch partitions.json, FS_FLASH_OFF, or the linker
 * length. Those still describe the table the board is running. The numbers
 * here are what the mover copies toward, and what a later explicit commit
 * writes. A boot with no arm file does not use them.
 *
 * Host-tested (tools/run_tests.py). No Pico headers: gcc builds this file. */
#ifndef BTGW_FS_LAYOUT_MAP_H
#define BTGW_FS_LAYOUT_MAP_H

#include <stdbool.h>
#include <stdint.h>

#define LAY_PT_OFF        0x000000u
#define LAY_PT_SIZE       (64u * 1024u)
#define LAY_SLOT_A        0x010000u
#define LAY_SLOT_SIZE     (1024u * 1024u)
#define LAY_SLOT_B        0x110000u
#define LAY_BLOB_NEW      0x210000u
#define LAY_BLOB_SIZE     (256u * 1024u)
#define LAY_FS_NEW        0x250000u
#define LAY_FS_NEW_SIZE   (1200u * 1024u)
#define LAY_FS_BLOCKS     (LAY_FS_NEW_SIZE / 4096u)
#define LAY_PEX_NEW       0x37C000u
#define LAY_PEX_NEW_SIZE  (512u * 1024u)
#define LAY_PEX_OLD       0x3BC000u
#define LAY_PEX_OLD_SIZE  (256u * 1024u)
#define LAY_NVM           0x3FC000u
#define LAY_NVM_SIZE      (16u * 1024u)
#define LAY_FS_OLD        (2u * 1024u * 1024u)
#define LAY_OLD_SUPER     LAY_FS_OLD
#define LAY_BLOB_OLD      0x1C0000u
#define LAY_OLD_SLOT_SIZE (864u * 1024u)
#define LAY_OLD_SLOT_B    0x0E8000u
#define LAY_SLACK_BLOCKS  8u

/* Wifi / LittleFS / PEX partition ids. Main A/B carry no id check. */
#define LAY_ID_WIFI  0x776966696669726dull
#define LAY_ID_LFS   0x6c66730000000000ull
#define LAY_ID_PEX   0x7065780000000000ull

typedef enum {
    LAY_PH_NONE = 0,
    LAY_PH_COPYING,
    LAY_PH_VERIFIED,
    LAY_PH_FORMATTING,   /* point of no return: never mount or format 0x200000 */
    LAY_PH_RESTORED,
    LAY_PH_SLOTA,
} lay_phase_t;

typedef enum {
    LAY_ACT_IDLE = 0,
    LAY_ACT_FORMAT_OLD,  /* nothing mounted, no move state: today's recovery */
    LAY_ACT_MOUNT_NEW,   /* move has started: 0x250000, never the old volume */
    LAY_ACT_MOVE,        /* arm file, image bought, old volume still trusted */
    LAY_ACT_WAIT_BUY,    /* arm file, but this boot is still try-before-you-buy */
    LAY_ACT_HOLD,        /* a copy was started and the old volume will not mount */
} lay_act_t;

lay_phase_t fs_layout_phase_parse(const char *name);
const char *fs_layout_phase_name(lay_phase_t phase);
/* state_known: the card was read. Unknown state must not format the old volume. */
lay_act_t fs_layout_decide(bool old_mounted, lay_phase_t phase, bool armed, bool buy_pending,
                           bool state_known);
/* FORMAT_OLD is the only act that may format LAY_FS_OLD, and only from NONE. */
bool fs_layout_may_format_old(lay_act_t act, lay_phase_t phase);
uint32_t fs_layout_mount_off(lay_act_t act);
bool fs_layout_map_ok(void);
/* The embedded picotool table names the five regions above. */
bool fs_layout_pt_matches(const uint8_t *bin, uint32_t len);

#endif
