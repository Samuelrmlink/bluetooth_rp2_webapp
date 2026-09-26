#ifndef KM003C_FS_H
#define KM003C_FS_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Flash layout — see linker/pico_flash_region.ld for the linker-enforced
 * half of this: the linker refuses to place firmware code/rodata/data
 * past FS_FLASH_OFF at all, so these two can never silently overlap no
 * matter how the firmware grows. FS_FLASH_OFF here must match the linker
 * fragment's FLASH LENGTH exactly.
 *
 * Overridable because the two boards this builds for have different parts:
 * the Adafruit Feather USB Host has 8MiB (2MiB firmware + 6MiB fs), while
 * the Pico W test target has only 2MiB and needs 1MiB + 1MiB. Getting this
 * wrong is silent — the defaults put the whole filesystem past the end of a
 * 2MiB chip — so fs.c asserts the region fits the board's real flash. */
/* 4 MB part. Firmware is capped at 2 MiB by linker/pico_flash_region.ld, the
 * filesystem takes the rest, and the last 16 KiB is BTstack's NVM (bonds and
 * the LE device DB) via pico_btstack_flash_bank -- RP2350-E10 spare sector plus
 * two flash banks, per pm220-pico2w. Stay below it. */
#ifndef FS_FLASH_OFF
#define FS_FLASH_OFF (2u * 1024u * 1024u)
#endif
#ifndef FS_BT_TAIL
#define FS_BT_TAIL   (16u * 1024u)
#endif
#ifndef FS_SIZE
#define FS_SIZE      ((2u * 1024u * 1024u) - FS_BT_TAIL)
#endif

#define FS_NAME_MAX 64   /* per path segment */
#define FS_PATH_MAX 128  /* full "a/b/c.ext" path */
#define FS_FILE_MAX (128u * 1024u)
#define FS_READS 8

bool fs_init(void);
bool fs_valid_path(const char *path);
bool fs_is_dir(const char *path);

int fs_list_json(const char *dir, char *buf, size_t cap);
int fs_delete(const char *name);
int fs_rename(const char *from, const char *to);
bool fs_stat(const char *name, size_t *size);

int fs_begin_write(const char *name);
int fs_write(const uint8_t *data, size_t len);
int fs_end_write(void);
void fs_abort_write(void);

int fs_begin_read(const char *name, size_t *size);
int fs_read(int h, uint8_t *buf, size_t cap);
int fs_rewind(int h, size_t n);
void fs_end_read(int h);

#endif
