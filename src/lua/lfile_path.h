/* Volume-qualified paths for the Lua `file` module, as pure logic so
 * test/host/lfile_path_test.c can drive it.
 *
 *   lfs:/a/b.ext   LittleFS, the tree /api/fs/ serves      -> LFILE_LFS, "a/b.ext"
 *   sd:/a/b.ext    the SD card, the tree /api/sd/files/ serves -> LFILE_SD, "a/b.ext"
 *   lfs:/  sd:/    the volume's root                         -> "" (a directory)
 *
 * The slash after the colon is optional and repeated or trailing slashes are
 * dropped (`sd:logs/` == `sd:/logs`), but an empty segment in the middle, a
 * "." or ".." segment, a NUL, or anything over the limits is a bad path.
 * Limits are fs.h's on both volumes: a segment at most FS_NAME_MAX (64)
 * bytes, the path at most FS_PATH_MAX (128).
 *
 * Characters: LittleFS takes what fs_valid_path() takes ([A-Za-z0-9._-], no
 * leading '.'), so a path this accepts is one the HTTP API serves too. The SD
 * card takes any FAT long-name character: everything but control characters,
 * DEL and \ : * ? " < > |. */
#ifndef BTGW_LFILE_PATH_H
#define BTGW_LFILE_PATH_H

#include <stdbool.h>
#include <stddef.h>

#define LFILE_LFS 1
#define LFILE_SD  2

#define LFILE_SEG_MAX  64
#define LFILE_PATH_MAX 128

/* Returns the volume, or 0 for a bad path. `out` (at least LFILE_PATH_MAX+1)
 * receives the volume-relative path, "" for the root. `len` is the length of
 * `in`, which need not be NUL-terminated (a Lua string may hold a NUL). */
int lfile_parse_path(const char *in, size_t len, char *out, size_t cap);

#endif
