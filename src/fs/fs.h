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
/* See the read hook in fs.c: on RP2350 flash is read through the alias that
 * ignores QMI address translation, so the filesystem stays reachable when
 * the firmware is booted from an A/B slot (src/ota/). */
#include "hardware/regs/addressmap.h"   /* the #ifdef below is false without it: that fault was found on the bench */
#ifdef XIP_NOCACHE_NOALLOC_NOTRANSLATE_BASE
#define FS_READ_BASE XIP_NOCACHE_NOALLOC_NOTRANSLATE_BASE
#else
#define FS_READ_BASE XIP_BASE
#endif
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

/* Iterate one directory, one level deep, the same reach as fs_list_json.
 *
 * fs_list_json already walks a directory but emits JSON, and the lfs handle
 * is a file-scope static in fs.c, so a caller that wants to build its own
 * answer -- the Lua layer listing every script on the filesystem -- has no
 * way in. This is that way in, and nothing more: still one level, because an
 * unbounded recursive walk over flash is not something to do casually.
 *
 * Returns the number of entries visited, or -1 if `dir` is not a directory. */
typedef void (*fs_walk_fn)(const char *name, bool is_dir, size_t size, void *ctx);
int fs_walk(const char *dir, fs_walk_fn cb, void *ctx);
/* Removes a file or an empty directory; FS_ERR_NOTEMPTY for one that is not. */
#define FS_ERR_NOTEMPTY (-39)
int fs_delete(const char *name);
/* Bytes of flash a file occupies (0 when stored inline in its directory). */
uint32_t fs_file_alloc(const char *name, uint32_t size);
int fs_rename(const char *from, const char *to);
bool fs_stat(const char *name, size_t *size);

int fs_begin_write(const char *name);
/* Returns the NUMBER OF BYTES WRITTEN, negative on error -- it does NOT
 * return 0 on success. Test it with `< 0`. Four call sites tested `!= 0`
 * and so treated every successful write as a failure; one of them skipped
 * fs_end_write() as a result, leaving the single write slot open and making
 * every subsequent fs_begin_write() in the firmware fail. */
int fs_write(const uint8_t *data, size_t len);
int fs_end_write(void);
void fs_abort_write(void);

/* Told the name of every file that was written (on fs_end_write), renamed
 * to (fs_rename) or deleted, after the fact. One listener: the Lua runtime,
 * which recompiles a changed module into its bytecode cache. Called from
 * whatever context did the write, so it must only take a note. */
typedef void (*fs_change_fn)(const char *name);
void fs_on_change(fs_change_fn fn);

int fs_begin_read(const char *name, size_t *size);
int fs_read(int h, uint8_t *buf, size_t cap);
int fs_rewind(int h, size_t n);
void fs_end_read(int h);

#endif
