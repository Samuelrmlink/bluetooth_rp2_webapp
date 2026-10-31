/* Boot and operator entry points for the layout mover. The decisions live
 * in fs_layout_map.h so a host build can test them without the SDK. */
#ifndef BTGW_FS_LAYOUT_H
#define BTGW_FS_LAYOUT_H

#include "autoconf.h"
#include <stdbool.h>
#include <stddef.h>

#if CONFIG_FS_LAYOUT
void fs_layout_boot(void);
bool fs_layout_suppress_migrate(void);
/* >0: sys.info should use this object. 0: the old migrator's line still applies. */
int fs_layout_status_json(char *buf, size_t cap);
int fs_layout_dry_json(char *buf, size_t cap);
/* 0 written, -1 files do not fit, -2 card, -3 already started, -4 buy pending. */
int fs_layout_arm(void);
/* confirm must be "write-table". 0: table matches and a normal reboot is scheduled. */
int fs_layout_commit(const char *confirm, char *err, size_t errcap);
#else
static inline void fs_layout_boot(void) {}
static inline bool fs_layout_suppress_migrate(void) { return false; }
static inline int fs_layout_status_json(char *buf, size_t cap) {
    (void)buf; (void)cap; return 0;
}
#endif

#endif
