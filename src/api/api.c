/*
 * The JSON envelope over the WebSocket, and dispatch into the module registry.
 *
 * Three message shapes, and only three:
 *   {"id":17,"op":"sys.info","params":{...}}      request
 *   {"id":17,"ok":true,"result":{...}}            reply
 *   {"id":17,"ok":false,"error":{"code":..,"message":..}}
 *   {"evt":"scan.device","data":{...}}            unsolicited
 *
 * Errors always take that one shape so a client has a single thing to check,
 * rather than a status code AND a body AND a thrown network error.
 *
 * Parsing is a positional scanner, not a JSON library: the inputs are small,
 * flat and machine-generated. EMITTING is where correctness actually matters,
 * because device names and script errors flow outward -- hence api_json_escape,
 * which everything user-supplied must pass through.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>

#include "api/api.h"
#include "util/crumb.h"
#include "util/stackmark.h"
#include "launch.h"
#include "api/ops.h"
#include "fs/fs.h"
#include "fs/fs_migrate.h"
#include "net/http.h"
#include "net/ws.h"
#include "net/cyw43_wait.h"
#include "pico/time.h"
#include "autoconf.h"
#if CONFIG_PEX
#include "pex/pex_gw.h"
#endif
#if CONFIG_LUA_ENABLE
#include "lua/lbind.h"
#include "lua/lrt.h"
#endif

/* One scratch buffer: every caller runs from the single cooperative loop, one
 * at a time, and each fills and consumes it within the call. Static because
 * core0's stack is small and this is on the deepest path it has.
 *
 * Sized to WS_TX_MAX, because this holds the whole framed reply -- envelope
 * plus result. Undersizing it truncates a large result mid-JSON, which reaches
 * the client as a parse error rather than as anything that points at the
 * cause; a scan listing found the edge immediately. */
static char out_buf[WS_TX_MAX];
static char scratch[WS_TX_MAX - 256];

char  *api_scratch(void)      { return scratch; }
size_t api_scratch_size(void) { return sizeof(scratch); }

/* Results must leave room for the envelope around them. */
_Static_assert(WS_TX_MAX >= 1024, "reply buffer too small for a GATT tree");

/* api_json_escape lives in json_escape.c so test/host can link it without the
 * filesystem, the transports and the pico headers this file needs. */

/* Finds "key" at the top level and returns the first byte of its value. */
static const char *json_find(const char *json, const char *key) {
    char pat[40];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) {
        return NULL;
    }
    p += strlen(pat);
    while (*p == ' ' || *p == '\t') p++;
    if (*p != ':') {
        return NULL;
    }
    p++;
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

bool api_json_str(const char *json, const char *key, char *out, size_t cap) {
    const char *p = json_find(json, key);
    if (!p || *p != '"') {
        return false;
    }
    p++;
    size_t n = 0;
    while (*p && *p != '"' && n + 1 < cap) {
        if (*p != '\\' || !p[1]) {
            out[n++] = *p++;
            continue;
        }
        /* Escapes must be DECODED, not merely skipped over. Storing the
         * character after the backslash raw turns every \n in a Lua source
         * string into a literal 'n', which produces a syntax error on line 1
         * and nothing that points at the cause. */
        p++;
        switch (*p) {
            case 'n':  out[n++] = '\n'; p++; break;
            case 't':  out[n++] = '\t'; p++; break;
            case 'r':  out[n++] = '\r'; p++; break;
            case 'b':  out[n++] = '\b'; p++; break;
            case 'f':  out[n++] = '\f'; p++; break;
            case '"':  out[n++] = '"';  p++; break;
            case '\\': out[n++] = '\\'; p++; break;
            case '/':  out[n++] = '/';  p++; break;
            case 'u': {
                /* Only the BMP-ASCII range is meaningful here; anything above
                 * it is passed through as '?' rather than mangling the byte
                 * stream. */
                unsigned cp = 0;
                if (sscanf(p + 1, "%4x", &cp) == 1) {
                    p += 5;
                    out[n++] = (cp < 0x80) ? (char)cp : '?';
                } else {
                    out[n++] = *p++;
                }
                break;
            }
            default: out[n++] = *p++; break;
        }
    }
    out[n] = 0;
    return *p == '"';
}

bool api_json_int(const char *json, const char *key, long *out) {
    const char *p = json_find(json, key);
    if (!p) {
        return false;
    }
    char *end;
    long v = strtol(p, &end, 0);
    if (end == p) {
        return false;
    }
    *out = v;
    return true;
}

bool api_json_bool(const char *json, const char *key, bool *out) {
    const char *p = json_find(json, key);
    if (!p) {
        return false;
    }
    if (strncmp(p, "true", 4) == 0)  { *out = true;  return true; }
    if (strncmp(p, "false", 5) == 0) { *out = false; return true; }
    return false;
}

/* The params object, or "{}" -- so op handlers never have to null-check. */
static const char *params_of(const char *json) {
    const char *p = json_find(json, "params");
    return (p && *p == '{') ? p : "{}";
}

void api_reply_ok(api_reply_fn reply, void *ctx, long id, const char *result_json) {
    snprintf(out_buf, sizeof(out_buf), "{\"id\":%ld,\"ok\":true,\"result\":%s}",
             id, result_json && *result_json ? result_json : "null");
    reply(out_buf, ctx);
}

void api_reply_err(api_reply_fn reply, void *ctx, long id, const char *code, const char *msg) {
    char esc[256];
    api_json_escape(esc, sizeof(esc), msg ? msg : "");
    snprintf(out_buf, sizeof(out_buf),
             "{\"id\":%ld,\"ok\":false,\"error\":{\"code\":\"%s\",\"message\":\"%s\"}}",
             id, code, esc);
    reply(out_buf, ctx);
}

/* central.c defines this when BLE is linked. A profile telemetry report is
 * the host-visible end of an application handshake. On a build with no LE
 * central the symbol stays null. */
void le_on_profile_telemetry(const char *topic, const char *json) __attribute__((weak));

void api_emit(const char *topic, const char *json) {
    if (le_on_profile_telemetry) { le_on_profile_telemetry(topic, json); }
    /* Static and full-size: a gatt.discovered for a device with a real service
     * set runs to several kilobytes, and a 1 KB stack buffer here silently
     * truncated it mid-string -- which reaches the browser as a JSON parse
     * error with nothing pointing at the cause. */
    static char line[WS_TX_MAX];
    int n = snprintf(line, sizeof(line), "{\"evt\":\"%s\",\"data\":%s}",
                     topic, json ? json : "null");
    if (n < 0 || (size_t)n >= sizeof(line)) {
        /* Say so rather than emit half an object. */
        snprintf(line, sizeof(line),
                 "{\"evt\":\"log\",\"data\":{\"level\":\"error\",\"message\":"
                 "\"event %s dropped: %d bytes exceeds the frame buffer\"}}", topic, n);
    }
    http_broadcast_line(line);
}

void api_handle_text(const char *text, api_reply_fn reply, void *ctx) {
    long id = 0;
    (void)api_json_int(text, "id", &id);

    char op[48];
    if (!api_json_str(text, "op", op, sizeof(op))) {
        api_reply_err(reply, ctx, id, "bad_request", "missing \"op\"");
        return;
    }

    const op_def_t *def = ops_lookup(op);
    if (def) {
        def->fn(params_of(text), reply, ctx, id);
        return;
    }
#if CONFIG_PEX
    /* Native modules register ops on the bus. This stays outside the Lua
     * block: a no-Lua image still runs printer_hub and the other PEX apps,
     * and their ops have to answer. */
    if (pex_gw_dispatch(op, params_of(text), reply, ctx, id)) {
        return;
    }
#endif
#if CONFIG_LUA_ENABLE
    /* A script may have registered this name. Built-ins are checked first, and
     * ops.register refuses to shadow one, so a client can trust that sys.info
     * always means what the firmware says it means. */
    if (lbind_dispatch(op, params_of(text), reply, ctx, id)) {
        return;
    }
#endif
    api_reply_err(reply, ctx, id, "unknown_op", op);
}

/* ---- sys.* : the one module that is always present ---------------------- */

/* sys.reboot: answer first, then reboot from the main loop half a second
 * later (api_reboot_due), so the reply and a sys.rebooting event reach every
 * client before the link goes. The reset itself is ota_reboot's, which
 * powers the radio down first (see ota_slot.c). */
static uint32_t s_reboot_at_ms;

static uint32_t now_ms(void) { return to_ms_since_boot(get_absolute_time()); }

static void op_sys_reboot(const char *params, api_reply_fn reply, void *ctx, long id) {
    (void)params;
    s_reboot_at_ms = now_ms() + 500;
    if (!s_reboot_at_ms) { s_reboot_at_ms = 1; }
    api_reply_ok(reply, ctx, id, "{\"rebooting\":true,\"in_ms\":500}");
    api_emit("sys.rebooting", "{}");
}

void api_reboot_in(uint32_t ms) {
    s_reboot_at_ms = now_ms() + ms;
    if (!s_reboot_at_ms) { s_reboot_at_ms = 1; }
}

bool api_reboot_due(void) {
    return s_reboot_at_ms && (int32_t)(now_ms() - s_reboot_at_ms) >= 0;
}

/* What stopped the previous boot (src/util/crumb.h): the main-loop stage,
 * the last few deep breadcrumbs, and a HardFault's frame and status. */
static void op_sys_fault(const char *params, api_reply_fn reply, void *ctx, long id) {
    (void)params;
    char r[900];
    crumb_report_json(r, sizeof r);
    api_reply_ok(reply, ctx, id, r);
}

static void op_sys_info(const char *params, api_reply_fn reply, void *ctx, long id) {
    (void)params;
    char mods[512];
    ops_modules_json(mods, sizeof(mods));
    char fsj[256];
    fs_migrate_json(fsj, sizeof fsj);
    /* The Lua arena by lifetime (lrt_mem_json): both sides and the gap,
     * the short-term scratches. */
    char memj[400] = "null";
#if CONFIG_LUA_ENABLE
    lrt_mem_json(memj, sizeof memj);
#endif
    static char result[2048];
    snprintf(result, sizeof(result),
             "{\"firmware\":\"bluetooth_rp2_webapp\",\"board\":\"%s\",\"uptime_ms\":%lu,"
             "\"watchdog_reboot\":%s,\"safe_mode\":%s,\"radio_wait_max_us\":%lu,"
             "\"radio_wait_max_at_ms\":%lu,\"radio_wait_max_asked_us\":%lu,"
             "\"radio_wait_usb_max_us\":%lu,\"radio_wait_total_ms\":%lu,"
             "\"transports\":[\"le\"%s],\"clients\":%d,\"modules\":%s,"
             "\"last_crumb\":%lu,\"last_fault_pc\":%lu,\"stack_max\":%lu,\"fs\":%s,\"lua_mem\":%s}",
             PICO_BOARD,
             (unsigned long)to_ms_since_boot(get_absolute_time()),
             /* The previous boot's loop stopped running. Reported here
              * because a device reached over WireGuard has no RTT, and a
              * wedge that silently reboots is a wedge nobody learns about. */
             boot_after_watchdog ? "true" : "false",
             launch_safe_mode() ? "true" : "false",
             /* The one wedge seen was inside the cyw43 ioctl wait. A single
              * wait of more than a few ms means the radio stopped answering,
              * which is the shape of that failure. */
             (unsigned long)cyw43_wait_max_us(),
             (unsigned long)cyw43_wait_max_at_ms(),
             (unsigned long)cyw43_wait_max_asked_us(),
             (unsigned long)cyw43_wait_usb_max_us(),
             (unsigned long)(cyw43_wait_total_us() / 1000u),
#if CONFIG_BT_CLASSIC_NAP
             ",\"classic\",\"bnep\"",
#elif CONFIG_BT_CLASSIC
             ",\"classic\"",
#else
             "",
#endif
             http_client_count(), mods,
             /* Where the previous boot's loop was when it stopped, and the PC
              * of a HardFault if that is what stopped it (src/util/crumb.h). */
             (unsigned long)crumb_last, (unsigned long)crumb_last_fault_pc,
             /* core0's deepest stack use since boot (util/stackmark.h). */
             (unsigned long)stackmark_max_used(),
             /* The mounted LittleFS, its migration (fs_migrate.h) and the
              * PEX partition it makes room for. */
             fsj, memj);
    api_reply_ok(reply, ctx, id, result);
}

/* sys.secret_put -- write cfg/<name>.json in one call.
 *
 * A convenience, NOT a security boundary. cfg/ is readable through the
 * ordinary filesystem API, so PUT /api/fs/cfg/<name>.json does the same job;
 * this exists because provisioning from a websocket session that is already
 * open is easier than opening an HTTP connection to do one write, and because
 * it validates the name rather than trusting a path.
 *
 * What protects a credential here is that cfg/ is git-ignored, not that the
 * device withholds it. The device is trusted; the repository is the thing
 * that must never see an account id or a private key.
 *
 * It follows script.stage's precedent in writing flash straight from the
 * WebSocket handler -- that is already how a staged script is persisted, and
 * provisioning happens once, not per frame.
 *
 * The reply reports the length and not the content, because a reply goes to
 * every client on the socket and there is no reason to broadcast it. */
static void op_sys_secret_put(const char *params, api_reply_fn reply, void *ctx,
                              long id) {
    char name[32];
    if (!api_json_str(params, "name", name, sizeof(name))) {
        api_reply_err(reply, ctx, id, "bad_request", "need \"name\"");
        return;
    }
    /* The name becomes a path, so it may be a bare identifier and nothing
     * else -- no separator, no dot, no traversal. */
    for (const char *q = name; *q; q++) {
        bool ok = (*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z') ||
                  (*q >= '0' && *q <= '9') || *q == '_' || *q == '-';
        if (!ok) {
            api_reply_err(reply, ctx, id, "bad_request",
                          "name must be letters, digits, underscore or dash");
            return;
        }
    }
    if (!name[0]) {
        api_reply_err(reply, ctx, id, "bad_request", "empty name");
        return;
    }

    static char body[1024];
    if (!api_json_str(params, "body", body, sizeof(body))) {
        api_reply_err(reply, ctx, id, "bad_request",
                      "need \"body\" (the file contents, as a JSON string)");
        return;
    }

    char path[64];
    snprintf(path, sizeof(path), "cfg/%s.json", name);
    size_t len = strlen(body);
    if (fs_begin_write(path) < 0) {
        api_reply_err(reply, ctx, id, "write_failed", "filesystem busy or unwritable");
        return;
    }
    if (fs_write((const uint8_t *)body, len) < 0 || fs_end_write() < 0) {
        api_reply_err(reply, ctx, id, "write_failed", "write failed");
        return;
    }
    char r[96];
    snprintf(r, sizeof(r), "{\"stored\":\"%s\",\"bytes\":%u,\"readable\":false}",
             path, (unsigned)len);
    api_reply_ok(reply, ctx, id, r);
}

static const op_def_t sys_ops[] = {
    { "sys.info", op_sys_info },
    { "sys.fault", op_sys_fault },
    { "sys.reboot", op_sys_reboot },
    { "sys.secret_put", op_sys_secret_put },
    { NULL, NULL }
};

static const char *const sys_topics[] = { "hello", "log", "sys.rebooting", NULL };

const op_module_t op_module_sys = {
    .ns = "sys", .ops = sys_ops, .topics = sys_topics, .init = NULL
};
