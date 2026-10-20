/*
 * script.* -- the Lua lifecycle over the API.
 *
 * The draft -> staged -> committed overlay exists because LittleFS runs on
 * flash with block_cycles 500, and saving on every edit through a
 * reverse-engineering session is real wear on a real part. A draft runs from
 * RAM and never touches flash; staging persists it without disturbing the
 * committed file; committing promotes it.
 */
#include <stdio.h>
#include <string.h>

#include "api/ops.h"
#include "lua/lrt.h"
#include "lua/lbind.h"
#include "net/ws.h"
#include "fs/fs.h"
#include "autoconf.h"

#if CONFIG_LUA_ENABLE

#define buf    api_scratch()
#define BUF_SZ api_scratch_size()

static bool name_of(const char *params, char *out, size_t cap) {
    return api_json_str(params, "script", out, cap) ||
           api_json_str(params, "name", out, cap);
}

/* --- the file listing ------------------------------------------------------
 * script.list used to report only the runtime's script SLOTS, and a slot is
 * created solely by script.start or script.draft. So a committed .lua file
 * that had never been started was invisible: the editor's dropdown showed one
 * of three runnable scripts and none of the modules. The source side already
 * worked -- script.source resolves lua/<name>.lua for any name, including a
 * slashed one -- so the whole gap was enumeration.
 *
 * Walks lua/ and staged/ one level, then one level into each subdirectory of
 * lua/, which is enough for lua/profiles/ and stops well short of an
 * unbounded recursion over flash.
 */
typedef struct {
    char  *out;
    size_t cap;
    size_t n;
    bool   first;
    const char *dir;      /* "lua" or "staged" */
    const char *sub;      /* subdirectory within lua/, or NULL */
    bool   want_dirs;     /* collect subdirectory names instead of emitting */
    char   subs[4][24];   /* subdirectories found, for the second pass */
    int    n_subs;
} flist_t;

/* The length of a Lua source's name without its suffix (.lua, or .lua.gz
 * for a compressed one), or 0 when it is not one. */
static size_t lua_stem(const char *name) {
    size_t n = strlen(name);
    if (n > 7 && strcmp(name + n - 7, ".lua.gz") == 0) { return n - 7; }
    return n > 4 && strcmp(name + n - 4, ".lua") == 0 ? n - 4 : 0;
}

static void flist_cb(const char *name, bool is_dir, size_t size, void *ctx) {
    flist_t *f = ctx;
    if (is_dir) {
        /* Remembered for a second pass rather than recursed into here: fs_walk
         * holds the directory open, and opening another inside the callback is
         * not something littlefs should be asked to do. */
        if (f->want_dirs && f->n_subs < (int)(sizeof f->subs / sizeof f->subs[0])) {
            snprintf(f->subs[f->n_subs], sizeof f->subs[0], "%s", name);
            f->n_subs++;
        }
        return;
    }
    size_t stem_len = lua_stem(name);
    if (!stem_len) {
        return;
    }
    if (f->n + 128 >= f->cap) {
        return;
    }
    /* The name is what script.source takes: no directory for a top-level
     * script, "<sub>/<stem>" for a module. */
    char stem[40];
    snprintf(stem, sizeof stem, "%.*s", (int)stem_len, name);

    bool staged = strcmp(f->dir, "staged") == 0;
    f->n += (size_t)snprintf(f->out + f->n, f->cap - f->n,
                             "%s{\"name\":\"%s%s%s\",\"kind\":\"%s\","
                             "\"tier\":\"%s\",\"bytes\":%u}",
                             f->first ? "" : ",",
                             f->sub ? f->sub : "", f->sub ? "/" : "", stem,
                             f->sub ? "module" : (staged ? "staged" : "script"),
                             staged ? "staged" : "committed",
                             (unsigned)size);
    f->first = false;
}

static size_t files_json(char *out, size_t cap) {
    flist_t f;
    memset(&f, 0, sizeof f);
    f.out = out; f.cap = cap; f.first = true;
    f.n = (size_t)snprintf(out, cap, "[");

    /* lua/ -- top level scripts, and note its subdirectories */
    f.dir = "lua"; f.sub = NULL; f.want_dirs = true;
    fs_walk("lua", flist_cb, &f);

    /* lua/<sub>/ -- modules */
    f.want_dirs = false;
    int n_subs = f.n_subs;
    for (int i = 0; i < n_subs; i++) {
        char path[64];
        snprintf(path, sizeof path, "lua/%s", f.subs[i]);
        f.sub = f.subs[i];
        fs_walk(path, flist_cb, &f);
    }

    /* staged/ -- the middle tier. A name here outranks the committed copy,
     * because staged is what would actually run; the webapp shows the tier so
     * that is visible rather than surprising. */
    f.dir = "staged"; f.sub = NULL;
    fs_walk("staged", flist_cb, &f);

    f.n += (size_t)snprintf(out + f.n, cap - f.n, "]");
    return f.n;
}

static void op_list(const char *params, api_reply_fn reply, void *ctx, long id) {
    (void)params;
    size_t n = (size_t)snprintf(buf, BUF_SZ, "{\"scripts\":");
    n += lrt_list_json(buf + n, BUF_SZ - n);
    n += (size_t)snprintf(buf + n, BUF_SZ - n, ",\"ops\":");
    n += lbind_ops_json(buf + n, BUF_SZ - n);
    n += (size_t)snprintf(buf + n, BUF_SZ - n, ",\"files\":");
    n += files_json(buf + n, BUF_SZ - n);
    n += (size_t)snprintf(buf + n, BUF_SZ - n, ",\"runtime\":");
    n += lrt_stats_json(buf + n, BUF_SZ - n);
    snprintf(buf + n, BUF_SZ - n, "}");
    api_reply_ok(reply, ctx, id, buf);
}

/* script.heap: the arena and every Lua object, after a full collection. */
static void op_heap(const char *params, api_reply_fn reply, void *ctx, long id) {
    (void)params;
    lrt_heap_json(buf, BUF_SZ);
    api_reply_ok(reply, ctx, id, buf);
}

/* script.arena {offset, len<=1024}: raw arena bytes as hex. */
static void op_arena(const char *params, api_reply_fn reply, void *ctx, long id) {
    long off = 0, len = 1024;
    (void)api_json_int(params, "offset", &off);
    (void)api_json_int(params, "len", &len);
    lrt_arena_json(off, len, buf, BUF_SZ);
    api_reply_ok(reply, ctx, id, buf);
}

/* script.vm_restart: a fresh Lua state without rebooting the board. */
static void op_vm_restart(const char *params, api_reply_fn reply, void *ctx, long id) {
    bool resume = false;
    (void)api_json_bool(params, "resume", &resume);
    lrt_vm_restart(resume, false, buf, BUF_SZ);
    api_reply_ok(reply, ctx, id, buf);
}

static void op_loaded(const char *params, api_reply_fn reply, void *ctx, long id) {
    (void)params;
    lrt_loaded_json(buf, BUF_SZ);
    api_reply_ok(reply, ctx, id, buf);
}

#define NEED_NAME(p) char nm[40]; \
    if (!name_of((p), nm, sizeof(nm))) { \
        api_reply_err(reply, ctx, id, "bad_request", "need \"script\""); return; }

static void op_start(const char *params, api_reply_fn reply, void *ctx, long id) {
    NEED_NAME(params);
    const char *err = "unknown error";
    if (!lrt_start(nm, &err)) { api_reply_err(reply, ctx, id, "start_failed", err); return; }
    api_reply_ok(reply, ctx, id, "{\"started\":true}");
}

static void op_stop(const char *params, api_reply_fn reply, void *ctx, long id) {
    NEED_NAME(params);
    if (!lrt_stop(nm)) { api_reply_err(reply, ctx, id, "not_found", "no such script"); return; }
    api_reply_ok(reply, ctx, id, "{\"stopped\":true}");
}

static void op_reload(const char *params, api_reply_fn reply, void *ctx, long id) {
    NEED_NAME(params);
    const char *err = "unknown error";
    if (!lrt_reload(nm, &err)) { api_reply_err(reply, ctx, id, "reload_failed", err); return; }
    api_reply_ok(reply, ctx, id, "{\"reloaded\":true}");
}

/* Pushes source into RAM and runs it. Deliberately does NOT write flash: the
 * committed file still reads as it did, which is what makes a fast edit loop
 * safe to use on a part with a finite erase budget. */
static void op_draft(const char *params, api_reply_fn reply, void *ctx, long id) {
    NEED_NAME(params);
    /* Bounded by the WebSocket receive buffer, which is deliberately small because
 * it is per-connection. Source too large for one frame goes through
 *   PUT /api/fs/staged/<name>.lua   then   script.reload
 * which streams to flash instead of buffering in RAM. */
    static char src[WS_RX_MAX - 128];
    if (!api_json_str(params, "source", src, sizeof(src))) {
        api_reply_err(reply, ctx, id, "bad_request",
                      "need \"source\"; send it in chunks with "
                      "\"offset\" and \"final\":false for anything larger than a frame");
        return;
    }
    /* Chunked so a large draft never needs a large frame: offset 0 starts a
     * new draft, later offsets append, and only the final chunk runs it. The
     * draft stays in RAM throughout -- no flash is written at any point. */
    long offset = 0;
    (void)api_json_int(params, "offset", &offset);
    bool final = true;
    (void)api_json_bool(params, "final", &final);

    const char *err = "unknown error";
    if (!lrt_draft_chunk(nm, (size_t)offset, src, strlen(src), &err)) {
        api_reply_err(reply, ctx, id, "draft_failed", err); return;
    }
    if (!final) {
        char r[96];
        snprintf(r, sizeof(r), "{\"draft\":true,\"bytes\":%u,\"flash_written\":false}",
                 (unsigned)lrt_draft_len(nm));
        api_reply_ok(reply, ctx, id, r);
        return;
    }
    bool run = true;
    (void)api_json_bool(params, "run", &run);
    if (run && !lrt_reload(nm, &err)) {
        api_reply_err(reply, ctx, id, "start_failed", err); return;
    }
    char r[96];
    snprintf(r, sizeof(r), "{\"draft\":true,\"bytes\":%u,\"flash_written\":false}",
             (unsigned)lrt_draft_len(nm));
    api_reply_ok(reply, ctx, id, r);
}

static void op_stage(const char *params, api_reply_fn reply, void *ctx, long id) {
    NEED_NAME(params);
    const char *err = "unknown error";
    if (!lrt_stage(nm, &err)) { api_reply_err(reply, ctx, id, "stage_failed", err); return; }
    api_reply_ok(reply, ctx, id, "{\"staged\":true}");
}

static void op_commit(const char *params, api_reply_fn reply, void *ctx, long id) {
    NEED_NAME(params);
    const char *err = "unknown error";
    if (!lrt_commit(nm, &err)) { api_reply_err(reply, ctx, id, "commit_failed", err); return; }
    api_reply_ok(reply, ctx, id, "{\"committed\":true}");
}

static void op_revert(const char *params, api_reply_fn reply, void *ctx, long id) {
    NEED_NAME(params);
    if (!lrt_revert(nm)) { api_reply_err(reply, ctx, id, "not_found", "nothing to revert"); return; }
    api_reply_ok(reply, ctx, id, "{\"reverted\":true}");
}

/* The REPL. Runs inside a script's environment when named, so live state is
 * inspectable rather than only exported functions being callable. */
static void op_eval(const char *params, api_reply_fn reply, void *ctx, long id) {
    static char chunk[1024];
    if (!api_json_str(params, "chunk", chunk, sizeof(chunk))) {
        api_reply_err(reply, ctx, id, "bad_request", "need \"chunk\""); return;
    }
    char nm[40] = "";
    (void)name_of(params, nm, sizeof(nm));
    char out[800];
    bool ok = lrt_eval(nm, chunk, out, sizeof(out));
    char esc[1600];
    api_json_escape(esc, sizeof(esc), out);
    char r[1700];
    snprintf(r, sizeof(r), "{\"ok\":%s,\"value\":\"%s\"}", ok ? "true" : "false", esc);
    api_reply_ok(reply, ctx, id, r);
}

/* Read the effective source back out. Paged like the upload: a draft may be
 * far larger than one frame, and this is the only way to retrieve it -- drafts
 * live in RAM, so the filesystem API cannot see them. */
static void op_source(const char *params, api_reply_fn reply, void *ctx, long id) {
    NEED_NAME(params);
    long offset = 0;
    (void)api_json_int(params, "offset", &offset);
    if (offset < 0) { offset = 0; }

    static char chunk[600];
    const char *tier = NULL;
    size_t total = 0;
    size_t n = lrt_source_read(nm, (size_t)offset, chunk, sizeof(chunk), &tier, &total);
    if (!tier) {
        api_reply_err(reply, ctx, id, "not_found",
                      "no draft, staged or committed source for that name");
        return;
    }
    /* The source is arbitrary text and must be escaped before it goes into
     * JSON -- it is full of newlines and quotes by construction. */
    static char esc[1400];
    api_json_escape(esc, sizeof(esc), chunk);
    char *out = buf;
    snprintf(out, BUF_SZ,
             "{\"script\":\"%s\",\"tier\":\"%s\",\"length\":%u,\"offset\":%ld,"
             "\"bytes\":%u,\"eof\":%s,\"chunk\":\"%s\"}",
             nm, tier, (unsigned)total, offset, (unsigned)n,
             ((size_t)offset + n >= total) ? "true" : "false", esc);
    api_reply_ok(reply, ctx, id, out);
}

/* script.autostart {name, enabled=true}: cfg/autostart.json, started at boot. */
static void op_autostart(const char *params, api_reply_fn reply, void *ctx, long id) {
    char nm[40];
    if (!name_of(params, nm, sizeof nm)) { api_reply_err(reply, ctx, id, "bad_request", "need \"name\""); return; }
    bool enabled = true;
    (void)api_json_bool(params, "enabled", &enabled);
    const char *err = "unknown error";
    char *file = api_scratch();          /* shared, consumed within this call */
    if (!lrt_autostart_set(nm, enabled, file, 1024, &err)) {
        api_reply_err(reply, ctx, id, "autostart_failed", err);
        return;
    }
    api_reply_ok(reply, ctx, id, file);
}

static const op_def_t lua_ops_tbl[] = {
    { "script.autostart", op_autostart },
    { "script.source", op_source },
    { "script.list",   op_list },
    { "script.loaded", op_loaded },
    { "script.heap",   op_heap },
    { "script.arena",  op_arena },
    { "script.vm_restart", op_vm_restart },
    { "script.start",  op_start },
    { "script.stop",   op_stop },
    { "script.reload", op_reload },
    { "script.draft",  op_draft },
    { "script.stage",  op_stage },
    { "script.commit", op_commit },
    { "script.revert", op_revert },
    { "script.eval",   op_eval },
    { NULL, NULL }
};

static const char *const lua_topics[] = { "script.error", "script.vm_restarted", "profile.*", NULL };

static bool lua_mod_init(void) { lrt_init(); return true; }

const op_module_t op_module_lua = {
    .ns = "script", .ops = lua_ops_tbl, .topics = lua_topics, .init = lua_mod_init
};

#endif
