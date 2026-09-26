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
#include "autoconf.h"

#if CONFIG_LUA_ENABLE

static char buf[3072];

static bool name_of(const char *params, char *out, size_t cap) {
    return api_json_str(params, "script", out, cap) ||
           api_json_str(params, "name", out, cap);
}

static void op_list(const char *params, api_reply_fn reply, void *ctx, long id) {
    (void)params;
    size_t n = (size_t)snprintf(buf, sizeof(buf), "{\"scripts\":");
    n += lrt_list_json(buf + n, sizeof(buf) - n);
    n += (size_t)snprintf(buf + n, sizeof(buf) - n, ",\"ops\":");
    n += lbind_ops_json(buf + n, sizeof(buf) - n);
    n += (size_t)snprintf(buf + n, sizeof(buf) - n, ",\"runtime\":");
    n += lrt_stats_json(buf + n, sizeof(buf) - n);
    snprintf(buf + n, sizeof(buf) - n, "}");
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
    static char src[8193];
    if (!api_json_str(params, "source", src, sizeof(src))) {
        api_reply_err(reply, ctx, id, "bad_request", "need \"source\""); return;
    }
    const char *err = "unknown error";
    if (!lrt_draft_set(nm, src, strlen(src), &err)) {
        api_reply_err(reply, ctx, id, "draft_failed", err); return;
    }
    bool run = true;
    (void)api_json_bool(params, "run", &run);
    if (run && !lrt_reload(nm, &err)) {
        api_reply_err(reply, ctx, id, "start_failed", err); return;
    }
    api_reply_ok(reply, ctx, id, "{\"draft\":true,\"flash_written\":false}");
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

static const op_def_t lua_ops_tbl[] = {
    { "script.list",   op_list },
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

static const char *const lua_topics[] = { "script.error", "profile.*", NULL };

static bool lua_mod_init(void) { lrt_init(); return true; }

const op_module_t op_module_lua = {
    .ns = "script", .ops = lua_ops_tbl, .topics = lua_topics, .init = lua_mod_init
};

#endif
