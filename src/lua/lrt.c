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
/* The cap on a whole draft, and on one module's source, not on one frame --
 * script.draft arrives in chunks precisely so the WebSocket receive buffer
 * does not have to be this large.
 *
 * 32 KB rather than 16: a real device profile is not small. The A2687's is
 * about 24 KB, most of it the comments that make it the protocol's
 * documentation, and at 16 KB it could be uploaded to the filesystem and then
 * not loaded -- which failed as "module not found", a thoroughly misleading
 * way to say "too big". The buffer is static so the link-time RAM check sees
 * it, and 16 KB more of ~180 KB free is affordable.
 *
 * The real fix is a streaming loader: lua_load with a reader would bound
 * module size by flash rather than by RAM. Worth doing when a profile
 * approaches this ceiling rather than pre-emptively. */
#define DRAFT_MAX 32768

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
 * first script is written, not after the first lockup.
 *
 * The count alone is not enough, and the gap is not theoretical. lua_sethook
 * counts VM instructions, and a C function is ONE instruction that cannot be
 * preempted, so a loop whose body is mostly a slow binding executes very few
 * VM instructions per unit of wall time. Measured: `while ... crypto.ecdh()`
 * held the whole cooperative loop for 3 seconds -- USB, lwIP and BTstack with
 * it -- and this hook never once fired, because 76 iterations is nowhere near
 * the 20000-instruction budget.
 *
 * So the hook also watches the clock. It still cannot interrupt the C call in
 * progress, but it fires on the first VM instruction after it, which bounds
 * the damage to one call instead of a whole loop of them. */
#define SLICE_MAX_MS 25

static uint32_t s_slice_start_ms;
static uint32_t s_slice_overruns;
static lua_State *s_current_co;   /* whoever lrt_tick is resuming right now */

static void hook(lua_State *co, lua_Debug *ar);

/* Called BY an expensive C binding, just before it returns.
 *
 * A C function cannot be preempted, so the only way to bound the damage from
 * a slow one is to give the loop control the instant it finishes. Arming the
 * hook for a single instruction does exactly that: the very next VM
 * instruction after the binding returns forces a yield. lrt_tick restores the
 * normal budget on the next resume.
 *
 * The result is that `while true do crypto.ecdh() end` costs the loop ONE
 * call, not an unbounded run of them -- which is what it cost before, because
 * 20000 VM instructions is over a thousand iterations and forty seconds of
 * wall time. */
void lrt_yield_soon(void) {
    if (s_current_co) { lua_sethook(s_current_co, hook, LUA_MASKCOUNT, 1); }
}

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

/* Shared by script loading and by require(). Safe because luaL_loadbuffer
 * compiles out of it immediately: by the time a chunk RUNS and can require
 * something else, its own source is no longer needed. */
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

/* --- require --------------------------------------------------------------
 * A protocol layer outgrows one file quickly -- a profile, its field
 * dictionary, shared helpers -- and without require the only way to split it
 * is to paste. So module resolution goes through the SAME draft -> staged ->
 * committed overlay a script does, which is what makes a multi-file profile
 * editable from the browser with no flash write.
 *
 * Lua's own require is not available: package and loadlib are not compiled
 * in, deliberately, because they would reach a real filesystem and a real
 * dlopen. This is a replacement, not a wrapper.
 *
 * A dot in the name is a directory separator, so require 'profiles.acme' is
 * lua/profiles/acme.lua. Modules are cached in a registry table, so a module
 * required twice is loaded once -- and so a module CHANGED on the device is
 * not reloaded until the runtime restarts the script that required it, which
 * is the same rule scripts themselves follow.
 *
 * A module gets the caller's environment, so it sees the same bindings and
 * cannot reach another script's globals. */
#define MODULE_CACHE "btgw.modules"

static bool read_module_src(const char *name, const char **src, size_t *len) {
    /* dots -> slashes, and nothing else: no "..", no leading slash, so a
     * module name cannot climb out of lua/. */
    char rel[64];
    size_t j = 0;
    for (size_t i = 0; name[i] && j + 1 < sizeof rel; i++) {
        char ch = name[i];
        if (ch == '.') { ch = '/'; }
        if (ch == '/' && (j == 0 || rel[j - 1] == '/')) { return false; }
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' || ch == '/')) {
            return false;
        }
        rel[j++] = ch;
    }
    rel[j] = 0;
    if (!j) { return false; }

    char path[96];
    snprintf(path, sizeof path, "staged/%s.lua", rel);
    if (read_fs(path, src_buf, sizeof(src_buf), len)) { *src = src_buf; return true; }
    snprintf(path, sizeof path, "lua/%s.lua", rel);
    if (read_fs(path, src_buf, sizeof(src_buf), len)) { *src = src_buf; return true; }
    return false;
}

static int l_require(lua_State *Ls) {
    const char *name = luaL_checkstring(Ls, 1);

    if (lua_getfield(Ls, LUA_REGISTRYINDEX, MODULE_CACHE) != LUA_TTABLE) {
        lua_pop(Ls, 1);
        lua_newtable(Ls);
        lua_pushvalue(Ls, -1);
        lua_setfield(Ls, LUA_REGISTRYINDEX, MODULE_CACHE);
    }
    if (lua_getfield(Ls, -1, name) != LUA_TNIL) { return 1; }   /* cached */
    lua_pop(Ls, 1);

    const char *src = NULL;
    size_t len = 0;
    if (!read_module_src(name, &src, &len)) {
        return luaL_error(Ls, "module '%s' not found in staged/ or lua/", name);
    }

    char chunkname[80];
    snprintf(chunkname, sizeof chunkname, "@%s", name);
    if (luaL_loadbuffer(Ls, src, len, chunkname) != LUA_OK) {
        return luaL_error(Ls, "module '%s': %s", name, lua_tostring(Ls, -1));
    }

    /* Run it in the CALLER's environment so a module sees the same bindings
     * and the same sandbox as the script that asked for it. */
    lua_Debug ar;
    if (lua_getstack(Ls, 1, &ar) && lua_getinfo(Ls, "f", &ar)) {
        if (lua_getupvalue(Ls, -1, 1)) {        /* pushes the caller's _ENV */
            lua_remove(Ls, -2);                 /* drop the caller function */
        } else {
            lua_pop(Ls, 1);
            lua_pushglobaltable(Ls);
        }
    } else {
        lua_pushglobaltable(Ls);
    }
    lua_setupvalue(Ls, -2, 1);                  /* chunk's _ENV := that env */

    lua_call(Ls, 0, 1);
    if (lua_isnil(Ls, -1)) {
        lua_pop(Ls, 1);
        lua_pushboolean(Ls, 1);      /* a module that returns nothing is still loaded */
    }
    lua_pushvalue(Ls, -1);
    lua_setfield(Ls, -3, name);      /* cache[name] = module */
    return 1;
}

/* Drop every cached module.
 *
 * Without this, require() defeats the draft tier for the one file you are
 * actually editing: push a new profile, restart the script, and the OLD
 * module is still what runs, because it is cached. That was not theoretical
 * -- it cost a confusing hardware run where a freshly uploaded profile
 * appeared to ignore a step that had just been added to it.
 *
 * Called on every script load, so "push and restart" means what it looks
 * like. Modules another script already holds a reference to are unaffected;
 * this only decides what the NEXT require reads. */
static void flush_modules(void) {
    if (!L) { return; }
    lua_newtable(L);
    lua_setfield(L, LUA_REGISTRYINDEX, MODULE_CACHE);
}

/* Drop ONE module from the cache, so its memory can be reclaimed.
 *
 * A profile's handshake is dead weight the moment the session is up: several
 * kilobytes of compiled chunk that will not be called again unless the link
 * drops. Clearing the cache entry is only half of it -- anything still
 * holding a reference to the module table keeps it alive, so the caller must
 * drop its own references too and then collect. Returns true if there was an
 * entry to remove.
 *
 * The trade is worth stating: re-requiring later re-reads the source from
 * flash and recompiles it, and compiling is the expensive part. Unloading
 * buys steady-state room at the cost of a peak on reconnect. */
bool lrt_unload_module(const char *name) {
    if (!L || !name) { return false; }
    if (lua_getfield(L, LUA_REGISTRYINDEX, MODULE_CACHE) != LUA_TTABLE) {
        lua_pop(L, 1);
        return false;
    }
    bool had = lua_getfield(L, -1, name) != LUA_TNIL;
    lua_pop(L, 1);
    if (had) {
        lua_pushnil(L);
        lua_setfield(L, -2, name);
    }
    lua_pop(L, 1);
    return had;
}

/* Bytes the Lua heap is actually holding, after a full collection. The arena
 * allocator's own figure includes blocks on its free list, which is the right
 * number for "will the next allocation fit" and the wrong one for "how much
 * is live". */
size_t lrt_collect(void) {
    if (!L) { return 0; }
    lua_gc(L, LUA_GCCOLLECT);
    lua_gc(L, LUA_GCCOLLECT);   /* twice: finalizers can free more */
    return heap_used;
}

/* --- start / stop --------------------------------------------------------- */
static bool load_tier(script_t *s, const char *src, size_t len, const char *tier,
                      const char **err) {
    flush_modules();
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

size_t lrt_source_read(const char *name, size_t offset, char *dst, size_t cap,
                       const char **tier, size_t *total) {
    *tier = NULL;
    *total = 0;
    if (cap == 0) { return 0; }

    script_t *s = find(name);
    const char *src = NULL;
    size_t len = 0;
    const char *t = resolve_src(s, name, &src, &len);
    if (!t) { return 0; }

    *tier = t;
    *total = len;
    if (offset >= len) { return 0; }
    size_t n = len - offset;
    if (n > cap - 1) { n = cap - 1; }
    memcpy(dst, src + offset, n);
    dst[n] = 0;
    return n;
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
        /* Restore the ordinary budget: a binding may have armed a one-shot
         * hook to hand control back after a slow call. */
        lua_sethook(co, hook, LUA_MASKCOUNT, CONFIG_LUA_HOOK_BUDGET);
        s_current_co = co;
        s_slice_start_ms = to_ms_since_boot(get_absolute_time());
        int rc = lua_resume(co, L, 0, &nres);
        s_current_co = NULL;
        if ((uint32_t)(to_ms_since_boot(get_absolute_time()) - s_slice_start_ms)
                > SLICE_MAX_MS) {
            /* One slow C binding, not a loop of them: report but do not kill.
             * A single P-256 operation is ~41 ms on this part and is a normal
             * thing for a handshake to do. */
            s_slice_overruns++;
        }
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
        "{\"arena_bytes\":%u,\"used\":%u,\"peak\":%u,\"hook_kills\":%lu,"
        "\"budget\":%d,\"slice_overruns\":%lu,\"slice_max_ms\":%d}",
        (unsigned)ARENA_BYTES, (unsigned)heap_used, (unsigned)heap_peak,
        (unsigned long)s_hook_kills, CONFIG_LUA_HOOK_BUDGET,
        (unsigned long)s_slice_overruns, SLICE_MAX_MS);
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

    /* dofile/loadfile would reach a filesystem the sandbox does not expose.
     * require is replaced rather than removed -- see l_require. */
    lua_pushnil(L); lua_setglobal(L, "dofile");
    lua_pushnil(L); lua_setglobal(L, "loadfile");
    lua_pushcfunction(L, l_require); lua_setglobal(L, "require");

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
