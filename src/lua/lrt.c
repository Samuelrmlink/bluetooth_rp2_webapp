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

#if CONFIG_BT_LE
#include "bt/le/central.h"
#endif

#include "lua/lheap.h"
#include "lua/lwait.h"

#if CONFIG_LUA_ENABLE

/* The arena lives in lheap.c so it can be tested on a host: see the comment
 * there, and test/host/heap_test.c, which asserts the invariants that two
 * reverted attempts at its in-place realloc broke. */

/* Lua's allocator hook. The work is in lheap_realloc; this only drops `ud`. */
static void *lrt_alloc(void *ud, void *ptr, size_t osize, size_t nsize) {
    (void)ud;
    return lheap_realloc(ptr, osize, nsize);
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
    uint32_t runtime_ms;   /* accumulated slice time, for taskstat */
    uint32_t wake_at_ms;   /* 0 = runnable now; set by sys.sleep */
    /* Why this script's last blocking wait ended. The resume ALWAYS carries
     * this: a coroutine is never woken without being told why. */
    lwait_reason_t wake_reason;
    uint32_t forced_yields; /* consecutive hook-forced yields; reset by a real one */
    char  *draft;        /* NULL unless a RAM draft exists */
    size_t draft_len;
#if CONFIG_BT_LE
    /* Links this script opened, so teardown can drop them.
     *
     * Without this a script that dies -- and "not enough memory" kills one
     * readily -- leaves its BLE connection up with nothing to close it. That
     * is not merely untidy: le_radio_busy() counts any active link, so the
     * one-radio guard then refuses EVERY WiFi join, permanently, and the only
     * way out is a reboot. Two dead scripts did exactly that. */
    uint8_t owned[CONFIG_BT_MAX_LINKS][6];
    uint8_t n_owned;
#endif
    /* What was compiled the last time this slot loaded: the tier it came from
     * and an FNV-1a hash of the exact bytes. Reported by script.loaded so a
     * client can tell "running what is on flash" from "running something the
     * file no longer says" -- which nothing could tell before, because a
     * running script keeps the code it compiled however the file changes. */
    uint8_t  src_tier;       /* SRC_TIER_* */
    uint32_t src_hash;
} script_t;

#define SRC_TIER_NONE      0
#define SRC_TIER_DRAFT     1
#define SRC_TIER_STAGED    2
#define SRC_TIER_COMMITTED 3

static uint8_t tier_code(const char *tier) {
    if (!tier) { return SRC_TIER_NONE; }
    if (tier[0] == 'd') { return SRC_TIER_DRAFT; }
    if (tier[0] == 's') { return SRC_TIER_STAGED; }
    return SRC_TIER_COMMITTED;
}

static const char *tier_name(uint8_t t) {
    return t == SRC_TIER_DRAFT ? "draft" : t == SRC_TIER_STAGED ? "staged"
         : t == SRC_TIER_COMMITTED ? "committed" : "none";
}

/* FNV-1a, 32-bit. Chosen for being a dozen lines on both sides of the wire --
 * the web editor computes the same function over its buffer -- not for
 * strength: this detects change, it does not authenticate anything. */
static uint32_t fnv1a(const void *data, size_t len) {
    const uint8_t *b = (const uint8_t *)data;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++) { h ^= b[i]; h *= 16777619u; }
    return h;
}

static lua_State *L;
static script_t scripts[MAX_SCRIPTS];
static bool s_ready;
static uint32_t s_hook_kills;

bool lrt_ready(void) { return s_ready; }

static void teardown(script_t *s);
static void release_modules(int slot);

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

/* --- blocking waits -------------------------------------------------------
 * The scheduler half of the contract in lwait.h. A script blocked on a token
 * is not resumed at all until that wait resolves, which is what turns a
 * profile that polled bt.notify_take() every 5 ms into one that runs when its
 * data arrives.
 *
 * This complements the instruction budget below rather than duplicating it:
 * the hook catches a coroutine that SPINS, and this catches one that BLOCKS. */
static int s_current_slot = -1;

int lrt_current_slot(void) { return s_current_slot; }

#if CONFIG_BT_LE
/* Record/forget a link against the script that opened it. Called from the
 * bt.connect and bt.disconnect bindings, which are the only way a link is
 * ever opened -- the JSON API has no connect op. */
void lrt_note_link(const uint8_t *addr) {
    if (s_current_slot < 0 || s_current_slot >= MAX_SCRIPTS) { return; }
    script_t *s = &scripts[s_current_slot];
    for (int i = 0; i < s->n_owned; i++) {
        if (memcmp(s->owned[i], addr, 6) == 0) { return; }
    }
    if (s->n_owned >= CONFIG_BT_MAX_LINKS) { return; }
    memcpy(s->owned[s->n_owned++], addr, 6);
}

void lrt_forget_link(const uint8_t *addr) {
    if (s_current_slot < 0 || s_current_slot >= MAX_SCRIPTS) { return; }
    script_t *s = &scripts[s_current_slot];
    for (int i = 0; i < s->n_owned; i++) {
        if (memcmp(s->owned[i], addr, 6) == 0) {
            /* Order does not matter, so fill the hole from the end. */
            memcpy(s->owned[i], s->owned[s->n_owned - 1], 6);
            s->n_owned--;
            return;
        }
    }
}
#endif

lwait_reason_t lrt_block_current(uint32_t token, uint8_t link, uint32_t timeout_ms) {
    if (s_current_slot < 0) { return LWAIT_CANCELLED; }
    return lwait_arm((uint8_t)s_current_slot, token, link, timeout_ms,
                     to_ms_since_boot(get_absolute_time()));
}

lwait_reason_t lrt_wake_reason(void) {
    if (s_current_slot < 0) { return LWAIT_CANCELLED; }
    return scripts[s_current_slot].wake_reason;
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

/* --- who is using which module -------------------------------------------
 * require() caches globally, which is right -- two scripts sharing a codec
 * should share one copy -- but it means nothing is ever released. A profile's
 * modules stayed resident after its device disconnected and its script
 * exited, and on a 128 KB arena that is the difference between the second
 * profile loading and failing with "not enough memory".
 *
 * So each cached module records which script slots required it, as a bitmask.
 * When a script goes -- exits, is stopped, is killed, or loses its link --
 * its bit is cleared, and a module nobody is left using is dropped from the
 * cache. This is the "unload when the LAST device using them disconnects"
 * rule: it falls out of reference counting rather than needing a device
 * model, because a script is what owns a device in the first place.
 *
 * A module required from outside a script (an ops handler, script.eval) is
 * PINNED: there is no slot to attribute it to and no later event that would
 * release it, so it stays until sys.unload asks for it by name. */
#define MAX_TRACKED_MODULES 24
#define MODULE_PINNED       0x80000000u

typedef struct {
    char     name[48];
    uint32_t users;          /* bit per script slot, plus MODULE_PINNED */
    /* As for scripts: where the resident copy was read from, and a hash of
     * it. Five bytes a slot, and the path needs no storing -- it follows
     * from the name and the tier (see module_path()). */
    uint8_t  tier;
    uint32_t hash;
} module_use_t;

static module_use_t s_mod_use[MAX_TRACKED_MODULES];

static void module_note_user(const char *name, int slot) {
    uint32_t bit = (slot >= 0 && slot < MAX_SCRIPTS)
                 ? (1u << slot) : MODULE_PINNED;
    int free_i = -1;
    for (int i = 0; i < MAX_TRACKED_MODULES; i++) {
        if (s_mod_use[i].name[0] == 0) {
            if (free_i < 0) { free_i = i; }
            continue;
        }
        if (strcmp(s_mod_use[i].name, name) == 0) {
            s_mod_use[i].users |= bit;
            return;
        }
    }
    if (free_i < 0) {
        /* Out of tracking slots: pin it rather than risk unloading a module
         * that someone is using. Losing memory beats losing correctness. */
        return;
    }
    snprintf(s_mod_use[free_i].name, sizeof s_mod_use[free_i].name, "%s", name);
    s_mod_use[free_i].users = bit;
}

/* Record what a fresh load of `name` compiled. Only on an actual load, never
 * on a cache hit, which reuses whatever the earlier load recorded. */
static void module_set_src(const char *name, uint8_t tier, uint32_t hash) {
    for (int i = 0; i < MAX_TRACKED_MODULES; i++) {
        if (s_mod_use[i].name[0] && strcmp(s_mod_use[i].name, name) == 0) {
            s_mod_use[i].tier = tier;
            s_mod_use[i].hash = hash;
            return;
        }
    }
}

static void module_forget(const char *name) {
    for (int i = 0; i < MAX_TRACKED_MODULES; i++) {
        if (s_mod_use[i].name[0] && strcmp(s_mod_use[i].name, name) == 0) {
            memset(&s_mod_use[i], 0, sizeof s_mod_use[i]);
            return;
        }
    }
}


static bool read_module_src(const char *name, const char **src, size_t *len,
                            uint8_t *tier) {
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
    if (read_fs(path, src_buf, sizeof(src_buf), len)) {
        *src = src_buf; *tier = SRC_TIER_STAGED; return true;
    }
    snprintf(path, sizeof path, "lua/%s.lua", rel);
    if (read_fs(path, src_buf, sizeof(src_buf), len)) {
        *src = src_buf; *tier = SRC_TIER_COMMITTED; return true;
    }
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
    if (lua_getfield(Ls, -1, name) != LUA_TNIL) {
        /* A cache HIT still adds this script to the users. Missing this is
         * how a module shared by two profiles gets dropped when the first of
         * them exits, leaving the second holding a stale reference. */
        module_note_user(name, lrt_current_slot());
        return 1;
    }
    lua_pop(Ls, 1);

    const char *src = NULL;
    size_t len = 0;
    uint8_t tier = SRC_TIER_NONE;
    if (!read_module_src(name, &src, &len, &tier)) {
        return luaL_error(Ls, "module '%s' not found in staged/ or lua/", name);
    }
    /* Hashed NOW, not after it runs: src is the shared src_buf, and the
     * lua_call below can require further modules, each of which reads its
     * own source into the same buffer. */
    uint32_t hash = fnv1a(src, len);

    char chunkname[80];
    snprintf(chunkname, sizeof chunkname, "@%s", name);
    if (luaL_loadbuffer(Ls, src, len, chunkname) != LUA_OK) {
        return luaL_error(Ls, "module '%s': %s", name, lua_tostring(Ls, -1));
    }

    /*
     * Run it in the CALLER's environment so a module sees the same bindings
     * and the same sandbox as the script that asked for it.
     *
     * Find the upvalue actually NAMED "_ENV" rather than assuming it is
     * upvalue 1. It is upvalue 1 only for a MAIN CHUNK; an ordinary function
     * captures whatever it references, in reference order, and _ENV lands
     * wherever it lands. Every require in this codebase used to sit at the
     * top level of a script or a module, where the assumption held -- the
     * first require from inside a function handed the new module a captured
     * local as its environment, and it died on the first global it touched
     * with "attempt to index a function value (upvalue '_ENV')".
     */
    lua_Debug ar;
    bool have_env = false;
    if (lua_getstack(Ls, 1, &ar) && lua_getinfo(Ls, "f", &ar)) {
        for (int i = 1; ; i++) {
            const char *upname = lua_getupvalue(Ls, -1, i);
            if (!upname) { break; }
            if (strcmp(upname, "_ENV") == 0) {
                lua_remove(Ls, -2);             /* drop the caller function */
                have_env = true;
                break;
            }
            lua_pop(Ls, 1);                     /* not this one */
        }
        if (!have_env) { lua_pop(Ls, 1); }      /* drop the caller function */
    }
    if (!have_env) { lua_pushglobaltable(Ls); }
    lua_setupvalue(Ls, -2, 1);                  /* chunk's _ENV := that env */

    lua_call(Ls, 0, 1);
    module_note_user(name, lrt_current_slot());
    module_set_src(name, tier, hash);
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
/*
 * Release every module this script was the last user of. Called whenever a
 * script stops for ANY reason -- a clean exit, an error, a stop, a reload or
 * a lost link -- because all five leave the same modules resident otherwise.
 */
#if CONFIG_BT_LE
/* Drop the BLE links a script opened.
 *
 * Called from teardown() AND from both paths where a script dies on its own
 * -- a runaway killed by the instruction hook, and a coroutine that ended
 * with an error. Those two do not go through teardown(), which is why an
 * out-of-memory death used to leave its connection up with nothing to close
 * it: le_radio_busy() then stayed true and the one-radio guard refused every
 * WiFi join until the board was rebooted. */
static void drop_links(script_t *s) {
    for (int i = 0; i < s->n_owned; i++) {
        le_disconnect(s->owned[i]);
    }
    s->n_owned = 0;
}
#else
#define drop_links(s) ((void)0)
#endif

static void release_modules(int slot) {
    if (slot < 0 || slot >= MAX_SCRIPTS) { return; }
    uint32_t bit = 1u << slot;
    size_t before = lheap_used();
    int freed = 0;
    for (int i = 0; i < MAX_TRACKED_MODULES; i++) {
        if (!s_mod_use[i].name[0]) { continue; }
        if (!(s_mod_use[i].users & bit)) { continue; }
        s_mod_use[i].users &= ~bit;
        if (s_mod_use[i].users != 0) { continue; }   /* someone else has it */
        char nm[48];
        snprintf(nm, sizeof nm, "%s", s_mod_use[i].name);
        if (lrt_unload_module(nm)) { freed++; }
    }
    if (freed) {
        lrt_collect();
        printf("lua: released %d module(s) with no users left, heap %u -> %u B\n",
               freed, (unsigned)before, (unsigned)lheap_used());
    }
}

bool lrt_unload_module(const char *name) {
    if (!L || !name) { return false; }
    module_forget(name);
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
    return lheap_used();
}

/* --- start / stop --------------------------------------------------------- */
static bool load_tier(script_t *s, const char *src, size_t len, const char *tier,
                      const char **err) {
    flush_modules();
    uint32_t hash = fnv1a(src, len);
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
    s->src_tier = tier_code(tier);
    s->src_hash = hash;
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
    /* Anything this script was the last user of goes with it. */
    release_modules((int)(s - scripts));

    /* Including its BLE links: a script is the only thing that can open one,
     * so if it is going away and nothing closes them, nothing ever will. */
    drop_links(s);

    /* Release the wait BEFORE the coroutine goes: a script stopped while
     * blocked must not leave a waiter pointing at a slot that is about to be
     * reused by the next script to start. */
    lwait_cancel((uint8_t)(s - scripts));

    if (s->co_ref != LUA_NOREF) {
        /* Close the thread rather than merely dropping the reference. A
         * coroutine killed mid-wait -- by a link loss, a stop or a reload --
         * must still run its to-be-closed variables, or every dropped link
         * leaks the ops.register entries, subscriptions and loaded modules
         * its profile took out. lua_closethread makes that guaranteed rather
         * than best-effort, and gives a profile an idiomatic place to put its
         * teardown. */
        lua_rawgeti(L, LUA_REGISTRYINDEX, s->co_ref);
        lua_State *co = lua_tothread(L, -1);
        if (co) { lua_closethread(co, L); }
        lua_pop(L, 1);
        luaL_unref(L, LUA_REGISTRYINDEX, s->co_ref);  s->co_ref = LUA_NOREF;
    }
    if (s->env_ref != LUA_NOREF) { luaL_unref(L, LUA_REGISTRYINDEX, s->env_ref); s->env_ref = LUA_NOREF; }
    /* Phase 7 releases this script's pin and bus claims here too. */
    s->wake_reason = LWAIT_PENDING;
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

    /* The watchdog half of the wait contract. A coroutine blocked on an event
     * that never arrives is exactly as stuck as one blocked forever, and
     * nothing else in the system would ever notice it. */
    lwait_expire(to_ms_since_boot(get_absolute_time()));

    for (int i = 0; i < MAX_SCRIPTS; i++) {
        script_t *s = &scripts[i];
        if (!s->used || !s->running || s->dead || s->co_ref == LUA_NOREF) { continue; }

        if (lwait_is_armed((uint8_t)i)) {
            /* Blocked. This is the line that replaces polling: the script is
             * not resumed at all until its wait resolves, rather than woken
             * every pass to ask whether anything has arrived yet. */
            lwait_reason_t r = lwait_state((uint8_t)i);
            if (r == LWAIT_PENDING) { continue; }

            uint32_t now = to_ms_since_boot(get_absolute_time());
            uint32_t waited = lwait_waited_ms((uint8_t)i, now);
            uint8_t  link = lwait_link((uint8_t)i);
            uint32_t tok  = lwait_token((uint8_t)i);
            lwait_take((uint8_t)i, &r);
            s->wake_reason = r;

            if (r != LWAIT_DELIVERED) {
                /* EVERY non-delivered wake is logged, with what was waited
                 * for, how long, and which link. This is the diagnostic that
                 * was missing when GATT discovery stalled silently: the
                 * script gave up, and nothing anywhere said why. */
                printf("lua: %s woke %s after %lu ms (token %08lx, link %u)\n",
                       s->name, lwait_reason_name(r), (unsigned long)waited,
                       (unsigned long)tok, (unsigned)link);
                char msg[220];
                snprintf(msg, sizeof msg,
                         "{\"script\":\"%s\",\"reason\":\"%s\",\"waited_ms\":%lu,"
                         "\"link\":%u}",
                         s->name, lwait_reason_name(r), (unsigned long)waited,
                         (unsigned)link);
                api_emit("script.wake", msg);
            }
            s->wake_at_ms = 0;
        } else if (s->wake_at_ms &&
                   (int32_t)(to_ms_since_boot(get_absolute_time()) - s->wake_at_ms) < 0) {
            /* sys.sleep yields a wake time rather than busy-waiting, because
             * a busy-wait here would stall the radio and the network with it. */
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
        s_current_slot = i;
        s_slice_start_ms = to_ms_since_boot(get_absolute_time());
        int rc = lua_resume(co, L, 0, &nres);
        s_current_co = NULL;
        s_current_slot = -1;
        uint32_t slice_ms = to_ms_since_boot(get_absolute_time()) - s_slice_start_ms;
        s->runtime_ms += slice_ms;
        if (slice_ms > SLICE_MAX_MS) {
            /* One slow C binding, not a loop of them: report but do not kill.
             * A single P-256 operation is ~41 ms on this part and is a normal
             * thing for a handshake to do. */
            s_slice_overruns++;
        }
        s->resumes++;
        if (rc == LUA_YIELD) {
            /* A binding that armed a wait before yielding is BLOCKED, not
             * spinning: it yields no values, which would otherwise look
             * exactly like an instruction-hook preemption and eventually get
             * the script killed for never yielding voluntarily. */
            if (lwait_is_armed((uint8_t)i)) {
                s->forced_yields = 0;
                lua_pop(co, nres);
                continue;
            }
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
                    release_modules(i);
                    drop_links(s);
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
        /* The script has finished, cleanly or not. Either way it is no longer
         * a user of anything it required, and on a 128 KB arena holding a
         * dead profile's modules is what stops the next one loading. */
        s->running = false;
        release_modules(i);
        drop_links(s);
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

/* The file a unit was compiled from, from its name and tier. Scripts are
 * lua/<name>.lua (or staged/); a module's dots are directories. A draft has
 * no file, which is exactly the point of a draft. */
static bool unit_path(const char *name, bool module, uint8_t tier, char *out, size_t cap) {
    if (tier != SRC_TIER_STAGED && tier != SRC_TIER_COMMITTED) { return false; }
    int n = snprintf(out, cap, "%s/", tier == SRC_TIER_STAGED ? "staged" : "lua");
    for (size_t i = 0; name[i] && (size_t)n + 5 < cap; i++) {
        out[n++] = (module && name[i] == '.') ? '/' : name[i];
    }
    out[n] = 0;
    return snprintf(out + n, cap - n, ".lua") == 4;
}

/* One unit of script.loaded's reply. `now` is the hash of the file at the
 * unit's path as it stands, so hash != now is "loaded, modified since"; null
 * when there is no file (a draft, or a file since deleted). */
static int unit_json(char *dst, size_t cap, bool first, const char *name, bool module,
                     uint8_t tier, uint32_t hash, uint32_t users) {
    char path[112];
    char now[16] = "null";
    size_t len = 0;
    if (unit_path(name, module, tier, path, sizeof path) &&
        read_fs(path, src_buf, sizeof(src_buf), &len)) {
        snprintf(now, sizeof now, "\"%08lx\"", (unsigned long)fnv1a(src_buf, len));
    }
    if (module) {
        return snprintf(dst, cap,
            "%s{\"name\":\"%s\",\"kind\":\"module\",\"tier\":\"%s\","
            "\"hash\":\"%08lx\",\"now\":%s,\"users\":%u}",
            first ? "" : ",", name, tier_name(tier), (unsigned long)hash, now,
            (unsigned)__builtin_popcount(users & ~MODULE_PINNED) +
                ((users & MODULE_PINNED) ? 1u : 0u));
    }
    return snprintf(dst, cap,
        "%s{\"name\":\"%s\",\"kind\":\"script\",\"tier\":\"%s\","
        "\"hash\":\"%08lx\",\"now\":%s}",
        first ? "" : ",", name, tier_name(tier), (unsigned long)hash, now);
}

/*
 * Everything resident in the arena, and whether each still matches its file.
 *
 * This is the one question the editor could not answer before: a running
 * script keeps the code it compiled, and a required module stays cached until
 * the script that required it restarts, so "the file on flash" and "what is
 * running" drift apart silently the moment anything is saved. Each unit
 * reports what it was compiled from (tier + hash) and what its file holds now.
 *
 * Stops cleanly and says so rather than overflowing: an oversized reply is
 * dropped whole by the websocket layer, which would read as "nothing is
 * loaded" -- the most misleading answer available.
 */
size_t lrt_loaded_json(char *dst, size_t cap) {
    size_t n = (size_t)snprintf(dst, cap, "{\"units\":[");
    bool first = true, truncated = false;
    const size_t reserve = 32;     /* room to close the object whatever happens */
    for (int i = 0; i < MAX_SCRIPTS && !truncated; i++) {
        const script_t *sc = &scripts[i];
        if (!sc->used || !sc->running) { continue; }
        int w = unit_json(dst + n, cap - n - reserve, first, sc->name, false,
                          sc->src_tier, sc->src_hash, 0);
        if (w < 0 || (size_t)w >= cap - n - reserve) { truncated = true; break; }
        n += (size_t)w; first = false;
    }
    for (int i = 0; i < MAX_TRACKED_MODULES && !truncated; i++) {
        const module_use_t *m = &s_mod_use[i];
        if (!m->name[0] || !m->users) { continue; }
        int w = unit_json(dst + n, cap - n - reserve, first, m->name, true,
                          m->tier, m->hash, m->users);
        if (w < 0 || (size_t)w >= cap - n - reserve) { truncated = true; break; }
        n += (size_t)w; first = false;
    }
    dst[n] = 0;   /* a refused unit may have left a partial write past n */
    return n + (size_t)snprintf(dst + n, cap - n, "],\"truncated\":%s}",
                                truncated ? "true" : "false");
}

size_t lrt_stats_json(char *dst, size_t cap) {
    return (size_t)snprintf(dst, cap,
        "{\"arena_bytes\":%u,\"used\":%u,\"peak\":%u,\"hook_kills\":%lu,"
        "\"budget\":%d,\"slice_overruns\":%lu,\"slice_max_ms\":%d}",
        (unsigned)lheap_capacity(), (unsigned)lheap_used(), (unsigned)lheap_peak(),
        (unsigned long)s_hook_kills, CONFIG_LUA_HOOK_BUDGET,
        (unsigned long)s_slice_overruns, SLICE_MAX_MS);
}

void lrt_stats(lrt_stats_t *out) {
    out->arena_bytes = (uint32_t)lheap_capacity();
    out->used = (uint32_t)lheap_used();
    out->peak = (uint32_t)lheap_peak();
    out->hook_kills = s_hook_kills;
    out->slice_overruns = s_slice_overruns;
    uint32_t n = 0;
    for (int i = 0; i < MAX_SCRIPTS; i++) if (scripts[i].used && scripts[i].running) n++;
    out->running = n;
}

int lrt_task_info(int i, lrt_task_info_t *out) {
    if (i < 0 || i >= MAX_SCRIPTS) return -1;
    const script_t *s = &scripts[i];
    if (!s->used) return 0;
    out->name = s->name;
    /* Zephyr's thread states, loosely: 1 running/ready, 2 blocked (sleeping
     * or waiting), 4 dead. */
    out->state = s->dead ? 4 : (s->running ? (s->wake_at_ms ? 2 : 1) : 4);
    out->resumes = s->resumes;
    out->runtime_ms = s->runtime_ms;
    return 1;
}

void lrt_init(void) {
    lheap_init();
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
