#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lua/lrt.h"
#include "lua/lbind.h"
#include "lua/lhttp.h"
#include "fs/fs.h"
#include "util/inflate.h"
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

/* --- sources, plain or compressed -------------------------------------------
 * A source is <base>.lua, or <base>.lua.gz: compressed by the BROWSER when it
 * saved or uploaded it (the board has no deflate), so a well-commented
 * module costs a quarter of its size on flash. When both exist the plain one
 * wins -- it is the one just saved, and the editor flags the other.
 *
 * Identity is the bytes AS STORED: a cache entry or a loaded unit records
 * the hash of the file on flash, so asking "has this changed?" never
 * inflates anything; only compiling does, straight into src_buf (which is
 * also the inflater's window -- util/inflate.c). The text must still fit
 * src_buf (32 KB) once inflated. */
static bool source_file(const char *base, char *path, size_t cap, bool *gz) {
    snprintf(path, cap, "%s.lua", base);
    if (fs_stat(path, NULL)) { *gz = false; return true; }
    snprintf(path, cap, "%s.lua.gz", base);
    if (fs_stat(path, NULL)) { *gz = true; return true; }
    return false;
}

/* FNV-1a of a file's stored bytes, read in pieces (never src_buf). */
static bool stored_hash(const char *path, uint32_t *hash, size_t *len) {
    size_t sz = 0;
    int h = fs_begin_read(path, &sz);
    if (h < 0) { return false; }
    uint8_t piece[128];
    uint32_t v = 2166136261u;
    size_t total = 0;
    int n;
    while ((n = fs_read(h, piece, sizeof piece)) > 0) {
        for (int i = 0; i < n; i++) { v ^= piece[i]; v *= 16777619u; }
        total += (size_t)n;
    }
    fs_end_read(h);
    if (n < 0) { return false; }
    *hash = v;
    *len = total;
    return true;
}

static int fs_piece(void *ctx, uint8_t *buf, size_t cap) {
    return fs_read(*(int *)ctx, buf, cap);
}

/* The text into src_buf: read a plain file, inflate a .gz one. */
static bool load_source(const char *path, bool gz, size_t *len) {
    if (!gz) { return read_fs(path, src_buf, sizeof(src_buf), len); }
    int h = fs_begin_read(path, NULL);
    if (h < 0) { return false; }
    long n = gz_inflate(fs_piece, &h, (uint8_t *)src_buf, sizeof(src_buf) - 1);
    fs_end_read(h);
    if (n < 0) {
        printf("lua: %s did not inflate (error %ld%s)\n", path, n,
               n == INFLATE_ETOOBIG ? ": over 32 KB uncompressed" : "");
        return false;
    }
    src_buf[n] = 0;
    *len = (size_t)n;
    return true;
}

/* A tier's source: path chosen, text in src_buf, identity of what is stored. */
static bool read_source(const char *base, const char **src, size_t *len, uint32_t *hash) {
    char path[112];
    bool gz;
    if (!source_file(base, path, sizeof path, &gz)) { return false; }
    if (!load_source(path, gz, len)) { return false; }
    *src = src_buf;
    if (!gz) { *hash = fnv1a(src_buf, *len); return true; }
    size_t stored = 0;
    return stored_hash(path, hash, &stored);
}

/* Returns the tier the source came from, or NULL when there is none. */
static const char *resolve_src(script_t *s, const char *name,
                               const char **src, size_t *len, uint32_t *hash) {
    if (s && s->draft) { *src = s->draft; *len = s->draft_len; *hash = fnv1a(s->draft, s->draft_len); return "draft"; }
    char base[80];
    snprintf(base, sizeof(base), "staged/%s", name);
    if (read_source(base, src, len, hash)) { return "staged"; }
    snprintf(base, sizeof(base), "lua/%s", name);
    if (read_source(base, src, len, hash)) { return "committed"; }
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


/* A module name as a relative path: dots -> slashes, and nothing else -- no
 * "..", no leading slash, so a module name cannot climb out of lua/. */
static bool module_rel(const char *name, char *rel, size_t cap) {
    size_t j = 0;
    for (size_t i = 0; name[i]; i++) {
        if (j + 1 >= cap) { return false; }
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
    return j > 0;
}

static bool read_module_src(const char *name, const char **src, size_t *len,
                            uint8_t *tier, uint32_t *hash) {
    char rel[64];
    if (!module_rel(name, rel, sizeof rel)) { return false; }
    char base[96];
    snprintf(base, sizeof base, "staged/%s", rel);
    if (read_source(base, src, len, hash)) { *tier = SRC_TIER_STAGED; return true; }
    snprintf(base, sizeof base, "lua/%s", rel);
    if (read_source(base, src, len, hash)) { *tier = SRC_TIER_COMMITTED; return true; }
    return false;
}

/* Tier and stored hash of a module's current file -- no inflating. */
static bool module_identity(const char *name, uint8_t *tier, uint32_t *hash, size_t *stored) {
    char rel[64], base[96], path[112];
    bool gz;
    if (!module_rel(name, rel, sizeof rel)) { return false; }
    snprintf(base, sizeof base, "staged/%s", rel);
    if (source_file(base, path, sizeof path, &gz)) { *tier = SRC_TIER_STAGED; return stored_hash(path, hash, stored); }
    snprintf(base, sizeof base, "lua/%s", rel);
    if (source_file(base, path, sizeof path, &gz)) { *tier = SRC_TIER_COMMITTED; return stored_hash(path, hash, stored); }
    return false;
}

/* Is the cached copy of `name` still what is on flash? Re-reads the source
 * (staged/ first, as a load would) and compares tier and hash with what the
 * load recorded. A module with no record -- tracking slots ran out -- counts
 * as stale and is simply reloaded. A module whose file has gone keeps its
 * cached copy: that is what is running, and reloading could only fail. */
static bool module_is_current(const char *name) {
    const module_use_t *u = NULL;
    for (int i = 0; i < MAX_TRACKED_MODULES; i++) {
        if (s_mod_use[i].name[0] && strcmp(s_mod_use[i].name, name) == 0) {
            u = &s_mod_use[i];
            break;
        }
    }
    if (!u || u->tier == SRC_TIER_NONE) { return false; }
    uint8_t tier = SRC_TIER_NONE;
    uint32_t hash = 0;
    size_t stored = 0;
    if (!module_identity(name, &tier, &hash, &stored)) { return true; }
    return tier == u->tier && hash == u->hash;
}

/* --- the bytecode cache ----------------------------------------------------
 * Compiling Lua on the board costs about twice what the compiled module
 * occupies, all at once, and lands as a run of growing arrays across a
 * first-fit arena: the driver's run.lua peaks at ~40 KB to leave ~17 KB.
 * With one device profile streaming, a second device's modules could not be
 * compiled at all ("not enough memory", largest free block ~12 KB).
 *
 * So a module under lua/ is compiled ONCE, when it changes, and kept as
 * stripped bytecode in cache/<path>.luac; require() loads that instead, at
 * about a quarter of the peak. The .lua stays the only source of truth:
 *
 *   - the cache file's header carries the fnv1a and length of the source it
 *     was compiled from, and is used only while they match the .lua on
 *     flash -- an edited, uploaded or renamed module is never run stale;
 *   - a write to lua/ (fs_on_change) queues the module, and lrt_tick
 *     recompiles it on the next tick -- normally long before it is next
 *     required;
 *   - at boot every module is checked, so a cache missed for any reason
 *     (out of memory at the time, the filesystem busy) is rebuilt while the
 *     arena is still empty;
 *   - a miss in require() compiles the source as before, then caches it.
 *
 * Only lua/ (committed) modules are cached. A staged draft is what is being
 * edited, so it is compiled from source, with its line numbers. Stripped
 * bytecode has none: an error in a cached module reads "?:-1". Push the
 * file again (or delete its .luac) and that module compiles from source
 * until it is next cached. Top-level lua/*.lua are scripts, not modules,
 * and are not cached. */
#define CACHE_HDR 16
/* A timeout of a wait shorter than this is a script polling, not news
 * (see the wake loop in lrt_tick). */
#define WAKE_QUIET_MS 1000u

#define CACHE_VER 2          /* 2: keyed on the source as STORED (.lua or .lua.gz) */

static void cache_path(const char *rel, char *out, size_t cap) {
    snprintf(out, cap, "cache/%s.luac", rel);
}

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* "BTGWLC", a version, the Lua version, then the source's hash and length. */
static void cache_header(uint8_t h[CACHE_HDR], uint32_t hash, uint32_t srclen) {
    memcpy(h, "BTGWLC", 6);
    h[6] = CACHE_VER;
    h[7] = (uint8_t)LUA_VERSION_NUM;
    put32(h + 8, hash);
    put32(h + 12, srclen);
}

/* Does the cache file exist and say it was compiled from exactly this
 * source? Reads only the header. */
static bool cache_matches(const char *path, uint32_t hash, uint32_t srclen) {
    size_t sz = 0;
    if (!fs_stat(path, &sz) || sz <= CACHE_HDR || sz > DRAFT_MAX) { return false; }
    int h = fs_begin_read(path, NULL);
    if (h < 0) { return false; }
    uint8_t got[CACHE_HDR], want[CACHE_HDR];
    int n = fs_read(h, got, CACHE_HDR);
    fs_end_read(h);
    cache_header(want, hash, srclen);
    return n == CACHE_HDR && memcmp(got, want, CACHE_HDR) == 0;
}

static int cache_writer(lua_State *Ls, const void *p, size_t sz, void *ud) {
    (void)Ls; (void)ud;
    return fs_write((const uint8_t *)p, sz) < 0 ? 1 : 0;
}

/* Dump the function on top of the stack as this module's cache. False if it
 * could not be written -- typically another write holding the filesystem's
 * one write slot -- which costs nothing but a compile next time. */
static bool cache_store(lua_State *Ls, const char *rel, uint32_t hash, uint32_t srclen) {
    char path[96];
    cache_path(rel, path, sizeof path);
    if (fs_begin_write(path) < 0) { return false; }
    uint8_t hdr[CACHE_HDR];
    cache_header(hdr, hash, srclen);
    bool ok = fs_write(hdr, CACHE_HDR) >= 0 && lua_dump(Ls, cache_writer, NULL, 1) == 0;
    if (!ok) { fs_abort_write(); fs_delete(path); return false; }
    if (fs_end_write() < 0) { fs_delete(path); return false; }
    return true;
}

/* Push this module's cached chunk and return true, or push nothing and
 * return false. Overwrites src_buf. */
static bool cache_load(lua_State *Ls, const char *rel, const char *chunkname,
                       uint32_t hash, uint32_t srclen) {
    char path[96];
    cache_path(rel, path, sizeof path);
    if (!cache_matches(path, hash, srclen)) { return false; }
    size_t len = 0;
    if (!read_fs(path, src_buf, sizeof(src_buf), &len) || len <= CACHE_HDR) { return false; }
    if (luaL_loadbufferx(Ls, src_buf + CACHE_HDR, len - CACHE_HDR, chunkname, "b") != LUA_OK) {
        printf("lua: cache for %s unusable (%s); compiling the source\n", rel, lua_tostring(Ls, -1));
        lua_pop(Ls, 1);
        fs_delete(path);
        return false;
    }
    return true;
}

/* Bring one module's cache up to date with its source: compile and store
 * if it is missing or stale, delete it if the source has gone. */
static void cache_refresh(const char *name) {
    char rel[64], path[96];
    if (!module_rel(name, rel, sizeof rel)) { return; }
    cache_path(rel, path, sizeof path);
    uint8_t tier = SRC_TIER_NONE;
    uint32_t hash = 0;
    size_t stored = 0;
    if (!module_identity(name, &tier, &hash, &stored)) {
        if (fs_stat(path, NULL)) { fs_delete(path); }
        return;
    }
    if (tier != SRC_TIER_COMMITTED) { return; }
    if (cache_matches(path, hash, (uint32_t)stored)) { return; }    /* nothing inflated */
    const char *src = NULL;
    size_t len = 0;
    uint32_t h2 = 0;
    if (!read_module_src(name, &src, &len, &tier, &h2)) { return; }
    char chunk[80];
    snprintf(chunk, sizeof chunk, "@%s", name);
    int top = lua_gettop(L);
    if (luaL_loadbufferx(L, src, len, chunk, "t") == LUA_OK) {
        if (cache_store(L, rel, hash, (uint32_t)stored)) {
            size_t sz = 0;
            fs_stat(path, &sz);
            printf("lua: cached %s (%u B source, %u B stored -> %u B)\n", name, (unsigned)len,
                   (unsigned)stored, (unsigned)sz);
        } else {
            printf("lua: %s compiled but its cache could not be written\n", name);
        }
    } else {
        printf("lua: %s not cached: %s\n", name, lua_tostring(L, -1));
    }
    lua_settop(L, top);
    lrt_collect();
}

/* Modules waiting to be (re)compiled, one per tick so a boot or a burst of
 * uploads never holds the main loop for long. */
#define CACHE_QUEUE 32
static char s_cache_q[CACHE_QUEUE][48];
static bool s_cache_scan;     /* walk lua/ for every module, then queue them */

static void cache_enqueue(const char *name) {
    int free_i = -1;
    for (int i = 0; i < CACHE_QUEUE; i++) {
        if (!s_cache_q[i][0]) { if (free_i < 0) { free_i = i; } continue; }
        if (strcmp(s_cache_q[i], name) == 0) { return; }
    }
    if (free_i < 0) { s_cache_scan = true; return; }   /* full: check everything */
    snprintf(s_cache_q[free_i], sizeof s_cache_q[free_i], "%s", name);
}

/* "lua/profiles/acme/run.lua" -> "profiles.acme.run"; false for anything
 * that is not a module (outside lua/, not .lua, or a top-level script). */
static bool module_of_path(const char *path, char *name, size_t cap) {
    if (strncmp(path, "lua/", 4) != 0) { return false; }
    const char *rel = path + 4;
    size_t n = strlen(rel);
    if (n > 7 && strcmp(rel + n - 3, ".gz") == 0) { n -= 3; }     /* x.lua.gz */
    if (n < 5 || strncmp(rel + n - 4, ".lua", 4) != 0 || !memchr(rel, '/', n - 4)) { return false; }
    if (n - 4 >= cap) { return false; }
    for (size_t i = 0; i < n - 4; i++) { name[i] = rel[i] == '/' ? '.' : rel[i]; }
    name[n - 4] = 0;
    return true;
}

/* fs_on_change: only takes a note -- it runs in whatever context wrote. */
static void on_fs_change(const char *path) {
    char name[48];
    if (module_of_path(path, name, sizeof name)) { cache_enqueue(name); }
}

typedef struct { char dirs[8][64]; int ndirs; char files[CACHE_QUEUE][64]; int nfiles; const char *at; } walk_t;
static void walk_cb(const char *entry, bool is_dir, size_t size, void *ctx) {
    (void)size;
    walk_t *w = ctx;
    char full[64];
    if (snprintf(full, sizeof full, "%s/%s", w->at, entry) >= (int)sizeof full) { return; }
    if (is_dir) {
        if (w->ndirs < 8) { snprintf(w->dirs[w->ndirs++], 64, "%s", full); }
    } else if (w->nfiles < CACHE_QUEUE) {
        snprintf(w->files[w->nfiles++], 64, "%s", full);
    }
}

static void cache_scan(void) {
    static walk_t w;     /* ~2.6 KB: off the stack */
    memset(&w, 0, sizeof w);
    snprintf(w.dirs[w.ndirs++], 64, "lua");
    for (int d = 0; d < w.ndirs; d++) {
        w.at = w.dirs[d];
        fs_walk(w.dirs[d], walk_cb, &w);
    }
    for (int i = 0; i < w.nfiles; i++) {
        char name[48];
        if (module_of_path(w.files[i], name, sizeof name)) { cache_enqueue(name); }
    }
}

static void cache_tick(void) {
    if (s_cache_scan) { s_cache_scan = false; cache_scan(); }
    for (int i = 0; i < CACHE_QUEUE; i++) {
        if (!s_cache_q[i][0]) { continue; }
        char name[48];
        snprintf(name, sizeof name, "%s", s_cache_q[i]);
        s_cache_q[i][0] = 0;
        cache_refresh(name);
        return;
    }
}

/* Is the value at idx a script environment (or the globals)? Stripped
 * bytecode keeps no upvalue names, so require cannot find the caller's
 * environment by the name "_ENV" when the caller is a cached module; an
 * environment is recognisable anyway -- lrt gives every script a table whose
 * metatable's __index is the global table. */
static bool is_env_table(lua_State *Ls, int idx) {
    if (!lua_istable(Ls, idx)) { return false; }
    idx = lua_absindex(Ls, idx);
    lua_pushglobaltable(Ls);
    bool env = lua_rawequal(Ls, idx, -1);
    if (!env && lua_getmetatable(Ls, idx)) {
        lua_getfield(Ls, -1, "__index");
        env = lua_rawequal(Ls, -1, -3);
        lua_pop(Ls, 2);
    }
    lua_pop(Ls, 1);
    return env;
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
        if (module_is_current(name)) {
            /* A cache HIT still adds this script to the users. Missing this
             * is how a module shared by two profiles gets dropped when the
             * first of them exits, leaving the second holding a stale
             * reference. */
            module_note_user(name, lrt_current_slot());
            return 1;
        }
        /* Edited since it was loaded: load it again. A script already
         * holding the old copy keeps it until it restarts. */
    }
    lua_pop(Ls, 1);

    /* Which file, and its identity as stored -- read before anything else
     * touches src_buf (the lua_call below can require further modules, each
     * reading its own source into the same buffer). A fresh cache is then
     * loaded without the source ever being read, let alone inflated. */
    uint8_t tier = SRC_TIER_NONE;
    uint32_t hash = 0;
    size_t stored = 0;
    if (!module_identity(name, &tier, &hash, &stored)) {
        return luaL_error(Ls, "module '%s' not found in staged/ or lua/", name);
    }

    char chunkname[80];
    snprintf(chunkname, sizeof chunkname, "@%s", name);
    char rel[64];
    bool cacheable = tier == SRC_TIER_COMMITTED && module_rel(name, rel, sizeof rel);
    if (!(cacheable && cache_load(Ls, rel, chunkname, hash, (uint32_t)stored))) {
        /* Not cached (or stale): the source, inflated if it is .gz. */
        const char *src = NULL;
        size_t len = 0;
        uint32_t h2 = 0;
        if (!read_module_src(name, &src, &len, &tier, &h2)) {
            return luaL_error(Ls, "module '%s' could not be read (a .gz that does not inflate, or over 32 KB)", name);
        }
        if (luaL_loadbufferx(Ls, src, len, chunkname, "t") != LUA_OK) {
            return luaL_error(Ls, "module '%s': %s", name, lua_tostring(Ls, -1));
        }
        if (cacheable && cache_store(Ls, rel, hash, (uint32_t)stored)) {
            printf("lua: cached %s on first use\n", name);
        }
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
        /* A cached (stripped) caller has no upvalue names: recognise its
         * environment by what it is instead. */
        for (int i = 1; !have_env; i++) {
            if (!lua_getupvalue(Ls, -1, i)) { break; }
            if (is_env_table(Ls, -1)) {
                lua_remove(Ls, -2);
                have_env = true;
            } else {
                lua_pop(Ls, 1);
            }
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

/* Script loads used to drop EVERY cached module here, so that "push and
 * restart" ran the pushed file rather than the cached copy (a freshly
 * uploaded profile once appeared to ignore a step just added to it). That
 * also meant a second script requiring the same modules compiled its own
 * copy of all of them -- two launchers of one profile, two drivers' worth of code, on
 * an arena that holds one. require() now revalidates a cache hit against the
 * file instead (module_is_current): an edited module still reloads, an
 * unchanged one is shared. */

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
    /* Its ops first: their handlers are what would keep the modules below
     * (and the script's whole environment) reachable after it has gone. */
    int freed = lbind_release_ops(slot);
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
        printf("lua: released %d op(s)/module(s) with no users left, heap %u -> %u B\n",
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
                      uint32_t hash, const char **err) {
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
    /* Not only a RUNNING one: a dead or finished script's state must go too,
     * or load_tier overwrites its references and leaks it (release_runtime). */
    if (s->running || s->co_ref != LUA_NOREF || s->env_ref != LUA_NOREF) { teardown(s); }

    const char *src = NULL; size_t len = 0;
    uint32_t hash = 0;
    const char *tier = resolve_src(s, name, &src, &len, &hash);
    if (!tier) { *err = "no draft, staged or committed source for that name"; return false; }

    if (load_tier(s, src, len, tier, hash, err)) {
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
        uint32_t hash2 = 0;
        const char *tier2 = resolve_src(s, name, &src2, &len2, &hash2);
        if (tier2 && load_tier(s, src2, len2, tier2, hash2, err)) {
            printf("lua: %s draft failed to load; fell back to %s\n", name, tier2);
            return true;
        }
    }
    return false;
}

/*
 * Drop a script's Lua state: its coroutine and its environment. Called by
 * teardown() and, since 1.82, the moment a script DIES or FINISHES.
 *
 * Before, a dead script kept both. A coroutine that ended in an error keeps
 * its whole stack -- every frame's locals -- so a profile that died of "not
 * enough memory" went on holding nearly all of it (measured: a device
 * profile, ~90 KB of a 160 KB arena). Worse, lrt_start tore down only a
 * RUNNING script, so restarting a dead or finished one overwrote co_ref and
 * env_ref without releasing them: the old coroutine leaked for the life of
 * the board, out of reach of any stop. One crash and the profile could
 * never load again ("module '<profile>': not enough memory") --
 * found 2026-09-30 by walking a dump of the arena: three threads in the
 * registry that no script slot referred to, one holding the dead driver.
 */
/* --- defragmenting by rebuilding -------------------------------------------
 * Lua never moves an object, so a few long-lived ones created mid-session (a
 * module's shared state, the string table growing) stay wherever first-fit
 * put them, and cut the free space into pieces: after five Anker driver
 * sessions, 3.3 KB of such blocks left the largest free block at 62 KB of
 * 146 KB free. Rebuilding the VM packs everything from the bottom again, so
 * when the last script stops and the free space is in pieces, the next tick
 * does that (lrt_vm_restart) -- unless something has started meanwhile. */
static bool s_defrag_due;
static bool s_restarting;

static bool any_running(const script_t *except) {
    for (int i = 0; i < MAX_SCRIPTS; i++) {
        if (&scripts[i] != except && scripts[i].used && scripts[i].running) { return true; }
    }
    return false;
}

static void defrag_check(const script_t *going) {
    if (s_restarting || any_running(going)) { return; }
    size_t free_b = lheap_capacity() - lheap_used();
    if (lheap_largest_free() * 4 < free_b * 3) { s_defrag_due = true; }
}

static void release_runtime(script_t *s) {
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
    /* Collect NOW. Nothing else may allocate for a long while after a script
     * stops -- the collector only steps on allocation -- so its whole
     * session (a driver's ~70 KB) sat in the arena as garbage and the arena
     * figure in Settings said it was still in use: 131 KB "used" with the
     * Anker driver stopped, 18 KB once collected (2026-10-01). The collect in
     * release_modules ran before this coroutine was let go, so it could not
     * reach any of it. */
    lrt_collect();
    defrag_check(s);
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

    release_runtime(s);
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
    uint32_t hash = 0;
    const char *t = resolve_src(s, name, &src, &len, &hash);
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
    if (s_defrag_due) {
        s_defrag_due = false;
        if (!any_running(NULL)) {
            static char r[256];
            lrt_vm_restart(false, true, r, sizeof r);   /* keep a dead script's error */
            return;
        }
    }
    cache_tick();
    lbind_pairing_pump();
#if CONFIG_NET_HTTP_CLIENT
    lhttp_tick();
#endif

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

            /* A short wait timing out is a script polling in slices (the
             * Anker loop waits 250 ms at a time for telemetry, and most
             * slices pass with nothing): routine, and reported 4x a second it
             * buried the Console. Not logged; the script has its answer. */
            bool routine = r == LWAIT_TIMEOUT && waited < WAKE_QUIET_MS;
            if (r != LWAIT_DELIVERED && !routine) {
                /* Every other non-delivered wake is logged, with what was
                 * waited for, how long, and which link. This is the diagnostic
                 * that was missing when GATT discovery stalled silently: the
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
                    release_runtime(s);
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
        /* After the error text is copied out of its stack: nothing of a
         * finished script's state is needed any more. */
        release_runtime(s);
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
    return true;            /* the base: source_file adds .lua or .lua.gz */
}

/* One unit of script.loaded's reply. `now` is the hash of the file at the
 * unit's path as it stands, so hash != now is "loaded, modified since"; null
 * when there is no file (a draft, or a file since deleted). */
static int unit_json(char *dst, size_t cap, bool first, const char *name, bool module,
                     uint8_t tier, uint32_t hash, uint32_t users) {
    char base[112], path[112] = "";
    char now[16] = "null";
    bool gz;
    uint32_t h = 0;
    size_t len = 0;
    if (unit_path(name, module, tier, base, sizeof base) &&
        source_file(base, path, sizeof path, &gz) && stored_hash(path, &h, &len)) {
        snprintf(now, sizeof now, "\"%08lx\"", (unsigned long)h);
    } else {
        path[0] = 0;
    }
    if (module) {
        return snprintf(dst, cap,
            "%s{\"name\":\"%s\",\"kind\":\"module\",\"tier\":\"%s\",\"path\":\"%s\","
            "\"hash\":\"%08lx\",\"now\":%s,\"users\":%u}",
            first ? "" : ",", name, tier_name(tier), path, (unsigned long)hash, now,
            (unsigned)__builtin_popcount(users & ~MODULE_PINNED) +
                ((users & MODULE_PINNED) ? 1u : 0u));
    }
    return snprintf(dst, cap,
        "%s{\"name\":\"%s\",\"kind\":\"script\",\"tier\":\"%s\",\"path\":\"%s\","
        "\"hash\":\"%08lx\",\"now\":%s}",
        first ? "" : ",", name, tier_name(tier), path, (unsigned long)hash, now);
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

/* --- heap diagnostics -------------------------------------------------------
 * script.heap: where the arena's bytes are, from two sides.
 *
 *   arena    the allocator's view: blocks, free blocks, and a histogram of
 *            free block sizes -- fragmentation is the shape of that histogram
 *   objects  Lua's view: every collectable object, walked off the GC lists,
 *            tallied by type with its exact size; the largest tables, named
 *            where they can be (the registry, the globals, a module, a
 *            script's environment); and each coroutine's stack
 *
 * Run it after a full collection (it does one first) and compare two runs:
 * a leak is whatever grows. script.arena reads the raw arena for offline
 * analysis (tools/lua_heapdump.py). Added 2026-10-01 after a leak took
 * three rounds of guesswork to pin down with only "used" and "largest free".
 */
#include "lstate.h"
#include "lfunc.h"
#include "lstring.h"
#include "ltable.h"

typedef struct { uint32_t n, bytes; } tally_t;
enum { T_SHRSTR, T_LNGSTR, T_TABLE, T_LCL, T_CCL, T_PROTO, T_UDATA, T_THREAD, T_UPVAL, T_OTHER, T_N };
static const char *const T_NAMES[T_N] = { "short_string", "long_string", "table", "lua_closure",
    "c_closure", "proto", "userdata", "thread", "upvalue", "other" };

static size_t obj_size(GCObject *o, int *kind) {
    switch (o->tt) {
        case LUA_VSHRSTR: *kind = T_SHRSTR; return sizelstring(gco2ts(o)->shrlen);
        case LUA_VLNGSTR: *kind = T_LNGSTR; return sizelstring(gco2ts(o)->u.lnglen);
        case LUA_VTABLE: {
            Table *t = gco2t(o);
            *kind = T_TABLE;
            return sizeof(Table) + sizeof(TValue) * luaH_realasize(t)
                 + (isdummy(t) ? 0 : sizeof(Node) * sizenode(t));
        }
        case LUA_VLCL: *kind = T_LCL; return sizeLclosure(gco2lcl(o)->nupvalues);
        case LUA_VCCL: *kind = T_CCL; return sizeCclosure(gco2ccl(o)->nupvalues);
        case LUA_VPROTO: {
            Proto *f = gco2p(o);
            *kind = T_PROTO;
            return sizeof(Proto) + sizeof(Instruction) * f->sizecode + sizeof(TValue) * f->sizek
                 + sizeof(Proto *) * f->sizep + f->sizelineinfo
                 + sizeof(AbsLineInfo) * f->sizeabslineinfo + sizeof(LocVar) * f->sizelocvars
                 + sizeof(Upvaldesc) * f->sizeupvalues;
        }
        case LUA_VUSERDATA: {
            Udata *u = gco2u(o);
            *kind = T_UDATA;
            return sizeudata(u->nuvalue, u->len);
        }
        case LUA_VTHREAD: {
            lua_State *th = gco2th(o);
            *kind = T_THREAD;
            return sizeof(lua_State) + sizeof(StackValue) * (size_t)(stacksize(th) + EXTRA_STACK)
                 + sizeof(CallInfo) * th->nci;
        }
        case LUA_VUPVAL: *kind = T_UPVAL; return sizeof(UpVal);
        default: *kind = T_OTHER; return 0;
    }
}

typedef struct { const void *p; uint32_t bytes; uint32_t asize, hsize; } bigtab_t;
#define TOP_TABLES 8

typedef struct {
    size_t used, n, live, free_bytes, free_n, largest;
    uint32_t hist_n[6], hist_b[6];
} walk_stats_t;
static const uint32_t HIST_LIM[5] = { 64, 256, 1024, 4096, 16384 };

static void arena_cb(const void *p, size_t size, bool free, void *ctx) {
    (void)p;
    walk_stats_t *w = ctx;
    w->n++;
    if (!free) { w->live += size; return; }
    w->free_n++;
    w->free_bytes += size;
    if (size > w->largest) { w->largest = size; }
    int b = 0;
    while (b < 5 && size >= HIST_LIM[b]) { b++; }
    w->hist_n[b]++;
    w->hist_b[b] += (uint32_t)size;
}

/* A name for a table, when it is one we can recognise. */
static void table_name(const void *t, char *out, size_t cap) {
    out[0] = 0;
    lua_pushglobaltable(L);
    if (lua_topointer(L, -1) == t) { snprintf(out, cap, "globals"); }
    lua_pop(L, 1);
    if (!out[0] && lua_topointer(L, LUA_REGISTRYINDEX) == t) { snprintf(out, cap, "registry"); }
    if (!out[0]) {
        for (int i = 0; i < MAX_SCRIPTS && !out[0]; i++) {
            if (!scripts[i].used || scripts[i].env_ref == LUA_NOREF) { continue; }
            lua_rawgeti(L, LUA_REGISTRYINDEX, scripts[i].env_ref);
            if (lua_topointer(L, -1) == t) { snprintf(out, cap, "env:%s", scripts[i].name); }
            lua_pop(L, 1);
        }
    }
    if (out[0]) { return; }
    /* Pop exactly what was pushed. 1.90 popped the module cache even when it
     * had returned early without fetching it -- for the registry and the
     * globals, i.e. always -- which corrupted the main thread's stack and
     * crashed the board on the first script.heap. */
    int top = lua_gettop(L);
    if (lua_getfield(L, LUA_REGISTRYINDEX, MODULE_CACHE) == LUA_TTABLE) {
        if (lua_topointer(L, -1) == t) { snprintf(out, cap, "module cache"); }
        lua_pushnil(L);
        while (!out[0] && lua_next(L, -2)) {
            if (lua_topointer(L, -1) == t && lua_type(L, -2) == LUA_TSTRING) {
                snprintf(out, cap, "module:%s", lua_tostring(L, -2));
            }
            lua_pop(L, 1);
        }
    }
    lua_settop(L, top);
}

size_t lrt_heap_json(char *dst, size_t cap) {
    if (!L) { return (size_t)snprintf(dst, cap, "{\"error\":\"no Lua state\"}"); }
    if (!lua_checkstack(L, 8)) { return (size_t)snprintf(dst, cap, "{\"error\":\"no Lua stack\"}"); }
    lrt_collect();
    walk_stats_t w = { 0 };
    lheap_walk(arena_cb, &w);
    size_t fail_last = 0, fail_count = 0;
    lheap_fail_stats(&fail_last, &fail_count);
    size_t n = (size_t)snprintf(dst, cap,
        "{\"arena\":{\"bytes\":%u,\"used\":%u,\"peak\":%u,\"blocks\":%u,\"free_blocks\":%u,"
        "\"free_bytes\":%u,\"largest_free\":%u,\"alloc_fails\":%u,\"alloc_fail_last\":%u,"
        "\"header\":%u,\"free_hist\":[",
        (unsigned)lheap_capacity(), (unsigned)lheap_used(), (unsigned)lheap_peak(), (unsigned)w.n,
        (unsigned)w.free_n, (unsigned)w.free_bytes, (unsigned)w.largest, (unsigned)fail_count,
        (unsigned)fail_last, (unsigned)lheap_header_size());
    static const char *const HN[6] = { "<64", "<256", "<1K", "<4K", "<16K", ">=16K" };
    for (int i = 0; i < 6; i++) {
        n += (size_t)snprintf(dst + n, cap - n, "%s{\"size\":\"%s\",\"n\":%u,\"bytes\":%u}",
                              i ? "," : "", HN[i], (unsigned)w.hist_n[i], (unsigned)w.hist_b[i]);
    }
    n += (size_t)snprintf(dst + n, cap - n, "]},");

    global_State *g = G(L);
    tally_t t[T_N] = { 0 };
    bigtab_t top[TOP_TABLES] = { 0 };
    uint32_t threads[MAX_SCRIPTS + 4] = { 0 };
    int nthreads = 0;
    GCObject *lists[4] = { g->allgc, g->finobj, g->tobefnz, g->fixedgc };
    for (int li = 0; li < 4; li++) {
        for (GCObject *o = lists[li]; o; o = o->next) {
            int k;
            size_t sz = obj_size(o, &k);
            t[k].n++;
            t[k].bytes += (uint32_t)sz;
            if (k == T_TABLE) {
                int m = 0;
                for (int i = 1; i < TOP_TABLES; i++) { if (top[i].bytes < top[m].bytes) { m = i; } }
                if (sz > top[m].bytes) {
                    Table *tb = gco2t(o);
                    top[m] = (bigtab_t){ tb, (uint32_t)sz, luaH_realasize(tb), isdummy(tb) ? 0 : sizenode(tb) };
                }
            } else if (k == T_THREAD && nthreads < (int)(sizeof threads / sizeof threads[0])) {
                threads[nthreads++] = (uint32_t)sz;
            }
        }
    }
    n += (size_t)snprintf(dst + n, cap - n, "\"objects\":{");
    uint32_t total = 0;
    for (int k = 0; k < T_N; k++) {
        total += t[k].bytes;
        n += (size_t)snprintf(dst + n, cap - n, "%s\"%s\":{\"n\":%u,\"bytes\":%u}",
                              k ? "," : "", T_NAMES[k], (unsigned)t[k].n, (unsigned)t[k].bytes);
    }
    n += (size_t)snprintf(dst + n, cap - n, ",\"total\":%u},\"strtab\":{\"size\":%d,\"used\":%d},"
                          "\"tables\":[", (unsigned)total, g->strt.size, g->strt.nuse);
    /* largest first */
    for (int a = 0; a < TOP_TABLES; a++) {
        for (int b = a + 1; b < TOP_TABLES; b++) {
            if (top[b].bytes > top[a].bytes) { bigtab_t x = top[a]; top[a] = top[b]; top[b] = x; }
        }
    }
    bool first = true;
    for (int i = 0; i < TOP_TABLES && n + 160 < cap; i++) {
        if (!top[i].bytes) { continue; }
        char nm[64];
        table_name(top[i].p, nm, sizeof nm);
        n += (size_t)snprintf(dst + n, cap - n, "%s{\"bytes\":%u,\"array\":%u,\"hash\":%u,\"name\":\"%s\"}",
                              first ? "" : ",", (unsigned)top[i].bytes, (unsigned)top[i].asize,
                              (unsigned)top[i].hsize, nm);
        first = false;
    }
    n += (size_t)snprintf(dst + n, cap - n, "],\"threads\":[");
    for (int i = 0; i < nthreads && n + 16 < cap; i++) {
        n += (size_t)snprintf(dst + n, cap - n, "%s%u", i ? "," : "", (unsigned)threads[i]);
    }
    n += (size_t)snprintf(dst + n, cap - n, "],\"unaccounted\":%d}",
                          (int)lheap_used() - (int)total);
    return n;
}

/* script.arena: `len` (<= 1024) raw bytes of the arena from `offset`, as hex,
 * with where the arena is -- block headers hold absolute pointers. */
size_t lrt_arena_json(long offset, long len, char *dst, size_t cap) {
    size_t size = lheap_capacity();
    if (offset < 0 || (size_t)offset >= size) { return (size_t)snprintf(dst, cap, "{\"error\":\"offset\"}"); }
    if (len <= 0 || len > 1024) { len = 1024; }
    if ((size_t)(offset + len) > size) { len = (long)(size - (size_t)offset); }
    const uint8_t *p = (const uint8_t *)lheap_base() + offset;
    size_t n = (size_t)snprintf(dst, cap, "{\"base\":%lu,\"size\":%u,\"header\":%u,\"offset\":%ld,\"len\":%ld,\"hex\":\"",
                                (unsigned long)(uintptr_t)lheap_base(), (unsigned)size,
                                (unsigned)lheap_header_size(), offset, len);
    static const char HX[] = "0123456789abcdef";
    for (long i = 0; i < len && n + 4 < cap; i++) {
        dst[n++] = HX[p[i] >> 4];
        dst[n++] = HX[p[i] & 15];
    }
    n += (size_t)snprintf(dst + n, cap - n, "\"}");
    return n;
}

/* script.vm_restart: stop every script, close the Lua state and start a new
 * one in a clean arena -- what a reboot does for Lua, without dropping WiFi,
 * WireGuard or Bluetooth. Drafts are kept (they live outside the arena). */
size_t lrt_vm_restart(bool resume, bool keep_status, char *dst, size_t cap) {
    if (!L) { return (size_t)snprintf(dst, cap, "{\"error\":\"no Lua state\"}"); }
    s_restarting = true;
    size_t before = lheap_used(), before_free = lheap_largest_free();
    int stopped = 0;
    static char was[MAX_SCRIPTS][NAME_MAX_LEN + 1];
    memset(was, 0, sizeof was);
    for (int i = 0; i < MAX_SCRIPTS; i++) {
        script_t *s = &scripts[i];
        if (!s->used) { continue; }
        if (s->running) { snprintf(was[i], sizeof was[i], "%s", s->name); }
        if (s->running || s->co_ref != LUA_NOREF) { stopped += s->running ? 1 : 0; teardown(s); }
        if (!keep_status) { s->dead = false; s->err[0] = 0; }
    }
    lbind_reset();
    memset(s_mod_use, 0, sizeof s_mod_use);
    lua_close(L);
    L = NULL;
    s_ready = false;
    lrt_init();
    s_restarting = false;
    s_defrag_due = false;
    int resumed = 0;
    for (int i = 0; resume && i < MAX_SCRIPTS; i++) {
        const char *err = NULL;
        if (was[i][0] && lrt_start(was[i], &err)) { resumed++; }
        else if (was[i][0]) { printf("lua: could not resume %s: %s\n", was[i], err ? err : "?"); }
    }
    printf("lua: VM restarted (%d script(s) stopped, %d resumed), arena %u -> %u B, largest free %u -> %u B\n",
           stopped, resumed, (unsigned)before, (unsigned)lheap_used(),
           (unsigned)before_free, (unsigned)lheap_largest_free());
    size_t n = (size_t)snprintf(dst, cap,
        "{\"restarted\":true,\"stopped\":%d,\"resumed\":%d,\"before\":%u,\"after\":%u,"
        "\"largest_free_before\":%u,\"largest_free\":%u}",
        stopped, resumed, (unsigned)before, (unsigned)lheap_used(),
        (unsigned)before_free, (unsigned)lheap_largest_free());
    api_emit("script.vm_restarted", dst);
    return n;
}

size_t lrt_stats_json(char *dst, size_t cap) {
    size_t fail_last = 0, fail_count = 0;
    lheap_fail_stats(&fail_last, &fail_count);
    return (size_t)snprintf(dst, cap,
        "{\"arena_bytes\":%u,\"used\":%u,\"peak\":%u,\"largest_free\":%u,"
        "\"alloc_fails\":%u,\"alloc_fail_last\":%u,\"hook_kills\":%lu,"
        "\"budget\":%d,\"slice_overruns\":%lu,\"slice_max_ms\":%d}",
        (unsigned)lheap_capacity(), (unsigned)lheap_used(), (unsigned)lheap_peak(),
        (unsigned)lheap_largest_free(), (unsigned)fail_count, (unsigned)fail_last,
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
    /* The string table starts at 512 slots (MINSTRTABSIZE, set in
     * third_party/lua/CMakeLists.txt) and lgc.c is patched never to shrink it
     * below that: growing it from 256 mid-session left its 2 KB array as an
     * island in the middle of the arena. A resize here was not enough --
     * the first collection shrank it straight back. */
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

    /* Recompile a module into the bytecode cache when its source changes,
     * and check every module once now, while the arena is empty. */
    fs_on_change(on_fs_change);
    s_cache_scan = true;

    s_ready = true;
    printf("lua: 5.4.8 up, %u KB arena, %d-instruction slice\n",
           CONFIG_LUA_HEAP_KB, CONFIG_LUA_HOOK_BUDGET);
}

#else   /* !CONFIG_LUA_ENABLE */
void lrt_init(void) {}
bool lrt_ready(void) { return false; }
void lrt_tick(void) {}
#endif
