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
/* 4 MB part (docs/flash-layout.md). From 2 MiB: LittleFS (FS_SIZE), then the
 * PEX partition (FS_PEX_SIZE), and the last 16 KiB is BTstack's NVM (bonds and
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
/* The region LittleFS had to itself until the PEX partition (2032 KiB): the
 * geometry of every filesystem formatted before it, which fs_init() still
 * mounts as it is (fs_legacy()) until fs_migrate.c has moved it. */
#ifndef FS_REGION_SIZE
#define FS_REGION_SIZE ((2u * 1024u * 1024u) - FS_BT_TAIL)
#endif
/* The PEX partition (docs/flash-layout.md, docs/pex-format.md 6.1): native
 * modules executed in place, at the END of the old LittleFS region so
 * LittleFS keeps its start. Only reserved so far -- nothing writes it. */
#ifndef FS_PEX_SIZE
#define FS_PEX_SIZE  (256u * 1024u)
#endif
#ifndef FS_SIZE
#define FS_SIZE      (FS_REGION_SIZE - FS_PEX_SIZE)
#endif
#define FS_PEX_OFF   (FS_FLASH_OFF + FS_SIZE)

#define FS_NAME_MAX 64   /* per path segment */
#define FS_PATH_MAX 128  /* full "a/b/c.ext" path */
/* The largest file PUT /api/fs, SMP fs upload and a Lua `file` handle may
 * write. 128 KiB until the Lua file API arrived; 1 MiB lets a browser drop a
 * firmware image for a peripheral into LittleFS. Nothing in LittleFS or in
 * the streaming paths cares (a 1 MiB file is a CTZ skip-list of 257 blocks);
 * the limit only keeps one file from eating the 2 MiB volume by accident. */
#define FS_FILE_MAX (1024u * 1024u)
#define FS_READS 8

bool fs_init(void);
/* The geometry actually mounted, in bytes: FS_SIZE, or the larger pre-PEX
 * size while a migration is pending (fs_legacy()). 0 when not mounted. */
uint32_t fs_mounted_size(void);
bool fs_legacy(void);
/* One line for /api/fs and sys.info while something about the filesystem is
 * not as it should be ("migration pending: no SD card"); "" otherwise. */
void fs_set_note(const char *note);
const char *fs_note(void);
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

/* A folder ("" = everything) as a ustar archive, streamed (fs.c). begin:
 * 0, or -2 when another archive is already streaming, -3 not a folder;
 * *length is the archive's exact size. read: bytes produced, 0 at the end. */
int fs_tar_begin(const char *folder, uint32_t mtime, uint64_t *length);
/* POST /api/tar {"paths":[...],"mtime":N,"id":N}. 0 ok; -2 too many paths;
 * -3 busy (an archive or a batch delete is already running); -4 a path is
 * missing; -1 bad JSON or a bad name. *length is the archive size. */
int fs_tar_begin_json(const char *json, size_t json_len, uint64_t *length);
void fs_tar_set_job(uint32_t id);
int fs_tar_read(uint8_t *buf, size_t cap);
void fs_tar_end(void);
bool fs_tar_busy(void);
/* POST /api/fs/remove. Same codes as the tar list, without -4: the pump
 * reports a path that will not delete. One path per fs_remove_pump. */
int fs_remove_begin_json(const char *json, size_t json_len);
void fs_remove_abort(void);
void fs_remove_pump(void);
/* 1 finished, 0 still running, -1 failed (*err set), -2 aborted. */
int fs_remove_result(int *removed, int *total, char *err, size_t errcap);
void fs_job_poll(void);
/* Create a folder and its parents. */
int fs_mkdir(const char *path);
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

/* A second listener, for the editor. `op` is "write", "rename", or "delete".
 * Same rule: it runs in the writer's context and must only take a note.
 * Files over FS_HASH_MAX are reported with a size and no hash. */
#define FS_HASH_MAX (256u * 1024u)
typedef void (*fs_watch_fn)(const char *name, const char *op);
void fs_on_watch(fs_watch_fn fn);

/* --- a pool of general-purpose handles (the Lua `file` module) -------------
 * Separate from the single write slot above, which PUT /api/fs, SMP, the
 * bytecode cache and sys.secret_put share: a script holding a file open must
 * never make an upload fail, and the other way round.
 *
 * Each handle has its own cache buffer (lfs_file_opencfg), in memory the
 * caller supplies, so opening one never touches the C heap. Opening a file for writing that anything else
 * has open (a pool handle, the write slot, a reader), or opening for reading a
 * file a pool handle is writing, is refused with FS_ERR_BUSY: LittleFS keeps
 * two handles on one file coherent only in narrow cases, and an upload that
 * replaces a file a script is reading is exactly the case where it does not.
 * fs_delete/fs_rename refuse an open file the same way.
 *
 * Returns >= 0 (a handle) or a negative LFS_ERR_* / FS_ERR_* code. */
#define FS_FILES 4
#define FS_ERR_BUSY    (-16)
#define FS_ERR_NOHANDLE (-1000)
#define FS_ERR_TOOLARGE (-1001)
enum { FS_O_READ = 0, FS_O_WRITE = 1, FS_O_APPEND = 2 };
/* `mem` is fs_file_mem() bytes the caller owns and keeps until fs_fclose()
 * returns (or until fs_fopen fails): free it then. */
size_t fs_file_mem(void);
int fs_fopen(const char *name, int mode, void *mem);
int fs_fread(int h, void *buf, size_t n);
/* Refuses (FS_ERR_TOOLARGE) a write that would take the file past
 * FS_FILE_MAX; nothing is written then. */
int fs_fwrite(int h, const void *buf, size_t n);
int fs_fseek(int h, uint32_t pos);
int32_t fs_ftell(int h);
int32_t fs_fsize(int h);
int fs_fclose(int h);
/* Used and total bytes of the volume (used = allocated blocks). */
bool fs_usage(uint32_t *used, uint32_t *total);
/* Seek a reader (fs_begin_read) to an absolute offset. */
int fs_read_seek(int h, size_t pos);

int fs_begin_read(const char *name, size_t *size);
int fs_read(int h, uint8_t *buf, size_t cap);
int fs_rewind(int h, size_t n);
void fs_end_read(int h);

#endif
