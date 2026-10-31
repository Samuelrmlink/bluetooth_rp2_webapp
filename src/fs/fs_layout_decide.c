#include "fs_layout_map.h"

#include <string.h>

static const char *const PH_NAME[] = {
    "none", "copying", "verified", "formatting", "restored", "slota"
};

const char *fs_layout_phase_name(lay_phase_t phase) {
    if ((unsigned)phase > LAY_PH_SLOTA) { return "none"; }
    return PH_NAME[phase];
}

lay_phase_t fs_layout_phase_parse(const char *name) {
    if (!name) { return LAY_PH_NONE; }
    for (int i = 0; i <= LAY_PH_SLOTA; i++) {
        if (strcmp(name, PH_NAME[i]) == 0) { return (lay_phase_t)i; }
    }
    return LAY_PH_NONE;
}

lay_act_t fs_layout_decide(bool old_mounted, lay_phase_t phase, bool armed, bool buy_pending,
                           bool state_known) {
    /* Once formatting is on the card the old superblock is not a filesystem,
     * even when a read of 0x200000 still returns one. */
    if (phase >= LAY_PH_FORMATTING) { return LAY_ACT_MOUNT_NEW; }
    if (phase == LAY_PH_COPYING || phase == LAY_PH_VERIFIED) {
        return old_mounted ? LAY_ACT_MOVE : LAY_ACT_HOLD;
    }
    /* No card, or a card we could not read: do not invent a format. */
    if (!state_known) { return old_mounted ? LAY_ACT_IDLE : LAY_ACT_HOLD; }
    if (!old_mounted) { return LAY_ACT_FORMAT_OLD; }
    if (armed && buy_pending) { return LAY_ACT_WAIT_BUY; }
    if (armed) { return LAY_ACT_MOVE; }
    return LAY_ACT_IDLE;
}

bool fs_layout_may_format_old(lay_act_t act, lay_phase_t phase) {
    return act == LAY_ACT_FORMAT_OLD && phase == LAY_PH_NONE;
}

uint32_t fs_layout_mount_off(lay_act_t act) {
    if (act == LAY_ACT_MOUNT_NEW) { return LAY_FS_NEW; }
    return LAY_FS_OLD;
}

bool fs_layout_map_ok(void) {
    if (LAY_PT_OFF != 0 || LAY_PT_SIZE != 64u * 1024u) { return false; }
    if (LAY_SLOT_A != LAY_PT_SIZE) { return false; }
    if (LAY_SLOT_B != LAY_SLOT_A + LAY_SLOT_SIZE) { return false; }
    if (LAY_BLOB_NEW != LAY_SLOT_B + LAY_SLOT_SIZE) { return false; }
    if (LAY_FS_NEW != LAY_BLOB_NEW + LAY_BLOB_SIZE) { return false; }
    if (LAY_PEX_NEW != LAY_FS_NEW + LAY_FS_NEW_SIZE) { return false; }
    if (LAY_NVM != LAY_PEX_NEW + LAY_PEX_NEW_SIZE) { return false; }
    if (LAY_NVM + LAY_NVM_SIZE != 4u * 1024u * 1024u) { return false; }
    if (LAY_FS_BLOCKS != 300u) { return false; }
    if (LAY_PEX_NEW + LAY_PEX_OLD_SIZE != LAY_PEX_OLD) { return false; }
    if (LAY_PEX_OLD + LAY_PEX_OLD_SIZE != LAY_NVM) { return false; }
    if (LAY_FS_OLD != 2u * 1024u * 1024u || LAY_OLD_SUPER != LAY_FS_OLD) { return false; }
    if (LAY_BLOB_OLD + LAY_BLOB_SIZE != LAY_OLD_SUPER) { return false; }
    if (LAY_OLD_SUPER + 4096u > LAY_BLOB_NEW) { return false; }
    if (LAY_SLOT_A + LAY_OLD_SLOT_SIZE != LAY_OLD_SLOT_B) { return false; }
    if (LAY_OLD_SLOT_B + LAY_OLD_SLOT_SIZE != LAY_BLOB_OLD) { return false; }
    if (LAY_SLOT_A + LAY_OLD_SLOT_SIZE > LAY_SLOT_B) { return false; }
    if (LAY_BLOB_NEW < LAY_OLD_SLOT_B + LAY_OLD_SLOT_SIZE) { return false; }
    if (LAY_PEX_NEW <= LAY_BLOB_OLD) { return false; }
    if (LAY_SLACK_BLOCKS != 8u) { return false; }
    return true;
}

/* Picobin partition-table item, the fields picotool writes. Little-endian
 * loads so the host test and the board read the same bytes. */
#define LAY_MARK_START 0xffffded3u
#define LAY_PT_ITEM    0x0au
#define LAY_LOC_FIRST  0x00001fffu
#define LAY_LOC_LAST   0x03ffe000u
#define LAY_PERM       0xfc000000u
#define LAY_HAS_ID     0x00000001u
#define LAY_HAS_NAME   0x00001000u
#define LAY_EXTRA_LSB  7
#define LAY_EXTRA_BITS 0x00000180u

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

bool fs_layout_pt_matches(const uint8_t *bin, uint32_t len) {
    if (!bin || len < 16 || (len % 4) != 0) { return false; }
    uint32_t nwords = len / 4;
    int marker = -1;
    for (uint32_t i = 0; i < nwords; i++) {
        if (rd32(bin + i * 4) == LAY_MARK_START) { marker = (int)i; break; }
    }
    if (marker < 0) { return false; }
    uint32_t j = (uint32_t)marker + 1;
    while (j + 1 < nwords) {
        uint32_t h = rd32(bin + j * 4);
        if ((h & 0xffu) == 0xffu) { break; }
        uint32_t size = (h & 0x40u) ? ((h >> 8) & 0xffu) : ((h >> 8) & 0xffffu);
        if (size < 2 || j + size > nwords) { return false; }
        if ((h & 0x7fu) == LAY_PT_ITEM) {
            if (((h >> 24) & 0x0fu) != 5u) { return false; }
            /* payload starts at the word after the header */
            uint32_t base = j + 1;
            uint32_t pend = j + size;
            uint32_t k = base + 1; /* skip unpartitioned flags */
            static const uint32_t want_off[5] = {
                LAY_SLOT_A, LAY_SLOT_B, LAY_BLOB_NEW, LAY_FS_NEW, LAY_PEX_NEW
            };
            static const uint32_t want_sz[5] = {
                LAY_SLOT_SIZE, LAY_SLOT_SIZE, LAY_BLOB_SIZE, LAY_FS_NEW_SIZE, LAY_PEX_NEW_SIZE
            };
            static const uint64_t want_id[5] = { 0, 0, LAY_ID_WIFI, LAY_ID_LFS, LAY_ID_PEX };
            static const int want_has[5] = { 0, 0, 1, 1, 1 };
            for (int p = 0; p < 5; p++) {
                if (k + 2 > pend) { return false; }
                uint32_t loc = rd32(bin + k * 4);
                uint32_t flags = rd32(bin + (k + 1) * 4);
                k += 2;
                uint32_t first = loc & LAY_LOC_FIRST;
                uint32_t last = (loc & LAY_LOC_LAST) >> 13;
                uint32_t off = first * 4096u;
                uint32_t sz = (last + 1u - first) * 4096u;
                if (off != want_off[p] || sz != want_sz[p]) { return false; }
                uint32_t fl = flags & ~LAY_PERM;
                uint64_t id = 0;
                int has = (fl & LAY_HAS_ID) != 0;
                if (has) {
                    if (k + 2 > pend) { return false; }
                    id = (uint64_t)rd32(bin + k * 4) | ((uint64_t)rd32(bin + (k + 1) * 4) << 32);
                    k += 2;
                }
                if (want_has[p] && (!has || id != want_id[p])) { return false; }
                uint32_t nf = (fl & LAY_EXTRA_BITS) >> LAY_EXTRA_LSB;
                if (k + nf > pend) { return false; }
                k += nf;
                if (fl & LAY_HAS_NAME) {
                    if (k >= pend) { return false; }
                    uint32_t w0 = rd32(bin + k * 4);
                    uint32_t name_len = w0 & 0xffu;
                    /* one word already, plus name_len/4 more (picotool's pad) */
                    uint32_t step = 1u + name_len / 4u;
                    if (k + step > pend) { return false; }
                    k += step;
                }
            }
            return true;
        }
        j += size;
    }
    return false;
}
