/*
 * The net group: the firmware's HTTP(S) client (src/net/httpc.h) for native
 * modules -- start, status, cancel; the body into a module buffer, a
 * LittleFS file (lfs:/), or a file on the SD card (sd:/); the completion
 * callback from the main loop.
 *
 * A to_file body lands in a 2 KiB ring from the module's pool first; room()
 * holds TCP back while it is full. The poll drains it: into the LittleFS file
 * directly, or, for the card, into the SD worker's 4 KiB transfer buffer,
 * which goes to the card as one write job when full, idle, or at the end.
 * A card download is written to "<path>.part" and renamed over the path only
 * once the whole body is on the card, so a failed or cancelled one never
 * leaves a short file under the real name.
 *
 * Memory: httpc has ONE allocator for everyone. In a Lua build it is the Lua
 * arena's top (what lhttp sets, copied below so a native request does not
 * fall back to the C heap, which a TLS session -- ~35 KB -- would exhaust and
 * pico_malloc would panic on). The session itself is the TLS pool. A build
 * with that pool and no Lua uses the pool for the request too. Without the
 * pool, https:// is refused.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "autoconf.h"
#include "pex/pex_gw.h"
#include "pico/time.h"
#include "fs/fs.h"
#if CONFIG_SD
#include "sd/sd_jobs.h"
#include "sd/sd_store.h"
#endif
#include "lua/lfile_path.h"
#include "net/httpc.h"
#if CONFIG_LUA_ENABLE || CONFIG_NET_HTTP_TLS
#include "lua/lheap.h"
#endif
#if CONFIG_LUA_ENABLE
#include "lua/lrt.h"
#endif

#define RING 2048u
#define SD_FILL_IDLE_MS 200u     /* a part-filled card buffer is written after this */

enum { V_NONE = 0, V_LFS, V_SD };
/* The card side: open the .part file, write each filled buffer, close, then
 * replace the real name with it (remove + rename), or remove it on failure. */
enum { SD_OPEN = 1, SD_OPENING, SD_READY, SD_WRITING, SD_CLOSING, SD_UNLINK, SD_RENAMING, SD_REMOVING, SD_DONE };

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
    uint8_t vol;                    /* V_NONE, V_LFS, V_SD */
    char ferr[48];                  /* why the file failed, for res.msg */
#if CONFIG_SD
    uint8_t sd;                     /* SD_* */
    int sd_file;                    /* open card file, or -1 */
    uint32_t sd_token, fill_token;  /* the job in flight; the buffer being filled */
    uint32_t fill_len, fill_ms, written;
    bool sd_ok;                     /* the body is complete and should replace the path */
#endif
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
#elif CONFIG_NET_HTTP_TLS
static void *pool_alloc(size_t n) {
    void *p = lheap_pool_alloc(n ? n : 1);
    if (p && n) { memset(p, 0, n); }
    return p;
}
static void pool_free(void *p) {
    if (p && lheap_pool_owns(p)) { lheap_pool_free(p); }
}
static void *pool_use(void *scope) { (void)scope; return NULL; }
static void pool_stat(size_t *used, size_t *largest) {
    lheap_pool_stats_t ps;
    lheap_pool_stats(&ps);
    *used = ps.used;
    *largest = ps.largest_free;
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

/* The card side's jobs and file, dropped without waiting (cancel, unload,
 * and the end of a finished one, where they are already idle). A cancelled
 * download leaves at most its "<path>.part" behind. */
static void sd_drop(hslot_t *h) {
#if CONFIG_SD
    if (h->vol != V_SD) { return; }
    if (h->fill_token) { sdj_abandon(h->fill_token); h->fill_token = 0; }
    if (h->sd_token) { sdj_abandon(h->sd_token); h->sd_token = 0; }
    if (h->sd_file >= 0) { sdj_close_async(h->sd_file); h->sd_file = -1; }
#else
    (void)h;
#endif
}

static void release(hslot_t *h) {
    if (h->id > 0) { httpc_cancel(h->id); }
    sd_drop(h);
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
#if !CONFIG_LUA_ENABLE && !CONFIG_NET_HTTP_TLS
    if (strncmp(q->url, "https:", 6) == 0) {
        pex_seterr(PEX_ENOTSUP, "https needs the TLS memory pool");
        return PEX_ENOTSUP;
    }
#endif
    hslot_t *h = NULL;
    for (int i = 0; i < HTTPC_MAX; i++) { if (!s_h[i].used) { h = &s_h[i]; break; } }
    if (!h) { return PEX_EBUSY; }
    memset(h, 0, sizeof *h);
    h->fh = -1;
    if (q->to_file) {
        int v = lfile_parse_path(q->to_file, strlen(q->to_file), h->fpath, sizeof h->fpath);
#if CONFIG_SD
        if (v == LFILE_SD && h->fpath[0]) {
            /* Refused now, not after the download: the path must leave room for ".part". */
            if (strlen(h->fpath) + 5 > SDJ_PATH_MAX) {
                pex_seterr(PEX_EINVAL, "to_file: sd:/ path too long");
                memset(h, 0, sizeof *h);
                return PEX_EINVAL;
            }
            h->ring = pex_alloc_long(RING);
            if (!h->ring) {
                printf("pex: http to_file sd:/%s: no PEX pool memory for its %u B ring\n", h->fpath, (unsigned)RING);
                memset(h, 0, sizeof *h);
                return PEX_ENOMEM;
            }
            h->vol = V_SD;
            h->sd = SD_OPEN;
            h->sd_file = -1;
            v = 0;                       /* handled */
        } else
#endif
        if (v != LFILE_LFS || !h->fpath[0]) {
            pex_seterr(PEX_EINVAL, CONFIG_SD ? "to_file must be lfs:/... or sd:/..." : "to_file must be lfs:/... (no SD card in this build)");
            memset(h, 0, sizeof *h);
            return PEX_EINVAL;
        }
    }
    if (q->to_file && h->vol != V_SD) {
        h->vol = V_LFS;
        h->ring = pex_alloc_long(RING);
        h->fmem = pex_alloc_long(fs_file_mem());
        if (!h->ring || !h->fmem) {
            printf("pex: http to_file %s: no PEX pool memory for its %u B ring and %u B file buffer\n",
                   h->fpath, (unsigned)RING, (unsigned)fs_file_mem());
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
#elif CONFIG_NET_HTTP_TLS
    static const httpc_alloc_fns fns = { pool_alloc, pool_free, pool_stat, pool_use };
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
            sd_drop(&s_h[i]);
            s_h[i].fmem = NULL;          /* the module's blocks go with it */
            s_h[i].ring = NULL;
            if (s_h[i].id > 0) { httpc_cancel(s_h[i].id); }
            memset(&s_h[i], 0, sizeof s_h[i]);
        }
    }
}

#if CONFIG_SD
static void sd_fail(hslot_t *h, const char *why, int rc) {
    if (!h->file_err) {
        h->file_err = true;
        snprintf(h->ferr, sizeof h->ferr, "%s (%d)", why, rc);
        printf("pex: http to_file sd:/%s: %s (%d)\n", h->fpath, why, rc);
    }
}

/* One pass of the card side. True once it has nothing left to do, so the
 * request may complete; false while a job is in flight or still to post.
 * Every job goes through the one SD worker slot: a refused post is tried
 * again on a later pass. */
static bool sd_step(hslot_t *h) {
    sdj_t q;
    if (h->sd_token) {
        sdj_t r;
        enum sdj_state st = sdj_poll(h->sd_token, &r);
        if (st == SDJ_POSTED || st == SDJ_FILLING) { return false; }
        if (st == SDJ_DONE) { sdj_finish(h->sd_token); }
        h->sd_token = 0;
        int rc = st == SDJ_DONE ? r.rc : LS_EIO;
        switch (h->sd) {
            case SD_OPENING:
                if (rc < 0) { sd_fail(h, "cannot open the file on the card", rc); h->sd = SD_DONE; }
                else { h->sd_file = rc; h->sd = SD_READY; }
                break;
            case SD_WRITING:
                if (rc < 0 || (uint32_t)rc != h->written) {
                    sd_fail(h, rc == LS_ENOSPC ? "the card is full" : "card write failed", rc);
                    h->sd = SD_CLOSING;      /* close, then remove the .part */
                    break;
                }
                h->sd = SD_READY;
                break;
            case SD_CLOSING:
                h->sd_file = -1;
                if (rc < 0) { sd_fail(h, "close failed", rc); }
                h->sd = h->sd_ok && !h->file_err ? SD_UNLINK : SD_REMOVING;
                break;
            case SD_UNLINK:                  /* the old file, if any: not there is fine */
                h->sd = SD_RENAMING;
                break;
            case SD_RENAMING:
                if (rc < 0) { sd_fail(h, "rename onto the path failed", rc); h->sd = SD_REMOVING; }
                else { h->sd = SD_DONE; }
                break;
            case SD_REMOVING:
                h->sd = SD_DONE;
                break;
        }
    }
    if (h->file_err) {
        /* The card failed: the rest of the body has nowhere to go. Stop the
         * download and drop what is buffered; close and remove follow. */
        h->rtail = h->rhead;
        if (h->fill_token) { sdj_abandon(h->fill_token); h->fill_token = 0; }
        if (!h->res.done) {
            if (h->id > 0) { httpc_cancel(h->id); h->id = 0; }
            h->res.done = 1;
        }
        if (h->sd == SD_READY) { h->sd = SD_CLOSING; }
    }
    if (h->sd == SD_DONE) { return true; }
    bool ok_status = h->res.status >= 200 && h->res.status <= 299;
    bool end = h->res.done && h->rhead == h->rtail;
    memset(&q, 0, sizeof q);
    if (h->sd == SD_OPEN) {
        /* Not before a 2xx: an error page never touches the card. */
        if (h->res.done && !ok_status) { h->rtail = h->rhead; h->sd = SD_DONE; return true; }
        if (!h->res.status) { return h->res.done; }
        if (!ok_status) { h->rtail = h->rhead; return h->res.done; }
        q.op = SDJ_OPEN;
        q.mode = LS_WR;
        snprintf(q.path, sizeof q.path, "%s.part", h->fpath);
        if (sdj_post(&q, &h->sd_token)) { h->sd = SD_OPENING; }
        return false;
    }
    if (h->sd == SD_READY && !h->file_err) {
        /* Ring -> the worker's transfer buffer, then one write job per 4 KiB. */
        if (h->rhead != h->rtail && !h->fill_token) {
            if (sdj_begin_write(h->sd_file, &h->fill_token)) { h->fill_len = 0; }
        }
        if (h->fill_token) {
            uint8_t *dst = sdj_buf();
            if (h->fill_len == 0 && h->rhead != h->rtail) { h->fill_ms = to_ms_since_boot(get_absolute_time()); }
            while (h->rhead != h->rtail && h->fill_len < SDJ_BUF) { dst[h->fill_len++] = h->ring[h->rtail++ % RING]; }
            bool full = h->fill_len >= SDJ_BUF;
            bool idle = h->fill_len && to_ms_since_boot(get_absolute_time()) - h->fill_ms >= SD_FILL_IDLE_MS;
            bool last = h->res.done && h->rhead == h->rtail;
            if (h->fill_len && (full || idle || last)) {
                h->written = h->fill_len;
                sdj_commit(h->fill_token, h->fill_len);
                h->sd_token = h->fill_token;
                h->fill_token = 0;
                h->fill_len = 0;
                h->sd = SD_WRITING;
                return false;
            }
            if (!h->fill_len && last) { sdj_abandon(h->fill_token); h->fill_token = 0; }
            else if (!last) { return false; }
        }
        if (!end) { return false; }
        h->sd_ok = !h->res.err && ok_status;
        h->sd = SD_CLOSING;
    }
    if (h->sd == SD_CLOSING) {
        if (h->sd_file < 0) { h->sd = h->sd_ok && !h->file_err ? SD_UNLINK : SD_REMOVING; return false; }
        q.op = SDJ_CLOSE;
        q.file = h->sd_file;
        sdj_post(&q, &h->sd_token);
        return false;
    }
    if (h->sd == SD_UNLINK) {
        q.op = SDJ_REMOVE;
        snprintf(q.path, sizeof q.path, "%s", h->fpath);
        sdj_post(&q, &h->sd_token);
        return false;
    }
    if (h->sd == SD_RENAMING) {
        q.op = SDJ_RENAME;
        snprintf(q.path, sizeof q.path, "%s.part", h->fpath);
        snprintf(q.path2, sizeof q.path2, "%s", h->fpath);
        sdj_post(&q, &h->sd_token);
        return false;
    }
    if (h->sd == SD_REMOVING) {
        q.op = SDJ_REMOVE;
        snprintf(q.path, sizeof q.path, "%s.part", h->fpath);
        sdj_post(&q, &h->sd_token);
        return false;
    }
    return false;
}
#endif

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
        if (h->vol == V_LFS && h->rhead != h->rtail) {
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
#if CONFIG_SD
        if (h->vol == V_SD && !sd_step(h)) { continue; }   /* the card still has work */
#endif
        if (h->res.done && (!h->ring || h->rhead == h->rtail)) {
            if (h->fmem) {
                fs_fclose(h->fh);
                h->fh = -1;
                bool ok = !h->res.err && !h->file_err && h->res.status >= 200 && h->res.status <= 299;
                if (!ok) { fs_delete(h->fpath); }
                if (h->file_err) { h->res.err = HTTPC_EIO; snprintf(h->res.msg, sizeof h->res.msg, "file write failed"); }
            }
            if (h->vol == V_SD && h->file_err && !h->res.err) {
                h->res.err = HTTPC_EIO;
                snprintf(h->res.msg, sizeof h->res.msg, "%s", h->ferr[0] ? h->ferr : "card write failed");
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
