#ifndef BTGW_FS_BATCH_H
#define BTGW_FS_BATCH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* One shared path table for a chosen-list tar or a batch delete. 32 paths,
 * 127 characters plus a NUL. The two operations never run together. */
#define FS_BATCH_MAX 32
#define FS_BATCH_PATH 128

/* Parse {"paths":["a","b"],"mtime":N,"id":N}. 0 ok, -1 bad (missing paths,
 * an empty array, an empty string, or a path that does not fit), -2 when
 * there are more than FS_BATCH_MAX paths. Does not truncate. */
int fs_batch_parse(const char *json, size_t len,
                   char paths[][FS_BATCH_PATH], int *npaths,
                   uint32_t *mtime, uint32_t *id);

/* True when path is a root or sits under one (root/...). "pex" does not
 * cover "pexfoo". */
bool fs_batch_covers(const char roots[][FS_BATCH_PATH], int n, const char *path);

/* The next index to act on. Closed: -1 and *index stays. Finished
 * (*index >= total): -2. Otherwise the current index, then *index advances. */
int fs_batch_next(int *index, int total, bool closed);

#endif
