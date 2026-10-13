/*
 * Ethernet (W5500) over the module registry: eth.status, eth.config.
 *
 * eth.config only validates and stages; eth_poll() applies it and writes
 * cfg/eth.json from the main loop. Handlers run inside an lwIP receive
 * callback, and neither restarting DHCP nor writing flash belongs there.
 */
#include <stdio.h>

#include "api/ops.h"
#include "net/eth_w5500.h"
#include "autoconf.h"

#if CONFIG_NET_ETH_W5500

static char buf[640];

static void op_eth_status(const char *params, api_reply_fn reply, void *ctx, long id) {
    (void)params;
    eth_status_json(buf, sizeof buf);
    api_reply_ok(reply, ctx, id, buf);
}

static void op_eth_config(const char *params, api_reply_fn reply, void *ctx, long id) {
    const char *why = "bad config";
    if (!eth_configure(params, &why)) {
        api_reply_err(reply, ctx, id, "bad_request", why);
        return;
    }
    api_reply_ok(reply, ctx, id, "{\"staged\":true}");
}

static const op_def_t eth_ops[] = {
    { "eth.status", op_eth_status },
    { "eth.config", op_eth_config },
    { NULL, NULL }
};

static const char *const eth_topics[] = { "eth.state", NULL };

const op_module_t op_module_eth = {
    .ns = "eth", .ops = eth_ops, .topics = eth_topics, .init = NULL
};

#endif
