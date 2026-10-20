/*
 * sd_store.h on FatFs (FAT12/16/32 and exFAT) over the SD card driver.
 * From km003c_rp2_webapp's log_store_fatfs.c, without the logger's
 * sector-buffered files: here every file goes through FatFs's window
 * (FF_FS_TINY), and transfers write whole sectors anyway (sd_api.c).
 */
#include "sd/sd_store.h"

#include <string.h>

#include "autoconf.h"
#include "ff.h"
#include "sd/sd_card.h"
#include "sd/sd_port.h"

#ifndef CONFIG_SD_XFERS
#define CONFIG_SD_XFERS 2
#endif
#ifndef CONFIG_SD_DETECT
#define CONFIG_SD_DETECT -1
#endif
/* One per transfer, one for the bench, one spare (rename/delete probes). */
#define LS_FATFS_FILES (CONFIG_SD_XFERS + 2)

/* How long a newly inserted card must read present on card detect before it
   is talked to: contacts bounce as it slides in, and a card is not ready the
   instant it gets power. */
#ifndef SD_SETTLE_MS
#define SD_SETTLE_MS 2000
#endif

struct ls_file {
    bool used;
    FIL fil;
};

static FATFS s_fs;
static bool s_mounted;
static enum ls_media s_media = LS_MEDIA_NONE;
static struct ls_file s_files[LS_FATFS_FILES];

static int map(FRESULT r) {
    switch (r) {
    case FR_OK:                  return LS_OK;
    case FR_NO_FILE:
    case FR_NO_PATH:             return LS_ENOENT;
    case FR_EXIST:               return LS_EEXIST;
    case FR_DENIED:              return LS_ENOSPC;   /* f_write/f_mkdir: no free cluster */
    case FR_TOO_MANY_OPEN_FILES: return LS_ENOMEM;
    case FR_NOT_READY:
    case FR_NOT_ENABLED:
    case FR_NO_FILESYSTEM:       return LS_ENOMEDIA;
    case FR_INVALID_NAME:
    case FR_INVALID_PARAMETER:
    case FR_INVALID_OBJECT:      return LS_EINVAL;
    default:                     return LS_EIO;
    }
}

bool ls_present(void) {
    return sd_card_present();
}

int ls_mount(void) {
    if (s_mounted) { return LS_OK; }
    /* Settle: present, continuously, for SD_SETTLE_MS (only meaningful with
     * a detect switch; without one the card is simply probed). */
    static uint64_t present_since;
    if (!sd_card_present()) {
        present_since = 0;
        s_media = LS_MEDIA_NONE;
        return LS_ENOMEDIA;
    }
    uint64_t now = sd_port_now_us();
#if CONFIG_SD_DETECT >= 0
    if (present_since == 0) { present_since = now ? now : 1; }
    if (now - present_since < (uint64_t)SD_SETTLE_MS * 1000u) {
        /* settling; a card that already failed to mount stays "error"
         * while it settles for the next try */
        if (s_media != LS_MEDIA_ERROR) { s_media = LS_MEDIA_NONE; }
        return LS_EBUSY;
    }
#else
    (void)now;
#endif
    sd_card_forget();                        /* identify afresh on every mount */
    FRESULT r = f_mount(&s_fs, "", 1);
    if (r != FR_OK) {
        /* Not ready with card detect saying present = a card that does not
         * answer: "error", not an endless "settling" (seen in mcusim with
         * the detect pin low and nothing on the bus). */
        s_media = r == FR_NOT_READY && !sd_card_present() ? LS_MEDIA_NONE : LS_MEDIA_ERROR;
        f_mount(NULL, "", 0);
        /* Settle again before the next try: a card that is half in, or
         * still waking, should not be hammered. */
        present_since = 0;
        return map(r);
    }
    s_mounted = true;
    s_media = LS_MEDIA_OK;
    return LS_OK;
}

void ls_unmount(void) {
    for (int i = 0; i < LS_FATFS_FILES; i++) {
        memset(&s_files[i], 0, sizeof s_files[i]);   /* abandoned: the card is gone or going */
    }
    f_mount(NULL, "", 0);
    s_mounted = false;
    if (s_media == LS_MEDIA_OK) { s_media = LS_MEDIA_ERROR; }
    if (!sd_card_present()) { s_media = LS_MEDIA_NONE; }
}

enum ls_media ls_media(void) { return s_media; }

const char *ls_fs_name(void) {
    if (!s_mounted) { return "-"; }
    switch (s_fs.fs_type) {
    case FS_FAT12: return "fat12";
    case FS_FAT16: return "fat16";
    case FS_FAT32: return "fat32";
    case FS_EXFAT: return "exfat";
    default:       return "?";
    }
}

ls_file *ls_open(const char *path, int mode) {
    if (!s_mounted) { return NULL; }
    BYTE fm;
    switch (mode & 0x0F) {
    case LS_RD:     fm = FA_READ | FA_OPEN_EXISTING; break;
    case LS_WR:     fm = FA_READ | FA_WRITE | FA_CREATE_ALWAYS; break;
    case LS_RW:     fm = FA_READ | FA_WRITE | FA_OPEN_EXISTING; break;
    case LS_APPEND: fm = FA_READ | FA_WRITE | FA_OPEN_APPEND; break;
    default:        return NULL;
    }
    for (int i = 0; i < LS_FATFS_FILES; i++) {
        struct ls_file *f = &s_files[i];
        if (f->used) { continue; }
        if (f_open(&f->fil, path, fm) != FR_OK) { return NULL; }
        f->used = true;
        return f;
    }
    return NULL;
}

int ls_close(ls_file *f) {
    if (!f || !f->used) { return LS_EINVAL; }
    FRESULT c = f_close(&f->fil);
    f->used = false;
    return map(c);
}

int ls_read(ls_file *f, void *buf, uint32_t len) {
    UINT br = 0;
    FRESULT r = f_read(&f->fil, buf, len, &br);
    return r == FR_OK ? (int)br : map(r);
}

int ls_write(ls_file *f, const void *buf, uint32_t len) {
    UINT bw = 0;
    FRESULT r = f_write(&f->fil, buf, len, &bw);
    if (r != FR_OK) { return map(r); }
    return bw == len ? (int)len : LS_ENOSPC;
}

int ls_seek(ls_file *f, uint64_t pos) { return map(f_lseek(&f->fil, (FSIZE_t)pos)); }
uint64_t ls_tell(ls_file *f) { return (uint64_t)f_tell(&f->fil); }
uint64_t ls_size(ls_file *f) { return (uint64_t)f_size(&f->fil); }
int ls_truncate(ls_file *f) { return map(f_truncate(&f->fil)); }
int ls_sync(ls_file *f) { return map(f_sync(&f->fil)); }

int ls_mkdir(const char *path) {
    if (!s_mounted) { return LS_ENOMEDIA; }
    return map(f_mkdir(path));
}

int ls_remove(const char *path) {
    if (!s_mounted) { return LS_ENOMEDIA; }
    return map(f_unlink(path));
}

int ls_rename(const char *from, const char *to) {
    if (!s_mounted) { return LS_ENOMEDIA; }
    return map(f_rename(from, to));
}

int ls_list(const char *path, ls_dir_cb cb, void *ctx) {
    if (!s_mounted) { return LS_ENOMEDIA; }
    /* DIR + FILINFO are ~300 bytes with LFN; static, one per depth (a
     * callback may list again: a delete's emptiness probe). */
    static DIR dirs[2];
    static FILINFO infos[2];
    static int depth;
    if (depth >= 2) { return LS_ENOMEM; }
    DIR *d = &dirs[depth];
    FILINFO *fi = &infos[depth];
    FRESULT r = f_opendir(d, path);
    if (r != FR_OK) { return map(r); }
    depth++;
    int ret = LS_OK;
    for (;;) {
        r = f_readdir(d, fi);
        if (r != FR_OK) { ret = map(r); break; }
        if (fi->fname[0] == 0) { break; }
        if (!cb(fi->fname, (fi->fattrib & AM_DIR) != 0, (uint64_t)fi->fsize, ctx)) { ret = 1; break; }
    }
    depth--;
    f_closedir(d);
    return ret;
}

int ls_statfs(uint64_t *free_bytes, uint64_t *total_bytes) {
    if (!s_mounted) { return LS_ENOMEDIA; }
    DWORD nfree = 0;
    FATFS *fs = NULL;
    FRESULT r = f_getfree("", &nfree, &fs);
    if (r != FR_OK) { return map(r); }
    uint64_t cl = (uint64_t)fs->csize * 512u;
    *free_bytes = (uint64_t)nfree * cl;
    *total_bytes = (uint64_t)(fs->n_fatent - 2) * cl;
    return LS_OK;
}

bool ls_card_info(struct ls_card *out) {
    const sd_info_t *in = sd_card_info();
    memset(out, 0, sizeof *out);
    if (!in->valid || !in->sectors) { return false; }
    out->bytes = in->sectors * 512u;
    out->sdhc = in->block_addr;
    out->cid_ok = in->cid_ok;
    if (in->cid_ok) {
        sd_cid_t c;
        sd_cid_decode(in->cid, &c);
        out->mid = c.mid;
        memcpy(out->oem, c.oem, 2);
        memcpy(out->name, c.name, 5);
        for (int i = 0; i < 5; i++) { if (out->name[i] == '"' || out->name[i] == '\\') { out->name[i] = '?'; } }
        for (int i = 0; i < 2; i++) { if (out->oem[i] == '"' || out->oem[i] == '\\') { out->oem[i] = '?'; } }
        out->rev_major = c.rev_major;
        out->rev_minor = c.rev_minor;
        out->serial = c.serial;
        out->year = c.year;
        out->month = c.month;
    }
    return true;
}

bool ls_can_format(void) { return true; }

void ls_card_counters(uint32_t *busy_timeouts, uint32_t *write_retries, uint32_t *dma_blocks) {
    const sd_stats_t *s = sd_card_stats();
    *busy_timeouts = s->busy_timeouts;
    *write_retries = s->write_retries;
    *dma_blocks = s->dma_blocks;
}

uint32_t ls_busy_max_us(bool reset) {
    sd_stats_t *s = sd_card_stats();
    uint32_t v = s->busy_max_us;
    if (reset) { s->busy_max_us = 0; }
    return v;
}

uint32_t ls_sectors_written(void) { return sd_card_stats()->sectors_written; }

int ls_format(int fmt, uint32_t cluster, uint32_t align, void *work, uint32_t len) {
    if (!sd_card_present()) { return LS_ENOMEDIA; }
    ls_unmount();
    if (!sd_card_ready() && sd_card_init() != SD_OK) { return LS_ENOMEDIA; }
    MKFS_PARM opt;
    memset(&opt, 0, sizeof opt);
    opt.fmt = fmt == LS_FMT_EXFAT ? FM_EXFAT : FM_FAT32;   /* no FM_SFD: partitioned, MBR */
    opt.n_fat = fmt == LS_FMT_EXFAT ? 1 : 2;               /* as the SD Association's formatter */
    opt.align = align;
    opt.n_root = 0;
    opt.au_size = cluster;
    FRESULT r = f_mkfs("", &opt, work, len);
    if (r == FR_MKFS_ABORTED) { return LS_EINVAL; }        /* the cluster size does not fit */
    return map(r);
}
