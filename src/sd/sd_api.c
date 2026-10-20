/* sd_api.h: SD card status and file transfers through the storage worker.
 * Ported from km003c_rp2_webapp (src/log/sd_api.c): the same /api/sd answers,
 * error map and transfer mechanics, without the logger -- the jobs are the
 * transfer slots themselves (sd_core.c's worker loop runs any slot that is
 * OPENING), the published card state is sd_core's, and f_mkfs borrows a
 * transfer ring as its work buffer (no transfer runs during a format). Added
 * here: SD_OP_MKDIR. */
#include "sd/sd_api.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "sd/sd_card.h"
#include "sd/sd_core.h"
#include "sd/sd_port.h"
#include "sd/sd_store.h"
#include "sd/sd_worker.h"

_Static_assert((SD_XFER_BUF & (SD_XFER_BUF - 1)) == 0 && SD_XFER_BUF >= 512,
               "SD_XFER_BUF: a power of two, at least one sector");

#define CHUNK 512u /* card I/O unit: whole aligned sectors where it can */

struct xfer {
    int state;
    int op;
    bool released; /* the HTTP side is done with it */
    bool in_done;  /* PUT: all the body is in the ring */
    char path[SD_PATH_MAX];
    char path2[SD_PATH_MAX];
    uint64_t len;  /* PUT: Content-Length */
    uint64_t size; /* GET: file size (UINT64_MAX: listing); PUT: bytes written */
    const char *mime;
    struct sd_reply res;
    uint32_t wr, rd; /* free-running ring positions */
    uint8_t buf[SD_XFER_BUF];
};

static struct xfer s_x[SD_XFERS];
static uint64_t s_xfer_bytes; /* moved to or from the card by transfers, since boot */

/* The last transfer that failed, and the last transfer connection that
 * ended other than by our own reply: for chasing a transfer that died. */
static struct {
    int op, status;
    char error[32];
    uint64_t bytes;
} s_last_fail;
static struct {
    int op, code;
    char why[24];
    uint64_t bytes;
    uint32_t count;
} s_last_end;


void sd_api_stats_reset(void) {
    struct sdw_stats w;
    sdw_get_stats(&w, true);
    ls_busy_max_us(true);
}

/* ---- shared helpers ------------------------------------------------------ */

static void set_reply(struct sd_reply *r, int status, int retry, const char *card, const char *err) {
    r->status = status;
    r->retry_after = retry;
    r->card = card;
    snprintf(r->error, sizeof(r->error), "%s", err ? err : "");
}

static bool fmt_running(void);
static int bench_json(char *buf, size_t cap);

const char *sd_card_state(void) {
    const struct sd_pub *p = sd_pub();
    if (p->formatting || fmt_running()) {
        return "formatting";
    }
    if (!p->present) {
        return "none";
    }
    if (p->media == LS_MEDIA_OK) {
        return "ok";
    }
    return p->media == LS_MEDIA_ERROR ? "error" : "settling";
}

bool sd_api_gate(struct sd_reply *r) {
    const char *st = sd_card_state();
    if (strcmp(st, "ok") == 0) {
        return true;
    }
    if (strcmp(st, "formatting") == 0) {
        set_reply(r, 503, 5, st, "formatting");
    } else if (strcmp(st, "none") == 0) {
        set_reply(r, 503, 5, st, "no card");
    } else if (strcmp(st, "settling") == 0) {
        set_reply(r, 503, 2, st, "card settling");
    } else {
        set_reply(r, 503, 0, st, "card not mounted");
    }
    return false;
}

static int hexv(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

bool sd_path_clean(char *path, size_t cap) {
    /* Percent-decode in place (the output is never longer). */
    size_t o = 0;
    for (size_t i = 0; path[i]; i++) {
        char c = path[i];
        if (c == '%' && hexv(path[i + 1]) >= 0 && hexv(path[i + 2]) >= 0) {
            c = (char)(hexv(path[i + 1]) << 4 | hexv(path[i + 2]));
            i += 2;
        }
        path[o++] = c;
    }
    path[o] = 0;
    /* Trim slashes at both ends. */
    size_t s = 0;
    while (path[s] == '/') {
        s++;
    }
    memmove(path, path + s, strlen(path + s) + 1);
    size_t n = strlen(path);
    while (n && path[n - 1] == '/') {
        path[--n] = 0;
    }
    if (n + 1 > cap || n >= SD_PATH_MAX) {
        return false;
    }
    const char *seg = path;
    for (size_t i = 0; i <= n; i++) {
        unsigned char c = (unsigned char)path[i];
        if (c == '/' || c == 0) {
            size_t sl = (size_t)(path + i - seg);
            if (n && (sl == 0 || (sl == 1 && seg[0] == '.') || (sl == 2 && seg[0] == '.' && seg[1] == '.'))) {
                return false;
            }
            seg = path + i + 1;
            continue;
        }
        if (c < 0x20 || c == 0x7F || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' ||
            c == '>' || c == '|') {
            return false; /* not a FAT name, or a way out of the tree */
        }
    }
    return true;
}

/* ---- main context: transfers --------------------------------------------- */

int sd_xfer_start(enum sd_op op, const char *path, const char *path2, uint64_t len, struct sd_reply *r) {
    sdw_lock();
    int x = -1;
    for (int i = 0; i < SD_XFERS && x < 0; i++) {
        if (s_x[i].state == SDX_FREE) {
            x = i;
        }
    }
    if (x < 0) {
        sdw_unlock();
        set_reply(r, 503, 1, sd_card_state(), "card busy");
        return -1;
    }
    struct xfer *t = &s_x[x];
    memset(t, 0, offsetof(struct xfer, buf));
    t->state = SDX_OPENING;
    t->op = op;
    snprintf(t->path, sizeof(t->path), "%s", path ? path : "");
    snprintf(t->path2, sizeof(t->path2), "%s", path2 ? path2 : "");
    t->len = len;
    sdw_unlock();
    sdw_kick();                    /* the worker runs every OPENING slot */
    return x;
}

enum sd_xstate sd_xfer_state(int x) {
    sdw_lock();
    int s = s_x[x].state;
    sdw_unlock();
    return (enum sd_xstate)s;
}

uint64_t sd_xfer_size(int x) {
    return s_x[x].size;
}

const char *sd_xfer_mime(int x) {
    return s_x[x].mime ? s_x[x].mime : "application/octet-stream";
}

const char *sd_xfer_name(int x) {
    const char *s = strrchr(s_x[x].path, '/');
    return s ? s + 1 : s_x[x].path;
}

void sd_xfer_result(int x, struct sd_reply *r) {
    sdw_lock();
    *r = s_x[x].res;
    sdw_unlock();
}

size_t sd_xfer_peek(int x, const uint8_t **p) {
    struct xfer *t = &s_x[x];
    sdw_lock();
    uint32_t n = t->wr - t->rd;
    uint32_t off = t->rd % SD_XFER_BUF;
    sdw_unlock();
    if (n > SD_XFER_BUF - off) {
        n = SD_XFER_BUF - off;
    }
    *p = t->buf + off;
    return n;
}

void sd_xfer_consume(int x, size_t n) {
    sdw_lock();
    s_x[x].rd += (uint32_t)n;
    sdw_unlock();
    sdw_kick();
}

size_t sd_xfer_put(int x, const uint8_t *p, size_t n) {
    struct xfer *t = &s_x[x];
    size_t done = 0;
    while (done < n) {
        sdw_lock();
        uint32_t space = SD_XFER_BUF - (t->wr - t->rd);
        uint32_t off = t->wr % SD_XFER_BUF;
        sdw_unlock();
        if (space > SD_XFER_BUF - off) {
            space = SD_XFER_BUF - off;
        }
        if (!space) {
            break;
        }
        size_t k = n - done < space ? n - done : space;
        memcpy(t->buf + off, p + done, k);
        sdw_lock();
        t->wr += (uint32_t)k;
        sdw_unlock();
        done += k;
    }
    if (done) {
        sdw_kick();
    }
    return done;
}

void sd_xfer_end_input(int x) {
    sdw_lock();
    s_x[x].in_done = true;
    sdw_unlock();
    sdw_kick();
}

void sd_xfer_release(int x) {
    if (x < 0 || x >= SD_XFERS) {
        return;
    }
    sdw_lock();
    struct xfer *t = &s_x[x];
    t->released = true;
    if (t->state == SDX_DONE || t->state == SDX_FAIL) {
        t->state = SDX_FREE; /* the worker is finished with it */
    }
    sdw_unlock();
    sdw_kick();
}

int sd_xfer_active(void) {
    int n = 0;
    sdw_lock();
    for (int i = 0; i < SD_XFERS; i++) {
        n += s_x[i].state != SDX_FREE;
    }
    sdw_unlock();
    return n;
}

int sd_api_status_json(char *buf, size_t cap) {
    const struct sd_pub *p = sd_pub();
    struct sd_format_status f;
    sd_format_get(&f);
    const sd_info_t *ci = sd_card_info();
    int n = snprintf(buf, cap,
                     "{\"ok\":true,\"present\":%s,\"mounted\":%s,\"state\":\"%s\",\"fs\":\"%s\","
                     "\"total\":%llu,\"free\":%llu,\"bus\":{\"type\":\"%s\",\"width\":%u,\"clock_khz\":%lu,\"crc\":%s},",
                     p->present ? "true" : "false", p->media == LS_MEDIA_OK ? "true" : "false", sd_card_state(),
                     p->fs ? p->fs : "-", (unsigned long long)p->total_bytes,
                     (unsigned long long)p->free_bytes, sd_card_bus() ? sd_card_bus()->name : "-",
                     ci->valid ? ci->bus_width : 0, (unsigned long)(ci->valid ? ci->clock_khz : 0),
                     ci->valid && ci->crc ? "true" : "false");
    if (n < 0 || (size_t)n >= cap) {
        return -1;
    }
    int m = sd_format_json(buf + n, cap - (size_t)n, &f);
    if (m < 0) {
        return -1;
    }
    n += m;
    m = bench_json(buf + n, cap - (size_t)n);
    if (m < 0 || (size_t)(n + m) >= cap) {
        return -1;
    }
    n += m;
    struct sdw_stats w;
    sdw_get_stats(&w, false);
    uint32_t cto = 0, cre = 0, cdma = 0;
    ls_card_counters(&cto, &cre, &cdma);
    const sd_stats_t *cs = sd_card_stats();
    m = snprintf(buf + n, cap - (size_t)n,
                 ",\"worker\":{\"stack\":%lu,\"stack_used\":%lu,\"xfers\":%d,\"xfer_max\":%d,"
                 "\"store_errors\":%lu,"
                 "\"slices\":%lu,\"slice_max_us\":%lu,\"slice_max_phase\":\"%s\",\"slices_over_2ms\":%lu,\"slices_over_10ms\":%lu,"
                 "\"slices_over_50ms\":%lu,"
                 "\"card_busy_max_us\":%lu,\"card_busy_timeouts\":%lu,\"card_write_retries\":%lu,\"card_dma_blocks\":%lu,"
                 "\"card_read_retries\":%lu,\"card_crc_errors\":%lu,\"card_inits\":%lu,\"card_recoveries\":%lu,"
                 "\"xfer_bytes\":%llu,"
                 "\"last_fail\":{\"op\":%d,\"status\":%d,\"error\":\"%s\",\"bytes\":%llu},"
                 "\"last_conn_end\":{\"op\":%d,\"why\":\"%s\",\"code\":%d,\"bytes\":%llu,\"count\":%lu}}}",
                 (unsigned long)sdw_stack_size(), (unsigned long)sdw_stack_used(), sd_xfer_active(), SD_XFERS,
                 (unsigned long)p->errors, (unsigned long)w.slices, (unsigned long)w.slice_max_us,
                 w.slice_max_phase ? w.slice_max_phase : "",
                 (unsigned long)w.over_2ms, (unsigned long)w.over_10ms, (unsigned long)w.over_50ms,
                 (unsigned long)ls_busy_max_us(false),
                 (unsigned long)cto, (unsigned long)cre, (unsigned long)cdma,
                 (unsigned long)cs->read_retries, (unsigned long)cs->crc_errors, (unsigned long)cs->inits,
                 (unsigned long)cs->recoveries,
                 (unsigned long long)s_xfer_bytes, s_last_fail.op, s_last_fail.status, s_last_fail.error,
                 (unsigned long long)s_last_fail.bytes, s_last_end.op, s_last_end.why, s_last_end.code,
                 (unsigned long long)s_last_end.bytes, (unsigned long)s_last_end.count);
    if (m < 0 || (size_t)(n + m) >= cap) {
        return -1;
    }
    return n + m;
}

/* ---- worker ---------------------------------------------------------------- */

void sd_xfer_note_end(int x, const char *why, int code) {
    if (x < 0 || x >= SD_XFERS) {
        return;
    }
    sdw_lock();
    s_last_end.op = s_x[x].op;
    s_last_end.code = code;
    snprintf(s_last_end.why, sizeof(s_last_end.why), "%s", why);
    s_last_end.bytes = s_x[x].op == SD_OP_PUT ? s_x[x].rd : s_x[x].wr;
    s_last_end.count++;
    sdw_unlock();
}

static void finish(struct xfer *t, int state, int status, int retry, const char *err) {
    if (state == SDX_FAIL) {
        sdw_lock();
        s_last_fail.op = t->op;
        s_last_fail.status = status;
        snprintf(s_last_fail.error, sizeof(s_last_fail.error), "%s", err ? err : "");
        s_last_fail.bytes = t->op == SD_OP_PUT ? t->rd : t->wr;
        sdw_unlock();
    }
    sdw_lock();
    set_reply(&t->res, status, retry, NULL, err);
    t->state = state;
    if (t->released) {
        t->state = SDX_FREE; /* nobody is waiting for the answer */
    }
    sdw_unlock();
}

static void fail_ls(struct xfer *t, int r) {
    switch (r) {
    case LS_ENOENT:
        finish(t, SDX_FAIL, 404, 0, "not found");
        break;
    case LS_EEXIST:
        finish(t, SDX_FAIL, 409, 0, "exists");
        break;
    case LS_ENOSPC:
        finish(t, SDX_FAIL, 507, 0, "card full");
        break;
    case LS_EINVAL:
        finish(t, SDX_FAIL, 400, 0, "bad path");
        break;
    case LS_ENOMEDIA:
        finish(t, SDX_FAIL, 503, 5, "no card");
        break;
    case LS_EBUSY:
    case LS_ENOMEM:
        finish(t, SDX_FAIL, 503, 1, "card busy");
        break;
    default:
        finish(t, SDX_FAIL, 500, 0, "card I/O error");
        break;
    }
}

static bool released(struct xfer *t) {
    sdw_lock();
    bool r = t->released;
    sdw_unlock();
    return r;
}

/* Waits for the HTTP side. False once it has let go. */
static bool wait_peer(struct xfer *t) {
    if (released(t)) {
        return false;
    }
    sdw_idle(5000);
    return !released(t);
}

static uint32_t ring_space(struct xfer *t) {
    sdw_lock();
    uint32_t s = SD_XFER_BUF - (t->wr - t->rd);
    sdw_unlock();
    return s;
}

/* Copies into the ring, waiting for room. */
static bool out(struct xfer *t, const char *s, size_t n) {
    while (n) {
        uint32_t space = ring_space(t);
        if (!space) {
            if (!wait_peer(t)) {
                return false;
            }
            continue;
        }
        uint32_t off = t->wr % SD_XFER_BUF;
        uint32_t k = SD_XFER_BUF - off < space ? SD_XFER_BUF - off : space;
        if (k > n) {
            k = (uint32_t)n;
        }
        memcpy(t->buf + off, s, k);
        sdw_lock();
        t->wr += k;
        sdw_unlock();
        s += k;
        n -= k;
    }
    return true;
}

static const char *mime_of(const char *name) {
    const char *e = strrchr(name, '.');
    if (!e) {
        return "application/octet-stream";
    }
    static const char *const map[][2] = {
        {".json", "application/json"},  {".txt", "text/plain; charset=utf-8"}, {".csv", "text/csv"},
        {".html", "text/html; charset=utf-8"}, {".log", "text/plain; charset=utf-8"},
        {".png", "image/png"},          {".jpg", "image/jpeg"}, {".svg", "image/svg+xml"},
    };
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (strcasecmp(e, map[i][0]) == 0) {
            return map[i][1];
        }
    }
    return "application/octet-stream";
}

/* -- listing: in batches, re-listing from the start and skipping what has
 * gone out, so no directory stays open across a wait (the store has a fixed
 * pool of directory objects, and another transfer may list too). */
struct lctx {
    struct xfer *t;
    uint32_t skip, seen, emitted;
    bool full;
};

static bool json_name(char *o, size_t cap, const char *name) {
    size_t w = 0;
    for (; *name; name++) {
        char c = *name;
        if (w + 3 >= cap) {
            return false;
        }
        if (c == '"' || c == '\\') {
            o[w++] = '\\';
        }
        o[w++] = c;
    }
    o[w] = 0;
    return true;
}

static bool on_entry(const char *name, bool is_dir, uint64_t size, void *vctx) {
    struct lctx *l = vctx;
    if (l->seen++ < l->skip) {
        return true;
    }
    char esc[2 * 64 + 4], line[200];
    if (!json_name(esc, sizeof(esc), name)) {
        return true; /* a name too long to list; FAT allows 255, we show 64 */
    }
    int n = is_dir ? snprintf(line, sizeof(line), "%s{\"name\":\"%s\",\"dir\":true}", l->skip + l->emitted ? "," : "", esc)
                   : snprintf(line, sizeof(line), "%s{\"name\":\"%s\",\"size\":%llu}", l->skip + l->emitted ? "," : "",
                              esc, (unsigned long long)size);
    if (n <= 0 || (size_t)n > ring_space(l->t)) {
        l->full = true;
        return false;
    }
    out(l->t, line, (size_t)n); /* fits: never waits */
    l->emitted++;
    return true;
}

static bool probe_cb(const char *name, bool is_dir, uint64_t size, void *vctx) {
    (void)name;
    (void)is_dir;
    (void)size;
    *(bool *)vctx = true;
    return false;
}

static void run_list(struct xfer *t) {
    char head[2 * SD_PATH_MAX + 32];
    char esc[2 * SD_PATH_MAX + 4];
    json_name(esc, sizeof(esc), t->path);
    int n = snprintf(head, sizeof(head), "{\"dir\":\"%s\",\"files\":[", esc);
    sdw_lock();
    t->size = UINT64_MAX;
    t->mime = "application/json";
    t->state = SDX_STREAM;
    sdw_unlock();
    if (!out(t, head, (size_t)n)) {
        finish(t, SDX_FAIL, 499, 0, "client gone");
        return;
    }
    uint32_t sent = 0;
    for (;;) {
        struct lctx l = {t, sent, 0, 0, false};
        int r = ls_list(t->path, on_entry, &l);
        if (r < 0) {
            fail_ls(t, r); /* headers are out: the HTTP side aborts */
            return;
        }
        sent += l.emitted;
        if (!l.full) {
            break;
        }
        if (!wait_peer(t)) {
            finish(t, SDX_FAIL, 499, 0, "client gone");
            return;
        }
    }
    if (!out(t, "]}", 2)) {
        finish(t, SDX_FAIL, 499, 0, "client gone");
        return;
    }
    finish(t, SDX_DONE, 200, 0, NULL);
}

static void run_get(struct xfer *t) {
    bool any = false;
    if (t->path[0] == 0 || ls_list(t->path, probe_cb, &any) >= 0) {
        run_list(t);
        return;
    }
    ls_file *f = ls_open(t->path, LS_RD);
    if (!f) {
        finish(t, SDX_FAIL, 404, 0, "not found");
        return;
    }
    sdw_lock();
    t->size = ls_size(f);
    t->mime = mime_of(t->path);
    t->state = SDX_STREAM;
    sdw_unlock();
    uint64_t left = t->size;
    while (left) {
        uint32_t space = ring_space(t);
        if (space < CHUNK) {
            if (!wait_peer(t)) {
                ls_close(f);
                finish(t, SDX_FAIL, 499, 0, "client gone");
                return;
            }
            continue;
        }
        /* wr moves in whole CHUNKs (but the last), so the free region from
         * it is contiguous to the end of the buffer: read as many sectors
         * as fit there, in one multi-block read. */
        uint32_t off = t->wr % SD_XFER_BUF;
        uint32_t contig = SD_XFER_BUF - off < space ? SD_XFER_BUF - off : space;
        uint32_t want = contig / CHUNK * CHUNK;
        if (want > left) {
            want = (uint32_t)left;
        }
        int r = ls_read(f, t->buf + t->wr % SD_XFER_BUF, want);
        if (r <= 0) {
            ls_close(f);
            fail_ls(t, r < 0 ? r : LS_EIO); /* pulled mid-file: the HTTP side aborts */
            return;
        }
        sdw_lock();
        t->wr += (uint32_t)r; /* short only at the end of the file */
        s_xfer_bytes += (uint64_t)r;
        sdw_unlock();
        left -= (uint64_t)r;
        sdw_maybe_yield(2000);
    }
    ls_close(f);
    finish(t, SDX_DONE, 200, 0, NULL);
}

/* Freeing a big file's clusters is one long FatFs call (a 1 GB file on the
 * km003c bench: hundreds of ms in one worker slice). So a big file is cut
 * down from the end in steps, yielding between them, and only then removed.
 * Returns ls_remove()'s result. */
#define SHRINK_STEP (32u * 1024u * 1024u)

static int remove_gently(const char *path) {
    ls_file *f = ls_open(path, LS_RW);
    if (f) {
        uint64_t size = ls_size(f);
        while (size > SHRINK_STEP) {
            size -= SHRINK_STEP;
            if (ls_seek(f, size) != LS_OK || ls_truncate(f) != LS_OK) {
                break;
            }
            sdw_maybe_yield(2000);
        }
        ls_close(f);
        sdw_maybe_yield(2000);
    }
    return ls_remove(path);
}

/* mkdir -p of the parent of `path`. */
static int mkparents(const char *path) {
    char p[SD_PATH_MAX];
    snprintf(p, sizeof(p), "%s", path);
    for (char *s = p; *s; s++) {
        if (*s == '/') {
            *s = 0;
            int r = ls_mkdir(p);
            *s = '/';
            if (r < 0 && r != LS_EEXIST) {
                return r;
            }
        }
    }
    return LS_OK;
}

static void run_put(struct xfer *t) {
    if (!t->path[0]) {
        finish(t, SDX_FAIL, 400, 0, "bad path");
        return;
    }
    uint64_t freeb = 0, total = 0;
    if (ls_statfs(&freeb, &total) == LS_OK && t->len > freeb) {
        finish(t, SDX_FAIL, 507, 0, "card full");
        return;
    }
    bool any = false;
    if (ls_list(t->path, probe_cb, &any) >= 0) {
        finish(t, SDX_FAIL, 409, 0, "is a directory");
        return;
    }
    int r = mkparents(t->path);
    if (r < 0) {
        fail_ls(t, r);
        return;
    }
    remove_gently(t->path); /* an older file of that name, if any */
    ls_file *f = ls_open(t->path, LS_WR);
    if (!f) {
        finish(t, SDX_FAIL, 500, 0, "card I/O error");
        return;
    }
    sdw_lock();
    t->state = SDX_STREAM;
    sdw_unlock();
    uint64_t got = 0;
    for (;;) {
        sdw_lock();
        uint32_t avail = t->wr - t->rd;
        bool in_done = t->in_done;
        sdw_unlock();
        /* Whole sectors while more is coming, as many as lie contiguous
         * (one multi-block write); the tail once it has all come. */
        if (avail >= CHUNK || (in_done && avail)) {
            uint32_t off = t->rd % SD_XFER_BUF;
            uint32_t k = SD_XFER_BUF - off < avail ? SD_XFER_BUF - off : avail;
            if (k >= CHUNK) {
                k = k / CHUNK * CHUNK;
            }
            int w = ls_write(f, t->buf + off, k);
            if (w < 0) {
                ls_close(f);
                remove_gently(t->path);
                fail_ls(t, w);
                return;
            }
            sdw_lock();
            t->rd += k;
            s_xfer_bytes += k;
            sdw_unlock();
            got += k;
            sdw_maybe_yield(2000);
            continue;
        }
        if (in_done) {
            break;
        }
        if (!wait_peer(t)) {
            ls_close(f);
            remove_gently(t->path); /* the upload never finished */
            finish(t, SDX_FAIL, 499, 0, "client gone");
            return;
        }
    }
    r = ls_close(f);
    if (r < 0) {
        ls_remove(t->path);
        fail_ls(t, r);
        return;
    }
    sdw_lock();
    t->size = got;
    sdw_unlock();
    finish(t, SDX_DONE, 200, 0, NULL);
}

static void run_mkdir(struct xfer *t) {
    if (!t->path[0]) {
        finish(t, SDX_FAIL, 400, 0, "bad path");
        return;
    }
    char p[SD_PATH_MAX + 2];
    snprintf(p, sizeof(p), "%s/", t->path);   /* mkparents makes every level of it */
    int r = mkparents(p);
    if (r < 0) {
        fail_ls(t, r);
        return;
    }
    finish(t, SDX_DONE, 200, 0, NULL);
}

static void run_delete(struct xfer *t) {
    if (!t->path[0]) {
        finish(t, SDX_FAIL, 400, 0, "bad path");
        return;
    }
    bool any = false;
    if (ls_list(t->path, probe_cb, &any) >= 0 && any) {
        finish(t, SDX_FAIL, 409, 0, "directory not empty");
        return;
    }
    int r = remove_gently(t->path);
    if (r < 0) {
        fail_ls(t, r);
        return;
    }
    finish(t, SDX_DONE, 200, 0, NULL);
}

/* ---- bench ------------------------------------------------------------- */

static struct {
    int state; /* SDF_* */
    uint32_t mb, chunk;
    uint32_t write_us, read_us;
    uint32_t busy_max_us;
    uint32_t done_kb;
    char error[40];
} s_bench;

#define BENCH_PATH "BTGW-BENCH.bin"

bool sd_bench_start(uint32_t mb, struct sd_reply *r) {
    if (!sd_api_gate(r)) {
        return false;
    }
    if (mb == 0 || mb > 4096) {
        mb = 10;
    }
    sdw_lock();
    bool running = s_bench.state == SDF_RUNNING;
    sdw_unlock();
    if (running) {
        set_reply(r, 409, 0, sd_card_state(), "bench already running");
        return false;
    }
    if (sd_xfer_start(SD_OP_BENCH, BENCH_PATH, NULL, (uint64_t)mb * 1000000u, r) < 0) {
        return false;
    }
    sdw_lock();
    memset(&s_bench, 0, sizeof(s_bench));
    s_bench.state = SDF_RUNNING;
    s_bench.mb = mb;
    s_bench.chunk = SD_XFER_BUF;
    sdw_unlock();
    return true;
}

static void bench_end(struct xfer *t, int state, const char *err) {
    sdw_lock();
    s_bench.state = state;
    s_bench.busy_max_us = ls_busy_max_us(false);
    snprintf(s_bench.error, sizeof(s_bench.error), "%s", err ? err : "");
    sdw_unlock();
    /* Nobody waits on this slot's answer: it frees itself. */
    sdw_lock();
    t->released = true;
    sdw_unlock();
    finish(t, state == SDF_DONE ? SDX_DONE : SDX_FAIL, 200, 0, NULL);
}

static void run_bench(struct xfer *t) {
    uint64_t total = t->len;
    for (uint32_t i = 0; i < SD_XFER_BUF; i++) {
        t->buf[i] = (uint8_t)(i * 31u + 7u);
    }
    ls_busy_max_us(true);
    ls_file *f = ls_open(BENCH_PATH, LS_WR);
    if (!f) {
        bench_end(t, SDF_ERROR, "open failed");
        return;
    }
    uint64_t t0 = sd_port_now_us();
    for (uint64_t done = 0; done < total; done += SD_XFER_BUF) {
        if (ls_write(f, t->buf, SD_XFER_BUF) != SD_XFER_BUF) {
            ls_close(f);
            ls_remove(BENCH_PATH);
            bench_end(t, SDF_ERROR, "write failed");
            return;
        }
        s_bench.done_kb = (uint32_t)(done / 1000u);
        sdw_maybe_yield(2000);
    }
    int r = ls_close(f);
    s_bench.write_us = (uint32_t)(sd_port_now_us() - t0);
    if (r < 0) {
        ls_remove(BENCH_PATH);
        bench_end(t, SDF_ERROR, "close failed");
        return;
    }
    f = ls_open(BENCH_PATH, LS_RD);
    if (!f) {
        bench_end(t, SDF_ERROR, "reopen failed");
        return;
    }
    t0 = sd_port_now_us();
    for (uint64_t done = 0; done < total; done += SD_XFER_BUF) {
        if (ls_read(f, t->buf, SD_XFER_BUF) <= 0) {
            break;
        }
        sdw_maybe_yield(2000);
    }
    s_bench.read_us = (uint32_t)(sd_port_now_us() - t0);
    ls_close(f);
    remove_gently(BENCH_PATH); /* a big file: in steps */
    bench_end(t, SDF_DONE, NULL);
}

static int bench_json(char *buf, size_t cap) {
    static const char *const st[] = {"idle", "running", "done", "error"};
    uint64_t bytes = (uint64_t)s_bench.mb * 1000000u;
    return snprintf(buf, cap,
                    ",\"bench\":{\"state\":\"%s\",\"mb\":%lu,\"chunk\":%lu,\"done_kb\":%lu,"
                    "\"write_kBps\":%lu,\"read_kBps\":%lu,\"card_busy_max_us\":%lu,\"error\":\"%s\"}",
                    st[s_bench.state & 3], (unsigned long)s_bench.mb, (unsigned long)s_bench.chunk,
                    (unsigned long)s_bench.done_kb,
                    (unsigned long)(s_bench.write_us ? bytes * 1000u / s_bench.write_us : 0),
                    (unsigned long)(s_bench.read_us ? bytes * 1000u / s_bench.read_us : 0),
                    (unsigned long)s_bench.busy_max_us, s_bench.error);
}

static void run_rename(struct xfer *t) {
    if (!t->path[0] || !t->path2[0]) {
        finish(t, SDX_FAIL, 400, 0, "bad path");
        return;
    }
    int r = mkparents(t->path2);
    if (r == LS_OK) {
        r = ls_rename(t->path, t->path2);
    }
    if (r < 0) {
        fail_ls(t, r);
        return;
    }
    finish(t, SDX_DONE, 200, 0, NULL);
}

void sd_xfer_run(int x) {
    struct xfer *t = &s_x[x];
    if (released(t)) {
        finish(t, SDX_FAIL, 499, 0, "client gone");
        return;
    }
    if (ls_media() != LS_MEDIA_OK) {
        fail_ls(t, LS_ENOMEDIA);
        return;
    }
    static const char *const phase[] = { [SD_OP_GET] = "get", [SD_OP_PUT] = "put", [SD_OP_DELETE] = "delete",
                                         [SD_OP_RENAME] = "rename", [SD_OP_BENCH] = "bench", [SD_OP_MKDIR] = "mkdir" };
    if ((unsigned)t->op < sizeof phase / sizeof phase[0] && phase[t->op]) { sdw_set_phase(phase[t->op]); }
    switch (t->op) {
    case SD_OP_GET:
        run_get(t);
        break;
    case SD_OP_PUT:
        run_put(t);
        break;
    case SD_OP_DELETE:
        run_delete(t);
        break;
    case SD_OP_RENAME:
        run_rename(t);
        break;
    case SD_OP_MKDIR:
        run_mkdir(t);
        break;
    case SD_OP_BENCH:
        run_bench(t);
        break;
    default:
        finish(t, SDX_FAIL, 400, 0, "bad request");
        break;
    }
}

/* ---- format ---------------------------------------------------------------- */

#define GIB (1024ull * 1024u * 1024u)

static struct sd_format_status s_fmt;
static uint32_t s_fmt_start; /* ls_sectors_written() when writing began */

static bool fmt_running(void) {
    sdw_lock();
    bool r = s_fmt.state == SDF_RUNNING;
    sdw_unlock();
    return r;
}

bool sd_format_running(void) {
    return fmt_running();
}

int sd_default_fs(uint64_t bytes) {
    /* SDXC (over 32 GiB) is exFAT by the SD spec; SDHC and smaller FAT32. */
    return bytes > 32 * GIB ? LS_FMT_EXFAT : LS_FMT_FAT32;
}

uint32_t sd_default_cluster(int fs, uint64_t bytes) {
    if (fs == LS_FMT_EXFAT) {
        /* The SD Association formatter's choices. */
        return bytes <= 32 * GIB ? 32768u : bytes <= 256 * GIB ? 131072u : 262144u;
    }
    /* 32 KB (the SD formatter's SDHC choice) from 4 GiB; below that the
     * largest that still leaves FAT32 its 65,525+ clusters. */
    if (bytes >= 4 * GIB) {
        return 32768u;
    }
    uint32_t c = 32768u;
    while (c > 4096u && bytes / c < 70000u) {
        c /= 2;
    }
    return c;
}

uint32_t sd_default_align(uint64_t bytes) {
    /* The card's allocation unit, so clusters never straddle an erase block:
     * 4 MiB up to 32 GiB, 16 MiB above. In sectors. */
    return bytes <= 32 * GIB ? 8192u : 32768u;
}

bool sd_cluster_ok(int fs, uint32_t cluster, uint64_t bytes, const char **err) {
    uint32_t hi = fs == LS_FMT_EXFAT ? 1048576u : 65536u;
    if (cluster < 4096 || cluster > hi || (cluster & (cluster - 1))) {
        *err = fs == LS_FMT_EXFAT ? "cluster: 4 KB to 1 MB, a power of two" : "cluster: 4 KB to 64 KB, a power of two";
        return false;
    }
    uint64_t sectors = bytes / 512u;
    uint64_t data = sectors > sd_default_align(bytes) * 2u ? sectors - sd_default_align(bytes) : 0;
    uint64_t clusters = data / (cluster / 512u);
    if (fs == LS_FMT_FAT32) {
        /* FAT32's own limits on the cluster count (with room for its FAT). */
        if (clusters < 65526u + 1024u) {
            *err = "too few clusters for FAT32 on this card: choose a smaller cluster";
            return false;
        }
        if (clusters > 0x0FFFFFF5u) {
            *err = "too many clusters for FAT32: choose a larger cluster";
            return false;
        }
    } else if (clusters < 16 || clusters > 0x7FFFFFFDu) {
        *err = "cluster count out of range for exFAT";
        return false;
    }
    return true;
}

/* Sectors f_mkfs will write, from the geometry: an estimate for the bar. */
static uint32_t format_total(int fs, uint32_t cluster, uint64_t bytes) {
    uint64_t spc = cluster / 512u;
    uint64_t clusters = bytes / 512u / spc;
    uint64_t fat = (clusters * 4u + 511u) / 512u;
    uint64_t t;
    if (fs == LS_FMT_FAT32) {
        t = 1 + 32 + 2 * fat + spc; /* MBR, reserved area, two FATs, the root cluster */
    } else {
        uint64_t bitmap = (clusters / 8u + 511u) / 512u;
        t = 1 + 24 + fat + bitmap + 12 + spc; /* MBR, boot regions, FAT, bitmap, up-case, root */
    }
    return t > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)t;
}

void sd_format_get(struct sd_format_status *out) {
    sdw_lock();
    *out = s_fmt;
    sdw_unlock();
    if (out->state == SDF_RUNNING && out->phase && strcmp(out->phase, "writing") == 0) {
        uint32_t d = ls_sectors_written() - s_fmt_start;
        out->done = d < out->total ? d : (out->total ? out->total - 1 : 0);
    }
}

static const char *fs_word(int fs) {
    return fs == LS_FMT_EXFAT ? "exfat" : "fat32";
}

int sd_format_json(char *buf, size_t cap, const struct sd_format_status *f) {
    static const char *const st[] = {"idle", "running", "done", "error"};
    int n = snprintf(buf, cap,
                     "\"format\":{\"available\":%s,\"state\":\"%s\",\"phase\":\"%s\",\"done\":%lu,\"total\":%lu,"
                     "\"fs\":\"%s\",\"cluster\":%lu,\"error\":\"%s\"}",
                     ls_can_format() ? "true" : "false", st[f->state & 3], f->phase ? f->phase : "",
                     (unsigned long)f->done, (unsigned long)f->total, f->fs ? fs_word(f->fs) : "",
                     (unsigned long)f->cluster, f->error);
    if (n < 0 || (size_t)n >= cap) {
        return -1;
    }
    struct ls_card c;
    int m;
    if (ls_card_info(&c)) {
        const char *type = c.bytes > 32 * GIB ? "SDXC" : c.sdhc ? "SDHC" : "SDSC";
        int dfs = sd_default_fs(c.bytes);
        m = snprintf(buf + n, cap - (size_t)n,
                     ",\"card\":{\"bytes\":%llu,\"type\":\"%s\"", (unsigned long long)c.bytes, type);
        if (m > 0 && (size_t)(n + m) < cap) {
            n += m;
        }
        if (c.cid_ok) {
            m = snprintf(buf + n, cap - (size_t)n,
                         ",\"cid\":{\"mid\":%u,\"oem\":\"%.2s\",\"name\":\"%.5s\",\"rev\":\"%u.%u\","
                         "\"serial\":\"0x%08lx\",\"date\":\"%04u-%02u\"}",
                         c.mid, c.oem, c.name, c.rev_major, c.rev_minor, (unsigned long)c.serial, c.year, c.month);
            if (m > 0 && (size_t)(n + m) < cap) {
                n += m;
            }
        }
        m = snprintf(buf + n, cap - (size_t)n,
                     "},\"defaults\":{\"fs\":\"%s\",\"cluster\":{\"fat32\":%lu,\"exfat\":%lu},\"align\":%lu}",
                     fs_word(dfs), (unsigned long)sd_default_cluster(LS_FMT_FAT32, c.bytes),
                     (unsigned long)sd_default_cluster(LS_FMT_EXFAT, c.bytes),
                     (unsigned long)sd_default_align(c.bytes) * 512u);
    } else {
        m = snprintf(buf + n, cap - (size_t)n, ",\"card\":null");
    }
    if (m < 0 || (size_t)(n + m) >= cap) {
        return -1;
    }
    n += m;
    m = snprintf(buf + n, cap - (size_t)n,
                 ",\"options\":{\"fat32\":[4096,8192,16384,32768,65536],"
                 "\"exfat\":[4096,8192,16384,32768,65536,131072,262144,524288,1048576]}");
    if (m < 0 || (size_t)(n + m) >= cap) {
        return -1;
    }
    return n + m;
}

bool sd_format_start(const char *fs, uint32_t cluster, struct sd_reply *r) {
    if (!ls_can_format()) {
        set_reply(r, 501, 0, sd_card_state(), "not a card: the log folder cannot be formatted");
        return false;
    }
    sdw_lock();
    bool running = s_fmt.state == SDF_RUNNING;
    sdw_unlock();
    if (running) {
        set_reply(r, 409, 0, "formatting", "format already running");
        return false;
    }
    struct ls_card c;
    if (!sd_pub()->present) {
        set_reply(r, 503, 5, "none", "no card");
        return false;
    }
    if (!ls_card_info(&c)) {
        set_reply(r, 503, 2, "settling", "card not ready");
        return false;
    }
    int f = fs && strcmp(fs, "exfat") == 0 ? LS_FMT_EXFAT : fs && strcmp(fs, "fat32") == 0 ? LS_FMT_FAT32 : 0;
    if (!f) {
        set_reply(r, 400, 0, sd_card_state(), "fs: \"fat32\" or \"exfat\"");
        return false;
    }
    if (!cluster) {
        cluster = sd_default_cluster(f, c.bytes);
    }
    const char *why = NULL;
    if (!sd_cluster_ok(f, cluster, c.bytes, &why)) {
        set_reply(r, 400, 0, sd_card_state(), why);
        return false;
    }
    sdw_lock();
    memset(&s_fmt, 0, sizeof(s_fmt));
    s_fmt.state = SDF_RUNNING;
    s_fmt.phase = "starting";
    s_fmt.fs = f;
    s_fmt.cluster = cluster;
    s_fmt.total = format_total(f, cluster, c.bytes);
    sdw_unlock();
    sdw_kick();                    /* the worker sees SDF_RUNNING and runs it */
    return true;
}

static void fmt_phase(const char *phase) {
    sdw_lock();
    s_fmt.phase = phase;
    sdw_unlock();
}

static void fmt_end(int state, const char *err) {
    sdw_lock();
    s_fmt.state = state;
    s_fmt.phase = state == SDF_DONE ? "done" : "failed";
    if (state == SDF_DONE) {
        s_fmt.done = s_fmt.total;
    }
    snprintf(s_fmt.error, sizeof(s_fmt.error), "%s", err ? err : "");
    sdw_unlock();
}

void sd_format_run(void) {
    fmt_phase("closing files");
    /* No transfer can start (the gate says "formatting"); wait for any
     * running one to end, then let go of the card. */
    for (int i = 0; i < 200 && sd_xfer_active(); i++) {
        sdw_idle(10000);
    }
    sd_core_release_card();
    struct ls_card c;
    if (!ls_card_info(&c)) {
        fmt_end(SDF_ERROR, "card not ready");
        return;
    }
    sdw_lock();
    s_fmt_start = ls_sectors_written();
    s_fmt.phase = "writing";
    int fs = s_fmt.fs;
    uint32_t cluster = s_fmt.cluster;
    sdw_unlock();
    /* f_mkfs's work buffer: transfer slot 0's ring (no transfer runs while
     * formatting). */
    int r = ls_format(fs, cluster, sd_default_align(c.bytes), s_x[0].buf, SD_XFER_BUF);
    sdw_lock();
    s_fmt.done = ls_sectors_written() - s_fmt_start;
    sdw_unlock();
    if (r < 0) {
        fmt_end(SDF_ERROR, r == LS_EINVAL ? "that cluster size does not fit this card"
                           : r == LS_ENOMEDIA ? "card removed" : "write failed");
        return;
    }
    fmt_phase("mounting");
    /* By the clock, not by passes: a yield is one main-loop pass (often a
     * millisecond), and a mount may first wait out the 2 s settle. */
    uint64_t t0 = sd_port_now_us();
    while (ls_media() != LS_MEDIA_OK && sd_port_now_us() - t0 < 10000000u) {
        sd_core_media_tick();          /* mounts it (after the settle, if needed) */
        sdw_idle(50000);
    }
    if (ls_media() != LS_MEDIA_OK) {
        fmt_end(SDF_ERROR, "formatted, but it did not mount");
        return;
    }
    fmt_end(SDF_DONE, NULL);
}
