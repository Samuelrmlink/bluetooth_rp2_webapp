/* Relocate LittleFS, the Wi-Fi blob and the PEX region to the map in
 * fs_layout_map.h, then copy this image into slot A and flash-update boot
 * it. The new partition table is linked in and not written here: fs.layout_commit
 * does that, only after a later boot has brought Wi-Fi and WireGuard back.
 *
 * The old volume starts at 2 MiB and runs through the new blob and the new
 * filesystem. Erasing it whole would destroy the copy. The only write to
 * 0x200000 is one sector, and only after the SD copy has been checked and
 * the state file already says "formatting". */
#include "fs/fs_layout.h"
#include "fs/fs_layout_map.h"

#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "api/api.h"
#include "fs/fs.h"
#include "fs/fs_internal.h"
#include "hardware/flash.h"
#include "hardware/watchdog.h"
#include "littlefs/lfs.h"
#include "ota/crc32.h"
#include "ota/ota_slot.h"
#include "pico/flash.h"
#include "pico/stdlib.h"
#include "pico/unique_id.h"
#include "sd/sd_api.h"
#include "sd/sd_core.h"
#include "sd/sd_jobs.h"
#include "sd/sd_store.h"

#define BLK          4096u
#define DEPTH        8
#define CFG_RAM_MAX  (8u * 1024u)
#define CARD_WAIT_MS 5000u
#define JOB_MS       20000u
#define ROOT_DIR     ".layout-migrate"
#define STATE_PATH   ROOT_DIR "/state"
#define STATE_TMP    ROOT_DIR "/state.tmp"
#define ARM_PATH     ROOT_DIR "/ARM"
#define BUSY_NAME    ".layout-busy"

extern const uint8_t fs_layout_pt[];
extern const uint32_t fs_layout_pt_len;

static lay_phase_t s_phase;
static bool s_suppress;
static bool s_on_new;
static char s_note[100];
static char s_board[17];
static uint8_t *s_buf;
static int s_ring = -1;
static uint8_t *s_cfg;
static uint32_t s_cfg_len, s_cfg_cap;
static bool s_cfg_ok;

static uint32_t now_ms(void) { return to_ms_since_boot(get_absolute_time()); }

static void note(const char *s) {
    snprintf(s_note, sizeof s_note, "%s", s ? s : "");
    printf("layout: %s\n", s_note);
    fs_set_note(s_note);
}

static void pump(void) {
    watchdog_update();
    sd_poll();
}

static void *try_alloc(size_t n) {
    extern char __StackLimit;
    char *brk = sbrk(0);
    size_t top = (brk != (char *)-1 && brk < &__StackLimit) ? (size_t)(&__StackLimit - brk) : 0;
    struct mallinfo mi = mallinfo();
    if (top < n + 2048u && (size_t)mi.fordblks < n + 2048u) { return NULL; }
    return malloc(n);
}

/* ---- flash ------------------------------------------------------------ */

typedef struct { uint32_t off; const uint8_t *data; uint32_t len; int prog; } fjob_t;

static void __not_in_flash_func(fjob_run)(void *p) {
    fjob_t *j = p;
    if (j->prog) { flash_range_program(j->off, j->data, j->len); }
    else { flash_range_erase(j->off, j->len); }
}

static int flash_op(int prog, uint32_t off, const uint8_t *data, uint32_t len) {
    fjob_t j = { .off = off, .data = data, .len = len, .prog = prog };
    int rc = flash_safe_execute(fjob_run, &j, 2000);
    return rc == PICO_OK ? 0 : -1;
}

static int flash_erase_span(uint32_t off, uint32_t len) {
    if ((off | len) % 4096u) { return -1; }
    if (off < LAY_SLOT_A) { return -1; }          /* sector 0 is the table commit's job */
    if (off + len > LAY_NVM) { return -1; }       /* never the bond store */
    for (uint32_t o = 0; o < len; o += 4096u) {
        watchdog_update();
        if (flash_op(0, off + o, NULL, 4096u) != 0) { return -1; }
    }
    return 0;
}

static int flash_crc(uint32_t off, uint32_t len, uint32_t *crc) {
    uint32_t c = 0;
    uint8_t page[256];
    for (uint32_t o = 0; o < len; o += sizeof page) {
        uint32_t n = len - o > sizeof page ? (uint32_t)sizeof page : len - o;
        memcpy(page, (const uint8_t *)(FS_READ_BASE + off + o), n);
        c = crc32_update(c, page, n);
        if ((o & 0xFFFFu) == 0) { watchdog_update(); }
    }
    *crc = c;
    return 0;
}

/* dst and src must not overlap. Pages are staged in RAM: flash_range_program
 * reads its source while XIP is off. */
static int flash_copy(uint32_t dst, uint32_t src, uint32_t len) {
    if ((dst | src | len) % 4096u || len == 0) { return -1; }
    if (!(dst + len <= src || src + len <= dst)) { return -1; }
    if (dst + len > LAY_NVM || src + len > 0x400000u) { return -1; }
    uint8_t page[256];
    for (uint32_t o = 0; o < len; o += 4096u) {
        watchdog_update();
        if (flash_op(0, dst + o, NULL, 4096u) != 0) { return -1; }
        for (uint32_t p = 0; p < 4096u; p += sizeof page) {
            memcpy(page, (const uint8_t *)(FS_READ_BASE + src + o + p), sizeof page);
            if (flash_op(1, dst + o + p, page, sizeof page) != 0) { return -1; }
        }
    }
    return 0;
}

/* ---- card jobs -------------------------------------------------------- */

static int job(sdj_t *q, const void *wr, void *rd) {
    uint32_t tok, t0 = now_ms();
    while (!sdj_post(q, &tok)) {
        if (now_ms() - t0 > JOB_MS) { return LS_EBUSY; }
        pump();
    }
    if (q->op == SDJ_WRITE && wr) { memcpy(sdj_buf(), wr, q->len); }
    sdj_t r;
    for (;;) {
        if (sdj_poll(tok, &r) == SDJ_DONE) { break; }
        if (now_ms() - t0 > JOB_MS) { sdj_abandon(tok); return LS_EIO; }
        pump();
    }
    if (q->op == SDJ_READ && rd && r.rc > 0) { memcpy(rd, sdj_buf(), (size_t)r.rc); }
    q->free_bytes = r.free_bytes;
    q->rc = r.rc;
    sdj_finish(tok);
    return r.rc;
}

static int sd_open(const char *path, int mode) {
    sdj_t q = { .op = SDJ_OPEN, .mode = mode };
    snprintf(q.path, sizeof q.path, "%s", path);
    return job(&q, NULL, NULL);
}

static int sd_read(int f, void *buf, uint32_t n) {
    sdj_t q = { .op = SDJ_READ, .file = f, .len = n };
    return job(&q, NULL, buf);
}

static int sd_write(int f, const void *buf, uint32_t n) {
    sdj_t q = { .op = SDJ_WRITE, .file = f, .len = n };
    return job(&q, buf, NULL);
}

static int sd_close(int f) {
    sdj_t q = { .op = SDJ_CLOSE, .file = f };
    return job(&q, NULL, NULL);
}

static int sd_path(int op, const char *a, const char *b) {
    sdj_t q = { .op = op };
    snprintf(q.path, sizeof q.path, "%s", a);
    if (b) { snprintf(q.path2, sizeof q.path2, "%s", b); }
    return job(&q, NULL, NULL);
}

static int sd_put(const char *path, const void *data, uint32_t len) {
    int f = sd_open(path, LS_WR);
    if (f < 0) { return f; }
    int r = len ? sd_write(f, data, len) : 0;
    int c = sd_close(f);
    if (r < 0) { return r; }
    if ((uint32_t)r != len) { return LS_EIO; }
    return c < 0 ? c : 0;
}

static bool sd_exists(const char *path) {
    sdj_t q = { .op = SDJ_STAT };
    snprintf(q.path, sizeof q.path, "%s", path);
    return job(&q, NULL, NULL) >= 0;
}

static bool wait_for_card(void) {
    uint32_t t0 = now_ms();
    while (now_ms() - t0 < CARD_WAIT_MS) {
        pump();
        const struct sd_pub *p = sd_pub();
        if (p->media == LS_MEDIA_OK) { return true; }
        if (!p->present && now_ms() - t0 > 1000u) { break; }
        sleep_us(500);
    }
    printf("layout: card not mounted after %lu ms\n", (unsigned long)(now_ms() - t0));
    return false;
}

/* ---- state ------------------------------------------------------------ */

typedef struct {
    lay_phase_t phase;
    unsigned run, attempts, entries, files;
    unsigned long bytes;
    uint32_t pex_crc, blob_crc;
    char board[17];
} lay_st_t;

static int state_write(const lay_st_t *s) {
    char b[320];
    int n = snprintf(b, sizeof b,
                     "btgw-layout 1\nphase=%s\nrun=%u\nattempts=%u\nentries=%u\nfiles=%u\n"
                     "bytes=%lu\npex=%08lx\nblob=%08lx\nboard=%s\n",
                     fs_layout_phase_name(s->phase), s->run, s->attempts, s->entries, s->files,
                     s->bytes, (unsigned long)s->pex_crc, (unsigned long)s->blob_crc, s->board);
    if (n < 0 || (size_t)n >= sizeof b) { return -1; }
    int r = sd_put(STATE_TMP, b, (uint32_t)n);
    if (r < 0) { return r; }
    sd_path(SDJ_REMOVE, STATE_PATH, NULL);
    r = sd_path(SDJ_RENAME, STATE_TMP, STATE_PATH);
    if (r == 0) { s_phase = s->phase; }
    return r;
}

static bool state_parse(const char *b, lay_st_t *s) {
    memset(s, 0, sizeof *s);
    if (strncmp(b, "btgw-layout 1\n", 14) != 0) { return false; }
    const char *p = b;
    while ((p = strchr(p, '\n')) != NULL) {
        p++;
        char k[16], v[24];
        if (sscanf(p, "%15[a-z_]=%23s", k, v) != 2) { continue; }
        if (!strcmp(k, "phase")) { s->phase = fs_layout_phase_parse(v); }
        else if (!strcmp(k, "run")) { s->run = (unsigned)strtoul(v, NULL, 10); }
        else if (!strcmp(k, "attempts")) { s->attempts = (unsigned)strtoul(v, NULL, 10); }
        else if (!strcmp(k, "entries")) { s->entries = (unsigned)strtoul(v, NULL, 10); }
        else if (!strcmp(k, "files")) { s->files = (unsigned)strtoul(v, NULL, 10); }
        else if (!strcmp(k, "bytes")) { s->bytes = strtoul(v, NULL, 10); }
        else if (!strcmp(k, "pex")) { s->pex_crc = (uint32_t)strtoul(v, NULL, 16); }
        else if (!strcmp(k, "blob")) { s->blob_crc = (uint32_t)strtoul(v, NULL, 16); }
        else if (!strcmp(k, "board")) { snprintf(s->board, sizeof s->board, "%s", v); }
    }
    return s->run > 0 && s->board[0];
}

static bool state_read(lay_st_t *s) {
    static const char *const names[] = { STATE_PATH, STATE_TMP };
    char b[321];
    for (int i = 0; i < 2; i++) {
        int f = sd_open(names[i], LS_RD);
        if (f < 0) { continue; }
        int n = sd_read(f, b, sizeof b - 1);
        sd_close(f);
        if (n <= 0) { continue; }
        b[n] = 0;
        if (state_parse(b, s) && strcmp(s->board, s_board) == 0) { return true; }
    }
    memset(s, 0, sizeof *s);
    return false;
}

/* ---- copy / restore --------------------------------------------------- */

typedef struct {
    lfs_dir_t d[DEPTH];
    size_t len[DEPTH];
    int depth;
    char path[FS_PATH_MAX + 1];
    struct lfs_info info;
} walker_t;

typedef struct {
    walker_t w;
    unsigned run, entries, files, dirs;
    unsigned long bytes;
    int man;
    char mbuf[256];
    uint32_t mlen, mcrc;
    char err[80];
} copy_t;

static void run_path(char *dst, size_t cap, unsigned run, const char *rest) {
    snprintf(dst, cap, ROOT_DIR "/%u/%s", run, rest);
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
        size_t n = (size_t)snprintf(w->path + len, sizeof w->path - len, "%s%s",
                                    len ? "/" : "", w->info.name);
        if (len + n > FS_PATH_MAX) { return LFS_ERR_NAMETOOLONG; }
        if (w->info.type == LFS_TYPE_DIR) {
            if (w->depth >= DEPTH) { return LFS_ERR_NOMEM; }
            int e = lfs_dir_open(fs_lfs(), &w->d[w->depth], w->path);
            if (e < 0) { return e; }
            w->len[w->depth++] = len + n;
        }
        return 1;
    }
    return 0;
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

static int cfg_rank(const char *path) {
    if (!strcmp(path, "cfg/wifi.json") || !strcmp(path, "cfg/wireguard.json")) { return 2; }
    if (!strncmp(path, "cfg/", 4)) { return 1; }
    return 0;
}

static void cfg_keep(const char *path, uint32_t size, const uint8_t *data, uint32_t off, uint32_t n) {
    if (!s_cfg || !s_cfg_ok || !cfg_rank(path)) { return; }
    size_t pl = strlen(path);
    if (off == 0) {
        if (s_cfg_len + 2 + pl + 4 + size > s_cfg_cap) { s_cfg_ok = false; return; }
        s_cfg[s_cfg_len] = (uint8_t)pl;
        s_cfg[s_cfg_len + 1] = (uint8_t)(pl >> 8);
        memcpy(s_cfg + s_cfg_len + 2, path, pl);
        memcpy(s_cfg + s_cfg_len + 2 + pl, &size, 4);
        s_cfg_len += (uint32_t)(2 + pl + 4);
    }
    if (s_cfg_len + n > s_cfg_cap) { s_cfg_ok = false; return; }
    memcpy(s_cfg + s_cfg_len, data, n);
    s_cfg_len += n;
}

static bool cfg_has(const char *path) {
    uint32_t p = 0;
    while (s_cfg && p + 6 < s_cfg_len) {
        uint16_t pl = (uint16_t)(s_cfg[p] | (s_cfg[p + 1] << 8));
        if (p + 2u + pl + 4u > s_cfg_len) { return false; }
        uint32_t size;
        memcpy(&size, s_cfg + p + 2 + pl, 4);
        if (pl == strlen(path) && memcmp(s_cfg + p + 2, path, pl) == 0 && size > 0) { return true; }
        if (p + 2u + pl + 4u + size > s_cfg_len) { return false; }
        p += 2u + pl + 4u + size;
    }
    return false;
}

static int copy_file(copy_t *c, const char *path, uint32_t size) {
    unsigned idx = c->files + 1;
    char sp[80];
    snprintf(sp, sizeof sp, ROOT_DIR "/%u/f/%u", c->run, idx);
    lfs_file_t lf;
    int r = lfs_file_open(fs_lfs(), &lf, path, LFS_O_RDONLY);
    if (r < 0) { snprintf(c->err, sizeof c->err, "read failed"); return r; }
    int f = sd_open(sp, LS_WR);
    if (f < 0) { lfs_file_close(fs_lfs(), &lf); snprintf(c->err, sizeof c->err, "SD create failed"); return f; }
    uint32_t crc = 0, total = 0;
    int rc = 0;
    for (;;) {
        watchdog_update();
        lfs_ssize_t n = lfs_file_read(fs_lfs(), &lf, s_buf, BLK);
        if (n < 0) { rc = (int)n; snprintf(c->err, sizeof c->err, "read failed"); break; }
        if (n == 0) { break; }
        crc = crc32_update(crc, s_buf, (size_t)n);
        cfg_keep(path, size, s_buf, total, (uint32_t)n);
        total += (uint32_t)n;
        int w = sd_write(f, s_buf, (uint32_t)n);
        if (w != n) { rc = w < 0 ? w : LS_ENOSPC; snprintf(c->err, sizeof c->err, "SD write failed"); break; }
    }
    lfs_file_close(fs_lfs(), &lf);
    int cl = sd_close(f);
    if (rc == 0 && (cl < 0 || total != size)) { rc = LS_EIO; snprintf(c->err, sizeof c->err, "size changed"); }
    if (rc < 0) { return rc; }
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
        snprintf(c->err, sizeof c->err, "SD readback mismatch");
        return rc < 0 ? rc : LS_EIO;
    }
    char line[FS_PATH_MAX + 40];
    int ln = snprintf(line, sizeof line, "F %u %lu %08lx %s\n", idx, (unsigned long)size,
                      (unsigned long)crc, path);
    if (ln < 0 || (size_t)ln >= sizeof line) { snprintf(c->err, sizeof c->err, "path too long"); return -1; }
    if (man_line(c, line) < 0) { snprintf(c->err, sizeof c->err, "manifest write failed"); return -1; }
    c->files++;
    c->entries++;
    c->bytes += size;
    if (c->files % 25 == 0) { printf("layout: %u files, %lu bytes on the card\n", c->files, c->bytes); }
    return 0;
}

static int copy_tree(copy_t *c) {
    int r = walker_start(&c->w);
    if (r < 0) { snprintf(c->err, sizeof c->err, "open /"); return r; }
    while ((r = walker_next(&c->w)) > 0) {
        if (c->w.info.type == LFS_TYPE_DIR) {
            char line[FS_PATH_MAX + 4];
            int ln = snprintf(line, sizeof line, "D %s\n", c->w.path);
            if (ln < 0 || (size_t)ln >= sizeof line || man_line(c, line) < 0) {
                snprintf(c->err, sizeof c->err, "manifest write failed");
                r = -1;
                break;
            }
            c->dirs++;
            c->entries++;
        } else {
            r = copy_file(c, c->w.path, c->w.info.size);
            if (r < 0) { break; }
        }
    }
    walker_stop(&c->w);
    return r < 0 ? r : 0;
}

typedef struct {
    int f;
    char buf[256];
    int len, pos;
    bool eof, ended;
    uint32_t crc, e_crc;
    unsigned e_entries;
} man_rd_t;

static int man_open(man_rd_t *m, unsigned run) {
    memset(m, 0, sizeof *m);
    char p[64];
    run_path(p, sizeof p, run, "manifest");
    m->f = sd_open(p, LS_RD);
    return m->f;
}

static int man_next(man_rd_t *m, char *line, size_t cap) {
    size_t n = 0;
    for (;;) {
        if (m->pos == m->len) {
            if (m->eof) { return n ? LS_EIO : 0; }
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

typedef struct { char kind; unsigned idx; uint32_t size, crc; const char *path; } entry_t;

static bool entry_parse(char *line, entry_t *e) {
    memset(e, 0, sizeof *e);
    e->kind = line[0];
    if (e->kind == 'D' && line[1] == ' ') { e->path = line + 2; return e->path[0] != 0; }
    if (e->kind != 'F') { return false; }
    unsigned long sz, cr;
    int off = 0;
    if (sscanf(line, "F %u %lu %lx %n", &e->idx, &sz, &cr, &off) != 3 || off <= 0) { return false; }
    e->size = (uint32_t)sz;
    e->crc = (uint32_t)cr;
    e->path = line + off;
    return e->path[0] != 0;
}

static int man_check(unsigned run, char *err, size_t errcap) {
    man_rd_t m;
    if (man_open(&m, run) < 0) { snprintf(err, errcap, "no manifest"); return -1; }
    char line[FS_PATH_MAX + 40];
    unsigned n = 0;
    int r;
    while ((r = man_next(&m, line, sizeof line)) > 0) {
        if (m.ended) { break; }
        entry_t e;
        if (!entry_parse(line, &e)) { r = -1; break; }
        n++;
        if (e.kind != 'F') { continue; }
        char sp[80];
        snprintf(sp, sizeof sp, ROOT_DIR "/%u/f/%u", run, e.idx);
        int f = sd_open(sp, LS_RD);
        if (f < 0) { r = f; break; }
        uint32_t crc = 0, tot = 0;
        int k;
        while ((k = sd_read(f, s_buf, BLK)) > 0) {
            crc = crc32_update(crc, s_buf, (size_t)k);
            tot += (uint32_t)k;
            watchdog_update();
        }
        sd_close(f);
        if (k < 0 || crc != e.crc || tot != e.size) {
            snprintf(err, errcap, "SD readback mismatch");
            sd_close(m.f);
            return -1;
        }
    }
    sd_close(m.f);
    if (r < 0 || !m.ended || m.e_entries != n || m.e_crc != m.crc) {
        snprintf(err, errcap, "manifest damaged");
        return -1;
    }
    return 0;
}

static void lfs_mkdirs(const char *path) {
    char b[FS_PATH_MAX + 1];
    snprintf(b, sizeof b, "%s", path);
    for (size_t i = 0; b[i]; i++) {
        if (b[i] != '/') { continue; }
        b[i] = 0;
        if (b[0]) { lfs_mkdir(fs_lfs(), b); }
        b[i] = '/';
    }
}

static int lfs_put(const char *path, const void *data, uint32_t len) {
    lfs_mkdirs(path);
    lfs_file_t f;
    int r = lfs_file_open(fs_lfs(), &f, path, LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC);
    if (r < 0) { return r; }
    lfs_ssize_t w = len ? lfs_file_write(fs_lfs(), &f, data, len) : 0;
    int c = lfs_file_close(fs_lfs(), &f);
    if (w != (lfs_ssize_t)len) { return -1; }
    return c;
}

static int file_crc(const char *path, uint32_t *crc, uint32_t *size) {
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

static int restore_file(unsigned run, const entry_t *e) {
    char sp[80];
    snprintf(sp, sizeof sp, ROOT_DIR "/%u/f/%u", run, e->idx);
    int f = sd_open(sp, LS_RD);
    if (f < 0) { return -1; }
    lfs_mkdirs(e->path);
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
    if (bad || n < 0 || crc != e->crc || tot != e->size) { return bad ? bad : -1; }
    uint32_t c2, s2;
    if (file_crc(e->path, &c2, &s2) != 0 || c2 != e->crc || s2 != e->size) { return -2; }
    return 0;
}

static int restore_cfg_ram(void) {
    if (!s_cfg || !s_cfg_ok) { return -1; }
    uint32_t p = 0;
    int bad = 0;
    lfs_mkdir(fs_lfs(), "cfg");
    while (p + 6 < s_cfg_len) {
        uint16_t pl = (uint16_t)(s_cfg[p] | (s_cfg[p + 1] << 8));
        if ((uint32_t)pl > FS_PATH_MAX || p + 2u + pl + 4u > s_cfg_len) { return -1; }
        char path[FS_PATH_MAX + 1];
        memcpy(path, s_cfg + p + 2, pl);
        path[pl] = 0;
        uint32_t size;
        memcpy(&size, s_cfg + p + 2 + pl, 4);
        if (p + 2u + pl + 4u + size > s_cfg_len) { return -1; }
        const uint8_t *data = s_cfg + p + 2 + pl + 4;
        if (lfs_put(path, data, size) < 0) { bad = -1; }
        uint32_t c2, s2;
        if (file_crc(path, &c2, &s2) != 0 || s2 != size || c2 != crc32_update(0, data, size)) { bad = -1; }
        p += 2u + pl + 4u + size;
        watchdog_update();
    }
    return bad;
}

static int restore_pass(unsigned run, bool cfg_pass, unsigned *done) {
    man_rd_t m;
    if (man_open(&m, run) < 0) { return -1; }
    char line[FS_PATH_MAX + 40];
    int r, rc = 0;
    while ((r = man_next(&m, line, sizeof line)) > 0 && !m.ended) {
        entry_t e;
        if (!entry_parse(line, &e)) { rc = -1; break; }
        int is_cfg = strncmp(e.path, "cfg/", 4) == 0 || strcmp(e.path, "cfg") == 0;
        if (is_cfg != cfg_pass) { continue; }
        if (e.kind == 'D') {
            lfs_mkdirs(e.path);
            lfs_mkdir(fs_lfs(), e.path);
        } else if (restore_file(run, &e) != 0) {
            rc = -1;
            break;
        }
        (*done)++;
    }
    sd_close(m.f);
    if (r < 0) { rc = -1; }
    return rc;
}

static int cfg_from_sd(unsigned run) {
    free(s_cfg);
    s_cfg = try_alloc(CFG_RAM_MAX);
    s_cfg_cap = s_cfg ? CFG_RAM_MAX : 0;
    s_cfg_len = 0;
    s_cfg_ok = s_cfg != NULL;
    if (!s_cfg) { return -1; }
    man_rd_t m;
    if (man_open(&m, run) < 0) { return -1; }
    char line[FS_PATH_MAX + 40];
    int r;
    while ((r = man_next(&m, line, sizeof line)) > 0 && !m.ended) {
        entry_t e;
        if (!entry_parse(line, &e) || e.kind != 'F' || cfg_rank(e.path) == 0) { continue; }
        char sp[80];
        snprintf(sp, sizeof sp, ROOT_DIR "/%u/f/%u", run, e.idx);
        int f = sd_open(sp, LS_RD);
        if (f < 0) { r = -1; break; }
        uint32_t got = 0;
        int n;
        while ((n = sd_read(f, s_buf, BLK)) > 0) {
            cfg_keep(e.path, e.size, s_buf, got, (uint32_t)n);
            got += (uint32_t)n;
            watchdog_update();
        }
        sd_close(f);
        if (n < 0 || got != e.size) { r = -1; break; }
    }
    sd_close(m.f);
    return r < 0 || !s_cfg_ok ? -1 : 0;
}

/* ---- regions, then slot A --------------------------------------------- */

static int regions_ready(uint32_t pex_crc, uint32_t blob_crc) {
    uint32_t c = 0;
    if (flash_crc(LAY_PEX_NEW, LAY_PEX_OLD_SIZE, &c) != 0 || c != pex_crc) {
        printf("layout: copying PEX %lu KB down\n", (unsigned long)(LAY_PEX_OLD_SIZE / 1024u));
        if (flash_copy(LAY_PEX_NEW, LAY_PEX_OLD, LAY_PEX_OLD_SIZE) != 0) { return -1; }
        if (flash_crc(LAY_PEX_NEW, LAY_PEX_OLD_SIZE, &c) != 0 || c != pex_crc) { return -1; }
    }
    if (flash_erase_span(LAY_PEX_OLD, LAY_PEX_OLD_SIZE) != 0) { return -1; }
    if (flash_crc(LAY_BLOB_NEW, LAY_BLOB_SIZE, &c) != 0 || c != blob_crc) {
        printf("layout: copying Wi-Fi blob to 0x%lx\n", (unsigned long)LAY_BLOB_NEW);
        if (flash_copy(LAY_BLOB_NEW, LAY_BLOB_OLD, LAY_BLOB_SIZE) != 0) { return -1; }
        if (flash_crc(LAY_BLOB_NEW, LAY_BLOB_SIZE, &c) != 0 || c != blob_crc) { return -1; }
    }
    /* The stale superblock only. The bytes after it are the new blob. */
    if (flash_op(0, LAY_OLD_SUPER, NULL, 4096u) != 0) { return -1; }
    return 0;
}

static int program_slot_a(void) {
    ota_slot_init();
    if (ota_slot_active() != 1) {
        printf("layout: not running from slot B (active %d); leaving slot A\n", ota_slot_active());
        return ota_slot_active() == 0 ? 0 : -1;
    }
    ota_part_t a, b;
    if (!ota_slot_get(0, &a) || !ota_slot_get(1, &b)) { return -1; }
    if (a.start != LAY_SLOT_A || a.size != LAY_OLD_SLOT_SIZE ||
        b.start != LAY_OLD_SLOT_B || b.size != LAY_OLD_SLOT_SIZE ||
        a.start + a.size != b.start) {
        printf("layout: slot table is not the 864 KiB map; not copying\n");
        return -1;
    }
    const uint8_t *src = ota_slot_bytes(1);
    if (!src) { return -1; }
    printf("layout: programming slot A from slot B (%lu KB)\n", (unsigned long)(a.size / 1024u));
    uint8_t page[256];
    for (uint32_t o = 0; o < a.size; o += 4096u) {
        watchdog_update();
        if (ota_slot_erase_sector(0, o) != 0) { return -1; }
        for (uint32_t p = 0; p < 4096u; p += sizeof page) {
            memcpy(page, src + o + p, sizeof page);
            if (ota_slot_program_page(0, o + p, page) != 0) { return -1; }
        }
        if ((o & 0xFFFFu) == 0) { printf("layout: slot A %lu / %lu KB\n",
                                         (unsigned long)(o / 1024u), (unsigned long)(a.size / 1024u)); }
    }
    uint32_t ca = 0, cb = 0;
    const uint8_t *dst = ota_slot_bytes(0);
    if (!dst) { return -1; }
    ca = crc32_update(0, dst, a.size);
    cb = crc32_update(0, src, b.size);
    watchdog_update();
    if (ca != cb) {
        printf("layout: slot A readback mismatch\n");
        return -1;
    }
    printf("layout: slot A matches slot B\n");
    return 1; /* caller flash-update reboots into A */
}

static int format_and_restore(lay_st_t *st) {
    fs_freeze_old(true);
    s_suppress = true;
    if (fs_is_mounted()) { fs_unmount(); }
    printf("layout: formatting %u KB at 0x%lx\n", LAY_FS_BLOCKS * 4u, (unsigned long)LAY_FS_NEW);
    fs_set_flash_off(LAY_FS_NEW, LAY_FS_BLOCKS);
    int r = fs_reformat(LAY_FS_BLOCKS);
    if (r != 0) { note("format of the new volume failed"); return -1; }
    s_on_new = true;
    char busy[32];
    int bn = snprintf(busy, sizeof busy, "{\"run\":%u}", st->run);
    if (lfs_put(BUSY_NAME, busy, (uint32_t)bn) < 0) { note("could not mark the new volume"); return -1; }
    unsigned done = 0;
    if (restore_pass(st->run, true, &done) != 0) {
        if (restore_cfg_ram() == 0) { note("cfg restored from RAM; file restore retries next boot"); }
        else { note("cfg restore failed; retries next boot"); }
        return -1;
    }
    if (restore_pass(st->run, false, &done) != 0) {
        note("file restore failed; retries next boot");
        return -1;
    }
    uint32_t c = 0, sz = 0;
    if (file_crc("cfg/wifi.json", &c, &sz) != 0 || sz == 0 ||
        file_crc("cfg/wireguard.json", &c, &sz) != 0 || sz == 0) {
        note("wifi or wireguard missing after restore");
        return -1;
    }
    char lj[160];
    int ln = snprintf(lj, sizeof lj,
                      "{\"run\":%u,\"fs_off\":%lu,\"fs_size\":%lu,\"pex_off\":%lu,\"pex_size\":%lu,"
                      "\"blob_off\":%lu}\n",
                      st->run, (unsigned long)LAY_FS_NEW, (unsigned long)LAY_FS_NEW_SIZE,
                      (unsigned long)LAY_PEX_NEW, (unsigned long)LAY_PEX_NEW_SIZE,
                      (unsigned long)LAY_BLOB_NEW);
    lfs_put("cfg/layout.json", lj, (uint32_t)ln);
    lfs_remove(fs_lfs(), BUSY_NAME);
    sd_path(SDJ_REMOVE, ARM_PATH, NULL);
    st->phase = LAY_PH_RESTORED;
    state_write(st);
    note("filesystem restored");
    return 0;
}

static void finish_on_a(lay_st_t *st) {
    int pr = program_slot_a();
    if (pr < 0) { note("slot A copy failed; the card copy is intact"); return; }
    if (pr == 0) {
        /* Already executing slot A. The table is still the old one. */
        st->phase = LAY_PH_SLOTA;
        state_write(st);
        s_phase = LAY_PH_SLOTA;
        note("slot A, old table, new filesystem");
        return;
    }
    st->phase = LAY_PH_SLOTA;
    if (state_write(st) != 0) { note("could not record slot A; not rebooting"); return; }
    printf("layout: flash-update reboot into slot A\n");
    ota_reboot(true);
}

static int begin_format(lay_st_t *st) {
    if (!s_cfg_ok || !cfg_has("cfg/wifi.json") || !cfg_has("cfg/wireguard.json")) {
        note("wifi and wireguard are not both in RAM; nothing formatted");
        return -1;
    }
    if (!fs_layout_pt_matches(fs_layout_pt, fs_layout_pt_len)) {
        note("embedded partition table does not match the map; nothing formatted");
        return -1;
    }
    uint32_t pex = 0, blob = 0;
    flash_crc(LAY_PEX_OLD, LAY_PEX_OLD_SIZE, &pex);
    flash_crc(LAY_BLOB_OLD, LAY_BLOB_SIZE, &blob);
    st->pex_crc = pex;
    st->blob_crc = blob;
    st->phase = LAY_PH_FORMATTING;
    st->attempts++;
    if (state_write(st) != 0) { note("could not record formatting; nothing formatted"); return -1; }
    s_phase = LAY_PH_FORMATTING;
    fs_freeze_old(true);
    s_suppress = true;
    if (regions_ready(st->pex_crc, st->blob_crc) != 0) {
        note("region copy failed; resume from the card");
        return -1;
    }
    if (format_and_restore(st) != 0) { return -1; }
    finish_on_a(st);
    return 0;
}

static int do_copy(lay_st_t *st) {
    lfs_ssize_t used = lfs_fs_size(fs_lfs());
    if (used < 0 || (uint32_t)used + LAY_SLACK_BLOCKS > LAY_FS_BLOCKS) {
        note("files do not fit in 1200 KiB; nothing moved");
        return -1;
    }
    sdj_t q = { .op = SDJ_STATFS };
    if (job(&q, NULL, NULL) < 0 || q.free_bytes < (uint64_t)4 * 1024u * 1024u) {
        note("SD card has no room for the copy; nothing moved");
        return -1;
    }
    copy_t *c = try_alloc(sizeof *c);
    if (!c) { note("out of memory"); return -1; }
    memset(c, 0, sizeof *c);
    c->run = st->run;
    char mp[64];
    run_path(mp, sizeof mp, st->run, "manifest");
    sd_path(SDJ_MKDIR, ROOT_DIR, NULL);
    char rd[32];
    snprintf(rd, sizeof rd, ROOT_DIR "/%u/f", st->run);
    sd_path(SDJ_MKDIR, rd, NULL);
    c->man = sd_open(mp, LS_WR);
    if (c->man < 0) { free(c); note("SD card not writable"); return -1; }
    /* walker_t holds eight directory handles. Core0's stack is 2 KB, and
     * this function is already under the copy, so the walk lives on the heap. */
    uint32_t cfg_need = 0;
    walker_t *w = try_alloc(sizeof *w);
    if (!w) { sd_close(c->man); free(c); note("out of memory"); return -1; }
    if (walker_start(w) == 0) {
        int wr;
        while ((wr = walker_next(w)) > 0) {
            if (w->info.type != LFS_TYPE_DIR && cfg_rank(w->path)) {
                cfg_need += 2u + (uint32_t)strlen(w->path) + 4u + w->info.size;
            }
        }
        walker_stop(w);
    }
    free(w);
    free(s_cfg);
    s_cfg_cap = cfg_need && cfg_need <= CFG_RAM_MAX ? cfg_need : (cfg_need ? CFG_RAM_MAX : 1);
    s_cfg = try_alloc(s_cfg_cap);
    s_cfg_len = 0;
    s_cfg_ok = s_cfg != NULL;
    printf("layout: copying to sd:/" ROOT_DIR "/%u (%ld blocks in use)\n", st->run, (long)used);
    int r = copy_tree(c);
    if (r == 0) {
        char e[48];
        int n = snprintf(e, sizeof e, "E %u %08lx\n", c->entries, (unsigned long)c->mcrc);
        if (c->mlen + (uint32_t)n > sizeof c->mbuf) { r = man_flush(c); }
        if (r == 0) {
            memcpy(c->mbuf + c->mlen, e, (size_t)n);
            c->mlen += (uint32_t)n;
            r = man_flush(c);
        }
    }
    int cl = sd_close(c->man);
    if (r == 0 && cl < 0) { r = cl; }
    st->entries = c->entries;
    st->files = c->files;
    st->bytes = c->bytes;
    if (r != 0) {
        printf("layout: copy failed: %s\n", c->err);
        free(c);
        note("SD copy failed; nothing formatted");
        return -1;
    }
    free(c);
    char err[64];
    if (man_check(st->run, err, sizeof err) != 0) {
        note("SD copy did not verify; nothing formatted");
        return -1;
    }
    st->phase = LAY_PH_VERIFIED;
    if (state_write(st) != 0) { note("could not record the copy; nothing formatted"); return -1; }
    printf("layout: copy verified, %u files, %lu bytes\n", st->files, st->bytes);
    return begin_format(st);
}

static void resume_past_ponr(lay_st_t *st) {
    fs_freeze_old(true);
    s_suppress = true;
    if (fs_is_mounted()) { fs_unmount(); }
    printf("layout: resuming phase %s run %u\n", fs_layout_phase_name(st->phase), st->run);
    if (st->phase == LAY_PH_SLOTA && ota_slot_active() == 0) {
        fs_set_flash_off(LAY_FS_NEW, LAY_FS_BLOCKS);
        if (fs_mount_at(LAY_FS_NEW, LAY_FS_BLOCKS) == 0) {
            s_on_new = true;
            s_phase = LAY_PH_SLOTA;
            note("slot A, old table, new filesystem");
            return;
        }
    }
    if (regions_ready(st->pex_crc, st->blob_crc) != 0) {
        note("region copy failed; not formatting the old volume");
        return;
    }
    fs_set_flash_off(LAY_FS_NEW, LAY_FS_BLOCKS);
    if (st->phase >= LAY_PH_RESTORED && fs_mount_at(LAY_FS_NEW, LAY_FS_BLOCKS) == 0) {
        uint32_t c = 0, sz = 0;
        if (file_crc("cfg/wifi.json", &c, &sz) == 0 && sz > 0) {
            s_on_new = true;
            finish_on_a(st);
            return;
        }
        fs_unmount();
    }
    if (st->attempts >= 3) { note("restore failed 3 times; the card copy is intact"); return; }
    if (cfg_from_sd(st->run) != 0) { note("cfg could not be reloaded from the card"); return; }
    st->attempts++;
    state_write(st);
    format_and_restore(st);
    if (fs_is_mounted()) { finish_on_a(st); }
}

static int take_buf(void) {
    s_ring = sd_xfer_borrow();
    s_buf = s_ring >= 0 ? sd_xfer_buf(s_ring) : try_alloc(BLK);
    return s_buf ? 0 : -1;
}

static void drop_buf(void) {
    if (s_ring >= 0) { sd_xfer_return(s_ring); }
    else { free(s_buf); }
    s_ring = -1;
    s_buf = NULL;
}

void fs_layout_boot(void) {
    ota_slot_init();
    pico_get_unique_board_id_string(s_board, sizeof s_board);
    bool old_mounted = fs_is_mounted();
    sd_init();
    bool card = wait_for_card();
    lay_st_t st;
    bool known = false;
    if (card && take_buf() == 0) {
        sd_path(SDJ_MKDIR, ROOT_DIR, NULL);
        known = true;
        if (state_read(&st)) { s_phase = st.phase; }
        else { memset(&st, 0, sizeof st); }
    } else {
        memset(&st, 0, sizeof st);
    }
    bool armed = card && s_buf && sd_exists(ARM_PATH);
    lay_act_t act = fs_layout_decide(old_mounted, s_phase, armed, ota_buy_pending(), known);
    printf("layout: phase %s act %d armed %d pending %d card %d\n",
           fs_layout_phase_name(s_phase), (int)act, armed, ota_buy_pending(), card);

    if (act == LAY_ACT_MOUNT_NEW) {
        /* The old superblock is not a filesystem anymore, even if it mounted. */
        fs_freeze_old(true);
        s_suppress = true;
        if (fs_is_mounted()) { fs_unmount(); }
        if (!s_buf) { note("move has started and the card is not readable"); return; }
        resume_past_ponr(&st);
        drop_buf();
        free(s_cfg);
        s_cfg = NULL;
        return;
    }
    if (act == LAY_ACT_FORMAT_OLD) {
        if (!fs_layout_may_format_old(act, s_phase)) { note("refusing to format the old volume"); drop_buf(); return; }
        printf("layout: no filesystem and no move state; formatting the old volume\n");
        fs_set_flash_off(FS_FLASH_OFF, 0);
        fs_reformat(FS_SIZE / 4096u);
        drop_buf();
        return;
    }
    if (act == LAY_ACT_HOLD) { note("old volume unavailable; not formatting"); drop_buf(); return; }
    if (act == LAY_ACT_WAIT_BUY) { note("layout armed, waiting for the OTA confirm"); drop_buf(); return; }
    if (act == LAY_ACT_MOVE) {
        if (!s_buf) { note("no buffer for the copy"); return; }
        if (s_phase == LAY_PH_VERIFIED) {
            char err[64];
            if (man_check(st.run, err, sizeof err) == 0 && cfg_from_sd(st.run) == 0) { begin_format(&st); }
            else { note("verified copy did not recheck; nothing formatted"); }
        } else {
            lay_st_t prev;
            unsigned next = 1;
            if (state_read(&prev) && prev.run) { next = prev.run + 1; }
            memset(&st, 0, sizeof st);
            st.run = next;
            st.phase = LAY_PH_COPYING;
            snprintf(st.board, sizeof st.board, "%s", s_board);
            if (state_write(&st) != 0) { note("could not record the copy; nothing moved"); }
            else { do_copy(&st); }
        }
        drop_buf();
        free(s_cfg);
        s_cfg = NULL;
        return;
    }
    drop_buf();
}

bool fs_layout_suppress_migrate(void) {
    return s_suppress || fs_flash_off() != FS_FLASH_OFF;
}

int fs_layout_status_json(char *buf, size_t cap) {
    if (!s_on_new && fs_flash_off() == FS_FLASH_OFF) { return 0; }
    return snprintf(buf, cap,
                    "{\"kb\":%lu,\"layout_kb\":%u,\"migration\":\"%s\","
                    "\"pex\":{\"offset\":%lu,\"size\":%u,\"state\":\"moved\"}}",
                    (unsigned long)(fs_mounted_size() / 1024u), (unsigned)(LAY_FS_NEW_SIZE / 1024u),
                    s_note[0] ? s_note : "layout",
                    (unsigned long)LAY_PEX_NEW, (unsigned)LAY_PEX_NEW_SIZE);
}

static int fit_blocks(uint32_t *used_out) {
    if (!fs_lfs()) { return -1; }
    lfs_ssize_t u = lfs_fs_size(fs_lfs());
    if (u < 0) { return -1; }
    *used_out = (uint32_t)u;
    return ((uint32_t)u + LAY_SLACK_BLOCKS <= LAY_FS_BLOCKS) ? 1 : 0;
}

int fs_layout_dry_json(char *buf, size_t cap) {
    uint32_t used = 0;
    int fit = fit_blocks(&used);
    int slot = ota_slot_active();
    int n = snprintf(buf, cap,
                     "{\"phase\":\"%s\",\"armed\":%s,\"used_blocks\":%lu,\"limit_blocks\":%u,"
                     "\"slack\":%u,\"fit\":%s,\"kb\":%lu,\"off\":%lu,\"buy_pending\":%s,\"slot\":%d,"
                     "\"note\":\"%s\"}",
                     fs_layout_phase_name(s_phase),
                     (s_phase < LAY_PH_FORMATTING && sd_pub()->media == LS_MEDIA_OK && sd_exists(ARM_PATH))
                         ? "true" : "false",
                     (unsigned long)used, LAY_FS_BLOCKS, LAY_SLACK_BLOCKS,
                     fit == 1 ? "true" : "false",
                     (unsigned long)(fs_mounted_size() / 1024u), (unsigned long)fs_flash_off(),
                     ota_buy_pending() ? "true" : "false", slot < 0 ? -1 : slot,
                     s_note);
    if (n < 0 || (size_t)n >= cap) { return -1; }
    return n;
}

int fs_layout_arm(void) {
    if (s_phase >= LAY_PH_FORMATTING || fs_flash_off() != FS_FLASH_OFF) { return -3; }
    if (ota_buy_pending()) { return -4; }
    uint32_t used = 0;
    int fit = fit_blocks(&used);
    if (fit < 0) { return -1; }
    if (fit == 0) { return -1; }
    if (sd_pub()->media != LS_MEDIA_OK) { return -2; }
    if (!s_buf && take_buf() != 0) { return -2; }
    sd_path(SDJ_MKDIR, ROOT_DIR, NULL);
    int r = sd_put(ARM_PATH, "1\n", 2);
    if (!s_suppress) { drop_buf(); }
    return r == 0 ? 0 : -2;
}

int fs_layout_commit(const char *confirm, char *err, size_t errcap) {
    if (!confirm || strcmp(confirm, "write-table") != 0) {
        snprintf(err, errcap, "confirm must be write-table");
        return -1;
    }
    if (ota_slot_active() != 0 || ota_buy_pending()) {
        snprintf(err, errcap, "run from a confirmed slot A");
        return -1;
    }
    if (s_phase != LAY_PH_SLOTA && s_phase != LAY_PH_RESTORED) {
        snprintf(err, errcap, "filesystem move is not finished");
        return -1;
    }
    if (fs_flash_off() != LAY_FS_NEW || !fs_is_mounted()) {
        snprintf(err, errcap, "new filesystem is not mounted");
        return -1;
    }
    ota_part_t a;
    if (!ota_slot_get(0, &a) || a.start != LAY_SLOT_A) {
        snprintf(err, errcap, "slot A is not at 0x10000");
        return -1;
    }
    if (a.size == LAY_SLOT_SIZE) {
        snprintf(err, errcap, "table already has 1 MiB slots");
        return 1;
    }
    if (a.size != LAY_OLD_SLOT_SIZE || a.start + a.size > LAY_SLOT_B) {
        snprintf(err, errcap, "running image reaches the erase");
        return -1;
    }
    if (!fs_layout_pt_matches(fs_layout_pt, fs_layout_pt_len) || fs_layout_pt_len > 4096u) {
        snprintf(err, errcap, "embedded table does not match the map");
        return -1;
    }
    printf("layout: erasing 0x%lx then writing the partition table\n", (unsigned long)LAY_SLOT_B);
    watchdog_update();
    if (flash_op(0, LAY_SLOT_B, NULL, 4096u) != 0) {
        snprintf(err, errcap, "could not erase the new slot B header");
        return -1;
    }
    if (flash_op(0, 0, NULL, 4096u) != 0) {
        snprintf(err, errcap, "could not erase the partition table");
        return -1;
    }
    uint8_t page[256];
    for (uint32_t o = 0; o < fs_layout_pt_len; o += sizeof page) {
        memset(page, 0xFF, sizeof page);
        uint32_t n = fs_layout_pt_len - o > sizeof page ? (uint32_t)sizeof page : fs_layout_pt_len - o;
        memcpy(page, fs_layout_pt + o, n);
        if (flash_op(1, o, page, sizeof page) != 0) {
            snprintf(err, errcap, "table program failed; do not reboot");
            return -1;
        }
    }
    if (memcmp((const uint8_t *)(FS_READ_BASE + 0), fs_layout_pt, fs_layout_pt_len) != 0) {
        snprintf(err, errcap, "table readback mismatch; do not reboot");
        return -1;
    }
    printf("layout: table readback matched; rebooting\n");
    api_reboot_in(1000);
    snprintf(err, errcap, "ok");
    return 0;
}
