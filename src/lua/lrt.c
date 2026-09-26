#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lua/lrt.h"
#include "lua/lbind.h"
#include "fs/fs.h"
#include "api/api.h"
#include "net/ws.h"
#include "autoconf.h"
#include "pico/time.h"

#if CONFIG_LUA_ENABLE

/* --- the arena -----------------------------------------------------------
 * A dedicated static block, not the system heap. A runaway script then hits
 * its own ceiling and gets an ordinary Lua out-of-memory error, instead of
 * starving lwIP or BTstack and taking the whole device down. Static so the
 * link-time RAM check can see every byte of it. */
#define ARENA_BYTES (CONFIG_LUA_HEAP_KB * 1024)
static uint8_t arena[ARENA_BYTES] __attribute__((aligned(8)));

/* A first-fit free list. Lua's allocation pattern is many small, short-lived
 * blocks, which this handles adequately; the point is the hard ceiling, not
 * peak allocator performance. */
typedef struct blk { size_t size; struct blk *next; bool free; } blk_t;
#define HDR ((size_t)((sizeof(blk_t) + 7u) & ~7u))
static blk_t *heap_head;
static size_t heap_used, heap_peak;

static void heap_init(void) {
    heap_head = (blk_t *)arena;
    heap_head->size = ARENA_BYTES - HDR;
    heap_head->next = NULL;
    heap_head->free = true;
    heap_used = 0;
    heap_peak = 0;
}

static void *arena_alloc(size_t want) {
    want = (want + 7u) & ~7u;
    for (blk_t *b = heap_head; b; b = b->next) {
        if (!b->free || b->size < want) {
            continue;
        }
        if (b->size >= want + HDR + 32) {           /* split */
            blk_t *n = (blk_t *)((uint8_t *)b + HDR + want);
            n->size = b->size - want - HDR;
            n->next = b->next;
            n->free = true;
            b->size = want;
            b->next = n;
        }
        b->free = false;
        heap_used += b->size;
        if (heap_used > heap_peak) { heap_peak = heap_used; }
        return (uint8_t *)b + HDR;
    }
    return NULL;
}

static void arena_free(void *p) {
    if (!p) {
        return;
    }
    blk_t *b = (blk_t *)((uint8_t *)p - HDR);
    b->free = true;
    heap_used -= b->size;
    for (blk_t *c = heap_head; c; c = c->next) {     /* coalesce forward */
        while (c->free && c->next && c->next->free) {
            c->size += HDR + c->next->size;
            c->next = c->next->next;
        }
    }
}

static void *lrt_alloc(void *ud, void *ptr, size_t osize, size_t nsize) {
    (void)ud;
    if (nsize == 0) { arena_free(ptr); return NULL; }
    void *np = arena_alloc(nsize);
    if (np && ptr) {
        memcpy(np, ptr, osize < nsize ? osize : nsize);
    }
    if (np && ptr) { arena_free(ptr); }
    return np;
}

/* --- state --------------------------------------------------------------- */
#define MAX_SCRIPTS 8
#define NAME_MAX_LEN 32
/* Matches the WebSocket receive buffer: a larger limit here could not be
 * reached over the wire anyway. */
/* The cap on a whole draft, not on one frame. Held in the system heap, which
 * has ~190 KB free, so this is generous without being reckless. */
#define DRAFT_MAX 16384

/* How many slices a script may burn without ever yielding on its own before it
 * is killed. At 20000 instructions a slice that is a lot of real work, so this
 * only catches something genuinely stuck. */
#define FORCED_YIELD_LIMIT 300

typedef struct {
    bool  used;
    char  name[NAME_MAX_LEN + 1];
    int   co_ref;        /* registry ref to the coroutine, LUA_NOREF when idle */
    int   env_ref;       /* registry ref to the script's environment table */
    bool  running;
    bool  dead;
    char  err[160];
    uint32_t resumes;
    uint32_t wake_at_ms;   /* 0 = runnable now; set by sys.sleep */
    uint32_t forced_yields; /* consecutive hook-forced yields; reset by a real one */
    char  *draft;        /* NULL unless a RAM draft exists */
    size_t draft_len;
} script_t;

static lua_State *L;
static script_t scripts[MAX_SCRIPTS];
static bool s_ready;
static uint32_t s_hook_kills;

bool lrt_ready(void) { return s_ready; }

static void teardown(script_t *s);

static script_t *find(const char *name) {
    for (int i = 0; i < MAX_SCRIPTS; i++) {
        if (scripts[i].used && strcmp(scripts[i].name, name) == 0) { return &scripts[i]; }
    }
    return NULL;
}

static script_t *alloc_script(const char *name) {
    script_t *s = find(name);
    if (s) { return s; }
    for (int i = 0; i < MAX_SCRIPTS; i++) {
        if (!scripts[i].used) {
            memset(&scripts[i], 0, sizeof(scripts[i]));
            scripts[i].used = true;
            scripts[i].co_ref = LUA_NOREF;
            scripts[i].env_ref = LUA_NOREF;
            snprintf(scripts[i].name, sizeof(scripts[i].name), "%s", name);
            return &scripts[i];
        }
    }
    return NULL;
}

/* --- the instruction budget ----------------------------------------------
 * The backstop against a script that never yields. It must exist before the
 * first script is written, not after the first lockup. */
static void hook(lua_State *co, lua_Debug *ar) {
    (void)ar;
    /* Inside a coroutine this yields; the scheduler resumes it next pass, so
     * ordinary long work still finishes, just politely. */
    if (lua_isyieldable(co)) {
        lua_yield(co, 0);
    } else {
        s_hook_kills++;
        luaL_error(co, "instruction budget exceeded without yielding");
    }
}

/* --- source resolution: draft -> staged -> committed ---------------------- */
static bool read_fs(const char *path, char *buf, size_t cap, size_t *out_len) {
    size_t sz = 0;
    if (!fs_stat(path, &sz) || sz >= cap) { return false; }
    int h = fs_begin_read(path, NULL);
    if (h < 0) { return false; }
    int n = fs_read(h, (uint8_t *)buf, sz);
    fs_end_read(h);
    if (n < 0) { return false; }
    buf[n] = 0;
    *out_len = (size_t)n;
    return true;
}

static char src_buf[DRAFT_MAX + 1];

/* Returns the tier the source came from, or NULL when there is none. */
static const char *resolve_src(script_t *s, const char *name,
                               const char **src, size_t *len) {
    if (s && s->draft) { *src = s->draft; *len = s->draft_len; return "draft"; }
    char path[80];
    snprintf(path, sizeof(path), "staged/%s.lua", name);
    if (read_fs(path, src_buf, sizeof(src_buf), len)) { *src = src_buf; return "staged"; }
    snprintf(path, sizeof(path), "lua/%s.lua", name);
    if (read_fs(path, src_buf, sizeof(src_buf), len)) { *src = src_buf; return "committed"; }
    return NULL;
}

/* --- start / stop --------------------------------------------------------- */
static bool load_tier(script_t *s, const char *src, size_t len, const char *tier,
                      const char **err) {
    char chunkname[48];
    snprintf(chunkname, sizeof(chunkname), "@%s(%s)", s->name, tier);
    if (luaL_loadbuffer(L, src, len, chunkname) != LUA_OK) {
        snprintf(s->err, sizeof(s->err), "%s", lua_tostring(L, -1));
        lua_pop(L, 1);
        *err = s->err;
        return false;
    }
    /* Each script gets its own environment, with the shared bindings behind it
     * via __index. A script cannot then clobber another's globals. */
    lua_newtable(L);
    lua_newtable(L);
    lua_pushglobaltable(L);
    lua_setfield(L, -2, "__index");
    lua_setmetatable(L, -2);
    lua_pushvalue(L, -1);
    s->env_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_setupvalue(L, -2, 1);                      /* _ENV */

    lua_State *co = lua_newthread(L);
    lua_pushvalue(L, -2);                          /* the loaded chunk */
    lua_xmove(L, co, 1);
    s->co_ref = luaL_ref(L, LUA_REGISTRYINDEX);    /* pops the thread */
    lua_pop(L, 1);                                 /* the chunk */

    lua_sethook(co, hook, LUA_MASKCOUNT, CONFIG_LUA_HOOK_BUDGET);
    s->running = true;
    s->dead = false;
    s->err[0] = 0;
    return true;
}

bool lrt_start(const char *name, const char **err) {
    if (!s_ready) { *err = "lua runtime is not up"; return false; }
    script_t *s = alloc_script(name);
    if (!s) { *err = "too many scripts"; return false; }
    if (s->running) { teardown(s); }

    const char *src = NULL; size_t len = 0;
    const char *tier = resolve_src(s, name, &src, &len);
    if (!tier) { *err = "no draft, staged or committed source for that name"; return false; }

    if (load_tier(s, src, len, tier, err)) {
        printf("lua: %s started from %s (%u bytes)\n", name, tier, (unsigned)len);
        return true;
    }
    /* A bad DRAFT must not leave nothing running: fall back to the tier under
     * it and say so. This is the property that makes editing over the wire
     * safe, since the thing being edited is also how you reach the board. */
    if (strcmp(tier, "draft") == 0) {
        char msg[200];
        snprintf(msg, sizeof(msg), "{\"script\":\"%s\",\"tier\":\"draft\",\"error\":\"", name);
        size_t n = strlen(msg);
        n += api_json_escape(msg + n, sizeof(msg) - n - 4, s->err);
        snprintf(msg + n, sizeof(msg) - n, "\"}");
        api_emit("script.error", msg);

        free(s->draft); s->draft = NULL; s->draft_len = 0;
        const char *src2 = NULL; size_t len2 = 0;
        const char *tier2 = resolve_src(s, name, &src2, &len2);
        if (tier2 && load_tier(s, src2, len2, tier2, err)) {
            printf("lua: %s draft failed to load; fell back to %s\n", name, tier2);
            return true;
        }
    }
    return false;
}

/* Tears down the coroutine and environment but KEEPS the entry and its draft.
 * A draft is source, not runtime state: destroying it here made reload (which
 * is stop-then-start) lose the very thing it was asked to reload. */
static void teardown(script_t *s) {
    if (s->co_ref != LUA_NOREF)  { luaL_unref(L, LUA_REGISTRYINDEX, s->co_ref);  s->co_ref = LUA_NOREF; }
    if (s->env_ref != LUA_NOREF) { luaL_unref(L, LUA_REGISTRYINDEX, s->env_ref); s->env_ref = LUA_NOREF; }
    /* Phase 7 releases this script's pin and bus claims here too. */
    s->running = false;
    s->dead = false;
}

bool lrt_stop(const char *name) {
    script_t *s = find(name);
    if (!s) { return false; }
    teardown(s);
    return true;
}

bool lrt_reload(const char *name, const char **err) {
    script_t *s = find(name);
    if (s) { teardown(s); }
    return lrt_start(name, err);
}

/* --- drafts / staging / commit -------------------------------------------- */
bool lrt_draft_chunk(const char *name, size_t offset, const char *src, size_t len,
                     const char **err) {
    if (offset + len > DRAFT_MAX) { *err = "draft exceeds DRAFT_MAX"; return false; }
    script_t *s = alloc_script(name);
    if (!s) { *err = "too many scripts"; return false; }

    if (offset == 0) {
        free(s->draft);
        s->draft = NULL;
        s->draft_len = 0;
    } else if (offset != s->draft_len) {
        /* Refuse a gap rather than silently assembling corrupt source, which
         * would surface later as a baffling syntax error. */
        *err = "chunk offset does not continue the draft";
        return false;
    }
    char *grown = realloc(s->draft, offset + len + 1);
    if (!grown) { *err = "out of memory"; return false; }
    memcpy(grown + offset, src, len);
    grown[offset + len] = 0;
    s->draft = grown;
    s->draft_len = offset + len;
    return true;
}

bool lrt_draft_set(const char *name, const char *src, size_t len, const char **err) {
    return lrt_draft_chunk(name, 0, src, len, err);
}

size_t lrt_draft_len(const char *name) {
    script_t *s = find(name);
    return s ? s->draft_len : 0;
}

static bool write_fs(const char *path, const char *data, size_t len, const char **err) {
    if (fs_begin_write(path) < 0) { *err = "filesystem busy or unwritable"; return false; }
    if (fs_write((const uint8_t *)data, len) < 0) { fs_end_write(); *err = "write failed"; return false; }
    return fs_end_write() >= 0;
}

bool lrt_stage(const char *name, const char **err) {
    script_t *s = find(name);
    if (!s || !s->draft) { *err = "no draft to stage"; return false; }
    char path[80];
    snprintf(path, sizeof(path), "staged/%s.lua", name);
    return write_fs(path, s->draft, s->draft_len, err);
}

bool lrt_commit(const char *name, const char **err) {
    const char *src = NULL; size_t len = 0;
    script_t *s = find(name);
    /* Commit promotes whatever is currently in effect, draft or staged. */
    if (s && s->draft) { src = s->draft; len = s->draft_len; }
    else {
        char path[80];
        snprintf(path, sizeof(path), "staged/%s.lua", name);
        if (!read_fs(path, src_buf, sizeof(src_buf), &len)) { *err = "nothing staged"; return false; }
        src = src_buf;
    }
    char dst[80];
    snprintf(dst, sizeof(dst), "lua/%s.lua", name);
    return write_fs(dst, src, len, err);
}

bool lrt_revert(const char *name) {
    script_t *s = find(name);
    if (s && s->draft) {
        free(s->draft); s->draft = NULL; s->draft_len = 0;
        return true;   /* the staged or committed tier beneath is now in effect */
    }
    char path[80];
    snprintf(path, sizeof(path), "staged/%s.lua", name);
    return fs_delete(path) >= 0;
}

/* --- the scheduler -------------------------------------------------------- */
void lrt_tick(void) {
    if (!s_ready) { return; }
    for (int i = 0; i < MAX_SCRIPTS; i++) {
        script_t *s = &scripts[i];
        if (!s->used || !s->running || s->dead || s->co_ref == LUA_NOREF) { continue; }
        /* sys.sleep yields a wake time rather than busy-waiting, because a
         * busy-wait here would stall the radio and the network with it. */
        if (s->wake_at_ms &&
            (int32_t)(to_ms_since_boot(get_absolute_time()) - s->wake_at_ms) < 0) {
            continue;
        }
        s->wake_at_ms = 0;

        lua_rawgeti(L, LUA_REGISTRYINDEX, s->co_ref);
        lua_State *co = lua_tothread(L, -1);
        lua_pop(L, 1);
        if (!co) { s->dead = true; continue; }

        int nres = 0;
        int rc = lua_resume(co, L, 0, &nres);
        s->resumes++;
        if (rc == LUA_YIELD) {
            /* A yield carrying one integer is sys.sleep telling us when it
             * wants to run again; a bare yield (the instruction hook) means
             * "resume me next pass". */
            if (nres >= 1 && lua_isinteger(co, -1)) {
                s->wake_at_ms = (uint32_t)lua_tointeger(co, -1);
                s->forced_yields = 0;          /* it yielded of its own accord */
            } else {
                /* Forced by the instruction hook. Tolerated for a while --
                 * ordinary long work should still finish -- but a script that
                 * NEVER yields voluntarily is a runaway, and leaving it to
                 * consume a slice on every pass forever is not a fix. */
                if (++s->forced_yields > FORCED_YIELD_LIMIT) {
                    lua_pop(co, nres);
                    s->running = false;
                    s->dead = true;
                    s_hook_kills++;
                    snprintf(s->err, sizeof(s->err),
                             "killed: %lu consecutive slices without yielding "
                             "(add a sys.sleep, or yield)",
                             (unsigned long)s->forced_yields);
                    char msg[300];
                    size_t n = (size_t)snprintf(msg, sizeof(msg),
                                                "{\"script\":\"%s\",\"killed\":true,\"error\":\"", s->name);
                    n += api_json_escape(msg + n, sizeof(msg) - n - 4, s->err);
                    snprintf(msg + n, sizeof(msg) - n, "\"}");
                    api_emit("script.error", msg);
                    printf("lua: %s %s\n", s->name, s->err);
                    continue;
                }
            }
            lua_pop(co, nres);
            continue;
        }
        s->running = false;
        if (rc != LUA_OK) {
            s->dead = true;
            snprintf(s->err, sizeof(s->err), "%s",
                     lua_tostring(co, -1) ? lua_tostring(co, -1) : "error");
            char msg[320];
            size_t n = (size_t)snprintf(msg, sizeof(msg), "{\"script\":\"%s\",\"error\":\"", s->name);
            n += api_json_escape(msg + n, sizeof(msg) - n - 4, s->err);
            snprintf(msg + n, sizeof(msg) - n, "\"}");
            api_emit("script.error", msg);
            printf("lua: %s died: %s\n", s->name, s->err);
        }
    }
}

/* --- eval ----------------------------------------------------------------- */
bool lrt_eval(const char *name, const char *chunk, char *out, size_t cap) {
    if (!s_ready) { snprintf(out, cap, "lua runtime is not up"); return false; }
    char buf[600];
    /* Try as an expression first so `tlv.decode(x)` prints its value, then as
     * a statement. This is what makes the console feel like a REPL. */
    snprintf(buf, sizeof(buf), "return %s", chunk);
    if (luaL_loadstring(L, buf) != LUA_OK) {
        lua_pop(L, 1);
        if (luaL_loadstring(L, chunk) != LUA_OK) {
            snprintf(out, cap, "%s", lua_tostring(L, -1));
            lua_pop(L, 1);
            return false;
        }
    }
    script_t *s = name && *name ? find(name) : NULL;
    if (s && s->env_ref != LUA_NOREF) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, s->env_ref);
        lua_setupvalue(L, -2, 1);          /* run inside the script's own state */
    }
    if (lua_pcall(L, 0, 1, 0) != LUA_OK) {
        snprintf(out, cap, "%s", lua_tostring(L, -1));
        lua_pop(L, 1);
        return false;
    }
    const char *r = luaL_tolstring(L, -1, NULL);
    snprintf(out, cap, "%s", r ? r : "nil");
    lua_pop(L, 2);
    return true;
}

/* --- introspection -------------------------------------------------------- */
size_t lrt_list_json(char *dst, size_t cap) {
    size_t n = (size_t)snprintf(dst, cap, "[");
    bool first = true;
    for (int i = 0; i < MAX_SCRIPTS; i++) {
        script_t *s = &scripts[i];
        if (!s->used) { continue; }
        char esc[180];
        api_json_escape(esc, sizeof(esc), s->err);
        n += (size_t)snprintf(dst + n, cap - n,
            "%s{\"name\":\"%s\",\"running\":%s,\"dead\":%s,\"draft\":%s,\"resumes\":%lu,\"error\":\"%s\"}",
            first ? "" : ",", s->name, s->running ? "true" : "false",
            s->dead ? "true" : "false", s->draft ? "true" : "false",
            (unsigned long)s->resumes, esc);
        first = false;
    }
    return n + (size_t)snprintf(dst + n, cap - n, "]");
}

size_t lrt_stats_json(char *dst, size_t cap) {
    return (size_t)snprintf(dst, cap,
        "{\"arena_bytes\":%u,\"used\":%u,\"peak\":%u,\"hook_kills\":%lu,\"budget\":%d}",
        (unsigned)ARENA_BYTES, (unsigned)heap_used, (unsigned)heap_peak,
        (unsigned long)s_hook_kills, CONFIG_LUA_HOOK_BUDGET);
}

void lrt_init(void) {
    heap_init();
    L = lua_newstate(lrt_alloc, NULL);
    if (!L) {
        printf("lua: could not create state in a %u KB arena\n", CONFIG_LUA_HEAP_KB);
        return;
    }
    /* Only the libraries that cannot reach the host. io, os, package and debug
     * are not compiled in at all -- see third_party/lua/CMakeLists.txt. */
    luaL_requiref(L, LUA_GNAME, luaopen_base, 1);      lua_pop(L, 1);
    luaL_requiref(L, LUA_TABLIBNAME, luaopen_table, 1); lua_pop(L, 1);
    luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 1); lua_pop(L, 1);
    luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math, 1); lua_pop(L, 1);
    luaL_requiref(L, LUA_COLIBNAME, luaopen_coroutine, 1); lua_pop(L, 1);
    luaL_requiref(L, LUA_UTF8LIBNAME, luaopen_utf8, 1); lua_pop(L, 1);

    /* dofile/loadfile would reach a filesystem the sandbox does not expose. */
    lua_pushnil(L); lua_setglobal(L, "dofile");
    lua_pushnil(L); lua_setglobal(L, "loadfile");

    lbind_open_all(L);

    s_ready = true;
    printf("lua: 5.4.8 up, %u KB arena, %d-instruction slice\n",
           CONFIG_LUA_HEAP_KB, CONFIG_LUA_HOOK_BUDGET);
}

#else   /* !CONFIG_LUA_ENABLE */
void lrt_init(void) {}
bool lrt_ready(void) { return false; }
void lrt_tick(void) {}
#endif
