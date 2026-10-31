/* Pack a littlefs image with the same geometry the ESP32-C5 firmware mounts:
 * 256-byte read/prog, 4 KB blocks, 96 blocks, lookahead 32, 500 cycles,
 * LFS_NAME_MAX 64. The firmware and this tool compile the same lfs.c.
 *
 *   gcc -DLFS_NAME_MAX=64 -DLFS_NO_DEBUG -DLFS_NO_WARN -DLFS_NO_ERROR \
 *       -I src/fs/littlefs tools/lfs_pack.c src/fs/littlefs/lfs.c \
 *       src/fs/littlefs/lfs_util.c -o lfs_pack
 *   lfs_pack out.bin hostfile:cfg/wifi.json ...
 *
 * The image starts as 0xFF. A mount of a blank image formats; this tool
 * formats once and writes the named files so the first boot mounts.
 */
#define LFS_NO_DEBUG 1
#define LFS_NO_WARN 1
#define LFS_NO_ERROR 1
#ifndef LFS_NAME_MAX
#define LFS_NAME_MAX 64
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lfs.h"

#define BLOCK 4096
#define BLOCKS 96

static uint8_t *image;

static int bd_read(const struct lfs_config *c, lfs_block_t block, lfs_off_t off, void *buf, lfs_size_t size) {
    (void)c;
    if (block >= BLOCKS) return LFS_ERR_IO;
    memcpy(buf, image + (size_t)block * BLOCK + off, size);
    return 0;
}
static int bd_prog(const struct lfs_config *c, lfs_block_t block, lfs_off_t off, const void *buf, lfs_size_t size) {
    (void)c;
    uint8_t *d = image + (size_t)block * BLOCK + off;
    const uint8_t *s = buf;
    for (lfs_size_t i = 0; i < size; i++) d[i] &= s[i];
    return 0;
}
static int bd_erase(const struct lfs_config *c, lfs_block_t block) {
    (void)c;
    if (block >= BLOCKS) return LFS_ERR_IO;
    memset(image + (size_t)block * BLOCK, 0xFF, BLOCK);
    return 0;
}
static int bd_sync(const struct lfs_config *c) { (void)c; return 0; }

static int mkdir_p(lfs_t *lfs, const char *path) {
    char tmp[160];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
        int err = lfs_mkdir(lfs, tmp);
        if (err && err != LFS_ERR_EXIST) return err;
        *p = '/';
    }
    return 0;
}

static int write_file(lfs_t *lfs, const char *path, const void *data, size_t n) {
    char parent[160];
    snprintf(parent, sizeof parent, "%s", path);
    char *slash = strrchr(parent, '/');
    if (slash && parent[0]) {
        *slash = 0;
        int err = mkdir_p(lfs, parent);
        if (err) return err;
        /* mkdir_p creates only ancestors. A one-level parent such as
         * "cfg" has no slash, so it still has to be created here. */
        err = lfs_mkdir(lfs, parent);
        if (err && err != LFS_ERR_EXIST) return err;
    }
    lfs_file_t f;
    int err = lfs_file_open(lfs, &f, path, LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC);
    if (err) return err;
    lfs_ssize_t w = lfs_file_write(lfs, &f, data, n);
    int c = lfs_file_close(lfs, &f);
    if (w < 0 || (size_t)w != n) return w < 0 ? (int)w : LFS_ERR_IO;
    return c;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s out.bin [hostfile:volpath ...]\n", argv[0]);
        return 2;
    }
    image = malloc((size_t)BLOCK * BLOCKS);
    if (!image) return 1;
    memset(image, 0xFF, (size_t)BLOCK * BLOCKS);
    uint8_t read_buf[256], prog_buf[256], lookahead[32];
    struct lfs_config cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.read = bd_read;
    cfg.prog = bd_prog;
    cfg.erase = bd_erase;
    cfg.sync = bd_sync;
    cfg.read_size = 256;
    cfg.prog_size = 256;
    cfg.block_size = BLOCK;
    cfg.block_count = BLOCKS;
    cfg.cache_size = 256;
    cfg.lookahead_size = sizeof lookahead;
    cfg.block_cycles = 500;
    cfg.read_buffer = read_buf;
    cfg.prog_buffer = prog_buf;
    cfg.lookahead_buffer = lookahead;
    lfs_t lfs;
    int err = lfs_format(&lfs, &cfg);
    if (!err) err = lfs_mount(&lfs, &cfg);
    if (err) { fprintf(stderr, "format/mount %d\n", err); return 1; }
    for (int i = 2; i < argc; i++) {
        char *colon = strchr(argv[i], ':');
        if (!colon || colon == argv[i]) { fprintf(stderr, "want hostfile:volpath, got %s\n", argv[i]); return 1; }
        *colon = 0;
        const char *host = argv[i];
        const char *vol = colon + 1;
        FILE *fp = fopen(host, "rb");
        if (!fp) { perror(host); return 1; }
        fseek(fp, 0, SEEK_END);
        long sz = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        char *buf = malloc((size_t)sz + 1);
        if (!buf || (sz && fread(buf, 1, (size_t)sz, fp) != (size_t)sz)) { fprintf(stderr, "read %s\n", host); return 1; }
        fclose(fp);
        err = write_file(&lfs, vol, buf, (size_t)sz);
        free(buf);
        if (err) { fprintf(stderr, "write %s -> %s: %d\n", host, vol, err); return 1; }
        fprintf(stderr, "packed %s (%ld bytes)\n", vol, sz);
    }
    lfs_unmount(&lfs);
    FILE *out = fopen(argv[1], "wb");
    if (!out || fwrite(image, 1, (size_t)BLOCK * BLOCKS, out) != (size_t)BLOCK * BLOCKS) {
        perror(argv[1]);
        return 1;
    }
    fclose(out);
    fprintf(stderr, "wrote %s (%d bytes)\n", argv[1], BLOCK * BLOCKS);
    return 0;
}
