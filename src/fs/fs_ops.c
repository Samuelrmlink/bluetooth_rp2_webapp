/* fs.stat: up to 32 editor paths, for a browser that reconnected.
 * LittleFS is answered in this call. An SD path is read by the storage
 * worker and comes back as fs.stated. fs.changed is the live notice. */
#include "autoconf.h"
#include "fs/fs_watch.h"
#include "fs/fs_layout.h"

#include "api/api.h"
#include "api/ops.h"

#if CONFIG_FS_LAYOUT
static void op_fs_layout(const char *params, api_reply_fn reply, void *ctx, long id) {
    (void)params;
    char *b = api_scratch();
    if (fs_layout_dry_json(b, api_scratch_size()) < 0) {
        api_reply_err(reply, ctx, id, "too_large", "layout");
        return;
    }
    api_reply_ok(reply, ctx, id, b);
}

static void op_fs_layout_arm(const char *params, api_reply_fn reply, void *ctx, long id) {
    (void)params;
    int r = fs_layout_arm();
    if (r == 0) {
        api_reply_ok(reply, ctx, id, "{\"armed\":true}");
        return;
    }
    const char *msg = r == -1 ? "files do not fit" :
                      r == -2 ? "sd card" :
                      r == -3 ? "move already started" :
                      r == -4 ? "image is not confirmed" : "arm failed";
    api_reply_err(reply, ctx, id, "refused", msg);
}

static void op_fs_layout_commit(const char *params, api_reply_fn reply, void *ctx, long id) {
    char confirm[32];
    char err[80];
    if (!api_json_str(params, "confirm", confirm, sizeof confirm)) {
        api_reply_err(reply, ctx, id, "bad_request", "need \"confirm\"");
        return;
    }
    int r = fs_layout_commit(confirm, err, sizeof err);
    if (r == 0) {
        api_reply_ok(reply, ctx, id, "{\"wrote\":true,\"rebooting\":true}");
        return;
    }
    if (r == 1) {
        api_reply_ok(reply, ctx, id, "{\"wrote\":false,\"already\":true}");
        return;
    }
    api_reply_err(reply, ctx, id, "refused", err[0] ? err : "commit failed");
}
#endif

static void op_fs_stat(const char *params, api_reply_fn reply, void *ctx, long id) {
    /* The path table lives in fs_watch. Copying 32 paths onto this stack
     * does not fit: core0's stack is 2 KB. */
    int rc = fs_watch_stat_begin(params);
    if (rc == -1) {
        api_reply_err(reply, ctx, id, "bad_request", "need \"paths\"");
        return;
    }
    if (rc == -3) {
        api_reply_err(reply, ctx, id, "bad_request", "at most 32 paths");
        return;
    }
    if (rc != 0) {
        api_reply_err(reply, ctx, id, "busy", "a stat is already running");
        return;
    }
    if (fs_watch_stat_state() == 1) {
        api_reply_ok(reply, ctx, id, "{\"pending\":true,\"event\":\"fs.stated\"}");
        return;
    }
    char *b = api_scratch();
    if (!fs_watch_stat_json(b, api_scratch_size())) {
        fs_watch_stat_drop();
        api_reply_err(reply, ctx, id, "too_large", "stat");
        return;
    }
    /* The poll emits fs.stated only for a card read. This reply already
     * carries the LittleFS JSON, so drop the ready flag and do not emit. */
    api_reply_ok(reply, ctx, id, b);
    fs_watch_stat_drop();
}

static bool fs_ops_init(void) {
    fs_on_watch(fs_watch_on_lfs);
    return true;
}

static const op_def_t fs_ops[] = {
    { "fs.stat", op_fs_stat },
#if CONFIG_FS_LAYOUT
    { "fs.layout", op_fs_layout },
    { "fs.layout_arm", op_fs_layout_arm },
    { "fs.layout_commit", op_fs_layout_commit },
#endif
    { NULL, NULL }
};

static const char *const fs_topics[] = { "fs.changed", "fs.stated", NULL };

const op_module_t op_module_fs = {
    .ns = "fs", .ops = fs_ops, .topics = fs_topics, .init = fs_ops_init
};
