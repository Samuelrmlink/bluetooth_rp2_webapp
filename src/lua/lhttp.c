/*
 * http -- the Lua face of src/net/httpc.c.
 *
 *   local id, err = http.start{ method=, url=, headers={}, body=, to_file=,
 *                               max_body=16384, timeout_ms=30000, follow=3,
 *                               insecure=false }
 *   http.status(id)  -> { state=, status=, bytes=, total= }
 *   http.await(id, ms) -> result | nil, "timeout"     (yields the script)
 *   http.cancel(id)
 *
 * The client runs on lwIP callbacks, outside any Lua call, so nothing it
 * calls back into may touch the Lua API: an allocation failure there would
 * raise through lwIP. Response headers and an in-memory body therefore go
 * into plain blocks of the Lua arena and only become Lua values in
 * http.await, inside the script's own call.
 *
 * The client's own memory -- the request, its URL and header copies, the
 * TLS session with mbedTLS's record buffers, handshake state and the trusted
 * roots -- is the TLS pool (httpc.c), not the Lua arena. What becomes or
 * comes from a Lua value -- a request body, the response headers and an
 * in-memory body (up to 64 KB), a LittleFS file buffer -- goes into the
 * request's own scratch (lheap_scratch_*: pages from the top of the arena,
 * all given back when the request is released), never between blocks that
 * outlive it.
 *
 * to_file: a 2xx body streams into the file (LittleFS through the fs pool,
 * the SD card through storage-worker jobs with a 4 KiB staging block, the
 * download held back by TCP flow control while the card writes). A failed
 * download removes the partial file. A non-2xx response is not written: its
 * body comes back in memory like any other, so a 404 page never overwrites
 * the target.
 *
 * Requests belong to the script that started them and are cancelled when it
 * stops. A finished request keeps its result until http.await collects it;
 * one nobody collects is dropped 60 s after it finished if a slot is needed.
 */
#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"

#include "autoconf.h"
#include "fs/fs.h"
#include "lua/lfile_path.h"
#include "lua/lheap.h"
#include "lua/lhttp.h"
#include "lua/lrt.h"
#include "lua/lwait.h"
#include "net/httpc.h"
#include "pico/time.h"

#if CONFIG_SD
#include "sd/sd_jobs.h"
#include "sd/sd_store.h"
#endif

#define CHUNK        2048
#define HDR_MAX      8192          /* response header bytes kept */
#define BODY_DEFAULT 16384
#define BODY_CAP     (64 * 1024)
#define STALE_MS     60000

typedef struct chunk { struct chunk *next; size_t len; uint8_t d[CHUNK]; } chunk_t;

enum { F_NONE = 0, F_LFS, F_SD };
enum { SDW_OPENING = 1, SDW_READY, SDW_WRITING, SDW_CLOSING, SDW_REMOVING, SDW_FAILED };

typedef struct {
    bool     used;
    int      id;
    int8_t   owner;
    bool     done;               /* the result is final (file closed too) */
    bool     net_done;           /* httpc said done */
    httpc_err_t err;
    char     msg[64];
    int      status;
    uint64_t bytes;
    uint32_t done_ms;
    /* headers: "name\0value\0" pairs */
    char    *hdr;
    size_t   hdr_len, hdr_cap;
    /* in-memory body */
    chunk_t *head, *tail;
    size_t   mem_len;
    size_t   max_body;
    bool     truncated;
    bool     want_cancel;        /* truncated: stop the transfer */
    /* to_file */
    int      fvol;
    char     fpath[LFILE_PATH_MAX + 1];
    bool     to_file;            /* asked for, and the response is 2xx */
    bool     file_err;
    httpc_err_t file_errc;
    int      lfs_h;
    void    *lfs_mem;
    size_t   lfs_budget;       /* flash bytes this pass may still write */
#if CONFIG_SD
    int      sd_state, sd_file;
    uint32_t sd_token;         /* a posted job: open, write, close, remove */
    uint32_t fill_token;       /* a WRITE being filled in place (sdj_begin_write) */
    size_t   fill_len;         /* bytes in it */
    size_t   written;          /* the committed write's length */
    uint32_t fill_since;       /* when its first byte arrived */
    uint8_t *early;            /* body that came with the headers, before the file was open */
    size_t   early_len;
#endif
    lheap_scratch_t *sc;       /* where all of this request's memory is */
} hreq_t;

static hreq_t s_r[HTTPC_MAX];

/* From the top of the arena -- and, like Lua's own allocator, collect and
 * try again before giving up: memory a script has dropped is garbage until
 * the collector runs, and the collector only runs when LUA allocates. Without
 * this a request failed "out of memory" with 30 KB of the arena reclaimable
 * (mcusim). Never reached from inside Lua's allocator (lheap_realloc), so a
 * collection here is safe; finalizers that run (file handles) do not
 * allocate through it. */
static void *place(size_t n) {
    lheap_scratch_t *sc = lheap_scratch_current();
    return sc ? lheap_scratch_alloc(sc, n) : lheap_alloc_high(n);
}

static void *arena_alloc(size_t n) {
    void *p = place(n);
    if (!p) {
        lrt_collect();
        p = place(n);
    }
    if (!p) {
        printf("lhttp: %u B not available: arena %u B used, largest free %u B\n",
               (unsigned)n, (unsigned)lheap_used(), (unsigned)lheap_largest_free());
        return NULL;
    }
    memset(p, 0, n);
    return p;
}

static void arena_stat(size_t *used, size_t *largest) {
    *used = lheap_used();
    *largest = lheap_largest_free();
}

static void *scope_use(void *scope) { return lheap_scratch_use(scope); }

static void lhttp_mem_init(void) {
    static const httpc_alloc_fns fns = { arena_alloc, lheap_free_any, arena_stat, scope_use };
    httpc_set_alloc(&fns);
}

static uint32_t now_ms(void) { return to_ms_since_boot(get_absolute_time()); }
static uint32_t token_of(int id) { return 0x48540000u ^ (uint32_t)id; }

static hreq_t *by_id(int id) {
    for (int i = 0; i < HTTPC_MAX; i++) { if (s_r[i].used && s_r[i].id == id) { return &s_r[i]; } }
    return NULL;
}

static void free_body(hreq_t *h) {
    for (chunk_t *c = h->head; c; ) { chunk_t *n = c->next; lheap_free_any(c); c = n; }
    h->head = h->tail = NULL;
}

static void release(hreq_t *h) {
    free_body(h);
    lheap_free_any(h->hdr);
    h->hdr = NULL;
    if (h->lfs_h >= 0 && h->fvol == F_LFS) { fs_fclose(h->lfs_h); h->lfs_h = -1; lheap_free_any(h->lfs_mem); h->lfs_mem = NULL; }
#if CONFIG_SD
    if (h->sd_token) { sdj_abandon(h->sd_token); h->sd_token = 0; }
    if (h->fill_token) { sdj_abandon(h->fill_token); h->fill_token = 0; }
    if (h->sd_file >= 0) { sdj_close_async(h->sd_file); h->sd_file = -1; }
    lheap_free_any(h->early);
    h->early = NULL;
#endif
    lheap_scratch_end(h->sc);          /* and whatever else the request had */
    h->sc = NULL;
    h->used = false;
}

static void mark_done(hreq_t *h) {
    if (h->done) { return; }
    h->done = true;
    h->done_ms = now_ms();
    lwait_deliver(token_of(h->id), LWAIT_NO_LINK, 0);
}

/* ---- the sink (lwIP callback context: no Lua here) ------------------------ */

static void on_status(void *u, int status) {
    hreq_t *h = u;
    h->status = status;
    if (h->fvol == F_NONE || status < 200 || status > 299) { return; }
    h->to_file = true;
    if (h->fvol == F_LFS) {
        h->lfs_mem = arena_alloc(fs_file_mem());
        h->lfs_h = fs_fopen(h->fpath, FS_O_WRITE, h->lfs_mem);
        if (h->lfs_h < 0) {
            lheap_free_any(h->lfs_mem);
            h->lfs_mem = NULL;
            h->file_err = true;
            h->file_errc = HTTPC_EIO;
            snprintf(h->msg, sizeof h->msg, "cannot open the file (%d)", h->lfs_h);
        }
    }
#if CONFIG_SD
    else {
        /* The body is written straight into the SD job's buffer (the
         * borrowed transfer ring: no 4 KiB of arena, which a busy arena had
         * no hole for -- measured 37 KB free, largest 3.4 KB). Only the body
         * that arrives with the headers, while the card is still opening the
         * file, needs somewhere: at most one parser piece (httpc.h). */
        h->early = arena_alloc(512);
        h->early_len = 0;
        if (!h->early) {
            h->file_err = true; h->file_errc = HTTPC_ENOMEM;
            snprintf(h->msg, sizeof h->msg, "out of memory");
            h->sd_state = SDW_FAILED;
            return;
        }
        h->sd_state = SDW_OPENING;       /* posted by lhttp_tick */
    }
#endif
}

static void on_header(void *u, const char *name, const char *value) {
    hreq_t *h = u;
    size_t nl = strlen(name) + 1, vl = strlen(value) + 1;
    size_t need = h->hdr_len + nl + vl;
    if (need > HDR_MAX) { return; }
    if (need > h->hdr_cap) {
        /* Grown as headers arrive (1 KiB, doubling): one fixed 8 KiB block
         * was a contiguous hole a busy arena often did not have. */
        size_t cap = h->hdr_cap ? h->hdr_cap : 1024;
        while (cap < need) { cap *= 2; }
        if (cap > HDR_MAX) { cap = HDR_MAX; }
        char *n = arena_alloc(cap);
        if (!n) { return; }                /* headers are dropped, not the response */
        if (h->hdr) { memcpy(n, h->hdr, h->hdr_len); lheap_free_any(h->hdr); }
        h->hdr = n;
        h->hdr_cap = cap;
    }
    memcpy(h->hdr + h->hdr_len, name, nl);
    memcpy(h->hdr + h->hdr_len + nl, value, vl);
    h->hdr_len += nl + vl;
}

static size_t room(void *u) {
    hreq_t *h = u;
    if (h->want_cancel || h->file_err) { return 0; }
    if (!h->to_file) { return (size_t)-1; }
    /* One flash sector a pass: a LittleFS write may erase one (~50-400 ms),
     * and the pump can move 32 KB in one go -- a 237 KB download held the
     * loop 4.3 s at a time (mcusim). TCP's window holds the rest back. */
    if (h->fvol == F_LFS) { return h->lfs_budget; }
#if CONFIG_SD
    if (h->sd_state == SDW_OPENING) { return h->early ? 512 - h->early_len : 0; }
    if (h->sd_state != SDW_READY) { return 0; }
    if (!h->fill_token) {
        if (!sdj_begin_write(h->sd_file, &h->fill_token)) { return 0; }   /* the card's job is busy */
        h->fill_len = 0;
    }
    return SDJ_BUF - h->fill_len;
#else
    return 0;
#endif
}

static void on_body(void *u, const uint8_t *d, size_t n) {
    hreq_t *h = u;
    h->bytes += n;
    if (h->to_file) {
        if (h->file_err) { return; }
        if (h->fvol == F_LFS) {
            int w = fs_fwrite(h->lfs_h, d, n);
            h->lfs_budget = n < h->lfs_budget ? h->lfs_budget - n : 0;
            if (w < 0) {
                h->file_err = true;
                h->file_errc = w == FS_ERR_TOOLARGE ? HTTPC_ETOOLARGE : HTTPC_EIO;
                snprintf(h->msg, sizeof h->msg, w == FS_ERR_TOOLARGE ? "file over the 1 MiB LittleFS limit"
                                                                     : "write failed (%d)", w);
            }
            return;
        }
#if CONFIG_SD
        /* room() bounds what arrives, except the piece that ends the
         * headers, which lands in `early`. Anything that does not fit is a
         * failure, never a silently shorter file. */
        uint8_t *dst;
        size_t cap, *len;
        if (h->fill_token) { dst = sdj_buf(); cap = SDJ_BUF; len = &h->fill_len; }
        else if (h->early) { dst = h->early; cap = 512; len = &h->early_len; }
        else { h->file_err = true; h->file_errc = HTTPC_EIO; return; }
        if (*len == 0) { h->fill_since = now_ms(); }
        size_t take = n <= cap - *len ? n : cap - *len;
        memcpy(dst + *len, d, take);
        *len += take;
        if (take < n) {
            h->file_err = true; h->file_errc = HTTPC_EIO;
            snprintf(h->msg, sizeof h->msg, "buffer overrun");
        }
#endif
        return;
    }
    size_t can = h->max_body > h->mem_len ? h->max_body - h->mem_len : 0;
    if (n > can) { n = can; h->truncated = true; h->want_cancel = true; }
    while (n) {
        if (!h->tail || h->tail->len == CHUNK) {
            chunk_t *c = arena_alloc(sizeof *c);
            if (!c) {
                h->file_err = true;
                h->file_errc = HTTPC_ENOMEM;
                snprintf(h->msg, sizeof h->msg, "out of memory for the body");
                return;
            }
            c->next = NULL;
            c->len = 0;
            if (h->tail) { h->tail->next = c; } else { h->head = c; }
            h->tail = c;
        }
        size_t take = CHUNK - h->tail->len;
        if (take > n) { take = n; }
        memcpy(h->tail->d + h->tail->len, d, take);
        h->tail->len += take;
        h->mem_len += take;
        d += take;
        n -= take;
    }
}

static void on_done(void *u, httpc_err_t err, const char *msg) {
    hreq_t *h = u;
    h->net_done = true;
    if (err != HTTPC_OK) {
        h->err = err;
        snprintf(h->msg, sizeof h->msg, "%s", msg ? msg : httpc_err_name(err));
    }
}

static const httpc_sink_t SINK = { on_status, on_header, room, on_body, on_done };

/* ---- completion, from the tick (main loop, outside Lua) --------------------- */

static void fail_file(hreq_t *h) {
    if (h->err == HTTPC_OK) { h->err = h->file_errc ? h->file_errc : HTTPC_EIO; }
}

static void finish_lfs(hreq_t *h) {
    if (h->lfs_h >= 0) {
        int r = fs_fclose(h->lfs_h);
        h->lfs_h = -1;
        lheap_free_any(h->lfs_mem);
        h->lfs_mem = NULL;
        if (r < 0 && h->err == HTTPC_OK) { h->err = HTTPC_EIO; snprintf(h->msg, sizeof h->msg, "close failed"); }
    }
    if (h->file_err) { fail_file(h); }
    if (h->err != HTTPC_OK && h->to_file) { fs_delete(h->fpath); }   /* no half files */
    mark_done(h);
}

#if CONFIG_SD
/* The SD side is a little state machine over jobs: open, write each filled
 * buffer, close, and on failure remove. */
#define FILL_IDLE_MS 200          /* a part-filled buffer goes to the card after this */

static void sd_step(hreq_t *h) {
    if (h->sd_token) {
        sdj_t r;
        enum sdj_state st = sdj_poll(h->sd_token, &r);
        if (st == SDJ_POSTED) { return; }
        sdj_finish(h->sd_token);
        h->sd_token = 0;
        int rc = st == SDJ_DONE ? r.rc : LS_EIO;
        switch (h->sd_state) {
            case SDW_OPENING:
                if (rc < 0) {
                    h->file_err = true; h->file_errc = HTTPC_EIO;
                    snprintf(h->msg, sizeof h->msg, "cannot open the file on the card (%d)", rc);
                    h->sd_state = SDW_FAILED;
                } else {
                    h->sd_file = rc;
                    h->sd_state = SDW_READY;
                }
                break;
            case SDW_WRITING:
                if (rc < 0 || (size_t)rc != h->written) {
                    h->file_err = true;
                    h->file_errc = rc == LS_ENOSPC ? HTTPC_ETOOLARGE : HTTPC_EIO;
                    snprintf(h->msg, sizeof h->msg, rc == LS_ENOSPC ? "the card is full" : "write failed (%d)", rc);
                    h->sd_state = SDW_FAILED;
                } else {
                    h->sd_state = SDW_READY;
                }
                break;
            case SDW_CLOSING:
                h->sd_file = -1;
                if (rc < 0 && h->err == HTTPC_OK) { h->err = HTTPC_EIO; snprintf(h->msg, sizeof h->msg, "close failed"); }
                if (h->err != HTTPC_OK) { h->sd_state = SDW_FAILED; break; }   /* closed; now remove it, below */
                mark_done(h);
                return;
            case SDW_REMOVING:
                mark_done(h);
                return;
        }
    }
    sdj_t q;
    memset(&q, 0, sizeof q);
    if (h->sd_state == SDW_OPENING) {
        q.op = SDJ_OPEN; q.mode = LS_WR;
        snprintf(q.path, sizeof q.path, "%s", h->fpath);
        sdj_post(&q, &h->sd_token);
        return;
    }
    if (h->sd_state == SDW_FAILED && h->fill_token) { sdj_abandon(h->fill_token); h->fill_token = 0; }
    if (h->sd_state == SDW_READY && h->early_len) {
        /* What came with the headers goes first. */
        if (!h->fill_token) {
            if (!sdj_begin_write(h->sd_file, &h->fill_token)) { return; }
            h->fill_len = 0;
        }
        memcpy(sdj_buf() + h->fill_len, h->early, h->early_len);
        h->fill_len += h->early_len;
        h->early_len = 0;
        lheap_free_any(h->early);
        h->early = NULL;
    }
    if (h->sd_state == SDW_READY && h->fill_token) {
        bool full = h->fill_len >= SDJ_BUF;
        bool idle = h->fill_len && (int32_t)(now_ms() - h->fill_since) >= FILL_IDLE_MS;
        if (h->fill_len && (full || h->net_done || idle)) {
            sdj_commit(h->fill_token, (uint32_t)h->fill_len);
            h->sd_token = h->fill_token;
            h->fill_token = 0;
            h->written = h->fill_len;
            h->fill_len = 0;
            h->sd_state = SDW_WRITING;
            return;
        }
        if (!h->fill_len && h->net_done) { sdj_abandon(h->fill_token); h->fill_token = 0; }
        else { return; }
    }
    bool end = h->net_done && (h->sd_state == SDW_FAILED ||
                               (h->sd_state == SDW_READY && !h->fill_token && !h->early_len));
    if (!end) { return; }
    if (h->file_err) { fail_file(h); }
    if (h->sd_file >= 0) {
        q.op = SDJ_CLOSE; q.file = h->sd_file;
        if (sdj_post(&q, &h->sd_token)) { h->sd_state = SDW_CLOSING; }
        return;
    }
    if (h->err != HTTPC_OK && h->sd_state != SDW_REMOVING) {
        q.op = SDJ_REMOVE;
        snprintf(q.path, sizeof q.path, "%s", h->fpath);
        if (sdj_post(&q, &h->sd_token)) { h->sd_state = SDW_REMOVING; }
        return;
    }
    mark_done(h);
}
#endif

#define LFS_PASS_BUDGET 4096

void lhttp_tick(void) {
    for (int i = 0; i < HTTPC_MAX; i++) { s_r[i].lfs_budget = LFS_PASS_BUDGET; }
    httpc_tick();
    for (int i = 0; i < HTTPC_MAX; i++) {
        hreq_t *h = &s_r[i];
        if (!h->used || h->done) { continue; }
        if (!h->net_done && (h->want_cancel || h->file_err)) {
            /* Truncated in memory, or the file failed: the rest of the body
             * is not wanted. */
            httpc_cancel(h->id);
            h->net_done = true;
        }
        if (!h->net_done) {
#if CONFIG_SD
            if (h->to_file && h->fvol == F_SD) { sd_step(h); }
#endif
            continue;
        }
        if (h->file_err) { fail_file(h); }
        if (!h->to_file || h->fvol == F_NONE) { mark_done(h); continue; }
        if (h->fvol == F_LFS) { finish_lfs(h); continue; }
#if CONFIG_SD
        sd_step(h);
#endif
    }
}

/* ---- Lua -------------------------------------------------------------------- */

static int field_int(lua_State *L, int t, const char *k, lua_Integer dflt) {
    lua_getfield(L, t, k);
    lua_Integer v = lua_isnil(L, -1) ? dflt : luaL_checkinteger(L, -1);
    lua_pop(L, 1);
    return (int)v;
}

static int fail(lua_State *L, const char *code) {
    lua_pushnil(L);
    lua_pushstring(L, code);
    return 2;
}

/* "K: v\r\n" for every string key of the table at idx, into a userdata
 * pushed on the stack; returns its text (NUL-terminated) or NULL if none. */
static const char *headers_text(lua_State *L, int idx) {
    if (!lua_istable(L, idx)) { lua_pushnil(L); return NULL; }
    size_t total = 1;
    for (int pass = 0; pass < 2; pass++) {
        char *dst = NULL;
        size_t at = 0;
        if (pass == 1) { dst = (char *)lua_newuserdatauv(L, total, 0); }
        lua_pushnil(L);
        while (lua_next(L, idx)) {
            if (lua_type(L, -2) == LUA_TSTRING && (lua_type(L, -1) == LUA_TSTRING || lua_type(L, -1) == LUA_TNUMBER)) {
                size_t kl = 0, vl = 0;
                const char *k = lua_tolstring(L, -2, &kl);
                lua_pushvalue(L, -1);                  /* tolstring on a copy: never convert the key's value in place */
                const char *v = lua_tolstring(L, -1, &vl);
                if (strpbrk(k, "\r\n:") || strlen(k) != kl || strpbrk(v, "\r\n") || strlen(v) != vl || !kl) {
                    luaL_error(L, "http.start: bad header %s", k);
                }
                if (pass == 0) { total += kl + vl + 4; }
                else { at += (size_t)snprintf(dst + at, total - at, "%s: %s\r\n", k, v); }
                lua_pop(L, 1);
            }
            lua_pop(L, 1);
        }
        if (pass == 1) { dst[at] = 0; return dst; }
    }
    return NULL;
}

static int l_start(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_settop(L, 1);
    lua_getfield(L, 1, "url");                                        /* 2 */
    const char *url = luaL_checkstring(L, 2);
    lua_getfield(L, 1, "method");                                     /* 3 */
    const char *method = luaL_optstring(L, 3, "GET");
    for (const char *m = method; *m; m++) {
        if (*m < 'A' || *m > 'Z') { return luaL_error(L, "http.start: method must be upper-case letters"); }
    }
    lua_getfield(L, 1, "body");                                       /* 4 */
    size_t body_len = 0;
    const char *body = lua_isnil(L, 4) ? NULL : luaL_checklstring(L, 4, &body_len);
    lua_getfield(L, 1, "headers");                                    /* 5 */
    const char *hdrs = headers_text(L, 5);                            /* 6 */
    lua_getfield(L, 1, "to_file");                                    /* 7 */
    int fvol = F_NONE;
    char fpath[LFILE_PATH_MAX + 1] = "";
    if (!lua_isnil(L, 7)) {
        size_t pl = 0;
        const char *p = luaL_checklstring(L, 7, &pl);
        int v = lfile_parse_path(p, pl, fpath, sizeof fpath);
        if (!v || !fpath[0]) { return fail(L, "badpath"); }
#if !CONFIG_SD
        if (v == LFILE_SD) { return fail(L, "nomedia"); }
#endif
        fvol = v == LFILE_LFS ? F_LFS : F_SD;
    }
    int max_body = field_int(L, 1, "max_body", BODY_DEFAULT);
    if (max_body < 0) { max_body = 0; }
    if (max_body > BODY_CAP) { max_body = BODY_CAP; }
    int timeout = field_int(L, 1, "timeout_ms", 30000);
    if (timeout <= 0) { timeout = 30000; }
    int follow = field_int(L, 1, "follow", 3);
    if (follow < 0) { follow = 0; }
    if (follow > 10) { follow = 10; }
    lua_getfield(L, 1, "insecure");
    bool insecure = lua_toboolean(L, -1);
    lua_pop(L, 1);

    /* A slot: free, or a finished result nobody collected for a minute. */
    hreq_t *h = NULL;
    for (int i = 0; i < HTTPC_MAX && !h; i++) { if (!s_r[i].used) { h = &s_r[i]; } }
    for (int i = 0; i < HTTPC_MAX && !h; i++) {
        if (s_r[i].done && (int32_t)(now_ms() - s_r[i].done_ms) > STALE_MS) {
            httpc_cancel(s_r[i].id);
            release(&s_r[i]);
            h = &s_r[i];
        }
    }
    if (!h) { return fail(L, "busy"); }

    lhttp_mem_init();
    memset(h, 0, sizeof *h);
    h->lfs_h = -1;
#if CONFIG_SD
    h->sd_file = -1;
#endif
    h->fvol = fvol;
    snprintf(h->fpath, sizeof h->fpath, "%s", fpath);
    h->max_body = (size_t)max_body;
    h->owner = (int8_t)lrt_current_slot();
    h->sc = lheap_scratch_begin(0);
    if (!h->sc) { lrt_collect(); h->sc = lheap_scratch_begin(0); }
    if (!h->sc) { return fail(L, "nomem"); }
    httpc_opts_t o = {
        .method = method, .url = url, .headers = hdrs, .body = (const uint8_t *)body, .body_len = body_len,
        .timeout_ms = (uint32_t)timeout, .follow = (uint8_t)follow, .insecure = insecure, .scope = h->sc,
    };
    httpc_err_t err = HTTPC_OK;
    h->used = true;
    lheap_scratch_t *prev = lheap_scratch_use(h->sc);
    int id = httpc_start(&o, &SINK, h, &err);
    lheap_scratch_use(prev);
    if (id < 0) {
        lheap_scratch_end(h->sc);
        h->sc = NULL;
        h->used = false;
        return fail(L, err == HTTPC_EPROTO ? "badurl" : err == HTTPC_ETLS ? "tls" : err == HTTPC_ENOMEM ? "nomem" : "busy");
    }
    h->id = id;
    lua_pushinteger(L, id);
    return 1;
}

static int l_status(lua_State *L) {
    int id = (int)luaL_checkinteger(L, 1);
    hreq_t *h = by_id(id);
    if (!h) { return fail(L, "badid"); }
    lua_createtable(L, 0, 4);
    httpc_state_t st;
    uint64_t bytes = 0;
    int64_t total = -1;
    int status = 0;
    const char *state;
    if (h->done) {
        state = h->err == HTTPC_OK ? "done" : "error";
        bytes = h->bytes;
        status = h->status;
    } else if (httpc_info(id, &st, &bytes, &total, &status)) {
        state = (st == HTTPC_DONE || st == HTTPC_ERROR) ? "recv" : httpc_state_name(st);  /* a file still closing */
    } else {
        state = "recv";
        bytes = h->bytes;
        status = h->status;
    }
    lua_pushstring(L, state);                 lua_setfield(L, -2, "state");
    lua_pushinteger(L, status);               lua_setfield(L, -2, "status");
    lua_pushinteger(L, (lua_Integer)bytes);   lua_setfield(L, -2, "bytes");
    if (total >= 0) { lua_pushinteger(L, (lua_Integer)total); lua_setfield(L, -2, "total"); }
    return 1;
}

/* The finished request as a Lua table; frees it. */
static int result(lua_State *L, hreq_t *h) {
    int id = h->id;
    if (h->err != HTTPC_OK) {
        lua_createtable(L, 0, 3);
        lua_pushstring(L, httpc_err_name(h->err)); lua_setfield(L, -2, "error");
        lua_pushstring(L, h->msg[0] ? h->msg : httpc_err_name(h->err)); lua_setfield(L, -2, "message");
        if (h->status) { lua_pushinteger(L, h->status); lua_setfield(L, -2, "status"); }
    } else {
        lua_createtable(L, 0, 5);
        lua_pushinteger(L, h->status);              lua_setfield(L, -2, "status");
        lua_pushinteger(L, (lua_Integer)h->bytes);  lua_setfield(L, -2, "bytes");
        lua_pushboolean(L, h->truncated);           lua_setfield(L, -2, "truncated");
        lua_newtable(L);
        for (size_t at = 0; h->hdr && at < h->hdr_len; ) {
            const char *n = h->hdr + at;
            const char *v = n + strlen(n) + 1;
            at += strlen(n) + strlen(v) + 2;
            /* A repeated header (set-cookie, via): joined, as RFC 9110 allows. */
            lua_getfield(L, -1, n);
            if (lua_isstring(L, -1)) {
                lua_pushfstring(L, "%s, %s", lua_tostring(L, -1), v);
                lua_remove(L, -2);
            } else {
                lua_pop(L, 1);
                lua_pushstring(L, v);
            }
            lua_setfield(L, -2, n);
        }
        lua_setfield(L, -2, "headers");
        if (!h->to_file) {
            luaL_Buffer b;
            luaL_buffinit(L, &b);
            for (chunk_t *c = h->head; c; c = c->next) { luaL_addlstring(&b, (const char *)c->d, c->len); }
            luaL_pushresult(&b);
            lua_setfield(L, -2, "body");
        }
    }
    httpc_cancel(id);
    release(h);
    lrt_yield_soon();
    return 1;
}

static int await_k(lua_State *L, int status, lua_KContext ctx) {
    (void)status;
    hreq_t *h = by_id((int)ctx);
    if (!h) { return fail(L, "cancelled"); }
    if (h->done) { return result(L, h); }
    return fail(L, lrt_wake_reason() == LWAIT_CANCELLED ? "cancelled" : "timeout");
}

static int l_await(lua_State *L) {
    int id = (int)luaL_checkinteger(L, 1);
    lua_Integer ms = luaL_optinteger(L, 2, (lua_Integer)LWAIT_MAX_MS);
    hreq_t *h = by_id(id);
    if (!h) { return fail(L, "badid"); }
    if (h->done) { return result(L, h); }
    if (ms <= 0) { return fail(L, "timeout"); }
    if (lrt_current_slot() < 0 || !lua_isyieldable(L)) {
        return luaL_error(L, "http.await waits, so it may only be called from a script's own code");
    }
    lwait_reason_t r = lrt_block_current(token_of(id), LWAIT_NO_LINK, (uint32_t)ms);
    if (r != LWAIT_PENDING) { return fail(L, lwait_reason_name(r)); }
    return lua_yieldk(L, 0, (lua_KContext)id, await_k);
}

static int l_cancel(lua_State *L) {
    int id = (int)luaL_checkinteger(L, 1);
    hreq_t *h = by_id(id);
    if (!h) { lua_pushboolean(L, 0); return 1; }
    httpc_cancel(id);
    if (h->to_file && !h->done) {
        if (h->fvol == F_LFS) {
            if (h->lfs_h >= 0) { fs_fclose(h->lfs_h); h->lfs_h = -1; lheap_free_any(h->lfs_mem); h->lfs_mem = NULL; }
            fs_delete(h->fpath);
        }
        /* SD: the file is closed in the background; the partial stays (no
         * job can be waited for here). */
    }
    release(h);
    lua_pushboolean(L, 1);
    return 1;
}

/* http.reserve(on) -> true. Kept for scripts written when the TLS record
 * buffers lived in the Lua arena and could be reserved and given back; they
 * are in the TLS pool now (httpc.c), which is never the scripts', so there
 * is nothing to reserve. */
static int l_reserve(lua_State *L) {
    lua_pushboolean(L, 1);
    return 1;
}

static int l_stats(lua_State *L) {
    httpc_stats_t s;
    httpc_stats(&s);
    lua_createtable(L, 0, 10);
    lua_pushinteger(L, s.requests);       lua_setfield(L, -2, "requests");
    lua_pushinteger(L, s.failures);       lua_setfield(L, -2, "failures");
    lua_pushinteger(L, s.tls_now);        lua_setfield(L, -2, "tls_sessions");
    /* The pool the TLS session and the requests' bookkeeping live in. */
    lua_pushinteger(L, s.pool_used);      lua_setfield(L, -2, "tls_bytes");
    lua_pushinteger(L, s.pool_peak);      lua_setfield(L, -2, "tls_peak_bytes");
    lua_pushinteger(L, s.pool_size);      lua_setfield(L, -2, "tls_pool");
    lua_pushinteger(L, s.pool_largest_free); lua_setfield(L, -2, "tls_pool_largest_free");
    lua_pushinteger(L, s.pool_fails);     lua_setfield(L, -2, "tls_pool_fails");
    lua_pushinteger(L, s.tls_waits);      lua_setfield(L, -2, "tls_waits");
    return 1;
}

void lhttp_open(lua_State *L) {
    static const luaL_Reg fns[] = {
        { "start", l_start }, { "status", l_status }, { "await", l_await },
        { "cancel", l_cancel }, { "stats", l_stats }, { "reserve", l_reserve }, { NULL, NULL } };
    lua_newtable(L);
    luaL_setfuncs(L, fns, 0);
    lua_setglobal(L, "http");
    lhttp_mem_init();
}

void lhttp_release(int slot) {
    for (int i = 0; i < HTTPC_MAX; i++) {
        hreq_t *h = &s_r[i];
        if (!h->used || h->owner != slot) { continue; }
        httpc_cancel(h->id);
        if (h->to_file && !h->done && h->fvol == F_LFS) {
            if (h->lfs_h >= 0) { fs_fclose(h->lfs_h); h->lfs_h = -1; lheap_free_any(h->lfs_mem); h->lfs_mem = NULL; }
            fs_delete(h->fpath);
        }
        release(h);
    }
}

void lhttp_reset(void) {
    for (int i = 0; i < HTTPC_MAX; i++) { if (s_r[i].used) { httpc_cancel(s_r[i].id); release(&s_r[i]); } }
    httpc_reset();
}
