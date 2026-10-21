/*
 * The net group: the firmware's HTTP(S) client (src/net/httpc.h) for native
 * modules -- start, status, cancel; the body into a module buffer or into a
 * LittleFS file; the completion callback from the main loop.
 *
 * Memory: httpc has ONE allocator for everyone. In a Lua build it is the Lua
 * arena's top (what lhttp sets, copied below so a native request does not
 * fall back to the C heap, which a TLS session -- ~35 KB -- would exhaust and
 * pico_malloc would panic on). Merge note: when feature/lua-net-fs's
 * dedicated TLS pool lands, point this at that allocator instead. Without
 * Lua, https:// is refused for the same reason.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "autoconf.h"
#include "pex/pex_gw.h"
#include "fs/fs.h"
#include "lua/lfile_path.h"
#include "net/httpc.h"
#if CONFIG_LUA_ENABLE
#include "lua/lheap.h"
#include "lua/lrt.h"
#endif

#define RING 2048u

typedef struct {
    bool used;
    pex_mod_t *mod;
    int id;
    pex_http_cb cb;
    void *arg;
    uint8_t *buf;
    size_t cap;
    pex_http_res_t res;
    /* to_file */
    int fh;
    void *fmem;
    char fpath[LFILE_PATH_MAX + 1];
    uint8_t *ring;
    uint32_t rhead, rtail;          /* free-running */
    bool file_err;
} hslot_t;
static hslot_t s_h[HTTPC_MAX];

#if CONFIG_LUA_ENABLE
/* httpc's allocator is one setting for every user (lhttp sets the same kind
 * at each Lua request): what one user's functions allocate, another's may
 * free. So these mirror lhttp's -- lheap_free_any frees arena and scratch
 * blocks, the scope is lheap's scratch -- except that a module's request
 * body comes from the TLS pool (httpc frees pool blocks itself, whatever is
 * set), so a native app's request costs the Lua arena nothing. */
static void *arena_alloc(size_t n) {
    void *p = lheap_pool_alloc(n ? n : 1);
    if (!p) { p = lheap_alloc_high(n); }
    if (!p) { lrt_collect(); p = lheap_alloc_high(n); }
    if (p) { memset(p, 0, n); }
    return p;
}
static void *scope_use(void *scope) { return lheap_scratch_use(scope); }
static void arena_stat(size_t *used, size_t *largest) {
    *used = lheap_used();
    *largest = lheap_largest_free();
}
#endif

static void on_status(void *u, int status) { ((hslot_t *)u)->res.status = status; }
static void on_header(void *u, const char *n, const char *v) {
    hslot_t *h = u;
    if (strcasecmp(n, "content-length") == 0) { h->res.total = (int64_t)strtoll(v, NULL, 10); }
}
static size_t room(void *u) {
    hslot_t *h = u;
    if (h->ring) { return RING - (h->rhead - h->rtail); }
    return 4096;                     /* a buffer: take everything, keep what fits */
}
static void on_body(void *u, const uint8_t *d, size_t n) {
    hslot_t *h = u;
    h->res.bytes += (uint32_t)n;
    if (h->ring) {
        for (size_t i = 0; i < n && h->rhead - h->rtail < RING; i++) { h->ring[h->rhead++ % RING] = d[i]; }
        return;
    }
    size_t have = h->res.bytes - n;
    if (have < h->cap) {
        size_t k = n < h->cap - have ? n : h->cap - have;
        memcpy(h->buf + have, d, k);
    }
    if (h->res.bytes > h->cap) { h->res.truncated = 1; }
}
static void on_done(void *u, httpc_err_t err, const char *msg) {
    hslot_t *h = u;
    h->res.err = (int)err;
    snprintf(h->res.msg, sizeof h->res.msg, "%s", msg ? msg : (err ? "error" : "ok"));
    h->res.done = 1;
}
static const httpc_sink_t SINK = { on_status, on_header, room, on_body, on_done };

static void release(hslot_t *h) {
    if (h->id > 0) { httpc_cancel(h->id); }
    if (h->fmem) {
        if (h->fh >= 0) { fs_fclose(h->fh); }
        pex_free(h->fmem);
    }
    if (h->ring) { pex_free(h->ring); }
    memset(h, 0, sizeof *h);
}

int pex_http_start(const pex_http_req_t *q, pex_http_cb done, void *arg) {
    if (!q || !q->url) { return PEX_EINVAL; }
    if (!q->to_file && (!q->buf || !q->buf_cap)) { return PEX_EINVAL; }
#if !CONFIG_LUA_ENABLE
    if (strncmp(q->url, "https:", 6) == 0) {
        pex_seterr(PEX_ENOTSUP, "https needs the TLS memory pool (Lua builds, for now)");
        return PEX_ENOTSUP;
    }
#endif
    hslot_t *h = NULL;
    for (int i = 0; i < HTTPC_MAX; i++) { if (!s_h[i].used) { h = &s_h[i]; break; } }
    if (!h) { return PEX_EBUSY; }
    memset(h, 0, sizeof *h);
    h->fh = -1;
    if (q->to_file) {
        if (lfile_parse_path(q->to_file, strlen(q->to_file), h->fpath, sizeof h->fpath) != LFILE_LFS || !h->fpath[0]) {
            pex_seterr(PEX_EINVAL, "to_file must be lfs:/... in this version");
            return PEX_EINVAL;
        }
        h->ring = pex_alloc_long(RING);
        h->fmem = pex_alloc_long(fs_file_mem());
        if (!h->ring || !h->fmem) {
            if (h->ring) { pex_free(h->ring); }
            if (h->fmem) { pex_free(h->fmem); }
            return PEX_ENOMEM;
        }
        h->fh = fs_fopen(h->fpath, FS_O_WRITE, h->fmem);
        if (h->fh < 0) { pex_free(h->ring); pex_free(h->fmem); memset(h, 0, sizeof *h); return PEX_EIO; }
    }
    h->used = true;
    h->mod = pex_self();
    h->cb = done;
    h->arg = arg;
    h->buf = q->buf;
    h->cap = q->buf_cap;
    h->res.total = -1;
#if CONFIG_LUA_ENABLE
    static const httpc_alloc_fns fns = { arena_alloc, lheap_free_any, arena_stat, scope_use };
    httpc_set_alloc(&fns);
#endif
    httpc_opts_t o = { .method = q->method ? q->method : "GET", .url = q->url, .headers = q->headers,
                       .body = q->body, .body_len = q->body_len, .timeout_ms = q->timeout_ms,
                       .follow = q->follow, .insecure = q->insecure != 0 };
    httpc_err_t err = HTTPC_OK;
    int id = httpc_start(&o, &SINK, h, &err);
    if (id < 0) {
        pex_seterr(PEX_EIO, "http start failed (%d)", (int)err);
        if (h->fmem) { fs_fclose(h->fh); fs_delete(h->fpath); }
        h->id = 0;
        release(h);
        return err == HTTPC_ENOMEM ? PEX_ENOMEM : PEX_EIO;
    }
    h->id = id;
    return (int)(h - s_h) + 1;
}

static hslot_t *slot(int id) {
    if (id < 1 || id > HTTPC_MAX || !s_h[id - 1].used) { return NULL; }
    return &s_h[id - 1];
}

int pex_http_status(int id, pex_http_res_t *out) {
    hslot_t *h = slot(id);
    if (!h || !out) { return PEX_ENOENT; }
    *out = h->res;
    return PEX_OK;
}

int pex_http_cancel(int id) {
    hslot_t *h = slot(id);
    if (!h || h->mod != pex_self()) { return PEX_ENOENT; }
    if (h->fmem && h->fh >= 0) { fs_fclose(h->fh); h->fh = -1; fs_delete(h->fpath); }
    release(h);
    return PEX_OK;
}

void pex_gw_net_unload(pex_mod_t *m) {
    for (int i = 0; i < HTTPC_MAX; i++) {
        if (s_h[i].used && s_h[i].mod == m) {
            if (s_h[i].fmem && s_h[i].fh >= 0) { fs_fclose(s_h[i].fh); s_h[i].fh = -1; fs_delete(s_h[i].fpath); }
            s_h[i].fmem = NULL;          /* the module's blocks go with it */
            s_h[i].ring = NULL;
            if (s_h[i].id > 0) { httpc_cancel(s_h[i].id); }
            memset(&s_h[i], 0, sizeof s_h[i]);
        }
    }
}

void pex_gw_net_poll(void) {
    bool any = false;
    for (int i = 0; i < HTTPC_MAX; i++) {
        hslot_t *h = &s_h[i];
        if (!h->used) { continue; }
        any = true;
        /* the request was dropped underneath (a Lua VM restart resets httpc) */
        httpc_state_t st;
        uint64_t by;
        int64_t tot;
        int status;
        if (!h->res.done && !httpc_info(h->id, &st, &by, &tot, &status)) {
            h->res.done = 1;
            h->res.err = HTTPC_ECANCELLED;
            snprintf(h->res.msg, sizeof h->res.msg, "request dropped (client reset)");
        }
        if (h->ring && h->rhead != h->rtail) {
            uint8_t tmp[256];
            uint32_t budget = 4096;
            while (h->rhead != h->rtail && budget) {
                uint32_t k = 0;
                while (k < sizeof tmp && h->rtail + k != h->rhead) { tmp[k] = h->ring[(h->rtail + k) % RING]; k++; }
                if (h->res.status < 200 || h->res.status > 299) { h->rtail += k; continue; }   /* only 2xx is written */
                if (!h->file_err && fs_fwrite(h->fh, tmp, k) != (int)k) { h->file_err = true; }
                h->rtail += k;
                budget = budget > k ? budget - k : 0;
            }
        }
        if (h->res.done && (!h->ring || h->rhead == h->rtail)) {
            if (h->fmem) {
                fs_fclose(h->fh);
                h->fh = -1;
                bool ok = !h->res.err && !h->file_err && h->res.status >= 200 && h->res.status <= 299;
                if (!ok) { fs_delete(h->fpath); }
                if (h->file_err) { h->res.err = HTTPC_EIO; snprintf(h->res.msg, sizeof h->res.msg, "file write failed"); }
            }
            pex_http_res_t res = h->res;
            pex_mod_t *m = h->mod;
            pex_http_cb cb = h->cb;
            void *arg = h->arg;
            release(h);
            if (cb && m) { pex_call(m, (uint32_t)(uintptr_t)cb, (uint32_t)(uintptr_t)arg, (uint32_t)(uintptr_t)&res, 0, 0); }
        }
    }
    if (any) { httpc_tick(); }     /* lrt ticks it too; a second tick per pass is harmless */
}
