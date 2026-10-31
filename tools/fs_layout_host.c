/* Host check for the layout mover's pure decisions. No SDK.
 *   gcc -Wall -Wextra -Werror -Isrc/fs tools/fs_layout_host.c src/fs/fs_layout_decide.c
 *   argv: <next-table.bin> <old-table.bin>
 */
#include "fs_layout_map.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int ncheck, nfail;

static void check(int cond, const char *msg) {
    ncheck++;
    if (!cond) {
        fprintf(stderr, "fail: %s\n", msg);
        nfail++;
    }
}

static int load(const char *path, uint8_t **out, uint32_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) { return 0; }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 0; }
    long sz = ftell(f);
    if (sz <= 0 || sz > 8192) { fclose(f); return 0; }
    rewind(f);
    uint8_t *b = malloc((size_t)sz);
    if (!b || fread(b, 1, (size_t)sz, f) != (size_t)sz) {
        free(b);
        fclose(f);
        return 0;
    }
    fclose(f);
    *out = b;
    *n = (uint32_t)sz;
    return 1;
}

int main(int argc, char **argv) {
    char msg[96];
    check(argc == 3, "need next.bin and old.bin");
    check(fs_layout_map_ok(), "map does not abut");
    check(fs_layout_phase_parse("formatting") == LAY_PH_FORMATTING, "parse formatting");
    check(fs_layout_phase_parse("nope") == LAY_PH_NONE, "unknown phase");
    check(strcmp(fs_layout_phase_name(LAY_PH_SLOTA), "slota") == 0, "name slota");

    for (int ph = 0; ph <= LAY_PH_SLOTA; ph++) {
        for (int mounted = 0; mounted < 2; mounted++) {
            for (int armed = 0; armed < 2; armed++) {
                for (int buy = 0; buy < 2; buy++) {
                    for (int known = 0; known < 2; known++) {
                        lay_act_t act = fs_layout_decide(mounted, (lay_phase_t)ph, armed, buy, known);
                        snprintf(msg, sizeof msg, "ph %d m%d a%d b%d k%d -> %d",
                                 ph, mounted, armed, buy, known, (int)act);
                        if (ph >= LAY_PH_FORMATTING) {
                            check(act == LAY_ACT_MOUNT_NEW, msg);
                            check(fs_layout_mount_off(act) == 0x250000u, msg);
                        }
                        if (!known) { check(act != LAY_ACT_FORMAT_OLD, msg); }
                        check(!fs_layout_may_format_old(act, (lay_phase_t)ph) ||
                              (act == LAY_ACT_FORMAT_OLD && ph == LAY_PH_NONE), msg);
                        if (act == LAY_ACT_MOUNT_NEW) {
                            check(fs_layout_mount_off(act) == 0x250000u, msg);
                        } else {
                            check(fs_layout_mount_off(act) == 0x200000u, msg);
                        }
                    }
                }
            }
        }
    }

    check(fs_layout_decide(1, LAY_PH_NONE, 0, 0, 1) == LAY_ACT_IDLE, "idle");
    check(fs_layout_decide(1, LAY_PH_NONE, 1, 0, 1) == LAY_ACT_MOVE, "armed move");
    check(fs_layout_decide(1, LAY_PH_NONE, 1, 1, 1) == LAY_ACT_WAIT_BUY, "wait for buy");
    check(fs_layout_decide(0, LAY_PH_NONE, 0, 0, 1) == LAY_ACT_FORMAT_OLD, "format old");
    check(fs_layout_may_format_old(LAY_ACT_FORMAT_OLD, LAY_PH_NONE), "may format old");
    check(!fs_layout_may_format_old(LAY_ACT_FORMAT_OLD, LAY_PH_FORMATTING), "no format after ponr");
    check(fs_layout_decide(0, LAY_PH_NONE, 0, 0, 0) == LAY_ACT_HOLD, "unknown unmounted");
    check(fs_layout_decide(1, LAY_PH_NONE, 1, 0, 0) == LAY_ACT_IDLE, "unknown mounted");
    check(fs_layout_decide(1, LAY_PH_COPYING, 0, 0, 1) == LAY_ACT_MOVE, "resume copy");
    check(fs_layout_decide(0, LAY_PH_VERIFIED, 1, 0, 1) == LAY_ACT_HOLD, "verified, old gone");
    check(fs_layout_decide(1, LAY_PH_FORMATTING, 0, 0, 0) == LAY_ACT_MOUNT_NEW, "ponr without card");

    if (argc == 3) {
        uint8_t *next = NULL, *old = NULL;
        uint32_t nn = 0, on = 0;
        check(load(argv[1], &next, &nn), "read next table");
        check(load(argv[2], &old, &on), "read old table");
        if (next) {
            check(fs_layout_pt_matches(next, nn), "next table matches the map");
            check(!fs_layout_pt_matches(next, 0), "empty rejected");
            check(!fs_layout_pt_matches(next, 3), "short rejected");
            check(!fs_layout_pt_matches(NULL, nn), "null rejected");
            int flipped = 0;
            for (uint32_t i = 0; i < nn; i++) {
                uint8_t save = next[i];
                next[i] = (uint8_t)(save ^ 0xffu);
                if (!fs_layout_pt_matches(next, nn)) { flipped++; }
                next[i] = save;
            }
            check(flipped > 0, "a mutated table was accepted");
        }
        /* partitions.json is already the destination map (1 MiB slots,
         * LittleFS at 0x250000). The mover used to prove that the live
         * 864 KiB table was NOT that map. It now proves the live table is. */
        if (old) { check(fs_layout_pt_matches(old, on), "live partition table is not the mover destination"); }
        free(next);
        free(old);
    }

    if (nfail) {
        fprintf(stderr, "%d of %d checks failed\n", nfail, ncheck);
        return 1;
    }
    printf("%d checks\n", ncheck);
    return 0;
}
