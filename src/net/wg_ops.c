/*
 * WireGuard over the module registry: status, up, down.
 *
 * `status` exists so the websocket can watch the tunnel rather than assume
 * it -- handshake state, the endpoint actually in use, counters, and
 * is_default_route, which is reported so a monitor can ASSERT the tunnel has
 * not quietly become the only way in rather than trusting a comment.
 */
#include <stdio.h>

#include "api/ops.h"
#include "net/wg.h"
#include "autoconf.h"

#if CONFIG_NET_WIREGUARD

static char buf[512];

static void op_wg_status(const char *params, api_reply_fn reply, void *ctx, long id) {
    (void)params;
    size_t n = wg_status_json(buf, sizeof buf);
    if (!n) { api_reply_err(reply, ctx, id, "unavailable", "no wireguard status"); return; }
    api_reply_ok(reply, ctx, id, buf);
}

static void op_wg_up(const char *params, api_reply_fn reply, void *ctx, long id) {
    (void)params;
    if (!wg_up()) {
        api_reply_err(reply, ctx, id, "not_configured",
                      "cfg/wireguard.json is missing or incomplete");
        return;
    }
    api_reply_ok(reply, ctx, id, "{\"up\":true}");
}

static void op_wg_down(const char *params, api_reply_fn reply, void *ctx, long id) {
    (void)params;
    wg_down();
    api_reply_ok(reply, ctx, id, "{\"up\":false}");
}

static const op_def_t wg_ops[] = {
    { "wg.status", op_wg_status },
    { "wg.up",     op_wg_up },
    { "wg.down",   op_wg_down },
    { NULL, NULL }
};

static const char *const wg_topics[] = { "wg.state", NULL };

const op_module_t op_module_wg = {
    .ns = "wg", .ops = wg_ops, .topics = wg_topics, .init = NULL
};

#endif
