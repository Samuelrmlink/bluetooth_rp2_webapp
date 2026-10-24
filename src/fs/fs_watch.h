#ifndef BTGW_FS_WATCH_H
#define BTGW_FS_WATCH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fs/fs.h"

/* FNV-1a, the same function the editor uses on the bytes it shows. */
#define FS_FNV_INIT 2166136261u
uint32_t fs_fnv_feed(uint32_t h, const uint8_t *p, size_t n);

/* Queue a change. Safe from the main loop and from the SD worker, as long
 * as the caller does not yield inside the call. `path` is an editor path
 * (`lua/x.lua`, `sd/projects/a/src/a.c`). */
void fs_watch_push(const char *path, const char *op, uint32_t size, uint32_t hash, bool has_hash);

/* Main loop: emit queued fs.changed events, and fs.stated when a stat
 * that had to read the card has finished. */
void fs_watch_poll(void);

/* The second LittleFS listener. Registered at startup. */
void fs_watch_on_lfs(const char *name, const char *op);

/* Worker: hash and stat one queued SD path, or do nothing. True when it
 * touched the card. */
bool fs_watch_sd_slice(void);

/* Begin a stat from the fs.stat params object. LittleFS rows are filled
 * here. SD rows are filled by fs_watch_sd_slice.
 * 0 started, -1 the params have no "paths" array, -2 a stat is in flight,
 * -3 more than FS_STAT_MAX paths. */
#define FS_STAT_MAX 32
int fs_watch_stat_begin(const char *params);

/* 0 idle, 1 waiting on the card, 2 the result is ready to emit (or already
 * answered, for a LittleFS-only request). */
int fs_watch_stat_state(void);

/* A LittleFS-only stat already went out in the op reply. Drop the ready
 * flag so the main loop does not emit fs.stated as well. */
void fs_watch_stat_drop(void);

/* Write the result JSON into dst. False if it does not fit. */
bool fs_watch_stat_json(char *dst, size_t cap);

/* Worker only. `rel` is the card path. Hashes the stored bytes when the
 * file is at most FS_HASH_MAX, then queues fs.changed as `sd/<rel>`. */
void fs_watch_note_sd(const char *rel, const char *op);

#endif
