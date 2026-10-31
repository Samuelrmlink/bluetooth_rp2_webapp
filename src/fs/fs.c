/* LittleFS mount + flash block device, adapted from the confirmed-working
 * pm220-pico2w (/home/samuel/Projects/pm220-pico2w/src/fs/fs.c) reference
 * — same block-device glue and file-handle API, generalized from that
 * project's two hardcoded top-level directories to arbitrary nested paths
 * (our web-dist/ tree has css/, js/, js/modals/, vendor/), and pointed at
 * this project's own flash offset/size (see fs.h) instead of reserving a
 * BTstack NVM tail this project has no equivalent of. */
#include "fs.h"
#include "fs_internal.h"
#include "fs_batch.h"
#include "fs_job.h"

#include <stdio.h>
#include <string.h>
#include "autoconf.h"
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
_Static_assert(FS_PEX_SIZE % FLASH_SECTOR_SIZE == 0, "pex partition must be sector-aligned");
_Static_assert(FS_SIZE + FS_PEX_SIZE == FS_REGION_SIZE, "LittleFS + PEX must fill their region");
_Static_assert(FS_FLASH_OFF + FS_REGION_SIZE + FS_BT_TAIL == PICO_FLASH_SIZE_BYTES,
               "filesystem, PEX and the BTstack NVM must fill the chip");

static lfs_t lfs;
static struct lfs_config cfg;
static uint8_t read_buf[FLASH_PAGE_SIZE];
static uint8_t prog_buf[FLASH_PAGE_SIZE];
static uint8_t lookahead_buf[32];
static bool mounted;
/* Blocks of the mounted geometry, and the limit the block device enforces
 * (never past the old LittleFS region: the BTstack NVM follows it). */
static uint32_t g_blocks;
static bool g_legacy;
static const char *g_note = "";

static lfs_file_t wfile;
static bool wopen;
static lfs_file_t rfiles[FS_READS];
static bool ropen[FS_READS];
/* What each reader and the write slot have open, as a hash, so a pool handle
 * can tell it would share a file (fs_fopen). A collision only refuses an open
 * that would have been safe. */
static uint32_t rhash[FS_READS];
static uint32_t whash;

/* A pool handle's bulk -- the LittleFS file, its cache, its name -- lives in
 * memory the caller supplies (fs_file_mem() bytes, the Lua arena in
 * practice): ~480 B a handle that the SD build's static RAM could not spare.
 * Only the bookkeeping is static. */
typedef struct {
    lfs_file_t f;
    struct lfs_file_config fcfg;
    uint8_t buf[FLASH_PAGE_SIZE];
    char name[FS_PATH_MAX + 1];
} pool_mem_t;

typedef struct {
    bool used;
    bool wr;
    bool dirty;
    uint32_t hash;
    pool_mem_t *m;
} pool_file_t;
static pool_file_t pool[FS_FILES];

size_t fs_file_mem(void) { return sizeof(pool_mem_t); }

static uint32_t name_hash(const char *s) {
    uint32_t h = 2166136261u;
    for (; *s; s++) { h ^= (uint8_t)*s; h *= 16777619u; }
    return h ? h : 1;
}

/* Is `name` open anywhere: the pool (writers only when only_writers), the
 * write slot, or (unless only_writers) a reader. */
static bool open_elsewhere(const char *name, bool only_writers) {
    uint32_t h = name_hash(name);
    for (int i = 0; i < FS_FILES; i++) {
        if (pool[i].used && pool[i].hash == h && (!only_writers || pool[i].wr)) { return true; }
    }
    if (wopen && whash == h) { return true; }
    if (!only_writers) {
        for (int i = 0; i < FS_READS; i++) {
            if (ropen[i] && rhash[i] == h) { return true; }
        }
    }
    return false;
}

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

#define BD_BLOCKS_MAX (FS_REGION_SIZE / FS_BLOCK)

/* g_fs_off stays FS_FLASH_OFF unless the layout mover retargets a mount that
 * has already passed its point of no return. g_read_max is that volume's
 * block count, so a mount at 0x250000 cannot be talked into a block count
 * that runs through PEX or the BTstack NVM. */
static uint32_t g_fs_off = FS_FLASH_OFF;
static uint32_t g_read_max = BD_BLOCKS_MAX;
static bool g_freeze_old;

static int bd_range_ok(lfs_block_t block) {
    if (block >= g_read_max) { return 0; }
    uint32_t end = g_fs_off + ((uint32_t)block + 1u) * FS_BLOCK;
    /* The NVM at 0x3FC000 is not a filesystem block on either map. */
    if (end > PICO_FLASH_SIZE_BYTES || end > 0x3FC000u) { return 0; }
    return 1;
}

static int bd_read(const struct lfs_config *c, lfs_block_t block, lfs_off_t off,
                   void *buffer, lfs_size_t size) {
    (void)c;
    if (!bd_range_ok(block) || off + size > FS_BLOCK) { return LFS_ERR_IO; }
    /* NOT XIP_BASE. Booted from an A/B partition, the ROM translates the
     * first 4 MB XIP window so the running slot appears at 0x10000000 and
     * leaves a HOLE past it -- a plain read of 0x10200000 would fault. The
     * no-translate alias reads flash by storage address, which is what the
     * SDK's own partition reader and BTstack's flash bank use on RP2350. */
    memcpy(buffer, (const uint8_t *)(FS_READ_BASE + g_fs_off + block * FS_BLOCK + off), size);
    return 0;
}

static int bd_prog(const struct lfs_config *c, lfs_block_t block, lfs_off_t off,
                   const void *buffer, lfs_size_t size) {
    (void)c;
    if (block >= g_blocks || !bd_range_ok(block) || off + size > FS_BLOCK) { return LFS_ERR_IO; }
    flash_job_t j = {
        .off = g_fs_off + block * FS_BLOCK + off,
        .data = buffer,
        .len = size,
        .prog = 1,
    };
    flash_do(&j);
    return 0;
}

static int bd_erase(const struct lfs_config *c, lfs_block_t block) {
    (void)c;
    if (block >= g_blocks || !bd_range_ok(block)) { return LFS_ERR_IO; }
    flash_job_t j = {
        .off = g_fs_off + block * FS_BLOCK,
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

/* blocks = 0: take the geometry from the superblock (lfs_mount reads it). */
static void cfg_init(uint32_t blocks) {
    memset(&cfg, 0, sizeof(cfg));
    cfg.read = bd_read;
    cfg.prog = bd_prog;
    cfg.erase = bd_erase;
    cfg.sync = bd_sync;
    cfg.read_size = FLASH_PAGE_SIZE;
    cfg.prog_size = FLASH_PAGE_SIZE;
    cfg.block_size = FS_BLOCK;
    cfg.block_count = blocks;
    cfg.cache_size = FLASH_PAGE_SIZE;
    cfg.lookahead_size = sizeof(lookahead_buf);
    cfg.block_cycles = 500;
    cfg.read_buffer = read_buf;
    cfg.prog_buffer = prog_buf;
    cfg.lookahead_buffer = lookahead_buf;
}

/* The filesystem as it is on flash, whatever its size: a superblock that
 * names a geometry other than FS_SIZE is NOT a reason to format.
 *
 * littlefs refuses (LFS_ERR_INVAL) a mount whose block_count differs from the
 * superblock's, and this used to format on any mount error -- so the first
 * firmware with a smaller FS_SIZE would have wiped cfg/ (Wi-Fi, WireGuard:
 * the only way in) on every board it reached. Now a mismatch is mounted with
 * the superblock's own geometry (block_count 0) and left for fs_migrate.c. */
static int mount_as_found(void) {
    cfg_init(0);
    g_blocks = g_read_max;                  /* reads only until the count is known */
    int err = lfs_mount(&lfs, &cfg);
    if (err) { return err; }
    struct lfs_fsinfo fi;
    if (lfs_fs_stat(&lfs, &fi) < 0 || fi.block_count == 0 || fi.block_count > g_read_max) {
        lfs_unmount(&lfs);
        return LFS_ERR_CORRUPT;
    }
    g_blocks = fi.block_count;
    cfg.block_count = fi.block_count;        /* what lfs->block_count already is */
    return 0;
}

bool fs_init(void) {
    cfg_init(FS_SIZE / FS_BLOCK);
    g_blocks = FS_SIZE / FS_BLOCK;
    int err = lfs_mount(&lfs, &cfg);
    if (err && mount_as_found() == 0) {
        mounted = true;
        g_legacy = g_blocks > FS_SIZE / FS_BLOCK;
        printf("fs: superblock says %lu KiB, not %lu: mounted as found%s\n",
               (unsigned long)(g_blocks * FS_BLOCK / 1024u), (unsigned long)(FS_SIZE / 1024u),
               g_legacy ? " (the pre-PEX layout; fs_migrate decides)" : "");
        return true;
    }
    if (err) {
#if CONFIG_FS_LAYOUT
        /* The layout mover formats the old volume only when no move has
         * started, and it never formats 0x200000 once the SD state says the
         * move has. A mount failure here is not a reason to erase. */
        printf("fs: mount %d, not formatting (layout mover decides)\n", err);
        return false;
#else
        cfg_init(FS_SIZE / FS_BLOCK);
        g_blocks = FS_SIZE / FS_BLOCK;
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
#endif
    }
    mounted = true;
    printf("fs: mounted %uKiB at flash 0x%lx\n", (unsigned)(g_blocks * FS_BLOCK / 1024u),
           (unsigned long)g_fs_off);
    return true;
}

uint32_t fs_mounted_size(void) { return mounted ? g_blocks * FS_BLOCK : 0; }
bool fs_legacy(void) { return mounted && g_legacy; }
void fs_set_note(const char *note) { g_note = note ? note : ""; }
const char *fs_note(void) { return g_note; }

#if !CONFIG_FS_MIGRATE
/* The mover is not in this image. An old geometry still mounts (fs_init
 * above); say so, and do not format it. */
void fs_migrate_boot(void) {
    if (!fs_legacy()) { return; }
    fs_set_note("migration pending: not in this build");
    printf("fs: pre-PEX layout, migration is not in this build\n");
}

void fs_migrate_tick(void) {}

int fs_migrate_json(char *buf, size_t cap) {
    const char *m = fs_legacy() ? "migration pending: not in this build" : "none";
    return snprintf(buf, cap,
                    "{\"kb\":%lu,\"layout_kb\":%u,\"migration\":\"%s\","
                    "\"pex\":{\"offset\":%lu,\"size\":%u,\"state\":\"%s\"}}",
                    (unsigned long)(fs_mounted_size() / 1024u), (unsigned)(FS_SIZE / 1024u), m,
                    (unsigned long)FS_PEX_OFF, (unsigned)FS_PEX_SIZE,
                    fs_legacy() ? "inside the old LittleFS until it migrates" : "reserved");
}
#endif

/* --- for fs_migrate.c only (fs_internal.h) --- */
lfs_t *fs_lfs(void) { return mounted ? &lfs : NULL; }

/* Nothing may be open: the migration runs at boot, before anything else
 * touches the filesystem. */
int fs_reformat(uint32_t blocks) {
    if (g_freeze_old && g_fs_off == FS_FLASH_OFF) {
        printf("fs: refused to format 0x%lx (layout move has started)\n", (unsigned long)g_fs_off);
        return LFS_ERR_IO;
    }
    if (blocks == 0 || (uint64_t)g_fs_off + (uint64_t)blocks * FS_BLOCK > 0x3FC000u) {
        printf("fs: refused to format 0x%lx (%lu blocks)\n",
               (unsigned long)g_fs_off, (unsigned long)blocks);
        return LFS_ERR_INVAL;
    }
    if (mounted) { lfs_unmount(&lfs); }
    mounted = false;
    g_legacy = false;
    g_read_max = blocks;
    cfg_init(blocks);
    g_blocks = blocks;
    int err = lfs_format(&lfs, &cfg);
    if (err) { return err; }
    err = lfs_mount(&lfs, &cfg);
    if (err) { return err; }
    mounted = true;
    return 0;
}

bool fs_is_mounted(void) { return mounted; }

void fs_unmount(void) {
    if (mounted) { lfs_unmount(&lfs); }
    mounted = false;
    g_legacy = false;
}

void fs_set_flash_off(uint32_t off, uint32_t read_max_blocks) {
    g_fs_off = off;
    g_read_max = read_max_blocks ? read_max_blocks : BD_BLOCKS_MAX;
}

uint32_t fs_flash_off(void) { return g_fs_off; }

void fs_freeze_old(bool freeze) { g_freeze_old = freeze; }

int fs_mount_at(uint32_t off, uint32_t blocks) {
    if (blocks == 0 || blocks > BD_BLOCKS_MAX) { return LFS_ERR_INVAL; }
    if ((uint64_t)off + (uint64_t)blocks * FS_BLOCK > 0x3FC000u) { return LFS_ERR_INVAL; }
    fs_unmount();
    g_fs_off = off;
    g_read_max = blocks;
    cfg_init(blocks);
    g_blocks = blocks;
    int err = lfs_mount(&lfs, &cfg);
    if (err) { return err; }
    struct lfs_fsinfo fi;
    if (lfs_fs_stat(&lfs, &fi) < 0 || fi.block_count != blocks) {
        lfs_unmount(&lfs);
        return LFS_ERR_CORRUPT;
    }
    mounted = true;
    g_legacy = false;
    printf("fs: mounted %uKiB at flash 0x%lx\n", (unsigned)(blocks * FS_BLOCK / 1024u),
           (unsigned long)off);
    return 0;
}

uint32_t fs_mounted_blocks(void) { return mounted ? g_blocks : 0; }

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
        n += (size_t)snprintf(buf + n, cap - n, "{\"block\":%u,", (unsigned)FS_BLOCK);
        if (g_note[0]) {
            n += (size_t)snprintf(buf + n, cap - n, "\"migration\":\"%s\",", g_note);
        }
        n += (size_t)snprintf(buf + n, cap - n, "\"files\":[");
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
static fs_watch_fn watch_hook;
static char wname[FS_PATH_MAX + 1];

void fs_on_change(fs_change_fn fn) { change_hook = fn; }
void fs_on_watch(fs_watch_fn fn) { watch_hook = fn; }

static void changed(const char *name, const char *op) {
    if (change_hook) { change_hook(name); }
    if (watch_hook) { watch_hook(name, op); }
}

/* A file, or an EMPTY directory: LittleFS itself refuses a directory with
 * anything in it (FS_ERR_NOTEMPTY), so nothing is ever removed recursively. */
int fs_delete(const char *name) {
    if (!mounted || !fs_valid_path(name)) {
        return -1;
    }
    if (open_elsewhere(name, false)) {
        return FS_ERR_BUSY;
    }
    int err = lfs_remove(&lfs, name);
    if (err == LFS_ERR_NOTEMPTY) { return FS_ERR_NOTEMPTY; }
    if (err >= 0) { changed(name, "delete"); }
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
    if (open_elsewhere(from, false)) {
        return FS_ERR_BUSY;
    }
    ensure_parent_dirs(to);
    int err = lfs_rename(&lfs, from, to);
    if (err == LFS_ERR_EXIST) {
        return 1;
    }
    if (err >= 0) { changed(from, "delete"); changed(to, "rename"); }
    return err < 0 ? err : 0;
}

int fs_begin_write(const char *name) {
    if (!mounted || !fs_valid_path(name) || fs_is_dir(name) || wopen) {
        return -1;
    }
    /* A script has it open: replacing it underneath would hand the script a
     * handle onto metadata that no longer exists. */
    for (int i = 0; i < FS_FILES; i++) {
        if (pool[i].used && pool[i].hash == name_hash(name)) { return FS_ERR_BUSY; }
    }
    snprintf(wname, sizeof wname, "%s", name);
    ensure_parent_dirs(name);
    int err = lfs_file_open(&lfs, &wfile, name, LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC);
    if (err < 0) {
        printf("fs: open write %s %d\n", name, err);
        return err;
    }
    wopen = true;
    whash = name_hash(name);
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
    if (err >= 0) { changed(wname, "write"); }
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
    for (int i = 0; i < FS_FILES; i++) {
        if (pool[i].used && pool[i].wr && pool[i].hash == name_hash(name)) { return FS_ERR_BUSY; }
    }
    int err = lfs_file_open(&lfs, &rfiles[h], name, LFS_O_RDONLY);
    if (err < 0) {
        return err;
    }
    ropen[h] = true;
    rhash[h] = name_hash(name);
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

int fs_read_seek(int h, size_t pos) {
    if (h < 0 || h >= FS_READS || !ropen[h]) {
        return -1;
    }
    return lfs_file_seek(&lfs, &rfiles[h], (lfs_soff_t)pos, LFS_SEEK_SET) < 0 ? -1 : 0;
}

/* --- the handle pool ------------------------------------------------------ */
int fs_fopen(const char *name, int mode, void *mem) {
    if (!mem) {
        return LFS_ERR_NOMEM;
    }
    if (!mounted || !fs_valid_path(name)) {
        return LFS_ERR_INVAL;
    }
    if (fs_is_dir(name)) {
        return LFS_ERR_ISDIR;
    }
    bool wr = mode != FS_O_READ;
    if (open_elsewhere(name, !wr)) {
        return FS_ERR_BUSY;
    }
    int h = -1;
    for (int i = 0; i < FS_FILES; i++) {
        if (!pool[i].used) { h = i; break; }
    }
    if (h < 0) {
        return FS_ERR_NOHANDLE;
    }
    pool_file_t *p = &pool[h];
    p->m = (pool_mem_t *)mem;
    memset(p->m, 0, sizeof *p->m);
    p->m->fcfg.buffer = p->m->buf;
    int flags = LFS_O_RDONLY;
    if (mode == FS_O_WRITE) {
        flags = LFS_O_RDWR | LFS_O_CREAT | LFS_O_TRUNC;
    } else if (mode == FS_O_APPEND) {
        flags = LFS_O_RDWR | LFS_O_CREAT | LFS_O_APPEND;
    }
    if (wr) {
        ensure_parent_dirs(name);
    }
    int err = lfs_file_opencfg(&lfs, &p->m->f, name, flags, &p->m->fcfg);
    if (err < 0) {
        return err;
    }
    p->used = true;
    p->wr = wr;
    p->dirty = mode == FS_O_WRITE;      /* a truncation is a change */
    p->hash = name_hash(name);
    snprintf(p->m->name, sizeof p->m->name, "%s", name);
    return h;
}

static pool_file_t *pool_at(int h) {
    return (h >= 0 && h < FS_FILES && pool[h].used) ? &pool[h] : NULL;
}

int fs_fread(int h, void *buf, size_t n) {
    pool_file_t *p = pool_at(h);
    if (!p) { return LFS_ERR_BADF; }
    return (int)lfs_file_read(&lfs, &p->m->f, buf, (lfs_size_t)n);
}

int fs_fwrite(int h, const void *buf, size_t n) {
    pool_file_t *p = pool_at(h);
    if (!p || !p->wr) { return LFS_ERR_BADF; }
    /* Append mode writes at the end whatever the position says. */
    lfs_soff_t at = (p->m->f.flags & LFS_O_APPEND) ? lfs_file_size(&lfs, &p->m->f) : lfs_file_tell(&lfs, &p->m->f);
    if (at < 0) { return (int)at; }
    if ((uint32_t)at + n > FS_FILE_MAX) { return FS_ERR_TOOLARGE; }
    size_t off = 0;
    while (off < n) {
        lfs_ssize_t w = lfs_file_write(&lfs, &p->m->f, (const uint8_t *)buf + off, (lfs_size_t)(n - off));
        if (w <= 0) { return w < 0 ? (int)w : LFS_ERR_IO; }
        off += (size_t)w;
    }
    p->dirty = true;
    return (int)off;
}

int fs_fseek(int h, uint32_t pos) {
    pool_file_t *p = pool_at(h);
    if (!p) { return LFS_ERR_BADF; }
    lfs_soff_t r = lfs_file_seek(&lfs, &p->m->f, (lfs_soff_t)pos, LFS_SEEK_SET);
    return r < 0 ? (int)r : 0;
}

int32_t fs_ftell(int h) {
    pool_file_t *p = pool_at(h);
    return p ? (int32_t)lfs_file_tell(&lfs, &p->m->f) : LFS_ERR_BADF;
}

int32_t fs_fsize(int h) {
    pool_file_t *p = pool_at(h);
    return p ? (int32_t)lfs_file_size(&lfs, &p->m->f) : LFS_ERR_BADF;
}

int fs_fclose(int h) {
    pool_file_t *p = pool_at(h);
    if (!p) { return LFS_ERR_BADF; }
    int err = lfs_file_close(&lfs, &p->m->f);
    bool dirty = p->dirty;
    p->used = false;
    if (dirty && err >= 0) { changed(p->m->name, "write"); }
    p->m = NULL;                 /* the caller frees it now */
    return err;
}

bool fs_usage(uint32_t *used, uint32_t *total) {
    if (!mounted) { return false; }
    lfs_ssize_t blocks = lfs_fs_size(&lfs);
    if (blocks < 0) { return false; }
    if (used) { *used = (uint32_t)blocks * FS_BLOCK; }
    if (total) { *total = g_blocks * FS_BLOCK; }
    return true;
}

void fs_end_read(int h) {
    if (h < 0 || h >= FS_READS || !ropen[h]) {
        return;
    }
    lfs_file_close(&lfs, &rfiles[h]);
    ropen[h] = false;
}

/* --- folders as tar -----------------------------------------------------------
 * GET /api/tar/<folder> (http.c) streams a folder -- or the whole filesystem
 * -- as a ustar archive, straight out of LittleFS: nothing is staged, the
 * board never holds more than one 512 B header. Files go in exactly as
 * stored (a .gz stays compressed), folders get entries of their own so an
 * empty one survives a restore.
 *
 * Names are relative to the folder's parent: downloading "lua" gives
 * "lua/pinecil.lua", ...; the whole filesystem gives "cfg/...", "lua/...".
 *
 * fs_tar_begin walks once to compute the exact length (Content-Length, so a
 * cut-off download is visible); fs_tar_read then produces it a piece at a
 * time. A file whose size changed in between is padded or cut to the size
 * its header promised; if the walk itself no longer matches the promised
 * length, http.c aborts the connection rather than finish a wrong archive.
 * One archive at a time: the state is static. */
#define TAR_DEPTH 8

typedef struct {
    bool active;
    char path[FS_PATH_MAX + 1];      /* the current entry's path on the filesystem */
    size_t base;                     /* where archive names start within path */
    lfs_dir_t dirs[TAR_DEPTH];
    size_t dlen[TAR_DEPTH];          /* path length of each open directory */
    int depth;
    uint32_t mtime;
    uint8_t hdr[512];
    size_t hdr_pos, hdr_len;         /* header bytes still to send */
    lfs_file_t file;
    bool file_open;
    uint32_t data_left;              /* file bytes still to send (header's size) */
    uint32_t pad_left;               /* zero bytes after the data */
    uint32_t end_left;               /* the two closing zero blocks */
    bool walked_out;                 /* every directory read to the end */
    bool list;                       /* POST /api/tar: roots in job_paths */
    int root_i, root_n;
    bool in_root;                    /* walking one directory root */
    int files_done, files_total;
    uint32_t job_id;
} tar_state_t;

static tar_state_t tar;

/* One path table, shared by a list tar and a batch delete. They never run
 * at the same time (each begin fails while the other is active). */
static char job_paths[FS_BATCH_MAX][FS_BATCH_PATH];

enum { REM_IDLE = 0, REM_RUN, REM_DONE, REM_ERR, REM_ABORT };
static int rem_state;
static int rem_i, rem_n;
static bool rem_closed;
static uint32_t rem_id;
static char rem_err[160];

static uint32_t pad512(uint32_t n) { return (512u - (n & 511u)) & 511u; }

static void octal(uint8_t *f, size_t w, uint32_t v) {
    f[w - 1] = 0;
    for (size_t i = w - 1; i-- > 0;) { f[i] = (uint8_t)('0' + (v & 7u)); v >>= 3; }
}

/* A ustar header for `name` (archive name; folders end in '/'). Names up to
 * 100 bytes go in `name`; longer ones are split at a '/' into prefix (155)
 * and name (100). False if it cannot be split -- never with FS_PATH_MAX 128. */
static bool tar_header(uint8_t *h, const char *name, bool dir, uint32_t size, uint32_t mtime) {
    memset(h, 0, 512);
    size_t n = strlen(name);
    if (n <= 100) {
        memcpy(h, name, n);
    } else {
        size_t cut = 0;
        for (size_t i = 0; i < n && i <= 155; i++) {
            if (name[i] == '/' && n - i - 1 <= 100 && n - i - 1 > 0) { cut = i; break; }
        }
        if (!cut) { return false; }
        memcpy(h + 345, name, cut);                 /* prefix */
        memcpy(h, name + cut + 1, n - cut - 1);     /* name */
    }
    octal(h + 100, 8, dir ? 0755u : 0644u);
    octal(h + 108, 8, 0);
    octal(h + 116, 8, 0);
    octal(h + 124, 12, dir ? 0 : size);
    octal(h + 136, 12, mtime);
    h[156] = dir ? '5' : '0';
    memcpy(h + 257, "ustar", 6);                    /* "ustar\0" */
    memcpy(h + 263, "00", 2);
    memcpy(h + 265, "btgw", 4);
    memcpy(h + 297, "btgw", 4);
    memset(h + 148, ' ', 8);
    uint32_t sum = 0;
    for (int i = 0; i < 512; i++) { sum += h[i]; }
    octal(h + 148, 7, sum);                         /* 6 digits, NUL ... */
    h[155] = ' ';                                   /* ... then a space */
    return true;
}

/* The archive's length for this folder: a pass over the same walk.
 * *nfiles counts files only (directory headers are not jobs). */
static int tar_count(char *path, size_t len, uint64_t *total, int *nfiles) {
    lfs_dir_t d;
    if (lfs_dir_open(&lfs, &d, len ? path : "/") < 0) { return -1; }
    struct lfs_info info;
    int rc = 0;
    while (rc == 0 && lfs_dir_read(&lfs, &d, &info) > 0) {
        if (strcmp(info.name, ".") == 0 || strcmp(info.name, "..") == 0) { continue; }
        size_t n = (size_t)snprintf(path + len, FS_PATH_MAX + 1 - len, "%s%s", len ? "/" : "", info.name);
        if (len + n > FS_PATH_MAX) { rc = -1; break; }
        *total += 512;
        if (info.type == LFS_TYPE_DIR) {
            rc = tar_count(path, len + n, total, nfiles);
        } else {
            *total += info.size + pad512(info.size);
            if (nfiles) { (*nfiles)++; }
        }
        path[len] = 0;
    }
    lfs_dir_close(&lfs, &d);
    return rc;
}

void fs_tar_end(void) {
    if (!tar.active) { return; }
    /* A finished archive has walked out and sent both end blocks, with no
     * header or file bytes left. Anything else is the connection closing
     * early: say so, and leave the note until the editor dismisses it. */
    bool finished = tar.walked_out && tar.end_left == 0 && !tar.file_open
        && tar.data_left == 0 && tar.pad_left == 0 && tar.hdr_pos >= tar.hdr_len;
    if (tar.job_id) {
        fs_job_set(tar.job_id, "tar", finished ? "done" : "abort",
                   tar.files_done, tar.files_total, "", 0);
    }
    if (tar.file_open) { lfs_file_close(&lfs, &tar.file); }
    for (int i = 0; i < tar.depth; i++) { lfs_dir_close(&lfs, &tar.dirs[i]); }
    memset(&tar, 0, sizeof tar);
}

bool fs_tar_busy(void) { return tar.active; }

int fs_tar_begin(const char *folder, uint32_t mtime, uint64_t *length) {
    if (!mounted) { return -1; }
    if (tar.active || rem_state != REM_IDLE) { return -2; }
    const char *f = folder ? folder : "";
    if (f[0] && !fs_is_dir(f)) { return -3; }
    memset(&tar, 0, sizeof tar);
    snprintf(tar.path, sizeof tar.path, "%s", f);
    size_t len = strlen(tar.path);
    const char *slash = strrchr(tar.path, '/');
    tar.base = slash ? (size_t)(slash - tar.path) + 1 : 0;
    tar.mtime = mtime;
    uint64_t total = 1024;                           /* the end blocks */
    if (len) { total += 512; }                       /* the folder's own entry */
    tar.files_total = 0;
    if (tar_count(tar.path, len, &total, &tar.files_total) < 0) { tar.path[0] = 0; return -1; }
    tar.path[len] = 0;
    if (lfs_dir_open(&lfs, &tar.dirs[0], len ? tar.path : "/") < 0) { return -1; }
    tar.dlen[0] = len;
    tar.depth = 1;
    tar.active = true;
    tar.end_left = 1024;
    if (len) {                                       /* "lua/" first */
        char nm[FS_PATH_MAX + 2];
        snprintf(nm, sizeof nm, "%s/", tar.path + tar.base);
        tar_header(tar.hdr, nm, true, 0, mtime);
        tar.hdr_len = 512;
    }
    *length = total;
    return 0;
}

static void tar_note_file(const char *path, uint32_t size) {
    if (!tar.job_id) { return; }
    tar.files_done++;
    fs_job_set(tar.job_id, "tar", "run", tar.files_done, tar.files_total, path, size);
}

/* Prepare the next entry's header (and open its file). False when the
 * walk is over. */
static bool tar_next(void) {
    struct lfs_info info;
    while (tar.depth > 0) {
        lfs_dir_t *d = &tar.dirs[tar.depth - 1];
        size_t dl = tar.dlen[tar.depth - 1];
        int r = lfs_dir_read(&lfs, d, &info);
        if (r <= 0) {
            lfs_dir_close(&lfs, d);
            tar.depth--;
            continue;
        }
        if (strcmp(info.name, ".") == 0 || strcmp(info.name, "..") == 0) { continue; }
        size_t n = (size_t)snprintf(tar.path + dl, sizeof tar.path - dl, "%s%s", dl ? "/" : "", info.name);
        if (dl + n > FS_PATH_MAX) { tar.path[dl] = 0; continue; }   /* cannot happen: fs_valid_path */
        size_t plen = dl + n;
        if (info.type == LFS_TYPE_DIR) {
            char nm[FS_PATH_MAX + 2];
            snprintf(nm, sizeof nm, "%s/", tar.path + tar.base);
            tar_header(tar.hdr, nm, true, 0, tar.mtime);
            if (tar.depth < TAR_DEPTH && lfs_dir_open(&lfs, &tar.dirs[tar.depth], tar.path) >= 0) {
                tar.dlen[tar.depth++] = plen;
            }
        } else {
            tar_header(tar.hdr, tar.path + tar.base, false, info.size, tar.mtime);
            tar.file_open = lfs_file_open(&lfs, &tar.file, tar.path, LFS_O_RDONLY) >= 0;
            tar.data_left = info.size;
            tar.pad_left = pad512(info.size);
            tar_note_file(tar.path, info.size);
        }
        tar.hdr_pos = 0;
        tar.hdr_len = 512;
        return true;
    }
    /* A list-mode directory root finishing is not the end of the archive. */
    if (!(tar.list && tar.in_root)) { tar.walked_out = true; }
    return false;
}

/* Folder mode is tar_next. List mode walks job_paths: a file is one header,
 * a directory is its header plus the existing walk, then the next root. */
static bool tar_advance(void) {
    if (!tar.list) { return tar_next(); }
    for (;;) {
        if (tar.in_root) {
            if (tar_next()) { return true; }
            tar.walked_out = false;
            tar.in_root = false;
            tar.root_i++;
            continue;
        }
        if (tar.root_i >= tar.root_n) {
            tar.walked_out = true;
            return false;
        }
        const char *p = job_paths[tar.root_i];
        snprintf(tar.path, sizeof tar.path, "%s", p);
        tar.base = 0;
        if (fs_is_dir(p)) {
            char nm[FS_PATH_MAX + 2];
            snprintf(nm, sizeof nm, "%s/", p);
            tar_header(tar.hdr, nm, true, 0, tar.mtime);
            tar.depth = 0;
            tar.in_root = false;
            if (lfs_dir_open(&lfs, &tar.dirs[0], tar.path) >= 0) {
                tar.dlen[0] = strlen(tar.path);
                tar.depth = 1;
                tar.in_root = true;
            } else {
                tar.root_i++;
            }
            tar.hdr_pos = 0;
            tar.hdr_len = 512;
            return true;
        }
        size_t sz = 0;
        if (!fs_stat(p, &sz)) { sz = 0; }
        tar_header(tar.hdr, p, false, (uint32_t)sz, tar.mtime);
        tar.file_open = lfs_file_open(&lfs, &tar.file, tar.path, LFS_O_RDONLY) >= 0;
        tar.data_left = (uint32_t)sz;
        tar.pad_left = pad512((uint32_t)sz);
        tar.hdr_pos = 0;
        tar.hdr_len = 512;
        tar_note_file(p, (uint32_t)sz);
        tar.root_i++;
        return true;
    }
}

/* Up to `cap` bytes of the archive. 0 once it is all out; < 0 on error. */
int fs_tar_read(uint8_t *buf, size_t cap) {
    if (!tar.active) { return -1; }
    size_t out = 0;
    while (out < cap) {
        if (tar.hdr_pos < tar.hdr_len) {
            size_t k = tar.hdr_len - tar.hdr_pos;
            if (k > cap - out) { k = cap - out; }
            memcpy(buf + out, tar.hdr + tar.hdr_pos, k);
            tar.hdr_pos += k;
            out += k;
            continue;
        }
        if (tar.data_left) {
            size_t k = tar.data_left < cap - out ? tar.data_left : cap - out;
            lfs_ssize_t got = tar.file_open ? lfs_file_read(&lfs, &tar.file, buf + out, k) : 0;
            if (got < 0) { return (int)got; }
            if ((size_t)got < k) { memset(buf + out + got, 0, k - (size_t)got); }  /* shrank: pad */
            tar.data_left -= (uint32_t)k;
            out += k;
            continue;
        }
        if (tar.file_open) { lfs_file_close(&lfs, &tar.file); tar.file_open = false; }
        if (tar.pad_left) {
            size_t k = tar.pad_left < cap - out ? tar.pad_left : cap - out;
            memset(buf + out, 0, k);
            tar.pad_left -= (uint32_t)k;
            out += k;
            continue;
        }
        if (!tar.walked_out && tar_advance()) { continue; }
        if (tar.end_left) {
            size_t k = tar.end_left < cap - out ? tar.end_left : cap - out;
            memset(buf + out, 0, k);
            tar.end_left -= (uint32_t)k;
            out += k;
            continue;
        }
        break;
    }
    return (int)out;
}

void fs_tar_set_job(uint32_t id) {
    if (tar.active) { tar.job_id = id; }
}

int fs_tar_begin_json(const char *json, size_t json_len, uint64_t *length) {
    if (!mounted || !json || !length) { return -1; }
    if (tar.active || rem_state != REM_IDLE) { return -3; }
    int n = 0;
    uint32_t mtime = 0, id = 0;
    int pr = fs_batch_parse(json, json_len, job_paths, &n, &mtime, &id);
    if (pr == -2) { return -2; }
    if (pr < 0) { return -1; }
    uint64_t total = 1024;
    int nfiles = 0;
    char scratch[FS_PATH_MAX + 1];
    for (int i = 0; i < n; i++) {
        if (!fs_valid_path(job_paths[i])) { return -1; }
        if (fs_is_dir(job_paths[i])) {
            snprintf(scratch, sizeof scratch, "%s", job_paths[i]);
            size_t len = strlen(scratch);
            total += 512;
            int nf = 0;
            if (tar_count(scratch, len, &total, &nf) < 0) { return -1; }
            nfiles += nf;
        } else {
            size_t sz = 0;
            if (!fs_stat(job_paths[i], &sz)) { return -4; }
            total += 512ull + (uint64_t)sz + (uint64_t)pad512((uint32_t)sz);
            nfiles++;
        }
    }
    memset(&tar, 0, sizeof tar);
    tar.list = true;
    tar.root_n = n;
    tar.mtime = mtime;
    tar.job_id = id;
    tar.files_total = nfiles;
    tar.base = 0;
    tar.active = true;
    tar.end_left = 1024;
    *length = total;
    return 0;
}

int fs_remove_begin_json(const char *json, size_t json_len) {
    if (!mounted || !json) { return -1; }
    if (tar.active || rem_state != REM_IDLE) { return -3; }
    int n = 0;
    uint32_t id = 0;
    int pr = fs_batch_parse(json, json_len, job_paths, &n, NULL, &id);
    if (pr == -2) { return -2; }
    if (pr < 0) { return -1; }
    for (int i = 0; i < n; i++) {
        if (!fs_valid_path(job_paths[i])) { return -1; }
    }
    rem_state = REM_RUN;
    rem_i = 0;
    rem_n = n;
    rem_closed = false;
    rem_id = id;
    rem_err[0] = 0;
    return 0;
}

void fs_remove_abort(void) {
    if (rem_state == REM_IDLE) { return; }
    if (rem_state == REM_RUN) {
        fs_job_set(rem_id, "delete", "abort", rem_i, rem_n, "", 0);
    }
    rem_state = REM_IDLE;
    rem_closed = false;
    rem_n = 0;
    rem_i = 0;
}

void fs_remove_pump(void) {
    if (rem_state != REM_RUN) { return; }
    if (rem_closed) {
        fs_job_set(rem_id, "delete", "abort", rem_i, rem_n, "", 0);
        rem_state = REM_ABORT;
        return;
    }
    if (rem_i >= rem_n) {
        fs_job_set(rem_id, "delete", "done", rem_n, rem_n, "", 0);
        rem_state = REM_DONE;
        return;
    }
    const char *p = job_paths[rem_i];
    fs_job_set(rem_id, "delete", "run", rem_i + 1, rem_n, p, 0);
    int rc = fs_delete(p);
    if (rc < 0) {
        snprintf(rem_err, sizeof rem_err, "%s: %s", p,
                 rc == FS_ERR_NOTEMPTY ? "folder is not empty" : "delete failed");
        rem_state = REM_ERR;
        return;
    }
    rem_i++;
    if (rem_i >= rem_n) {
        fs_job_set(rem_id, "delete", "done", rem_n, rem_n, "", 0);
        rem_state = REM_DONE;
    }
}

int fs_remove_result(int *removed, int *total, char *err, size_t errcap) {
    if (rem_state == REM_IDLE || rem_state == REM_RUN) { return 0; }
    if (removed) { *removed = rem_i; }
    if (total) { *total = rem_n; }
    int st = rem_state == REM_DONE ? 1 : rem_state == REM_ABORT ? -2 : -1;
    if (st < 0 && err && errcap) {
        snprintf(err, errcap, "%s", rem_err[0] ? rem_err : "delete failed");
    }
    rem_state = REM_IDLE;
    rem_closed = false;
    return st;
}

/* PUT /api/fs/<path>/ -- an (empty) folder, parents included. */
int fs_mkdir(const char *path) {
    if (!mounted || !fs_valid_path(path)) { return -1; }
    if (fs_is_dir(path)) { return 0; }
    char child[FS_PATH_MAX + 2];
    snprintf(child, sizeof child, "%s/x", path);
    ensure_parent_dirs(child);                    /* makes every prefix of path, path included */
    return fs_is_dir(path) ? 0 : -1;
}
