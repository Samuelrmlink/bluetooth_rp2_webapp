/* The SD card's files: a small POSIX-ish API on FatFs (sd_store.c), ported
 * from km003c_rp2_webapp's log_store.h. Paths are '/'-separated and relative
 * to the card's root. Every call returns 0 / a count on success and a
 * negative LS_E* on failure. Storage worker only (sd_worker.h). */
#ifndef BTGW_SD_STORE_H
#define BTGW_SD_STORE_H

#include <stdbool.h>
#include <stdint.h>

enum {
    LS_OK = 0,
    LS_EIO = -1,     /* card / disk error: the logger treats it as "card gone" */
    LS_ENOENT = -2,
    LS_EEXIST = -3,
    LS_ENOSPC = -4,
    LS_ENOMEM = -5,  /* no free file handle */
    LS_ENOMEDIA = -6,
    LS_EINVAL = -7,
    LS_EBUSY = -8,   /* the card is being formatted / remounted */
};

/* Media state. */
enum ls_media {
    LS_MEDIA_NONE = 0, /* no card (or no log root) */
    LS_MEDIA_OK = 1,
    LS_MEDIA_ERROR = 2, /* present but unmountable / failed */
};

typedef struct ls_file ls_file;

enum {
    LS_RD = 1,     /* read only, must exist */
    LS_WR = 2,     /* read/write, create, truncate */
    LS_RW = 3,     /* read/write, must exist, no truncate */
    LS_APPEND = 4, /* write, create, position at end */
};

/* Mounts (or re-checks) the media. Cheap to call when already mounted. */
int ls_mount(void);
/* Whether the card is (still) there, as far as card detect can tell
   without talking to it (always true with no detect switch). */
bool ls_present(void);
void ls_unmount(void);
/* Incremented by every unmount: an ls_file from an older count is gone. */
uint32_t ls_mount_count(void);
enum ls_media ls_media(void);
/* "fat12/fat16/fat32/exfat", "-" when not mounted */
const char *ls_fs_name(void);

ls_file *ls_open(const char *path, int mode);
int ls_close(ls_file *f);
int ls_read(ls_file *f, void *buf, uint32_t len);       /* bytes read, 0 at EOF */
int ls_write(ls_file *f, const void *buf, uint32_t len); /* == len or error */
int ls_seek(ls_file *f, uint64_t pos);
uint64_t ls_tell(ls_file *f);
uint64_t ls_size(ls_file *f);
int ls_truncate(ls_file *f); /* at the current position */
int ls_sync(ls_file *f);

int ls_mkdir(const char *path); /* LS_EEXIST is not an error to callers that mkdir -p */
int ls_remove(const char *path); /* a file or an empty directory */
int ls_rename(const char *from, const char *to); /* LS_EEXIST if `to` exists */

/* Calls cb for every entry (not "." / "..") of a directory. Stops early,
 * returning 1, when cb returns false. */
typedef bool (*ls_dir_cb)(const char *name, bool is_dir, uint64_t size, void *ctx);
int ls_list(const char *path, ls_dir_cb cb, void *ctx);

int ls_statfs(uint64_t *free_bytes, uint64_t *total_bytes);

/* The card itself, for the status JSON. False before a card has answered.
 * Readable from the main context. */
struct ls_card {
    uint64_t bytes;
    bool sdhc;       /* block-addressed: SDHC/SDXC */
    bool cid_ok;
    uint8_t mid;     /* manufacturer */
    char oem[3];
    char name[6];
    uint8_t rev_major, rev_minor;
    uint32_t serial;
    uint16_t year;
    uint8_t month;
};
bool ls_card_info(struct ls_card *out);

/* Formatting: an MBR ("MS-DOS") partition table and one FAT32 or exFAT
 * partition. Unmounts first. `align` is the data area alignment in sectors;
 * `work` is f_mkfs's buffer (a sector multiple). */
enum { LS_FMT_FAT32 = 1, LS_FMT_EXFAT = 2 };
bool ls_can_format(void);
int ls_format(int fmt, uint32_t cluster, uint32_t align, void *work, uint32_t len);
/* Sectors written to the card since boot (format progress). */
uint32_t ls_sectors_written(void);
/* Longest the card has held "busy" after a write; `reset` clears it. */
uint32_t ls_busy_max_us(bool reset);
/* Card waits that timed out, and writes retried after one (both since boot). */
void ls_card_counters(uint32_t *busy_timeouts, uint32_t *write_retries, uint32_t *dma_blocks);


#endif
