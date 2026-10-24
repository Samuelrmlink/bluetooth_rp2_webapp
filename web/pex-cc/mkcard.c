/* Write a FAT image (SFD, whole-card volume) the firmware's FatFs can mount.
 * Usage: mkcard OUT.IMG DIR
 * Every file under DIR is stored as pex-cc/<relative path>. pexcc-host is skipped.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>
/* FatFs names its directory type DIR, which is also POSIX dirent's DIR. */
#define DIR FATFS_DIR
#include "ff.h"
#undef DIR
#include "diskio.h"
#include <dirent.h>

#define SECTOR 512u
#define NSECTORS (32u * 1024u * 1024u / SECTOR)

static uint8_t *img;
static uint32_t nsec = NSECTORS;

void ff_yield(void) {}

DSTATUS disk_initialize(BYTE pdrv) { (void)pdrv; return 0; }
DSTATUS disk_status(BYTE pdrv) { (void)pdrv; return 0; }

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count) {
    (void)pdrv;
    if ((uint64_t)sector + count > nsec) return RES_PARERR;
    memcpy(buff, img + (uint64_t)sector * SECTOR, (size_t)count * SECTOR);
    return RES_OK;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count) {
    (void)pdrv;
    if ((uint64_t)sector + count > nsec) return RES_PARERR;
    memcpy(img + (uint64_t)sector * SECTOR, buff, (size_t)count * SECTOR);
    return RES_OK;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff) {
    (void)pdrv;
    if (cmd == CTRL_SYNC) return RES_OK;
    if (cmd == GET_SECTOR_COUNT) { *(LBA_t *)buff = nsec; return RES_OK; }
    if (cmd == GET_BLOCK_SIZE) { *(DWORD *)buff = 1; return RES_OK; }
    return RES_PARERR;
}

static void die_fr(const char *what, FRESULT r) {
    fprintf(stderr, "mkcard: %s failed (%d)\n", what, (int)r);
    exit(1);
}

static void mkdir_p(const char *path) {
    char buf[256];
    size_t n = strlen(path);
    size_t i;
    if (n >= sizeof buf) { fprintf(stderr, "mkcard: path too long\n"); exit(1); }
    memcpy(buf, path, n + 1);
    for (i = 1; i < n; i++) {
        if (buf[i] != '/') continue;
        buf[i] = 0;
        FRESULT r = f_mkdir(buf);
        if (r != FR_OK && r != FR_EXIST) die_fr(buf, r);
        buf[i] = '/';
    }
}

static void put_file(const char *dst, const char *src) {
    FILE *fp = fopen(src, "rb");
    FIL out;
    FRESULT r;
    UINT bw;
    uint8_t buf[4096];
    size_t n;
    if (!fp) { perror(src); exit(1); }
    mkdir_p(dst);
    r = f_open(&out, dst, FA_WRITE | FA_CREATE_ALWAYS);
    if (r != FR_OK) die_fr(dst, r);
    while ((n = fread(buf, 1, sizeof buf, fp)) > 0) {
        r = f_write(&out, buf, (UINT)n, &bw);
        if (r != FR_OK || bw != n) die_fr(dst, r == FR_OK ? FR_DISK_ERR : r);
    }
    fclose(fp);
    r = f_close(&out);
    if (r != FR_OK) die_fr(dst, r);
    fprintf(stderr, "  %s\n", dst);
}

static void walk(const char *dir, const char *rel) {
    DIR *d = opendir(dir);
    struct dirent *e;
    if (!d) { perror(dir); exit(1); }
    while ((e = readdir(d)) != NULL) {
        char child[512], sub[256], dst[256];
        struct stat st;
        if (e->d_name[0] == '.') continue;
        if (strcmp(e->d_name, "pexcc-host") == 0) continue;
        snprintf(child, sizeof child, "%s/%s", dir, e->d_name);
        if (stat(child, &st) != 0) continue;
        if (rel[0]) snprintf(sub, sizeof sub, "%s/%s", rel, e->d_name);
        else snprintf(sub, sizeof sub, "%s", e->d_name);
        if (S_ISDIR(st.st_mode)) walk(child, sub);
        else if (S_ISREG(st.st_mode)) {
            snprintf(dst, sizeof dst, "0:/pex-cc/%s", sub);
            put_file(dst, child);
        }
    }
    closedir(d);
}

int main(int argc, char **argv) {
    FATFS fs;
    MKFS_PARM opt;
    BYTE work[64 * 1024];
    FRESULT r;
    FILE *fp;
    if (argc != 3) {
        fprintf(stderr, "usage: mkcard OUT.IMG DIR\n");
        return 2;
    }
    img = calloc(1, (size_t)nsec * SECTOR);
    if (!img) { fprintf(stderr, "mkcard: out of memory\n"); return 1; }
    memset(&opt, 0, sizeof opt);
    opt.fmt = FM_ANY | FM_SFD;
    r = f_mkfs("0:", &opt, work, sizeof work);
    if (r != FR_OK) die_fr("f_mkfs", r);
    r = f_mount(&fs, "0:", 1);
    if (r != FR_OK) die_fr("f_mount", r);
    r = f_mkdir("0:/pex-cc");
    if (r != FR_OK && r != FR_EXIST) die_fr("mkdir pex-cc", r);
    walk(argv[2], "");
    f_mount(NULL, "0:", 0);
    fp = fopen(argv[1], "wb");
    if (!fp || fwrite(img, SECTOR, nsec, fp) != nsec) {
        perror(argv[1]);
        return 1;
    }
    fclose(fp);
    fprintf(stderr, "wrote %s (%u bytes)\n", argv[1], nsec * SECTOR);
    return 0;
}
