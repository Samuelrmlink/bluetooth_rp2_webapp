/*
 * file -- files on LittleFS and on the SD card, from Lua.
 *
 *   local h, err = file.open("sd:/fw/img.bin", "r")     -- "r" | "w" | "a"
 *   local chunk = h:read(4096)                          -- "" at end of file
 *   h:write(s)  h:seek(pos)  h:tell()  h:size()  h:close()
 *   file.stat(p) file.list(dir) file.mkdir(p) file.remove(p) file.rename(a, b)
 *   file.media()
 *
 * Paths carry their volume (lfile_path.h). Failures are `nil, "<code>"`
 * (enoent eexist enospc eio nomedia busy badpath toolarge nohandle notempty);
 * only arguments of the wrong type raise.
 *
 * LittleFS is on the same core and its calls are synchronous: a read is a
 * memcpy from XIP, a write may erase a sector (~50-400 ms on this flash), so
 * writes go in 4 KiB pieces with a yield between them, and the loop gets a
 * turn after every one (lrt_yield_soon).
 *
 * The SD card is only ever touched by the storage worker (src/sd). An SD call
 * posts a job (sd_jobs.h) and YIELDS the script until the worker has run it,
 * polling once per main-loop pass. So SD calls -- and LittleFS writes larger
 * than 4 KiB -- may only be made from a script's own coroutine, never from an
 * ops.register or bt.on_pairing handler; those get an error.
 *
 * Handles: FS_FILES (4) on LittleFS and SDJ_FILES (4) on the card, eight in
 * all. A handle belongs to the script that opened it and is closed when that
 * script stops, as well as by h:close(), <close> and the collector.
 */
#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"

#include "autoconf.h"
#include "fs/fs.h"
#include "fs/littlefs/lfs.h"
#include "lua/lfile.h"
#include "lua/lfile_path.h"
#include "lua/lrt.h"
#include "pico/time.h"

#if CONFIG_SD
#include "sd/sd_api.h"
#include "sd/sd_core.h"
#include "sd/sd_jobs.h"
#include "sd/sd_store.h"
#endif

#define HANDLE_MT   "file.handle"
#define READ_MAX    4096
#define WRITE_PIECE 4096
#define N_HANDLES   (FS_FILES + 4)
#define N_SCRIPTS   8                  /* MAX_SCRIPTS in lrt.c */

typedef struct {
    bool     used;
    uint8_t  vol;
    int8_t   owner;        /* script slot, -1 = none (an op handler) */
    int      h;            /* fs pool handle, or SD job-table file */
    uint32_t gen;          /* matches the userdata; a closed slot's old handles go stale */
    uint64_t pos, size;    /* SD: as the worker last reported */
} lf_t;

typedef struct { int slot; uint32_t gen; } lf_ud;

static lf_t s_h[N_HANDLES];
static lua_State *s_gL;
static uint32_t s_gen = 1;

static int fail(lua_State *L, const char *code) {
    lua_pushnil(L);
    lua_pushstring(L, code);
    return 2;
}

static const char *lfs_code(int err) {
    switch (err) {
        case LFS_ERR_NOENT:    return "enoent";
        case LFS_ERR_EXIST:    return "eexist";
        case LFS_ERR_NOSPC:    return "enospc";
        case LFS_ERR_ISDIR:
        case LFS_ERR_NOTDIR:
        case LFS_ERR_INVAL:
        case LFS_ERR_NAMETOOLONG: return "badpath";
        case LFS_ERR_NOTEMPTY: return "notempty";
        case LFS_ERR_NOMEM:    return "nohandle";
        case FS_ERR_BUSY:      return "busy";
        case FS_ERR_NOHANDLE:  return "nohandle";
        case FS_ERR_TOOLARGE:  return "toolarge";
        default:               return "eio";
    }
}

static const char *parse(lua_State *L, int idx, int *vol, char *rel) {
    size_t len = 0;
    const char *p = luaL_checklstring(L, idx, &len);
    *vol = lfile_parse_path(p, len, rel, LFILE_PATH_MAX + 1);
    return p;
}

static lf_t *check_handle(lua_State *L, int idx, lf_ud **udp) {
    lf_ud *ud = (lf_ud *)luaL_checkudata(L, idx, HANDLE_MT);
    if (udp) { *udp = ud; }
    if (ud->slot < 0 || ud->slot >= N_HANDLES) { return NULL; }
    lf_t *f = &s_h[ud->slot];
    return (f->used && f->gen == ud->gen) ? f : NULL;
}

static int new_handle(lua_State *L, uint8_t vol, int h, uint64_t size, uint64_t pos) {
    int slot = -1;
    for (int i = 0; i < N_HANDLES; i++) {
        if (!s_h[i].used) { slot = i; break; }
    }
    if (slot < 0) { return -1; }
    lf_t *f = &s_h[slot];
    memset(f, 0, sizeof *f);
    f->used = true;
    f->vol = vol;
    f->owner = (int8_t)lrt_current_slot();
    f->h = h;
    f->gen = s_gen++;
    if (!s_gen) { s_gen = 1; }
    f->size = size;
    f->pos = pos;
    lf_ud *ud = (lf_ud *)lua_newuserdatauv(L, sizeof(lf_ud), 0);
    ud->slot = slot;
    ud->gen = f->gen;
    luaL_setmetatable(L, HANDLE_MT);
    return slot;
}

/* Close without waiting: the collector, <close>, a stopping script. */
static void close_now(lf_t *f) {
    if (!f->used) { return; }
    if (f->vol == LFILE_LFS) {
        fs_fclose(f->h);
    }
#if CONFIG_SD
    else {
        sdj_close_async(f->h);
    }
#endif
    f->used = false;
}

/* ------------------------------------------------------------------------
 * Waiting for the storage worker. One pending operation per script (a script
 * is one coroutine); its state lives here, its Lua values on the coroutine's
 * own stack, which a yield preserves. The continuation polls once a pass. */
enum {
    P_NONE = 0,
    P_LFS_WRITE,          /* LittleFS write in pieces: yield between them */
#if CONFIG_SD
    P_SD_OPEN, P_SD_READ, P_SD_WRITE, P_SD_SEEK, P_SD_CLOSE,
    P_SD_STAT, P_SD_LIST, P_SD_MKDIR, P_SD_REMOVE, P_SD_RENAME,
#endif
};

typedef struct {
    int      kind;
    uint32_t token;        /* 0 = not posted yet */
    int      hslot;        /* the file handle slot, for file ops */
    size_t   done;         /* write: bytes written so far */
    uint64_t list_pos;     /* list: entries already collected */
#if CONFIG_SD
    /* The request, in a userdata on the Lua heap held by req_ref: ~300 B
     * that only a waiting script needs, kept out of static RAM. The worker
     * copies it at post time and never sees this memory. */
    sdj_t   *req;
    int      req_ref;
#endif
} pend_t;

static pend_t s_pend[N_SCRIPTS];

static pend_t *pend_here(lua_State *L) {
    int slot = lrt_current_slot();
    if (slot < 0 || slot >= N_SCRIPTS || !lua_isyieldable(L)) {
        luaL_error(L, "this file operation waits, so it may only be called from a "
                      "script's own code (not an ops.register or bt.on_pairing handler)");
        return NULL;
    }
    return &s_pend[slot];
}

static int resume_k(lua_State *L, int status, lua_KContext ctx);

#if CONFIG_SD
static void req_drop(lua_State *L, pend_t *p) {
    if (p->req && p->req_ref > 0) { luaL_unref(L, LUA_REGISTRYINDEX, p->req_ref); }
    p->req = NULL;
    p->req_ref = 0;
}

static void req_new(lua_State *L, pend_t *p) {
    req_drop(L, p);
    p->req = (sdj_t *)lua_newuserdatauv(L, sizeof(sdj_t), 0);
    memset(p->req, 0, sizeof *p->req);
    p->req_ref = luaL_ref(L, LUA_REGISTRYINDEX);
}
#endif

/* Come back next pass. An integer yield is sys.sleep(0) to the scheduler, so
 * it counts as voluntary and never trips the runaway killer. */
static int again(lua_State *L) {
    lua_pushinteger(L, (lua_Integer)to_ms_since_boot(get_absolute_time()));
    return lua_yieldk(L, 1, 0, resume_k);
}

/* The LittleFS write: one piece now, the rest after yields. Arguments are
 * (h, s) at stack 1 and 2. */
static int lfs_write_step(lua_State *L, pend_t *p) {
    lf_t *f = &s_h[p->hslot];
    size_t len = 0;
    const char *s = lua_tolstring(L, 2, &len);
    if (!f->used) { p->kind = P_NONE; return fail(L, "eio"); }
    size_t n = len - p->done > WRITE_PIECE ? WRITE_PIECE : len - p->done;
    int w = fs_fwrite(f->h, s + p->done, n);
    lrt_yield_soon();
    if (w < 0) { p->kind = P_NONE; return fail(L, lfs_code(w)); }
    p->done += (size_t)w;
    if (p->done < len) { return again(L); }
    p->kind = P_NONE;
    lua_pushboolean(L, 1);
    return 1;
}

#if CONFIG_SD
static const char *sd_code(int rc) {
    switch (rc) {
        case LS_ENOENT:   return "enoent";
        case LS_EEXIST:   return "eexist";
        case LS_ENOSPC:   return "enospc";
        case LS_ENOMEM:   return "nohandle";
        case LS_ENOMEDIA: return "nomedia";
        case LS_EINVAL:   return "badpath";
        case LS_EBUSY:    return "busy";
        case SDJ_ENOTEMPTY: return "notempty";
        default:          return "eio";
    }
}

/* Whether the card can take a job now; *code says why not. */
static bool sd_ready(const char **code) {
    const char *st = sd_card_state();
    if (strcmp(st, "ok") == 0) { return true; }
    *code = strcmp(st, "formatting") == 0 || strcmp(st, "settling") == 0 ? "busy"
          : strcmp(st, "none") == 0 ? "nomedia" : "eio";
    return false;
}

static int sd_begin(lua_State *L, pend_t *p, int kind) {
    const char *code = NULL;
    if (!sd_ready(&code)) { return fail(L, code); }
    p->kind = kind;
    p->token = 0;
    return resume_k(L, LUA_YIELD, 0);
}

static void list_unpack(lua_State *L, int tbl, int count, lua_Integer *n) {
    const uint8_t *b = sdj_buf();
    uint32_t off = 0;
    for (int i = 0; i < count; i++) {
        bool dir = b[off] != 0;
        uint8_t nl = b[off + 1];
        uint32_t sz = (uint32_t)b[off + 2] | ((uint32_t)b[off + 3] << 8) |
                      ((uint32_t)b[off + 4] << 16) | ((uint32_t)b[off + 5] << 24);
        lua_createtable(L, 0, 3);
        lua_pushlstring(L, (const char *)b + off + SDJ_LIST_HDR, nl);
        lua_setfield(L, -2, "name");
        lua_pushboolean(L, dir);
        lua_setfield(L, -2, "dir");
        lua_pushinteger(L, dir ? 0 : (lua_Integer)sz);
        lua_setfield(L, -2, "size");
        lua_rawseti(L, tbl, ++*n);
        off += SDJ_LIST_HDR + nl;
    }
}

/* A finished job: turn it into the call's results, or post the next piece. */
static int sd_done(lua_State *L, pend_t *p, const sdj_t *r) {
    lf_t *f = (p->hslot >= 0 && p->hslot < N_HANDLES) ? &s_h[p->hslot] : NULL;
    if (f && (p->kind == P_SD_READ || p->kind == P_SD_WRITE || p->kind == P_SD_SEEK) && r->rc >= 0) {
        f->pos = r->tell;
        f->size = r->size;
    }
    int kind = p->kind;
    if (kind != P_SD_WRITE || r->rc < 0) { p->kind = P_NONE; }
    if (r->rc < 0 && kind != P_SD_STAT) {
        sdj_finish(p->token);
        if (kind == P_SD_CLOSE && f) { f->used = false; }
        return fail(L, sd_code(r->rc));
    }
    switch (kind) {
        case P_SD_OPEN: {
            sdj_finish(p->token);
            if (new_handle(L, LFILE_SD, r->rc, r->size, r->tell) < 0) {
                sdj_close_async(r->rc);
                return fail(L, "nohandle");
            }
            return 1;
        }
        case P_SD_READ:
            lua_pushlstring(L, (const char *)sdj_buf(), (size_t)r->rc);
            sdj_finish(p->token);
            return 1;
        case P_SD_WRITE: {
            sdj_finish(p->token);
            size_t len = 0;
            (void)lua_tolstring(L, 2, &len);
            p->done += (size_t)r->rc;
            if (r->rc == 0 || p->done >= len) {
                p->kind = P_NONE;
                lua_pushboolean(L, 1);
                return 1;
            }
            p->token = 0;                  /* the next piece */
            return resume_k(L, LUA_YIELD, 0);
        }
        case P_SD_CLOSE:
            sdj_finish(p->token);
            if (f) { f->used = false; }
            lua_pushboolean(L, 1);
            return 1;
        case P_SD_STAT:
            sdj_finish(p->token);
            if (r->rc < 0) { return fail(L, sd_code(r->rc)); }
            lua_createtable(L, 0, 2);
            lua_pushinteger(L, r->is_dir ? 0 : (lua_Integer)r->size);
            lua_setfield(L, -2, "size");
            lua_pushboolean(L, r->is_dir);
            lua_setfield(L, -2, "dir");
            return 1;
        case P_SD_LIST: {
            /* The result table sits at stack 2, made before the first yield. */
            lua_Integer n = (lua_Integer)p->list_pos;
            list_unpack(L, 2, r->rc, &n);
            p->list_pos = (uint64_t)n;
            sdj_finish(p->token);
            if (r->more) {
                p->kind = P_SD_LIST;
                p->token = 0;
                p->req->pos = p->list_pos;
                return resume_k(L, LUA_YIELD, 0);
            }
            lua_settop(L, 2);
            return 1;
        }
        default:
            sdj_finish(p->token);
            lua_pushboolean(L, 1);
            return 1;
    }
}
#endif

static int resume_k(lua_State *L, int status, lua_KContext ctx) {
    (void)status; (void)ctx;
    int slot = lrt_current_slot();
    if (slot < 0 || slot >= N_SCRIPTS) { return fail(L, "eio"); }
    pend_t *p = &s_pend[slot];
    if (p->kind == P_LFS_WRITE) { return lfs_write_step(L, p); }
#if CONFIG_SD
    if (p->kind == P_NONE) { return fail(L, "eio"); }
    if (!p->token) {
        if (p->kind == P_SD_WRITE) {
            lf_t *f = &s_h[p->hslot];
            if (!f->used) { p->kind = P_NONE; return fail(L, "eio"); }
            size_t len = 0;
            const char *s = lua_tolstring(L, 2, &len);
            size_t n = len - p->done > SDJ_BUF ? SDJ_BUF : len - p->done;
            p->req->len = (uint32_t)n;
            p->req->file = f->h;
            if (!sdj_post(p->req, &p->token)) { return again(L); }
            /* Filled after the post succeeded (until then the buffer may be
             * another caller's), and before the worker can run: it only runs
             * between passes, from the main loop. */
            memcpy(sdj_buf(), s + p->done, n);
            return again(L);
        }
        if (!sdj_post(p->req, &p->token)) { return again(L); }
        return again(L);
    }
    sdj_t r;
    enum sdj_state st = sdj_poll(p->token, &r);
    if (st == SDJ_POSTED) { return again(L); }
    if (st != SDJ_DONE) { p->kind = P_NONE; return fail(L, "eio"); }
    return sd_done(L, p, &r);
#else
    return fail(L, "eio");
#endif
}

/* ------------------------------------------------------------------------ */

static int l_open(lua_State *L) {
    int vol; char rel[LFILE_PATH_MAX + 1];
    parse(L, 1, &vol, rel);
    const char *mode = luaL_optstring(L, 2, "r");
    int m = strcmp(mode, "r") == 0 ? FS_O_READ : strcmp(mode, "w") == 0 ? FS_O_WRITE
          : strcmp(mode, "a") == 0 ? FS_O_APPEND : -1;
    if (m < 0) { return luaL_argerror(L, 2, "mode is \"r\", \"w\" or \"a\""); }
    if (!vol || !rel[0]) { return fail(L, "badpath"); }
    if (vol == LFILE_LFS) {
        int free_slot = 0;
        for (int i = 0; i < N_HANDLES; i++) { if (!s_h[i].used) { free_slot = 1; break; } }
        if (!free_slot) { return fail(L, "nohandle"); }
        if (m == FS_O_READ && !fs_stat(rel, NULL)) {
            return fail(L, fs_is_dir(rel) ? "badpath" : "enoent");
        }
        int h = fs_fopen(rel, m);
        if (h < 0) { return fail(L, lfs_code(h)); }
        int32_t sz = fs_fsize(h);
        int32_t at = fs_ftell(h);
        if (new_handle(L, LFILE_LFS, h, sz < 0 ? 0 : (uint64_t)sz, at < 0 ? 0 : (uint64_t)at) < 0) {
            fs_fclose(h);
            return fail(L, "nohandle");
        }
        return 1;
    }
#if CONFIG_SD
    pend_t *p = pend_here(L);
    req_new(L, p);
    p->req->op = SDJ_OPEN;
    p->req->mode = m == FS_O_READ ? LS_RD : m == FS_O_WRITE ? LS_WR : LS_APPEND;
    snprintf(p->req->path, sizeof p->req->path, "%s", rel);
    p->hslot = -1;
    int any = 0;
    for (int i = 0; i < N_HANDLES; i++) { if (!s_h[i].used) { any = 1; break; } }
    if (!any) { return fail(L, "nohandle"); }
    lua_settop(L, 2);
    return sd_begin(L, p, P_SD_OPEN);
#else
    return fail(L, "nomedia");
#endif
}

static int l_read(lua_State *L) {
    lf_t *f = check_handle(L, 1, NULL);
    lua_Integer n = luaL_optinteger(L, 2, READ_MAX);
    if (!f) { return fail(L, "eio"); }
    if (n <= 0) { lua_pushliteral(L, ""); return 1; }
    if (n > READ_MAX) { n = READ_MAX; }
    if (f->vol == LFILE_LFS) {
        luaL_Buffer b;
        char *dst = luaL_buffinitsize(L, &b, (size_t)n);
        int r = fs_fread(f->h, dst, (size_t)n);
        if (r < 0) { return fail(L, lfs_code(r)); }
        luaL_pushresultsize(&b, (size_t)r);
        return 1;
    }
#if CONFIG_SD
    pend_t *p = pend_here(L);
    req_new(L, p);
    p->req->op = SDJ_READ;
    p->req->file = f->h;
    p->req->len = (uint32_t)n;
    p->hslot = (int)(f - s_h);
    lua_settop(L, 2);
    return sd_begin(L, p, P_SD_READ);
#else
    return fail(L, "eio");
#endif
}

static int l_write(lua_State *L) {
    lf_t *f = check_handle(L, 1, NULL);
    size_t len = 0;
    luaL_checklstring(L, 2, &len);
    if (!f) { return fail(L, "eio"); }
    lua_settop(L, 2);
    if (len == 0) { lua_pushboolean(L, 1); return 1; }
    if (f->vol == LFILE_LFS) {
        if (len <= WRITE_PIECE || !lua_isyieldable(L) || lrt_current_slot() < 0) {
            /* Small, or from a handler that cannot wait: all at once. */
            int w = fs_fwrite(f->h, lua_tostring(L, 2), len);
            lrt_yield_soon();
            if (w < 0) { return fail(L, lfs_code(w)); }
            lua_pushboolean(L, 1);
            return 1;
        }
        pend_t *p = pend_here(L);
        p->kind = P_LFS_WRITE;
        p->hslot = (int)(f - s_h);
        p->done = 0;
        return lfs_write_step(L, p);
    }
#if CONFIG_SD
    pend_t *p = pend_here(L);
    req_new(L, p);
    p->req->op = SDJ_WRITE;
    p->hslot = (int)(f - s_h);
    p->done = 0;
    return sd_begin(L, p, P_SD_WRITE);
#else
    return fail(L, "eio");
#endif
}

static int l_seek(lua_State *L) {
    lf_t *f = check_handle(L, 1, NULL);
    lua_Integer pos = luaL_checkinteger(L, 2);
    if (!f) { return fail(L, "eio"); }
    if (pos < 0) { return fail(L, "badpath"); }
    if (f->vol == LFILE_LFS) {
        int r = fs_fseek(f->h, (uint32_t)pos);
        if (r < 0) { return fail(L, lfs_code(r)); }
        lua_pushinteger(L, pos);
        return 1;
    }
#if CONFIG_SD
    pend_t *p = pend_here(L);
    req_new(L, p);
    p->req->op = SDJ_SEEK;
    p->req->file = f->h;
    p->req->pos = (uint64_t)pos;
    p->hslot = (int)(f - s_h);
    lua_settop(L, 2);
    return sd_begin(L, p, P_SD_SEEK);
#else
    return fail(L, "eio");
#endif
}

/* SEEK's result is the new position, like io's. */
static int l_tell(lua_State *L) {
    lf_t *f = check_handle(L, 1, NULL);
    if (!f) { return fail(L, "eio"); }
    if (f->vol == LFILE_LFS) {
        int32_t t = fs_ftell(f->h);
        if (t < 0) { return fail(L, lfs_code(t)); }
        lua_pushinteger(L, t);
        return 1;
    }
    lua_pushinteger(L, (lua_Integer)f->pos);
    return 1;
}

static int l_size(lua_State *L) {
    lf_t *f = check_handle(L, 1, NULL);
    if (!f) { return fail(L, "eio"); }
    if (f->vol == LFILE_LFS) {
        int32_t t = fs_fsize(f->h);
        if (t < 0) { return fail(L, lfs_code(t)); }
        lua_pushinteger(L, t);
        return 1;
    }
    lua_pushinteger(L, (lua_Integer)f->size);
    return 1;
}

static int l_close(lua_State *L) {
    lf_t *f = check_handle(L, 1, NULL);
    if (!f) { lua_pushboolean(L, 1); return 1; }     /* already closed: idempotent */
    if (f->vol == LFILE_LFS) {
        int r = fs_fclose(f->h);
        f->used = false;
        if (r < 0) { return fail(L, lfs_code(r)); }
        lua_pushboolean(L, 1);
        return 1;
    }
#if CONFIG_SD
    if (!lua_isyieldable(L) || lrt_current_slot() < 0) {
        close_now(f);                  /* cannot wait here: close in the background */
        lua_pushboolean(L, 1);
        return 1;
    }
    pend_t *p = pend_here(L);
    req_new(L, p);
    p->req->op = SDJ_CLOSE;
    p->req->file = f->h;
    p->hslot = (int)(f - s_h);
    lua_settop(L, 1);
    return sd_begin(L, p, P_SD_CLOSE);
#else
    return fail(L, "eio");
#endif
}

/* __gc and __close: no waiting, whatever the context. */
static int l_gc(lua_State *L) {
    lf_t *f = check_handle(L, 1, NULL);
    if (f) { close_now(f); }
    return 0;
}

static int l_tostring(lua_State *L) {
    lf_t *f = check_handle(L, 1, NULL);
    lua_pushfstring(L, "file (%s)", f ? (f->vol == LFILE_LFS ? "lfs" : "sd") : "closed");
    return 1;
}

static int l_stat(lua_State *L) {
    int vol; char rel[LFILE_PATH_MAX + 1];
    parse(L, 1, &vol, rel);
    if (!vol) { return fail(L, "badpath"); }
    if (vol == LFILE_LFS) {
        size_t sz = 0;
        if (!rel[0] || fs_is_dir(rel)) {
            lua_createtable(L, 0, 2);
            lua_pushinteger(L, 0); lua_setfield(L, -2, "size");
            lua_pushboolean(L, 1); lua_setfield(L, -2, "dir");
            return 1;
        }
        if (!fs_stat(rel, &sz)) { return fail(L, "enoent"); }
        lua_createtable(L, 0, 2);
        lua_pushinteger(L, (lua_Integer)sz); lua_setfield(L, -2, "size");
        lua_pushboolean(L, 0); lua_setfield(L, -2, "dir");
        return 1;
    }
#if CONFIG_SD
    pend_t *p = pend_here(L);
    req_new(L, p);
    p->req->op = SDJ_STAT;
    snprintf(p->req->path, sizeof p->req->path, "%s", rel);
    p->hslot = -1;
    lua_settop(L, 1);
    return sd_begin(L, p, P_SD_STAT);
#else
    return fail(L, "nomedia");
#endif
}

typedef struct { lua_State *L; lua_Integer n; } walk_ctx;
static void walk_cb(const char *name, bool is_dir, size_t size, void *vctx) {
    walk_ctx *w = vctx;
    lua_createtable(w->L, 0, 3);
    lua_pushstring(w->L, name);     lua_setfield(w->L, -2, "name");
    lua_pushboolean(w->L, is_dir);  lua_setfield(w->L, -2, "dir");
    lua_pushinteger(w->L, is_dir ? 0 : (lua_Integer)size); lua_setfield(w->L, -2, "size");
    lua_rawseti(w->L, -2, ++w->n);
}

static int l_list(lua_State *L) {
    int vol; char rel[LFILE_PATH_MAX + 1];
    parse(L, 1, &vol, rel);
    if (!vol) { return fail(L, "badpath"); }
    lua_settop(L, 1);
    if (vol == LFILE_LFS) {
        if (rel[0] && !fs_is_dir(rel)) { return fail(L, fs_stat(rel, NULL) ? "badpath" : "enoent"); }
        lua_newtable(L);
        walk_ctx w = { L, 0 };
        if (fs_walk(rel, walk_cb, &w) < 0) { lua_pop(L, 1); return fail(L, "eio"); }
        return 1;
    }
#if CONFIG_SD
    pend_t *p = pend_here(L);
    req_new(L, p);
    p->req->op = SDJ_LIST;
    snprintf(p->req->path, sizeof p->req->path, "%s", rel);
    p->hslot = -1;
    p->list_pos = 0;
    lua_newtable(L);                       /* stack 2: the result, across yields */
    return sd_begin(L, p, P_SD_LIST);
#else
    return fail(L, "nomedia");
#endif
}

static int l_mkdir(lua_State *L) {
    int vol; char rel[LFILE_PATH_MAX + 1];
    parse(L, 1, &vol, rel);
    if (!vol) { return fail(L, "badpath"); }
    if (!rel[0]) { lua_pushboolean(L, 1); return 1; }
    if (vol == LFILE_LFS) {
        if (fs_stat(rel, NULL)) { return fail(L, "eexist"); }     /* a file is in the way */
        if (fs_mkdir(rel) < 0) { return fail(L, "eio"); }
        lrt_yield_soon();
        lua_pushboolean(L, 1);
        return 1;
    }
#if CONFIG_SD
    pend_t *p = pend_here(L);
    req_new(L, p);
    p->req->op = SDJ_MKDIR;
    snprintf(p->req->path, sizeof p->req->path, "%s", rel);
    p->hslot = -1;
    lua_settop(L, 1);
    return sd_begin(L, p, P_SD_MKDIR);
#else
    return fail(L, "nomedia");
#endif
}

static int l_remove(lua_State *L) {
    int vol; char rel[LFILE_PATH_MAX + 1];
    parse(L, 1, &vol, rel);
    if (!vol || !rel[0]) { return fail(L, "badpath"); }
    if (vol == LFILE_LFS) {
        if (!fs_stat(rel, NULL) && !fs_is_dir(rel)) { return fail(L, "enoent"); }
        int r = fs_delete(rel);
        lrt_yield_soon();
        if (r == FS_ERR_NOTEMPTY) { return fail(L, "notempty"); }
        if (r < 0) { return fail(L, lfs_code(r)); }
        lua_pushboolean(L, 1);
        return 1;
    }
#if CONFIG_SD
    pend_t *p = pend_here(L);
    req_new(L, p);
    p->req->op = SDJ_REMOVE;
    snprintf(p->req->path, sizeof p->req->path, "%s", rel);
    p->hslot = -1;
    lua_settop(L, 1);
    return sd_begin(L, p, P_SD_REMOVE);
#else
    return fail(L, "nomedia");
#endif
}

static int l_rename(lua_State *L) {
    int v1, v2; char a[LFILE_PATH_MAX + 1], b[LFILE_PATH_MAX + 1];
    parse(L, 1, &v1, a);
    parse(L, 2, &v2, b);
    if (!v1 || !v2 || !a[0] || !b[0]) { return fail(L, "badpath"); }
    if (v1 != v2) { return fail(L, "badpath"); }          /* not across volumes */
    if (v1 == LFILE_LFS) {
        if (!fs_stat(a, NULL) && !fs_is_dir(a)) { return fail(L, "enoent"); }
        if (fs_stat(b, NULL) || fs_is_dir(b)) { return fail(L, "eexist"); }
        int r = fs_rename(a, b);
        lrt_yield_soon();
        if (r == 1) { return fail(L, "eexist"); }
        if (r < 0) { return fail(L, lfs_code(r)); }
        lua_pushboolean(L, 1);
        return 1;
    }
#if CONFIG_SD
    pend_t *p = pend_here(L);
    req_new(L, p);
    p->req->op = SDJ_RENAME;
    snprintf(p->req->path, sizeof p->req->path, "%s", a);
    snprintf(p->req->path2, sizeof p->req->path2, "%s", b);
    p->hslot = -1;
    lua_settop(L, 2);
    return sd_begin(L, p, P_SD_RENAME);
#else
    return fail(L, "nomedia");
#endif
}

static int l_media(lua_State *L) {
    lua_createtable(L, 0, 2);
    lua_createtable(L, 0, 3);
    uint32_t used = 0, total = 0;
    bool ok = fs_usage(&used, &total);
    lua_pushboolean(L, ok);                      lua_setfield(L, -2, "ok");
    lua_pushinteger(L, ok ? (lua_Integer)(total - used) : 0); lua_setfield(L, -2, "free");
    lua_pushinteger(L, (lua_Integer)total);      lua_setfield(L, -2, "total");
    lua_setfield(L, -2, "lfs");
    lua_createtable(L, 0, 5);
#if CONFIG_SD
    const char *st = sd_card_state();
    const struct sd_pub *pub = sd_pub();
    const char *state = strcmp(st, "ok") == 0 ? "ok" : strcmp(st, "error") == 0 ? "error" : "none";
    if (strcmp(st, "formatting") == 0) { state = "error"; }
    lua_pushstring(L, state);                    lua_setfield(L, -2, "state");
    lua_pushstring(L, st);                       lua_setfield(L, -2, "detail");
    lua_pushinteger(L, (lua_Integer)pub->free_bytes);  lua_setfield(L, -2, "free");
    lua_pushinteger(L, (lua_Integer)pub->total_bytes); lua_setfield(L, -2, "total");
    lua_pushstring(L, pub->fs ? pub->fs : "-"); lua_setfield(L, -2, "fs");
#else
    lua_pushstring(L, "absent");                 lua_setfield(L, -2, "state");
    lua_pushinteger(L, 0);                       lua_setfield(L, -2, "free");
    lua_pushinteger(L, 0);                       lua_setfield(L, -2, "total");
#endif
    lua_setfield(L, -2, "sd");
    return 1;
}

void lfile_open(lua_State *L) {
    s_gL = L;
    static const luaL_Reg methods[] = {
        { "read", l_read }, { "write", l_write }, { "seek", l_seek },
        { "tell", l_tell }, { "size", l_size }, { "close", l_close },
        { NULL, NULL } };
    luaL_newmetatable(L, HANDLE_MT);
    lua_newtable(L);
    luaL_setfuncs(L, methods, 0);
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, l_gc);       lua_setfield(L, -2, "__gc");
    lua_pushcfunction(L, l_gc);       lua_setfield(L, -2, "__close");
    lua_pushcfunction(L, l_tostring); lua_setfield(L, -2, "__tostring");
    lua_pop(L, 1);

    static const luaL_Reg fns[] = {
        { "open", l_open }, { "stat", l_stat }, { "list", l_list },
        { "mkdir", l_mkdir }, { "remove", l_remove }, { "rename", l_rename },
        { "media", l_media }, { NULL, NULL } };
    lua_newtable(L);
    luaL_setfuncs(L, fns, 0);
    lua_setglobal(L, "file");
}

/* A script is going: its files close, and a job it was waiting for is let
 * go (it finishes on its own; a file it opened is closed). */
void lfile_release(int slot) {
    for (int i = 0; i < N_HANDLES; i++) {
        if (s_h[i].used && s_h[i].owner == slot) { close_now(&s_h[i]); }
    }
    if (slot >= 0 && slot < N_SCRIPTS) {
#if CONFIG_SD
        if (s_pend[slot].kind != P_NONE && s_pend[slot].token) { sdj_abandon(s_pend[slot].token); }
        if (s_gL) { req_drop(s_gL, &s_pend[slot]); }
#endif
        memset(&s_pend[slot], 0, sizeof s_pend[slot]);
    }
}

/* The Lua state is being closed: every handle goes, whoever owned it. */
void lfile_reset(void) {
    for (int i = 0; i < N_SCRIPTS; i++) { lfile_release(i); }
    for (int i = 0; i < N_HANDLES; i++) { close_now(&s_h[i]); }
    s_gL = NULL;
}
