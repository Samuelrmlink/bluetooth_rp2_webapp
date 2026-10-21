/* Host tool for the flash-migration tests: copies a folder out of an SD card
 * image (FAT32 or exFAT, no partition table or MBR, as mcusim's sdcard
 * device takes them) with the firmware's own FatFs (third_party/fatfs).
 *
 *   sd_tool get <card.img> <folder> <outdir>     recursive, prints counts only
 *   sd_tool put <card.img> <file> <path>         one file onto the card
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "ff.h"
#include "diskio.h"

static FILE *img;

DSTATUS disk_status(BYTE pdrv) { (void)pdrv; return img ? 0 : STA_NOINIT; }
DSTATUS disk_initialize(BYTE pdrv) { (void)pdrv; return img ? 0 : STA_NOINIT; }
DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count) {
    (void)pdrv;
    if (fseeko(img, (off_t)sector * 512, SEEK_SET) || fread(buff, 512, count, img) != count) { return RES_ERROR; }
    return RES_OK;
}
DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count) {
    (void)pdrv;
    if (fseeko(img, (off_t)sector * 512, SEEK_SET) || fwrite(buff, 512, count, img) != count) { return RES_ERROR; }
    return RES_OK;
}
DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff) {
    (void)pdrv;
    if (cmd == CTRL_SYNC) { fflush(img); return RES_OK; }
    if (cmd == GET_SECTOR_COUNT) {
        fseeko(img, 0, SEEK_END);
        *(LBA_t *)buff = (LBA_t)(ftello(img) / 512);
        return RES_OK;
    }
    if (cmd == GET_SECTOR_SIZE) { *(WORD *)buff = 512; return RES_OK; }
    if (cmd == GET_BLOCK_SIZE) { *(DWORD *)buff = 1; return RES_OK; }
    return RES_PARERR;
}
void ff_yield(void) {}
DWORD get_fattime(void) { return ((DWORD)(2026 - 1980) << 25) | (10u << 21) | (20u << 16); }

static int nfiles, ndirs;

static int get(const char *src, const char *dst) {
    DIR d;
    FILINFO fi;
    if (f_opendir(&d, src) != FR_OK) { fprintf(stderr, "opendir %s\n", src); return -1; }
    if (mkdir(dst, 0700) && errno != EEXIST) { perror(dst); return -1; }
    while (f_readdir(&d, &fi) == FR_OK && fi.fname[0]) {
        char s[512], t[1024];
        snprintf(s, sizeof s, "%s/%s", src, fi.fname);
        snprintf(t, sizeof t, "%s/%s", dst, fi.fname);
        if (fi.fattrib & AM_DIR) {
            ndirs++;
            if (get(s, t)) { return -1; }
            continue;
        }
        FIL f;
        if (f_open(&f, s, FA_READ) != FR_OK) { fprintf(stderr, "open %s\n", s); return -1; }
        FILE *o = fopen(t, "wb");
        if (!o) { perror(t); return -1; }
        static BYTE buf[8192];
        UINT n;
        while (f_read(&f, buf, sizeof buf, &n) == FR_OK && n) { fwrite(buf, 1, n, o); }
        fclose(o);
        f_close(&f);
        nfiles++;
    }
    f_closedir(&d);
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 5 || (strcmp(argv[1], "get") && strcmp(argv[1], "put"))) {
        fprintf(stderr, "usage: sd_tool get <img> <folder> <outdir> | put <img> <file> <path>\n");
        return 2;
    }
    img = fopen(argv[2], argv[1][0] == 'p' ? "r+b" : "rb");
    if (!img) { perror(argv[2]); return 1; }
    static FATFS fs;
    if (f_mount(&fs, "", 1) != FR_OK) { fprintf(stderr, "mount failed\n"); return 1; }
    if (argv[1][0] == 'g') {
        if (get(argv[3], argv[4])) { return 1; }
        printf("files %d dirs %d\n", nfiles, ndirs);
        return 0;
    }
    FILE *in = fopen(argv[3], "rb");
    FIL f;
    if (!in || f_open(&f, argv[4], FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) { fprintf(stderr, "put failed\n"); return 1; }
    static BYTE buf[8192];
    size_t n;
    UINT w;
    while ((n = fread(buf, 1, sizeof buf, in)) > 0) { f_write(&f, buf, (UINT)n, &w); }
    f_close(&f);
    f_unmount("");
    fclose(img);
    return 0;
}
