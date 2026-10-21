/* fs.c's insides, for fs_migrate.c alone: the migration walks, reads and
 * rewrites the whole filesystem at boot, before anything else has a file
 * open, which the handle API in fs.h is not shaped for. */
#ifndef BTGW_FS_INTERNAL_H
#define BTGW_FS_INTERNAL_H

#include <stdint.h>
#include "littlefs/lfs.h"

lfs_t *fs_lfs(void);                 /* NULL when not mounted */
uint32_t fs_mounted_blocks(void);
/* Unmount, format `blocks` blocks from FS_FLASH_OFF, mount. 0 or LFS_ERR_*. */
int fs_reformat(uint32_t blocks);

#endif
