/* fs.c's insides, for fs_migrate.c alone: the migration walks, reads and
 * rewrites the whole filesystem at boot, before anything else has a file
 * open, which the handle API in fs.h is not shaped for. */
#ifndef BTGW_FS_INTERNAL_H
#define BTGW_FS_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>
#include "littlefs/lfs.h"

lfs_t *fs_lfs(void);                 /* NULL when not mounted */
uint32_t fs_mounted_blocks(void);
bool fs_is_mounted(void);
void fs_unmount(void);
/* read_max_blocks 0 keeps the historical LittleFS-region cap. */
void fs_set_flash_off(uint32_t off, uint32_t read_max_blocks);
uint32_t fs_flash_off(void);
/* After the layout move's point of no return: fs_reformat refuses off == FS_FLASH_OFF. */
void fs_freeze_old(bool freeze);
/* Mount exactly `blocks` at `off`. A superblock with any other count is not mounted. */
int fs_mount_at(uint32_t off, uint32_t blocks);
/* Unmount, format `blocks` blocks from the current offset, mount. 0 or LFS_ERR_*. */
int fs_reformat(uint32_t blocks);

#endif
