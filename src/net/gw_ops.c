/* gw.peers, gw.hostname, gw.forward, gw.unforward.
 * Handlers run inside an lwIP receive callback. Anything that writes a file
 * or opens a pcb is staged for gw_lan_poll. */
#include "autoconf.h"

#if CONFIG_NET_GW_LAN && CONFIG_NET_HTTP_SERVER

#include <stdio.h>
#include <string.h>

#include "api/api.h"
#include "api/ops.h"
#include "net/gw_lan.h"

#define buf api_scratch()

static void op_peers(const char *params, api_reply_fn reply, void *ctx, long id) {
    (void)params;
    gw_peers_json(buf, api_scratch_size());
    api_reply_ok(reply, ctx, id, buf);
}

static void op_hostname(const char *params, api_reply_fn reply, void *ctx, long id) {
    char name[64];
    if (!params) { params = "{}"; }
    if (!api_json_str(params, "name", name, sizeof name)) {
        char esc[128];
        api_json_escape(esc, sizeof esc, gw_active_name());
        snprintf(buf, api_scratch_size(),
                 "{\"name\":\"%s\",\"source\":\"%s\"}", esc, gw_name_source());
        api_reply_ok(reply, ctx, id, buf);
        return;
    }
    const char *why = "bad hostname";
    if (!gw_hostname_stage(name, &why)) {
        api_reply_err(reply, ctx, id, "bad_request", why);
        return;
    }
    api_reply_ok(reply, ctx, id, "{\"staged\":true}");
}

static void op_forward(const char *params, api_reply_fn reply, void *ctx, long id) {
    char proto[8], to[64];
    long listen = 0, port = 0;
    if (!params) { params = "{}"; }
    const char *why = "proto, listen, to, and port are required";
    if (!api_json_str(params, "proto", proto, sizeof proto) ||
        !api_json_int(params, "listen", &listen) ||
        !api_json_str(params, "to", to, sizeof to) ||
        !api_json_int(params, "port", &port) ||
        !gw_forward_stage(proto, listen, to, port, &why)) {
        api_reply_err(reply, ctx, id, "bad_request", why);
        return;
    }
    api_reply_ok(reply, ctx, id, "{\"staged\":true}");
}

static void op_unforward(const char *params, api_reply_fn reply, void *ctx, long id) {
    char proto[8];
    long listen = 0;
    if (!params) { params = "{}"; }
    const char *why = "proto and listen are required";
    if (!api_json_str(params, "proto", proto, sizeof proto) ||
        !api_json_int(params, "listen", &listen) ||
        !gw_unforward_stage(proto, listen, &why)) {
        api_reply_err(reply, ctx, id, "bad_request", why);
        return;
    }
    api_reply_ok(reply, ctx, id, "{\"staged\":true}");
}

static const op_def_t gw_ops[] = {
    { "gw.peers", op_peers },
    { "gw.hostname", op_hostname },
    { "gw.forward", op_forward },
    { "gw.unforward", op_unforward },
    { NULL, NULL }
};

static const char *const gw_topics[] = { NULL };

const op_module_t op_module_gw = {
    .ns = "gw", .ops = gw_ops, .topics = gw_topics, .init = NULL
};

#endif
