/* The pre-PEX LittleFS -> the smaller one, through the SD card. fs_migrate.h
 * says what and why; docs/flash-layout.md has the state machine and every
 * failure case. The rules this file keeps:
 *
 *  1. Never format unless a complete copy is on the card AND has been read
 *     back and checked (size + CRC-32 of every file, CRC of the manifest).
 *  2. Before formatting, the card's state file says "formatting": from then
 *     on the old filesystem is never trusted again, and every boot restores
 *     from the card until a restore has been verified.
 *  3. cfg/ (Wi-Fi, Ethernet, WireGuard: the way back in) is restored first,
 *     and is also held in RAM while this boot migrates, as a last resort.
 *  4. Runs at boot, before the network config is read. It feeds the watchdog
 *     and drives the SD worker (sd_poll) itself; card I/O stays in the worker
 *     (sd_jobs.h), flash I/O here, in bounded steps.
 *
 * Files on the card are numbered (.lfs-migrate/<run>/f/<n>) with their paths
 * in a text manifest: FAT is case-insensitive and has its own name rules and
 * a 128-byte job path limit; numbered files sidestep all three. */
#include "fs/fs_migrate.h"

#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "autoconf.h"
#include "fs/fs.h"
#include "fs/fs_internal.h"
#include "hardware/watchdog.h"
#include "pico/stdlib.h"
#include "pico/unique_id.h"
#include "ota/crc32.h"
#if CONFIG_OTA
#include "ota/ota_slot.h"
#endif
#if CONFIG_SD
#include "sd/sd_api.h"
#include "sd/sd_core.h"
#include "sd/sd_jobs.h"
#include "sd/sd_store.h"
#endif
#include "api/api.h"

#define BLK          4096u
#define OLD_BLOCKS   (FS_REGION_SIZE / BLK)
#define NEW_BLOCKS   (FS_SIZE / BLK)
#define MARKER       "cfg/migrated.json"     /* on flash: this filesystem is a verified migration */
/* On flash: a restore was started and not finished. In the root, so it is
 * the FIRST thing a format is followed by (no mkdir before it: a cut after a
 * "cfg" folder and before this file looked like a normal filesystem with no
 * cfg/ in it -- found by the power-cut test). A dot name: the API cannot
 * touch it (fs_valid_path). */
#define INPROGRESS   ".migrating"
#define MIG_DIR      ".lfs-migrate"
#define STATE        MIG_DIR "/state"
#define STATE_TMP    MIG_DIR "/state.tmp"
#define MAX_ATTEMPTS 3                       /* boots that may each try a restore */
#define CFG_RAM_MAX  (8u * 1024u)
#define DEPTH        10

static enum { MIG_NONE, MIG_DONE, MIG_MIGRATED, MIG_PENDING } s_kind;
static char s_msg[120];
static char s_board[17];
static bool s_wait_buy;

static uint32_t now_ms(void) { return to_ms_since_boot(get_absolute_time()); }

static void set_pending(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void set_pending(const char *fmt, ...) {
    char why[88];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(why, sizeof why, fmt, ap);
    va_end(ap);
    snprintf(s_msg, sizeof s_msg, "migration pending: %s", why);
    s_kind = MIG_PENDING;
    fs_set_note(s_msg);
    printf("migrate: %s\n", s_msg);
}

static bool lfs_exists(const char *path) {
    struct lfs_info info;
    return lfs_stat(fs_lfs(), path, &info) >= 0;
}

static bool root_empty(void) {
    lfs_dir_t d;
    if (lfs_dir_open(fs_lfs(), &d, "/") < 0) { return false; }
    struct lfs_info info;
    bool empty = true;
    while (lfs_dir_read(fs_lfs(), &d, &info) > 0) {
        if (strcmp(info.name, ".") && strcmp(info.name, "..")) { empty = false; break; }
    }
    lfs_dir_close(fs_lfs(), &d);
    return empty;
}

#if CONFIG_SD
static int lfs_put(const char *path, const void *data, size_t len) {
    lfs_file_t f;
    int r = lfs_file_open(fs_lfs(), &f, path, LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC);
    if (r < 0) { return r; }
    lfs_ssize_t w = lfs_file_write(fs_lfs(), &f, data, (lfs_size_t)len);
    r = lfs_file_close(fs_lfs(), &f);
    return w != (lfs_ssize_t)len ? LFS_ERR_IO : r;
}

/* mkdir -p for everything before the last '/' of path (and path itself
 * when `self`). */
static void lfs_mkdirs(const char *path, bool self) {
    char b[FS_PATH_MAX + 1];
    snprintf(b, sizeof b, "%s", path);
    size_t n = strlen(b);
    for (size_t i = 0; i <= n; i++) {
        if (b[i] != '/' && (b[i] != 0 || !self)) { continue; }
        char c = b[i];
        b[i] = 0;
        if (b[0]) { lfs_mkdir(fs_lfs(), b); }
        b[i] = c;
        if (!c) { break; }
    }
}

/* ===================================================================== */

/* The C heap is small in the SD build (~20 KB, and pico_malloc PANICS when
 * it runs out), so nothing here is allocated without checking first: what
 * is left between the heap's top and the stack, with a margin. */
static void *try_alloc(size_t n) {
    extern char __StackLimit;
    char *brk = sbrk(0);
    size_t top = (brk != (char *)-1 && brk < &__StackLimit) ? (size_t)(&__StackLimit - brk) : 0;
    struct mallinfo mi = mallinfo();
    if (top < n + 2048u && (size_t)mi.fordblks < n + 2048u) { return NULL; }
    return malloc(n);
}

/* ---- the card, through the storage worker's job slot ---------------- */

static void pump(void) {
    watchdog_update();
    sd_poll();
}

#define JOB_TIMEOUT_MS 20000u

/* Posts q, drives the worker until it is done, copies a READ's data out
 * (or a WRITE's in). Returns the job's rc (LS_E* < 0). */
static int job(sdj_t *q, const void *wr, void *rd, sdj_t *out) {
    uint32_t tok, t0 = now_ms();
    while (!sdj_post(q, &tok)) {
        if (now_ms() - t0 > JOB_TIMEOUT_MS) { return LS_EBUSY; }
        pump();
    }
    if (q->op == SDJ_WRITE && wr) { memcpy(sdj_buf(), wr, q->len); }
    sdj_t r;
    for (;;) {
        if (sdj_poll(tok, &r) == SDJ_DONE) { break; }
        if (now_ms() - t0 > JOB_TIMEOUT_MS) { sdj_abandon(tok); return LS_EIO; }
        pump();
    }
    if (q->op == SDJ_READ && rd && r.rc > 0) { memcpy(rd, sdj_buf(), (size_t)r.rc); }
    sdj_finish(tok);
    if (out) { *out = r; }
    return r.rc;
}

static int sd_open(const char *path, int mode) {
    sdj_t q = { .op = SDJ_OPEN, .mode = mode };
    snprintf(q.path, sizeof q.path, "%s", path);
    return job(&q, NULL, NULL, NULL);
}

static int sd_read(int f, void *buf, uint32_t n) {
    sdj_t q = { .op = SDJ_READ, .file = f, .len = n };
    return job(&q, NULL, buf, NULL);
}

static int sd_write(int f, const void *buf, uint32_t n) {
    sdj_t q = { .op = SDJ_WRITE, .file = f, .len = n };
    return job(&q, buf, NULL, NULL);
}

static int sd_close(int f) {
    sdj_t q = { .op = SDJ_CLOSE, .file = f };
    return job(&q, NULL, NULL, NULL);
}

static int sd_path_op(int op, const char *a, const char *b) {
    sdj_t q = { .op = op };
    snprintf(q.path, sizeof q.path, "%s", a);
    if (b) { snprintf(q.path2, sizeof q.path2, "%s", b); }
    return job(&q, NULL, NULL, NULL);
}

/* A whole small file onto the card, closed (FatFs syncs on close). */
static int sd_put(const char *path, const void *data, uint32_t len) {
    int f = sd_open(path, LS_WR);
    if (f < 0) { return f; }
    int r = len ? sd_write(f, data, len) : 0;
    int c = sd_close(f);
    if (r < 0) { return r; }
    if ((uint32_t)r != len) { return LS_EIO; }
    return c;
}

/* ---- the state file --------------------------------------------------- */

enum { PH_NONE = 0, PH_COPYING, PH_FORMATTING, PH_DONE };
static const char *const PH_NAME[] = { "none", "copying", "formatting", "done" };

typedef struct {
    int phase;
    unsigned run, attempts, entries, files;
    unsigned long bytes;
    unsigned old_blocks, new_blocks;
    char board[17];
} mig_state_t;

static int state_write(const mig_state_t *s) {
    char b[256];
    int n = snprintf(b, sizeof b,
                     "btgw-lfs-migrate 1\nphase=%s\nrun=%u\nattempts=%u\nentries=%u\nfiles=%u\n"
                     "bytes=%lu\nold_blocks=%u\nnew_blocks=%u\nboard=%s\n",
                     PH_NAME[s->phase], s->run, s->attempts, s->entries, s->files, s->bytes,
                     s->old_blocks, s->new_blocks, s->board);
    int r = sd_put(STATE_TMP, b, (uint32_t)n);
    if (r < 0) { return r; }
    /* tmp, then replace: a cut between the remove and the rename leaves the
     * tmp, which state_read() also looks at. */
    sd_path_op(SDJ_REMOVE, STATE, NULL);
    return sd_path_op(SDJ_RENAME, STATE_TMP, STATE);
}

static bool state_parse(const char *b, mig_state_t *s) {
    memset(s, 0, sizeof *s);
    if (strncmp(b, "btgw-lfs-migrate 1\n", 19) != 0) { return false; }
    const char *p = b;
    while ((p = strchr(p, '\n')) != NULL) {
        p++;
        char k[16], v[24];
        if (sscanf(p, "%15[a-z_]=%23[^\n]", k, v) != 2) { continue; }
        if (!strcmp(k, "phase")) {
            for (int i = 0; i < 4; i++) { if (!strcmp(v, PH_NAME[i])) { s->phase = i; } }
        } else if (!strcmp(k, "run")) { s->run = (unsigned)strtoul(v, NULL, 10); }
        else if (!strcmp(k, "attempts")) { s->attempts = (unsigned)strtoul(v, NULL, 10); }
        else if (!strcmp(k, "entries")) { s->entries = (unsigned)strtoul(v, NULL, 10); }
        else if (!strcmp(k, "files")) { s->files = (unsigned)strtoul(v, NULL, 10); }
        else if (!strcmp(k, "bytes")) { s->bytes = strtoul(v, NULL, 10); }
        else if (!strcmp(k, "old_blocks")) { s->old_blocks = (unsigned)strtoul(v, NULL, 10); }
        else if (!strcmp(k, "new_blocks")) { s->new_blocks = (unsigned)strtoul(v, NULL, 10); }
        else if (!strcmp(k, "board")) { snprintf(s->board, sizeof s->board, "%s", v); }
    }
    return s->phase != PH_NONE && s->run > 0;
}

static bool state_read(mig_state_t *s) {
    static const char *const names[] = { STATE, STATE_TMP };
    char b[257];
    for (int i = 0; i < 2; i++) {
        int f = sd_open(names[i], LS_RD);
        if (f < 0) { continue; }
        int n = sd_read(f, b, sizeof b - 1);
        sd_close(f);
        if (n <= 0) { continue; }
        b[n] = 0;
        if (state_parse(b, s)) { return true; }
    }
    memset(s, 0, sizeof *s);
    return false;
}

/* ---- the copy: flash -> card ------------------------------------------ */

static uint8_t *s_buf;               /* BLK bytes: an idle SD transfer ring, or the heap */
static int s_ring = -1;
static uint8_t *s_cfg;               /* cfg/ files: [u16 len][path][u32 size][data]... */
static uint32_t s_cfg_len;
static bool s_cfg_ok;                /* every cfg/ file fit */
static uint32_t s_cfg_cap;

/* Depth-first over the whole filesystem, folders before their content,
 * without recursion: core0's stack is 2 KB nominal, and this runs under
 * littlefs and SD calls of its own. */
typedef struct {
    lfs_dir_t d[DEPTH];
    size_t len[DEPTH];
    int depth;
    char path[FS_PATH_MAX + 1];
    struct lfs_info info;
} walker_t;

typedef struct {
    walker_t w;
    unsigned run;
    unsigned entries, files, dirs;
    unsigned long bytes;
    int man;                         /* manifest file on the card */
    char mbuf[512];                  /* manifest bytes not yet written */
    uint32_t mlen, mcrc;
    char err[64];
    int rc;
} copy_t;

static void run_path(char *dst, size_t cap, unsigned run, const char *rest) {
    snprintf(dst, cap, MIG_DIR "/%u/%s", run, rest);
}

static int man_flush(copy_t *c) {
    if (!c->mlen) { return 0; }
    int r = sd_write(c->man, c->mbuf, c->mlen);
    if (r != (int)c->mlen) { return r < 0 ? r : LS_EIO; }
    c->mlen = 0;
    return 0;
}

static int man_line(copy_t *c, const char *line) {
    size_t n = strlen(line);
    c->mcrc = crc32_update(c->mcrc, line, n);
    if (c->mlen + n > sizeof c->mbuf) {
        int r = man_flush(c);
        if (r < 0) { return r; }
    }
    memcpy(c->mbuf + c->mlen, line, n);
    c->mlen += (uint32_t)n;
    return 0;
}

static void cfg_keep(const char *path, uint32_t size, const uint8_t *data, uint32_t off, uint32_t n) {
    if (!s_cfg_ok || !s_cfg) { return; }
    size_t pl = strlen(path);
    if (off == 0) {
        if (s_cfg_len + 2 + pl + 4 + size > s_cfg_cap) { s_cfg_ok = false; return; }
        s_cfg[s_cfg_len] = (uint8_t)pl;
        s_cfg[s_cfg_len + 1] = (uint8_t)(pl >> 8);
        memcpy(s_cfg + s_cfg_len + 2, path, pl);
        memcpy(s_cfg + s_cfg_len + 2 + pl, &size, 4);
        s_cfg_len += (uint32_t)(2 + pl + 4);
    }
    memcpy(s_cfg + s_cfg_len, data, n);
    s_cfg_len += n;
}

static bool is_cfg(const char *path) {
    return strcmp(path, "cfg") == 0 || strncmp(path, "cfg/", 4) == 0;
}

/* One file: flash -> card, then the card's copy read back and checked. */
static int copy_file(copy_t *c, const char *path, uint32_t size) {
    unsigned idx = c->files + 1;
    char sp[SDJ_PATH_MAX + 1], name[16];
    snprintf(name, sizeof name, "f/%u", idx);
    run_path(sp, sizeof sp, c->run, name);
    lfs_file_t lf;
    int r = lfs_file_open(fs_lfs(), &lf, path, LFS_O_RDONLY);
    if (r < 0) { snprintf(c->err, sizeof c->err, "read %s", path); return r; }
    int f = sd_open(sp, LS_WR);
    if (f < 0) { lfs_file_close(fs_lfs(), &lf); snprintf(c->err, sizeof c->err, "SD create failed"); return f; }
    uint32_t crc = 0, total = 0;
    int rc = 0;
    for (;;) {
        watchdog_update();
        lfs_ssize_t n = lfs_file_read(fs_lfs(), &lf, s_buf, BLK);
        if (n < 0) { rc = (int)n; snprintf(c->err, sizeof c->err, "read %s", path); break; }
        if (n == 0) { break; }
        crc = crc32_update(crc, s_buf, (size_t)n);
        if (is_cfg(path)) { cfg_keep(path, size, s_buf, total, (uint32_t)n); }
        total += (uint32_t)n;
        int w = sd_write(f, s_buf, (uint32_t)n);
        if (w != n) { rc = w < 0 ? w : LS_ENOSPC; snprintf(c->err, sizeof c->err, "SD write failed"); break; }
    }
    lfs_file_close(fs_lfs(), &lf);
    int cr = sd_close(f);
    if (rc == 0 && cr < 0) { rc = cr; snprintf(c->err, sizeof c->err, "SD close failed"); }
    if (rc == 0 && total != size) { rc = LS_EIO; snprintf(c->err, sizeof c->err, "%s changed size", path); }
    if (rc < 0) { return rc; }
    /* Read the card's copy back. */
    f = sd_open(sp, LS_RD);
    if (f < 0) { snprintf(c->err, sizeof c->err, "SD reopen failed"); return f; }
    uint32_t crc2 = 0, total2 = 0;
    for (;;) {
        int n = sd_read(f, s_buf, BLK);
        if (n < 0) { rc = n; break; }
        if (n == 0) { break; }
        crc2 = crc32_update(crc2, s_buf, (size_t)n);
        total2 += (uint32_t)n;
    }
    sd_close(f);
    if (rc < 0 || crc2 != crc || total2 != total) {
        snprintf(c->err, sizeof c->err, "SD copy of %s does not verify", path);
        return rc < 0 ? rc : LS_EIO;
    }
    char line[FS_PATH_MAX + 48];
    snprintf(line, sizeof line, "F %u %lu %08lx %s\n", idx, (unsigned long)size, (unsigned long)crc, path);
    r = man_line(c, line);
    if (r < 0) { snprintf(c->err, sizeof c->err, "SD manifest write failed"); return r; }
    c->files++;
    c->entries++;
    c->bytes += size;
    if (c->files % 25 == 0) {
        printf("migrate: %u files, %lu bytes on the card\n", c->files, c->bytes);
    }
    return 0;
}

static int walker_start(walker_t *w) {
    memset(w, 0, sizeof *w);
    int r = lfs_dir_open(fs_lfs(), &w->d[0], "/");
    if (r < 0) { return r; }
    w->depth = 1;
    return 0;
}

static void walker_stop(walker_t *w) {
    while (w->depth > 0) { lfs_dir_close(fs_lfs(), &w->d[--w->depth]); }
}

/* 1: an entry in w->path / w->info; 0: the end; < 0: an error. A folder
 * returned here is entered next. */
static int walker_next(walker_t *w) {
    while (w->depth > 0) {
        int k = w->depth - 1;
        int r = lfs_dir_read(fs_lfs(), &w->d[k], &w->info);
        if (r < 0) { return r; }
        if (r == 0) {
            lfs_dir_close(fs_lfs(), &w->d[k]);
            w->depth--;
            continue;
        }
        if (!strcmp(w->info.name, ".") || !strcmp(w->info.name, "..")) { continue; }
        size_t len = w->len[k];
        size_t n = (size_t)snprintf(w->path + len, sizeof w->path - len, "%s%s", len ? "/" : "", w->info.name);
        if (len + n > FS_PATH_MAX) { return LFS_ERR_NAMETOOLONG; }
        if (w->info.type == LFS_TYPE_DIR) {
            if (w->depth >= DEPTH) { return LFS_ERR_INVAL; }
            int e = lfs_dir_open(fs_lfs(), &w->d[w->depth], w->path);
            if (e < 0) { return e; }
            w->len[w->depth++] = len + n;
        }
        return 1;
    }
    return 0;
}

/* Count what a copy will hold (no card I/O). */
static int tally(walker_t *w, unsigned *files, unsigned long *bytes, uint32_t *cfg_need) {
    int r = walker_start(w);
    while (r >= 0 && (r = walker_next(w)) > 0) {
        if (w->info.type != LFS_TYPE_DIR) {
            (*files)++;
            *bytes += w->info.size;
            if (is_cfg(w->path)) { *cfg_need += 2u + (uint32_t)strlen(w->path) + 4u + w->info.size; }
        }
    }
    walker_stop(w);
    return r;
}

static int walk(copy_t *c) {
    walker_t *w = &c->w;
    int r = walker_start(w);
    if (r < 0) { snprintf(c->err, sizeof c->err, "open /"); return r; }
    while ((r = walker_next(w)) > 0) {
        if (w->info.type == LFS_TYPE_DIR) {
            char line[FS_PATH_MAX + 4];
            snprintf(line, sizeof line, "D %s\n", w->path);
            r = man_line(c, line);
            if (r < 0) { snprintf(c->err, sizeof c->err, "SD manifest write failed"); break; }
            c->dirs++;
            c->entries++;
        } else {
            r = copy_file(c, w->path, w->info.size);
            if (r < 0) { break; }
        }
    }
    if (r < 0 && !c->err[0]) { snprintf(c->err, sizeof c->err, "walk failed"); }
    walker_stop(w);
    return r < 0 ? r : 0;
}

/* ---- the manifest, read back ------------------------------------------ */

typedef struct {
    int f;
    char buf[512];
    int len, pos;
    bool eof;
    uint32_t crc;      /* over every line before the "E" line */
    bool ended;        /* saw the "E" line */
    unsigned e_entries;
    uint32_t e_crc;
} man_rd_t;

static int man_open(man_rd_t *m, unsigned run) {
    memset(m, 0, sizeof *m);
    char p[64];
    run_path(p, sizeof p, run, "manifest");
    m->f = sd_open(p, LS_RD);
    return m->f;
}

/* Next line without its '\n'; 0 at the end, < 0 on an error. */
static int man_next(man_rd_t *m, char *line, size_t cap) {
    size_t n = 0;
    for (;;) {
        if (m->pos == m->len) {
            if (m->eof) { return n ? LS_EIO : 0; }          /* a cut-off last line */
            int r = sd_read(m->f, m->buf, sizeof m->buf);
            if (r < 0) { return r; }
            if (r == 0) { m->eof = true; continue; }
            m->len = r;
            m->pos = 0;
        }
        char ch = m->buf[m->pos++];
        if (n + 1 >= cap) { return LS_EINVAL; }
        line[n++] = ch;
        if (ch == '\n') { break; }
    }
    line[n] = 0;
    if (line[0] == 'E') {
        unsigned long cr;
        m->ended = sscanf(line, "E %u %lx", &m->e_entries, &cr) == 2;
        m->e_crc = (uint32_t)cr;
    } else {
        m->crc = crc32_update(m->crc, line, n);
    }
    line[n - 1] = 0;
    return 1;
}

/* An entry: 'D' with path, or 'F' with idx/size/crc/path. */
typedef struct { char kind; unsigned idx; uint32_t size, crc; const char *path; } entry_t;

static bool entry_parse(char *line, entry_t *e) {
    memset(e, 0, sizeof *e);
    e->kind = line[0];
    if (e->kind == 'D' && line[1] == ' ') { e->path = line + 2; return fs_valid_path(e->path); }
    if (e->kind != 'F') { return false; }
    unsigned long sz, cr;
    int off = 0;
    if (sscanf(line, "F %u %lu %lx %n", &e->idx, &sz, &cr, &off) != 3 || !off) { return false; }
    e->size = (uint32_t)sz;
    e->crc = (uint32_t)cr;
    e->path = line + off;
    return fs_valid_path(e->path);
}

/* The whole manifest, checked: its own CRC, its count, and (deep) every
 * file on the card read and checked against it. */
static int man_check(unsigned run, bool deep, unsigned *entries, char *err, size_t errcap) {
    man_rd_t m;
    if (man_open(&m, run) < 0) { snprintf(err, errcap, "no manifest on the card"); return LS_ENOENT; }
    char line[FS_PATH_MAX + 48];
    unsigned n = 0;
    int r;
    while ((r = man_next(&m, line, sizeof line)) > 0) {
        if (m.ended) { break; }
        entry_t e;
        if (!entry_parse(line, &e)) { r = LS_EINVAL; break; }
        n++;
        if (!deep || e.kind != 'F') { continue; }
        char sp[64], name[16];
        snprintf(name, sizeof name, "f/%u", e.idx);
        run_path(sp, sizeof sp, run, name);
        int f = sd_open(sp, LS_RD);
        if (f < 0) { r = f; break; }
        uint32_t crc = 0, tot = 0;
        int k;
        while ((k = sd_read(f, s_buf, BLK)) > 0) { crc = crc32_update(crc, s_buf, (size_t)k); tot += (uint32_t)k; }
        sd_close(f);
        if (k < 0 || crc != e.crc || tot != e.size) {
            snprintf(err, errcap, "SD copy of %s is damaged", e.path);
            sd_close(m.f);
            return LS_EIO;
        }
    }
    sd_close(m.f);
    if (r < 0 || !m.ended || m.e_entries != n || m.e_crc != m.crc) {
        snprintf(err, errcap, "the manifest on the card is incomplete or damaged");
        return LS_EIO;
    }
    if (entries) { *entries = n; }
    return 0;
}

/* ---- the restore: card -> flash ---------------------------------------- */

static int flash_crc(const char *path, uint32_t *crc, uint32_t *size) {
    lfs_file_t f;
    int r = lfs_file_open(fs_lfs(), &f, path, LFS_O_RDONLY);
    if (r < 0) { return r; }
    *crc = 0;
    *size = 0;
    lfs_ssize_t n;
    while ((n = lfs_file_read(fs_lfs(), &f, s_buf, BLK)) > 0) {
        *crc = crc32_update(*crc, s_buf, (size_t)n);
        *size += (uint32_t)n;
        watchdog_update();
    }
    lfs_file_close(fs_lfs(), &f);
    return n < 0 ? (int)n : 0;
}

/* -1: the card's copy is bad (or unreadable); -2: flash would not take it. */
static int restore_file(unsigned run, const entry_t *e) {
    char sp[64], name[16];
    snprintf(name, sizeof name, "f/%u", e->idx);
    run_path(sp, sizeof sp, run, name);
    for (int attempt = 0; attempt < 3; attempt++) {
        int f = sd_open(sp, LS_RD);
        if (f < 0) { continue; }
        lfs_mkdirs(e->path, false);
        lfs_file_t lf;
        if (lfs_file_open(fs_lfs(), &lf, e->path, LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC) < 0) {
            sd_close(f);
            return -2;
        }
        uint32_t crc = 0, tot = 0;
        int n, bad = 0;
        while ((n = sd_read(f, s_buf, BLK)) > 0) {
            crc = crc32_update(crc, s_buf, (size_t)n);
            tot += (uint32_t)n;
            if (lfs_file_write(fs_lfs(), &lf, s_buf, (lfs_size_t)n) != n) { bad = -2; break; }
            watchdog_update();
        }
        sd_close(f);
        if (lfs_file_close(fs_lfs(), &lf) < 0 && !bad) { bad = -2; }
        if (bad) { return bad; }
        if (n < 0 || crc != e->crc || tot != e->size) { continue; }      /* the card misread: again */
        uint32_t c2, s2;
        if (flash_crc(e->path, &c2, &s2) == 0 && c2 == e->crc && s2 == e->size) { return 0; }
    }
    return -1;
}

/* The cfg/ files held in RAM, back onto flash (the last resort). */
static int restore_cfg_from_ram(void) {
    if (!s_cfg || !s_cfg_ok) { return -1; }
    uint32_t p = 0;
    int bad = 0;
    lfs_mkdir(fs_lfs(), "cfg");
    while (p < s_cfg_len) {
        uint16_t pl = (uint16_t)(s_cfg[p] | (s_cfg[p + 1] << 8));
        char path[FS_PATH_MAX + 1];
        memcpy(path, s_cfg + p + 2, pl);
        path[pl] = 0;
        uint32_t size;
        memcpy(&size, s_cfg + p + 2 + pl, 4);
        const uint8_t *data = s_cfg + p + 2 + pl + 4;
        lfs_mkdirs(path, false);
        if (lfs_put(path, data, size) < 0) { bad = -1; }
        uint32_t c2, s2;
        if (flash_crc(path, &c2, &s2) < 0 || s2 != size || c2 != crc32_update(0, data, size)) { bad = -1; }
        p += 2u + pl + 4u + size;
        watchdog_update();
    }
    printf("migrate: cfg/ written back from RAM%s\n", bad ? " WITH ERRORS" : "");
    return bad;
}

/* One pass over the manifest: cfg/ entries (cfg_pass) or the rest. */
static int restore_pass(unsigned run, bool cfg_pass, unsigned *done, char *err, size_t errcap) {
    man_rd_t m;
    if (man_open(&m, run) < 0) { snprintf(err, errcap, "manifest unreadable"); return -1; }
    char line[FS_PATH_MAX + 48];
    int r, rc = 0;
    while ((r = man_next(&m, line, sizeof line)) > 0 && !m.ended) {
        entry_t e;
        if (!entry_parse(line, &e)) { snprintf(err, errcap, "manifest damaged"); rc = -1; break; }
        if (is_cfg(e.path) != cfg_pass) { continue; }
        if (e.kind == 'D') {
            lfs_mkdirs(e.path, true);
            struct lfs_info info;
            if (lfs_stat(fs_lfs(), e.path, &info) < 0 || info.type != LFS_TYPE_DIR) {
                snprintf(err, errcap, "mkdir %s failed", e.path);
                rc = -2;
                break;
            }
        } else {
            int k = restore_file(run, &e);
            if (k < 0) {
                snprintf(err, errcap, k == -1 ? "SD copy of %s unreadable" : "flash refused %s", e.path);
                rc = k;
                break;
            }
        }
        (*done)++;
        if (*done % 25 == 0) { printf("migrate: %u entries restored\n", *done); }
    }
    if (r < 0 && rc == 0) { snprintf(err, errcap, "manifest unreadable"); rc = -1; }
    sd_close(m.f);
    return rc;
}

/* ---- the phases ---------------------------------------------------------- */

static mig_state_t s_st;

static bool copy_phase(copy_t *c, unsigned run) {
    unsigned files = 0;
    unsigned long bytes = 0;
    uint32_t cfg_need = 0;
    if (tally(&c->w, &files, &bytes, &cfg_need) < 0) {
        set_pending("cannot walk the old filesystem");
        return false;
    }
    lfs_ssize_t used = lfs_fs_size(fs_lfs());
    if (used < 0 || (uint32_t)used + 8u > NEW_BLOCKS) {
        set_pending("files need %d KB, the new filesystem has %u", (int)(used * 4), NEW_BLOCKS * 4u);
        return false;
    }
    sdj_t q = { .op = SDJ_STATFS };
    sdj_t out;
    int r = job(&q, NULL, NULL, &out);
    /* Room for every file rounded up to a big cluster, and then some. */
    uint64_t need = bytes + (uint64_t)files * 128u * 1024u + 1024u * 1024u;
    if (r < 0 || out.free_bytes < need) {
        set_pending("SD card full (needs %lu KB free)", (unsigned long)(need / 1024u));
        return false;
    }
    printf("migrate: copying %u files, %lu bytes (%ld blocks in use) to sd:/" MIG_DIR "/%u\n",
           files, bytes, (long)used, run);

    memset(&s_st, 0, sizeof s_st);
    s_st.phase = PH_COPYING;
    s_st.run = run;
    s_st.old_blocks = fs_mounted_blocks();
    s_st.new_blocks = NEW_BLOCKS;
    snprintf(s_st.board, sizeof s_st.board, "%s", s_board);
    if ((r = state_write(&s_st)) < 0) {
        set_pending("SD card not writable (%d)", r);
        return false;
    }
    memset(c, 0, sizeof *c);
    c->run = run;
    char mp[64];
    run_path(mp, sizeof mp, run, "manifest");
    c->man = sd_open(mp, LS_WR);
    if (c->man < 0) {
        set_pending("SD card not writable (%d)", c->man);
        return false;
    }
    /* cfg/ also in RAM (rule 3): a few hundred bytes a file. */
    s_cfg = cfg_need <= CFG_RAM_MAX ? try_alloc(cfg_need ? cfg_need : 1) : NULL;
    s_cfg_cap = cfg_need;
    s_cfg_ok = s_cfg != NULL;
    s_cfg_len = 0;
    if (!s_cfg_ok) { printf("migrate: cfg/ (%lu bytes) does not fit in RAM; the card's copy only\n", (unsigned long)cfg_need); }
    r = walk(c);
    if (r == 0) {
        /* The end line: the entry count and the CRC of every line before it. */
        char e[48];
        int n = snprintf(e, sizeof e, "E %u %08lx\n", c->entries, (unsigned long)c->mcrc);
        if (c->mlen + (uint32_t)n > sizeof c->mbuf) { r = man_flush(c); }
        if (r == 0) {
            memcpy(c->mbuf + c->mlen, e, (size_t)n);
            c->mlen += (uint32_t)n;
            r = man_flush(c);
        }
        if (r < 0) { snprintf(c->err, sizeof c->err, "SD manifest write failed"); }
    }
    int cr = sd_close(c->man);
    if (r == 0 && cr < 0) { r = cr; snprintf(c->err, sizeof c->err, "SD manifest close failed"); }
    if (r < 0) {
        set_pending("SD copy failed: %s (%d); nothing formatted", c->err, r);
        return false;
    }
    char err[64] = "";
    unsigned n = 0;
    if (man_check(run, false, &n, err, sizeof err) < 0 || n != c->entries) {
        set_pending("SD copy failed: %s; nothing formatted", err[0] ? err : "manifest");
        return false;
    }
    printf("migrate: copy verified: %u files, %u folders, %lu bytes; cfg/ %s RAM\n", c->files, c->dirs,
           c->bytes, s_cfg_ok ? "also in" : "NOT in");
    s_st.entries = c->entries;
    s_st.files = c->files;
    s_st.bytes = c->bytes;
    return true;
}

/* Format the new geometry and restore from the card's copy `run`. The state
 * file already says "formatting" (rule 2) when this returns past its first
 * check. `fresh`: the copy was made and verified this boot. */
static bool restore_phase(bool fresh, uint32_t t0) {
    char err[64] = "";
    if (s_st.attempts >= MAX_ATTEMPTS) {
        set_pending("restore failed %u times; restore by hand from sd:/" MIG_DIR, s_st.attempts);
        return false;
    }
    /* A copy from an earlier boot is checked file by file before anything
     * on flash is touched; this boot's was checked as it was written. */
    if (!fresh && fs_legacy()) {
        unsigned n;
        if (man_check(s_st.run, true, &n, err, sizeof err) < 0) {
            set_pending("%s; nothing formatted", err);
            return false;
        }
    }
    s_st.phase = PH_FORMATTING;
    s_st.attempts++;
    int r = state_write(&s_st);
    if (r < 0 && fs_legacy()) {
        set_pending("SD card not writable (%d); nothing formatted", r);
        return false;
    }
    printf("migrate: formatting LittleFS as %u KB (run %u, attempt %u)\n", NEW_BLOCKS * 4u, s_st.run,
           s_st.attempts);
    bool ok = false;
    unsigned done = 0;
    for (int tries = 0; tries < 2 && !ok; tries++) {
        watchdog_update();
        r = fs_reformat(NEW_BLOCKS);
        if (r < 0) { snprintf(err, sizeof err, "format failed (%d)", r); continue; }
        char ip[48];
        int n = snprintf(ip, sizeof ip, "{\"run\":%u}", s_st.run);
        if (lfs_put(INPROGRESS, ip, (size_t)n) < 0) { snprintf(err, sizeof err, "flash write failed"); continue; }
        done = 0;
        err[0] = 0;
        if (restore_pass(s_st.run, true, &done, err, sizeof err) == 0 &&
            restore_pass(s_st.run, false, &done, err, sizeof err) == 0) {
            ok = true;
        } else {
            printf("migrate: restore attempt %d: %s\n", tries + 1, err);
        }
    }
    if (!ok) {
        if (fs_lfs() && restore_cfg_from_ram() == 0) {
            set_pending("restore incomplete: %s; cfg/ written from RAM, retried at the next boot", err);
        } else {
            set_pending("restore incomplete: %s; retried at the next boot", err);
        }
        return false;
    }
    if (s_st.entries && done != s_st.entries) {
        set_pending("restore incomplete: %u of %u entries restored", done, s_st.entries);
        return false;
    }
    /* Done, in an order a cut can only leave resumable: card first, then
     * the marker, then the in-progress note goes. */
    s_st.phase = PH_DONE;
    state_write(&s_st);
    char mk[200];
    int n = snprintf(mk, sizeof mk,
                     "{\"from_kb\":%u,\"to_kb\":%u,\"entries\":%u,\"files\":%u,\"bytes\":%lu,"
                     "\"run\":%u,\"sd\":\"" MIG_DIR "/%u\",\"ms\":%lu}",
                     s_st.old_blocks * 4u, NEW_BLOCKS * 4u, s_st.entries, s_st.files, s_st.bytes, s_st.run,
                     s_st.run, (unsigned long)(now_ms() - t0));
    lfs_put(MARKER, mk, (size_t)n);
    lfs_remove(fs_lfs(), INPROGRESS);
    s_kind = MIG_MIGRATED;
    snprintf(s_msg, sizeof s_msg, "migrated this boot: %u entries, %lu bytes, %lu ms", done, s_st.bytes,
             (unsigned long)(now_ms() - t0));
    fs_set_note("");
    printf("migrate: DONE -- %s; LittleFS %u KB, PEX partition free at 0x%lx (%u KB)\n", s_msg,
           NEW_BLOCKS * 4u, (unsigned long)FS_PEX_OFF, (unsigned)(FS_PEX_SIZE / 1024u));
    return true;
}

#define CARD_WAIT_MS 5000u

static bool wait_for_card(void) {
    uint32_t t0 = now_ms();
    while (now_ms() - t0 < CARD_WAIT_MS) {
        pump();
        const struct sd_pub *p = sd_pub();
        if (p->media == LS_MEDIA_OK) { return true; }
        /* No card in the socket: card detect says so within a few passes. */
        if (!p->present && now_ms() - t0 > 1000u) { return false; }
        sleep_us(500);
    }
    return false;
}

static void migrate_sd(bool legacy, bool inprog) {
    uint32_t t0 = now_ms();
    sd_init();
    if (!wait_for_card()) {
        if (legacy) { set_pending("no SD card"); }
        else if (inprog) { set_pending("restore incomplete: no SD card"); }
        return;
    }
    mig_state_t st;
    bool have = state_read(&st);
    bool ours = have && strcmp(st.board, s_board) == 0;
    if (have && !ours) { printf("migrate: the card's state is another board's (%s); ignored\n", st.board); }
    s_ring = sd_xfer_borrow();
    s_buf = s_ring >= 0 ? sd_xfer_buf(s_ring) : try_alloc(BLK);
    if (!s_buf) { set_pending("out of memory"); return; }
    if (!legacy) {
        /* The new geometry, but a migration was cut short. */
        if (ours && st.phase == PH_FORMATTING) {
            printf("migrate: resuming the restore of run %u\n", st.run);
            s_st = st;
            restore_phase(false, t0);
        } else if (ours && st.phase == PH_DONE && inprog) {
            s_st = st;
            s_st.attempts = 0;
            restore_phase(false, t0);       /* verified restore again: cheap, and certain */
        } else if (inprog) {
            set_pending("restore incomplete: no copy for this board on the card");
        }
    } else if (ours && st.phase == PH_FORMATTING && st.old_blocks == fs_mounted_blocks()) {
        /* A format had begun: rule 2, the old filesystem is not trusted. */
        printf("migrate: run %u had begun formatting; restoring from the card\n", st.run);
        s_st = st;
        restore_phase(false, t0);
    } else {
        unsigned run = !have ? 1 : (ours && st.phase == PH_COPYING) ? st.run : st.run + 1;
        copy_t *c = try_alloc(sizeof *c);
        if (!c) {
            set_pending("out of memory");
        } else if (copy_phase(c, run)) {
            free(c);
            c = NULL;
            restore_phase(true, t0);
        }
        free(c);
    }
    free(s_cfg);
    s_cfg = NULL;
    if (s_ring >= 0) { sd_xfer_return(s_ring); } else { free(s_buf); }
    s_ring = -1;
    s_buf = NULL;
}

#endif /* CONFIG_SD */

/* ===================================================================== */

void fs_migrate_boot(void) {
    if (!fs_lfs()) { return; }
    pico_get_unique_board_id_string(s_board, sizeof s_board);
    bool legacy = fs_legacy();
    bool marker = lfs_exists(MARKER);
    bool inprog = lfs_exists(INPROGRESS);
    if (!legacy) {
        if (marker) {
            s_kind = MIG_DONE;
            if (inprog) { lfs_remove(fs_lfs(), INPROGRESS); }
            return;
        }
        /* An empty filesystem may be a migration cut right after its format,
         * before the in-progress note was written. */
        if (!inprog && !root_empty()) { s_kind = MIG_NONE; return; }
    }
#if !CONFIG_SD
    if (legacy) { set_pending("no SD card support in this build"); }
    else if (inprog) { set_pending("restore incomplete; no SD card support in this build"); }
#else
#if CONFIG_OTA
    if (legacy) {
        /* Not on a try-before-you-buy boot: until it is bought, any reboot
         * goes back to the old firmware, which would format a filesystem in
         * the new geometry (it formats on any mount error). */
        ota_slot_init();
        if (ota_buy_pending()) {
            s_wait_buy = true;
            set_pending("waiting for the OTA confirm, then it reboots and migrates");
            return;
        }
    }
#endif
    printf("migrate: %s\n", legacy ? "the filesystem is in the pre-PEX layout: migrating through the SD card"
                                   : "a migration was cut short: checking the SD card");
    migrate_sd(legacy, inprog);
#endif
}

void fs_migrate_tick(void) {
#if CONFIG_OTA && CONFIG_SD
    if (s_wait_buy && ota_bought()) {
        s_wait_buy = false;
        printf("migrate: the image is bought; rebooting in 3 s to migrate the filesystem\n");
        api_reboot_in(3000);
    }
#endif
}

int fs_migrate_json(char *buf, size_t cap) {
    const char *m = s_kind == MIG_NONE ? "none"
                  : s_kind == MIG_DONE ? "done"
                  : s_msg;
    return snprintf(buf, cap,
                    "{\"kb\":%lu,\"layout_kb\":%u,\"migration\":\"%s\","
                    "\"pex\":{\"offset\":%lu,\"size\":%u,\"state\":\"%s\"}}",
                    (unsigned long)(fs_mounted_size() / 1024u), (unsigned)(FS_SIZE / 1024u), m,
                    (unsigned long)FS_PEX_OFF, (unsigned)FS_PEX_SIZE,
                    fs_legacy() ? "inside the old LittleFS until it migrates" : "reserved");
}
