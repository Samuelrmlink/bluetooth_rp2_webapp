/* Host tool for the flash-migration tests (test/sim/lfs_migrate.py): build a
 * LittleFS image with the firmware's own littlefs (src/fs/littlefs, v2.10)
 * and geometry (4 KiB blocks, 256 B read/prog/cache, 32 B lookahead), or
 * read one back out.
 *
 *   lfs_tool mk   <image> <block_count> <dir>   format, copy <dir> in
 *   lfs_tool dump <image> <outdir>              every file and folder out
 *   lfs_tool info <image>                       block_count from the superblock
 *
 * <image> is the filesystem region alone (block 0 = FS_FLASH_OFF); `mk`
 * writes block_count * 4096 bytes, erased cells as 0xff like real flash.
 * `dump` mounts with block_count 0 (whatever the superblock says), the same
 * way the firmware detects the old geometry. Prints names and sizes only. */
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "lfs.h"

#define BLOCK 4096u

static uint8_t *img;
static size_t img_len;

static int rd(const struct lfs_config *c, lfs_block_t b, lfs_off_t o, void *buf, lfs_size_t n) {
    (void)c;
    if ((size_t)b * BLOCK + o + n > img_len) { return LFS_ERR_IO; }
    memcpy(buf, img + (size_t)b * BLOCK + o, n);
    return 0;
}
static int pr(const struct lfs_config *c, lfs_block_t b, lfs_off_t o, const void *buf, lfs_size_t n) {
    (void)c;
    if ((size_t)b * BLOCK + o + n > img_len) { return LFS_ERR_IO; }
    uint8_t *d = img + (size_t)b * BLOCK + o;
    const uint8_t *s = buf;
    for (lfs_size_t i = 0; i < n; i++) { d[i] &= s[i]; }   /* NOR: program only clears bits */
    return 0;
}
static int er(const struct lfs_config *c, lfs_block_t b) {
    (void)c;
    if ((size_t)(b + 1) * BLOCK > img_len) { return LFS_ERR_IO; }
    memset(img + (size_t)b * BLOCK, 0xff, BLOCK);
    return 0;
}
static int sy(const struct lfs_config *c) { (void)c; return 0; }

static uint8_t rbuf[256], pbuf[256], la[32];
static struct lfs_config cfg = {
    .read = rd, .prog = pr, .erase = er, .sync = sy,
    .read_size = 256, .prog_size = 256, .block_size = BLOCK,
    .cache_size = 256, .lookahead_size = 32, .block_cycles = 500,
    .read_buffer = rbuf, .prog_buffer = pbuf, .lookahead_buffer = la,
};
static lfs_t lfs;

static int copy_in(const char *host, const char *rel) {
    DIR *d = opendir(host);
    if (!d) { perror(host); return -1; }
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) { continue; }
        char hp[1024], lp[512];
        snprintf(hp, sizeof hp, "%s/%s", host, e->d_name);
        snprintf(lp, sizeof lp, "%s%s%s", rel, rel[0] ? "/" : "", e->d_name);
        struct stat st;
        if (stat(hp, &st)) { perror(hp); return -1; }
        if (S_ISDIR(st.st_mode)) {
            int r = lfs_mkdir(&lfs, lp);
            if (r && r != LFS_ERR_EXIST) { fprintf(stderr, "mkdir %s: %d\n", lp, r); return -1; }
            if (copy_in(hp, lp)) { return -1; }
            continue;
        }
        FILE *f = fopen(hp, "rb");
        if (!f) { perror(hp); return -1; }
        lfs_file_t lf;
        int r = lfs_file_open(&lfs, &lf, lp, LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC);
        if (r) { fprintf(stderr, "open %s: %d\n", lp, r); fclose(f); return -1; }
        uint8_t buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, f)) > 0) {
            if (lfs_file_write(&lfs, &lf, buf, n) != (lfs_ssize_t)n) { fprintf(stderr, "write %s\n", lp); return -1; }
        }
        fclose(f);
        if (lfs_file_close(&lfs, &lf)) { fprintf(stderr, "close %s\n", lp); return -1; }
    }
    closedir(d);
    return 0;
}

static int nfiles, ndirs;

static int copy_out(const char *rel, const char *host) {
    lfs_dir_t d;
    if (lfs_dir_open(&lfs, &d, rel[0] ? rel : "/")) { return -1; }
    struct lfs_info info;
    int rc = 0;
    while (rc == 0 && lfs_dir_read(&lfs, &d, &info) > 0) {
        if (!strcmp(info.name, ".") || !strcmp(info.name, "..")) { continue; }
        char lp[512], hp[1024];
        snprintf(lp, sizeof lp, "%s%s%s", rel, rel[0] ? "/" : "", info.name);
        snprintf(hp, sizeof hp, "%s/%s", host, info.name);
        if (info.type == LFS_TYPE_DIR) {
            if (mkdir(hp, 0700) && errno != EEXIST) { perror(hp); rc = -1; break; }
            ndirs++;
            rc = copy_out(lp, hp);
            continue;
        }
        lfs_file_t lf;
        if (lfs_file_open(&lfs, &lf, lp, LFS_O_RDONLY)) { rc = -1; break; }
        FILE *f = fopen(hp, "wb");
        if (!f) { perror(hp); rc = -1; break; }
        uint8_t buf[4096];
        lfs_ssize_t n;
        while ((n = lfs_file_read(&lfs, &lf, buf, sizeof buf)) > 0) { fwrite(buf, 1, (size_t)n, f); }
        fclose(f);
        lfs_file_close(&lfs, &lf);
        if (n < 0) { rc = -1; break; }
        nfiles++;
    }
    lfs_dir_close(&lfs, &d);
    return rc;
}

static int load(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return -1; }
    fseek(f, 0, SEEK_END);
    img_len = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    img = malloc(img_len);
    if (fread(img, 1, img_len, f) != img_len) { fclose(f); return -1; }
    fclose(f);
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 5 && !strcmp(argv[1], "mk")) {
        cfg.block_count = (lfs_size_t)strtoul(argv[3], NULL, 0);
        img_len = (size_t)cfg.block_count * BLOCK;
        img = malloc(img_len);
        memset(img, 0xff, img_len);
        if (lfs_format(&lfs, &cfg) || lfs_mount(&lfs, &cfg)) { fprintf(stderr, "format failed\n"); return 1; }
        if (copy_in(argv[4], "")) { return 1; }
        printf("used %ld of %u blocks\n", (long)lfs_fs_size(&lfs), (unsigned)cfg.block_count);
        lfs_unmount(&lfs);
        FILE *f = fopen(argv[2], "wb");
        if (!f || fwrite(img, 1, img_len, f) != img_len) { perror(argv[2]); return 1; }
        fclose(f);
        return 0;
    }
    if ((argc == 4 && !strcmp(argv[1], "dump")) || (argc == 3 && !strcmp(argv[1], "info"))) {
        if (load(argv[2])) { return 1; }
        cfg.block_count = 0;
        int r = lfs_mount(&lfs, &cfg);
        if (r) { printf("mount failed %d\n", r); return 2; }
        struct lfs_fsinfo fi;
        lfs_fs_stat(&lfs, &fi);
        if (argv[1][0] == 'i') {
            printf("block_count %u used %ld\n", (unsigned)fi.block_count, (long)lfs_fs_size(&lfs));
            return 0;
        }
        if (mkdir(argv[3], 0700) && errno != EEXIST) { perror(argv[3]); return 1; }
        if (copy_out("", argv[3])) { fprintf(stderr, "dump failed\n"); return 1; }
        printf("block_count %u files %d dirs %d\n", (unsigned)fi.block_count, nfiles, ndirs);
        return 0;
    }
    fprintf(stderr, "usage: lfs_tool mk <img> <blocks> <dir> | dump <img> <outdir> | info <img>\n");
    return 2;
}
