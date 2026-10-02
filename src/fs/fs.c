/* LittleFS mount + flash block device, adapted from the confirmed-working
 * pm220-pico2w (/home/samuel/Projects/pm220-pico2w/src/fs/fs.c) reference
 * — same block-device glue and file-handle API, generalized from that
 * project's two hardcoded top-level directories to arbitrary nested paths
 * (our web-dist/ tree has css/, js/, js/modals/, vendor/), and pointed at
 * this project's own flash offset/size (see fs.h) instead of reserving a
 * BTstack NVM tail this project has no equivalent of. */
#include "fs.h"

#include <stdio.h>
#include <string.h>
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "pico/flash.h"
#include "littlefs/lfs.h"

#define FS_BLOCK FLASH_SECTOR_SIZE

_Static_assert(FS_FLASH_OFF % FLASH_SECTOR_SIZE == 0, "fs offset must be sector-aligned");
_Static_assert(FS_SIZE % FLASH_SECTOR_SIZE == 0, "fs size must be sector-aligned");
/* Against the board's real flash size rather than a hardcoded 8MiB: the
 * Pico W test target has 2MiB, where the default 2+6 layout would put the
 * entire filesystem past the end of the chip — an error this catches at
 * compile time instead of at the first write. */
_Static_assert(FS_BT_TAIL % FLASH_SECTOR_SIZE == 0, "bt nvm tail must be sector-aligned");
_Static_assert(FS_FLASH_OFF + FS_SIZE + FS_BT_TAIL <= PICO_FLASH_SIZE_BYTES,
               "fs region plus the BTstack NVM tail exceeds the board's flash");
_Static_assert(FS_FLASH_OFF + FS_SIZE <= PICO_FLASH_SIZE_BYTES,
               "fs region exceeds the board's flash size");

static lfs_t lfs;
static struct lfs_config cfg;
static uint8_t read_buf[FLASH_PAGE_SIZE];
static uint8_t prog_buf[FLASH_PAGE_SIZE];
static uint8_t lookahead_buf[32];
static bool mounted;

static lfs_file_t wfile;
static bool wopen;
static lfs_file_t rfiles[FS_READS];
static bool ropen[FS_READS];

typedef struct {
    uint32_t off;
    const uint8_t *data;
    size_t len;
    int prog;
} flash_job_t;

static void __not_in_flash_func(flash_job_run)(void *p) {
    flash_job_t *j = p;
    if (j->prog) {
        flash_range_program(j->off, j->data, j->len);
    } else {
        flash_range_erase(j->off, j->len);
    }
}

/* flash_safe_execute() parks core1 for the operation — through
 * src/usb/km_flashsafe.c's helper, which keeps core1's USB frame interrupt
 * alive while it waits, so the meters do not see the bus go quiet for the
 * 45–400 ms an erase takes. The fallback below runs without the park (core1
 * never armed: the diag/test builds); it is not safe with a USB host
 * running and is only reached then. */
static void flash_do(flash_job_t *j) {
    int rc = flash_safe_execute(flash_job_run, j, 2000);
    if (rc == PICO_OK) {
        return;
    }
    uint32_t ints = save_and_disable_interrupts();
    flash_job_run(j);
    restore_interrupts(ints);
}

static int bd_read(const struct lfs_config *c, lfs_block_t block, lfs_off_t off,
                   void *buffer, lfs_size_t size) {
    (void)c;
    /* NOT XIP_BASE. Booted from an A/B partition, the ROM translates the
     * first 4 MB XIP window so the running slot appears at 0x10000000 and
     * leaves a HOLE past it -- a plain read of 0x10200000 would fault. The
     * no-translate alias reads flash by storage address, which is what the
     * SDK's own partition reader and BTstack's flash bank use on RP2350. */
    memcpy(buffer, (const uint8_t *)(FS_READ_BASE + FS_FLASH_OFF + block * FS_BLOCK + off), size);
    return 0;
}

static int bd_prog(const struct lfs_config *c, lfs_block_t block, lfs_off_t off,
                   const void *buffer, lfs_size_t size) {
    (void)c;
    flash_job_t j = {
        .off = FS_FLASH_OFF + block * FS_BLOCK + off,
        .data = buffer,
        .len = size,
        .prog = 1,
    };
    flash_do(&j);
    return 0;
}

static int bd_erase(const struct lfs_config *c, lfs_block_t block) {
    (void)c;
    flash_job_t j = {
        .off = FS_FLASH_OFF + block * FS_BLOCK,
        .data = NULL,
        .len = FS_BLOCK,
        .prog = 0,
    };
    flash_do(&j);
    return 0;
}

static int bd_sync(const struct lfs_config *c) {
    (void)c;
    return 0;
}

static bool valid_segment(const char *s, size_t len) {
    if (len == 0 || len > FS_NAME_MAX || s[0] == '.') {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        char ch = s[i];
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
            (ch >= '0' && ch <= '9') || ch == '.' || ch == '_' || ch == '-') {
            continue;
        }
        return false;
    }
    return true;
}

/* Accepts any number of "/"-separated segments (our web-dist/ tree nests
 * a few levels deep: js/modals/debug.js) — each segment individually
 * charset-checked by valid_segment, no ".." or empty segments, no leading
 * "/". This is deliberately more general than pm220-pico2w's fs_valid_path
 * (which only allowed its two hardcoded top-level dirs), since we're
 * serving an arbitrary static asset tree rather than a fixed app schema. */
bool fs_valid_path(const char *path) {
    if (!path || !path[0] || strlen(path) > FS_PATH_MAX || path[0] == '/') {
        return false;
    }
    const char *seg = path;
    for (;;) {
        const char *slash = strchr(seg, '/');
        size_t len = slash ? (size_t)(slash - seg) : strlen(seg);
        if (!valid_segment(seg, len)) {
            return false;
        }
        if (!slash) {
            return true;
        }
        seg = slash + 1;
    }
}

/* Creates every "/"-separated directory prefix of path that doesn't exist
 * yet — littlefs (like most filesystems) won't auto-create parents on
 * LFS_O_CREAT, and our upload API accepts nested paths in one PUT. */
static void ensure_parent_dirs(const char *path) {
    char buf[FS_PATH_MAX + 1];
    size_t n = strlen(path);
    if (n > FS_PATH_MAX) {
        n = FS_PATH_MAX;
    }
    memcpy(buf, path, n);
    buf[n] = 0;
    for (size_t i = 0; i < n; i++) {
        if (buf[i] != '/') {
            continue;
        }
        buf[i] = 0;
        int err = lfs_mkdir(&lfs, buf);
        if (err && err != LFS_ERR_EXIST) {
            printf("fs: mkdir %s failed %d\n", buf, err);
        }
        buf[i] = '/';
    }
}

bool fs_is_dir(const char *path) {
    if (!mounted) {
        return false;
    }
    if (!path || !path[0] || strcmp(path, "/") == 0) {
        return true;
    }
    struct lfs_info info;
    return lfs_stat(&lfs, path, &info) >= 0 && info.type == LFS_TYPE_DIR;
}

static void cfg_init(void) {
    memset(&cfg, 0, sizeof(cfg));
    cfg.read = bd_read;
    cfg.prog = bd_prog;
    cfg.erase = bd_erase;
    cfg.sync = bd_sync;
    cfg.read_size = FLASH_PAGE_SIZE;
    cfg.prog_size = FLASH_PAGE_SIZE;
    cfg.block_size = FS_BLOCK;
    cfg.block_count = FS_SIZE / FS_BLOCK;
    cfg.cache_size = FLASH_PAGE_SIZE;
    cfg.lookahead_size = sizeof(lookahead_buf);
    cfg.block_cycles = 500;
    cfg.read_buffer = read_buf;
    cfg.prog_buffer = prog_buf;
    cfg.lookahead_buffer = lookahead_buf;
}

bool fs_init(void) {
    cfg_init();
    int err = lfs_mount(&lfs, &cfg);
    if (err) {
        printf("fs: mount %d, formatting\n", err);
        err = lfs_format(&lfs, &cfg);
        if (err) {
            printf("fs: format failed %d\n", err);
            return false;
        }
        err = lfs_mount(&lfs, &cfg);
        if (err) {
            printf("fs: remount failed %d\n", err);
            return false;
        }
    }
    mounted = true;
    printf("fs: mounted %uKiB at flash 0x%lx\n", (unsigned)(FS_SIZE / 1024u),
           (unsigned long)FS_FLASH_OFF);
    return true;
}

int fs_walk(const char *dir, fs_walk_fn cb, void *ctx) {
    if (!mounted || !cb) {
        return -1;
    }
    const char *open_path = "/";
    if (dir && dir[0] && strcmp(dir, "/") != 0) {
        if (!fs_is_dir(dir)) {
            return -1;
        }
        open_path = dir;
    }
    lfs_dir_t ldir;
    if (lfs_dir_open(&lfs, &ldir, open_path) < 0) {
        return -1;
    }
    struct lfs_info info;
    int n = 0;
    while (lfs_dir_read(&lfs, &ldir, &info) > 0) {
        if (strcmp(info.name, ".") == 0 || strcmp(info.name, "..") == 0) {
            continue;
        }
        cb(info.name, info.type == LFS_TYPE_DIR, (size_t)info.size, ctx);
        n++;
    }
    lfs_dir_close(&lfs, &ldir);
    return n;
}

/* How much flash a file really takes, beyond its own size.
 *
 * LittleFS keeps a small file (up to cache_size, 256 B here) INLINE, inside
 * its directory's metadata block: it owns no blocks at all. A larger file is
 * a CTZ skip-list of whole blocks, and every block after the first spends
 * 4 * (ctz(i) + 1) bytes on back-pointers -- so a 4096 B file fits one
 * block (the first carries none) and a 4097 B one needs two.
 * Mirrors lfs_ctz_index (static in lfs.c); checked against a brute-force
 * model of the layout for every size to 70 KB. Returns bytes. */
static uint32_t ctz_blocks(uint32_t size) {
    if (size == 0) { return 0; }
    uint32_t off = size - 1;
    uint32_t b = FS_BLOCK - 2 * 4;
    uint32_t i = off / b;
    if (i == 0) { return 1; }
    i = (off - 4 * ((uint32_t)__builtin_popcount(i - 1) + 2)) / b;
    return i + 1;
}

uint32_t fs_file_alloc(const char *name, uint32_t size) {
    lfs_file_t f;
    if (lfs_file_open(&lfs, &f, name, LFS_O_RDONLY) < 0) { return 0; }
    bool inl = (f.flags & LFS_F_INLINE) != 0;
    lfs_file_close(&lfs, &f);
    return inl ? 0 : ctz_blocks(size) * FS_BLOCK;
}

int fs_list_json(const char *dir, char *buf, size_t cap) {
    if (!mounted || !buf || cap < 16) {
        return -1;
    }
    const char *open_path = "/";
    const char *shown = "";
    if (dir && dir[0] && strcmp(dir, "/") != 0) {
        if (!fs_is_dir(dir)) {
            return -1;
        }
        open_path = dir;
        shown = dir;
    }
    size_t n = 0;
    /* block: the allocation unit; each file's alloc is what it occupies on
     * flash (0 when it is stored inline in this directory's metadata). */
    if (shown[0]) {
        n += (size_t)snprintf(buf + n, cap - n, "{\"dir\":\"%s\",\"block\":%u,\"files\":[", shown, (unsigned)FS_BLOCK);
    } else {
        n += (size_t)snprintf(buf + n, cap - n, "{\"block\":%u,\"files\":[", (unsigned)FS_BLOCK);
    }
    lfs_dir_t ldir;
    if (lfs_dir_open(&lfs, &ldir, open_path) < 0) {
        if (shown[0]) {
            snprintf(buf, cap, "{\"dir\":\"%s\",\"files\":[]}", shown);
        } else {
            snprintf(buf, cap, "{\"files\":[]}");
        }
        return 0;
    }
    struct lfs_info info;
    int first = 1;
    while (lfs_dir_read(&lfs, &ldir, &info) > 0) {
        if (strcmp(info.name, ".") == 0 || strcmp(info.name, "..") == 0) {
            continue;
        }
        if (!first) {
            n += (size_t)snprintf(buf + n, cap - n, ",");
        }
        first = 0;
        if (info.type == LFS_TYPE_DIR) {
            n += (size_t)snprintf(buf + n, cap - n, "{\"name\":\"%s\",\"dir\":true}", info.name);
        } else {
            char full[FS_PATH_MAX + 1];
            snprintf(full, sizeof full, "%s%s%s", shown, shown[0] ? "/" : "", info.name);
            n += (size_t)snprintf(buf + n, cap - n, "{\"name\":\"%s\",\"size\":%u,\"alloc\":%u}",
                                  info.name, (unsigned)info.size, (unsigned)fs_file_alloc(full, info.size));
        }
        if (n + 32 >= cap) {
            break;
        }
    }
    lfs_dir_close(&lfs, &ldir);
    n += (size_t)snprintf(buf + n, cap - n, "]}");
    return 0;
}

bool fs_stat(const char *name, size_t *size) {
    if (!mounted || !fs_valid_path(name)) {
        return false;
    }
    struct lfs_info info;
    if (lfs_stat(&lfs, name, &info) < 0 || info.type != LFS_TYPE_REG) {
        return false;
    }
    if (size) {
        *size = (size_t)info.size;
    }
    return true;
}

static fs_change_fn change_hook;
static char wname[FS_PATH_MAX + 1];

void fs_on_change(fs_change_fn fn) { change_hook = fn; }

static void changed(const char *name) {
    if (change_hook) { change_hook(name); }
}

/* A file, or an EMPTY directory: LittleFS itself refuses a directory with
 * anything in it (FS_ERR_NOTEMPTY), so nothing is ever removed recursively. */
int fs_delete(const char *name) {
    if (!mounted || !fs_valid_path(name)) {
        return -1;
    }
    int err = lfs_remove(&lfs, name);
    if (err == LFS_ERR_NOTEMPTY) { return FS_ERR_NOTEMPTY; }
    if (err >= 0) { changed(name); }
    return err < 0 ? err : 0;
}

int fs_rename(const char *from, const char *to) {
    if (!mounted || !fs_valid_path(from) || !fs_valid_path(to)) {
        return -1;
    }
    if (strcmp(from, to) == 0) {
        return 0;
    }
    if (fs_stat(to, NULL)) {
        return 1;
    }
    ensure_parent_dirs(to);
    int err = lfs_rename(&lfs, from, to);
    if (err == LFS_ERR_EXIST) {
        return 1;
    }
    if (err >= 0) { changed(from); changed(to); }
    return err < 0 ? err : 0;
}

int fs_begin_write(const char *name) {
    if (!mounted || !fs_valid_path(name) || fs_is_dir(name) || wopen) {
        return -1;
    }
    snprintf(wname, sizeof wname, "%s", name);
    ensure_parent_dirs(name);
    int err = lfs_file_open(&lfs, &wfile, name, LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC);
    if (err < 0) {
        printf("fs: open write %s %d\n", name, err);
        return err;
    }
    wopen = true;
    return 0;
}

int fs_write(const uint8_t *data, size_t len) {
    if (!wopen) {
        return -1;
    }
    size_t off = 0;
    while (off < len) {
        lfs_ssize_t n = lfs_file_write(&lfs, &wfile, data + off, len - off);
        if (n <= 0) {
            return n < 0 ? (int)n : -1;
        }
        off += (size_t)n;
    }
    return (int)off;
}

int fs_end_write(void) {
    if (!wopen) {
        return -1;
    }
    int err = lfs_file_close(&lfs, &wfile);
    wopen = false;
    if (err >= 0) { changed(wname); }
    return err;
}

void fs_abort_write(void) {
    if (wopen) {
        lfs_file_close(&lfs, &wfile);
        wopen = false;
    }
}

int fs_begin_read(const char *name, size_t *size) {
    if (!mounted || !fs_valid_path(name)) {
        return -1;
    }
    int h = -1;
    for (int i = 0; i < FS_READS; i++) {
        if (!ropen[i]) {
            h = i;
            break;
        }
    }
    if (h < 0) {
        return -1;
    }
    int err = lfs_file_open(&lfs, &rfiles[h], name, LFS_O_RDONLY);
    if (err < 0) {
        return err;
    }
    ropen[h] = true;
    if (size) {
        *size = (size_t)lfs_file_size(&lfs, &rfiles[h]);
    }
    return h;
}

int fs_read(int h, uint8_t *buf, size_t cap) {
    if (h < 0 || h >= FS_READS || !ropen[h]) {
        return -1;
    }
    return (int)lfs_file_read(&lfs, &rfiles[h], buf, cap);
}

int fs_rewind(int h, size_t n) {
    if (h < 0 || h >= FS_READS || !ropen[h]) {
        return -1;
    }
    lfs_soff_t pos = lfs_file_tell(&lfs, &rfiles[h]);
    if (pos < 0 || (lfs_soff_t)n > pos) {
        return -1;
    }
    if (lfs_file_seek(&lfs, &rfiles[h], pos - (lfs_soff_t)n, LFS_SEEK_SET) < 0) {
        return -1;
    }
    return 0;
}

void fs_end_read(int h) {
    if (h < 0 || h >= FS_READS || !ropen[h]) {
        return;
    }
    lfs_file_close(&lfs, &rfiles[h]);
    ropen[h] = false;
}
