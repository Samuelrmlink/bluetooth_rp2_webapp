/* LittleFS on the "littlefs" partition. Geometry matches the RP2350 volume
 * (256-byte read/prog, 4 KB blocks, 32-byte lookahead, 500 block cycles) so
 * cfg/wifi.json written by either target is the same file. */
#ifndef C5_FS_LFS_H
#define C5_FS_LFS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

int fs_mount(void);
void fs_lock(void);
void fs_unlock(void);

bool fs_stat(const char *path, size_t *size);
bool fs_is_dir(const char *path);
int fs_read_file(const char *path, void *dst, size_t cap, size_t *got);
int fs_write_file(const char *path, const void *src, size_t n);
int fs_mkdir_p(const char *path);
int fs_delete(const char *path);
int fs_rename(const char *from, const char *to);
int fs_list_json(const char *dir, char *buf, size_t cap);

/* Open for a streaming read. The caller locks around each pread. */
typedef struct fs_rd fs_rd_t;
fs_rd_t *fs_open_read(const char *path, uint32_t *size);
int fs_pread(fs_rd_t *f, uint32_t off, void *dst, uint32_t n);
void fs_close_read(fs_rd_t *f);

/* Streaming write. Parent directories are created. */
typedef struct fs_wr fs_wr_t;
fs_wr_t *fs_open_write(const char *path);
int fs_pwrite(fs_wr_t *f, const void *src, uint32_t n);
int fs_close_write(fs_wr_t *f, bool abort);

#endif
