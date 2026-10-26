/*
 * WiFi over the module registry: scan, join, forget, mode.
 *
 * The C side of this already existed (src/net/wifi.c) and was reachable only
 * from the firmware itself. All this adds is the registry front-end, so the
 * same operations are available from the browser, from ws_client.py and from
 * a Lua script without any per-front-end work.
 *
 * Every entry point here is ASYNCHRONOUS on purpose, and that is wifi.h's
 * rule rather than a choice made here: nothing may touch the radio
 * synchronously, because these handlers run from an lwIP receive callback and
 * restarting a radio -- or writing flash -- from inside one produces a lockup
 * that looks like a hardware fault. So wifi.scan returns having started
 * nothing and the caller reads wifi.scan_results; wifi.connect returns
 * "joining" rather than "joined".
 */
#include <stdio.h>
#include <string.h>

#include "api/ops.h"
#include "config/config.h"
#include "net/wifi.h"
#include "autoconf.h"
/* After autoconf.h: CONFIG_BT_LE has to be defined before it is tested. */
#if CONFIG_BT_LE
#include "bt/bt.h"
#endif

#if CONFIG_NET_WIFI

/* One shared buffer, as elsewhere: a handler runs to completion before the
 * next is dispatched, and the network list is the largest thing it holds. */
#define WIFI_BUF_SZ 2048
static char buf[WIFI_BUF_SZ];

static void op_wifi_status(const char *params, api_reply_fn reply, void *ctx, long id) {
    (void)params;
    int n = wifi_status_json(buf, sizeof buf);
    if (n <= 0) { api_reply_err(reply, ctx, id, "unavailable", "no wifi status"); return; }
    api_reply_ok(reply, ctx, id, buf);
}

static void op_wifi_scan(const char *params, api_reply_fn reply, void *ctx, long id) {
    (void)params;
    bool started = wifi_scan_start();
    snprintf(buf, sizeof buf, "{\"started\":%s,\"scanning\":%s}",
             started ? "true" : "false", wifi_scanning() ? "true" : "false");
    /* A refusal is not an error: "already scanning" is the common case and a
     * UI that polls should not have to treat it as a failure. */
    api_reply_ok(reply, ctx, id, buf);
}

/* Passed through verbatim. wifi_scan_json already returns a complete object
 * -- {"ok":..,"scanning":..,"aps":[..]} -- and wrapping it produced
 * result.networks.aps, which is the sort of shape a UI author discovers at
 * the worst moment. */
static void op_wifi_scan_results(const char *params, api_reply_fn reply, void *ctx, long id) {
    (void)params;
    int n = wifi_scan_json(buf, sizeof buf);
    if (n <= 0) { api_reply_err(reply, ctx, id, "unavailable", "no scan results"); return; }
    api_reply_ok(reply, ctx, id, buf);
}

static void op_wifi_networks(const char *params, api_reply_fn reply, void *ctx, long id) {
    (void)params;
    int n = wifi_networks_json(buf, sizeof buf);
    if (n <= 0) { api_reply_err(reply, ctx, id, "unavailable", "no network store"); return; }
    api_reply_ok(reply, ctx, id, buf);
}

static void op_wifi_connect(const char *params, api_reply_fn reply, void *ctx, long id) {
    char ssid[CFG_SSID_MAX + 1], psk[CFG_PSK_MAX + 1];
    if (!api_json_str(params, "ssid", ssid, sizeof ssid) || !ssid[0]) {
        api_reply_err(reply, ctx, id, "bad_request", "need \"ssid\"");
        return;
    }
    /* No "psk" means "use what is stored", which is how a saved network is
     * rejoined without the UI having to hold the password. */
    bool have_psk = api_json_str(params, "psk", psk, sizeof psk);
    if (!wifi_connect(ssid, have_psk ? psk : NULL)) {
        /* Two reasons, and the caller can act on only one of them, so say
         * which. A join while BLE holds the radio wedges the driver, so it is
         * refused rather than attempted -- see the comment on wifi_connect. */
        api_reply_err(reply, ctx, id, "rejected",
                      bt_radio_busy()
                        ? "Bluetooth is using the shared radio; stop scanning "
                          "and disconnect devices, then join"
                        : "the credential store refused that ssid/psk");
        return;
    }
    snprintf(buf, sizeof buf, "{\"joining\":true,\"ssid\":\"%s\"}", ssid);
    api_reply_ok(reply, ctx, id, buf);
}

static void op_wifi_forget(const char *params, api_reply_fn reply, void *ctx, long id) {
    char ssid[CFG_SSID_MAX + 1];
    if (!api_json_str(params, "ssid", ssid, sizeof ssid) || !ssid[0]) {
        api_reply_err(reply, ctx, id, "bad_request", "need \"ssid\"");
        return;
    }
    bool had = cfg_net_forget(ssid);
    snprintf(buf, sizeof buf, "{\"forgotten\":%s}", had ? "true" : "false");
    api_reply_ok(reply, ctx, id, buf);
}

static void op_wifi_mode(const char *params, api_reply_fn reply, void *ctx, long id) {
    char mode[16];
    if (api_json_str(params, "mode", mode, sizeof mode)) {
        if (!cfg_set_mode(mode)) {
            api_reply_err(reply, ctx, id, "bad_request",
                          "mode must be one of auto, sta, ap, both, off");
            return;
        }
        /* Asks for the arrangement; wifi_poll() is what moves the radio. */
        wifi_apply_mode();
    }
    snprintf(buf, sizeof buf, "{\"mode\":\"%s\"}", cfg_mode());
    api_reply_ok(reply, ctx, id, buf);
}

static void op_wifi_ap(const char *params, api_reply_fn reply, void *ctx, long id) {
    char ssid[CFG_SSID_MAX + 1], psk[CFG_PSK_MAX + 1];
    if (api_json_str(params, "ssid", ssid, sizeof ssid)) {
        bool have_psk = api_json_str(params, "psk", psk, sizeof psk);
        if (!cfg_set_ap(ssid, have_psk ? psk : NULL)) {
            api_reply_err(reply, ctx, id, "rejected", "the store refused that ap config");
            return;
        }
        wifi_apply_mode();
    }
    snprintf(buf, sizeof buf, "{\"ssid\":\"%s\"}", cfg_ap_ssid());
    api_reply_ok(reply, ctx, id, buf);
}

static const op_def_t wifi_ops[] = {
    { "wifi.status",       op_wifi_status },
    { "wifi.scan",         op_wifi_scan },
    { "wifi.scan_results", op_wifi_scan_results },
    { "wifi.networks",     op_wifi_networks },
    { "wifi.connect",      op_wifi_connect },
    { "wifi.forget",       op_wifi_forget },
    { "wifi.mode",         op_wifi_mode },
    { "wifi.ap",           op_wifi_ap },
    { NULL, NULL }
};

static const char *const wifi_topics[] = {
    "wifi.state", "wifi.scan_done", NULL
};

const op_module_t op_module_wifi = {
    .ns = "wifi", .ops = wifi_ops, .topics = wifi_topics, .init = NULL
};

#endif /* CONFIG_NET_WIFI */
