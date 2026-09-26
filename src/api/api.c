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
#include "api/ops.h"
#include "net/http.h"
#include "net/ws.h"
#include "pico/time.h"
#include "autoconf.h"
#if CONFIG_LUA_ENABLE
#include "lua/lbind.h"
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

size_t api_json_escape(char *dst, size_t cap, const char *src) {
    size_t n = 0;
    for (; *src && n + 7 < cap; src++) {
        unsigned char ch = (unsigned char)*src;
        switch (ch) {
            case '"':  n += (size_t)snprintf(dst + n, cap - n, "\\\""); break;
            case '\\': n += (size_t)snprintf(dst + n, cap - n, "\\\\"); break;
            case '\n': n += (size_t)snprintf(dst + n, cap - n, "\\n");  break;
            case '\r': n += (size_t)snprintf(dst + n, cap - n, "\\r");  break;
            case '\t': n += (size_t)snprintf(dst + n, cap - n, "\\t");  break;
            default:
                if (ch < 0x20) {
                    n += (size_t)snprintf(dst + n, cap - n, "\\u%04x", ch);
                } else {
                    dst[n++] = (char)ch;
                }
        }
    }
    if (n < cap) {
        dst[n] = 0;
    }
    return n;
}

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

void api_emit(const char *topic, const char *json) {
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

static void op_sys_info(const char *params, api_reply_fn reply, void *ctx, long id) {
    (void)params;
    char mods[512];
    ops_modules_json(mods, sizeof(mods));
    char result[768];
    snprintf(result, sizeof(result),
             "{\"firmware\":\"bluetooth_rp2_webapp\",\"uptime_ms\":%lu,"
             "\"transports\":[\"le\"%s],\"clients\":%d,\"modules\":%s}",
             (unsigned long)to_ms_since_boot(get_absolute_time()),
#if CONFIG_BT_CLASSIC
             ",\"classic\"",
#else
             "",
#endif
             http_client_count(), mods);
    api_reply_ok(reply, ctx, id, result);
}

static const op_def_t sys_ops[] = {
    { "sys.info", op_sys_info },
    { NULL, NULL }
};

static const char *const sys_topics[] = { "hello", "log", NULL };

const op_module_t op_module_sys = {
    .ns = "sys", .ops = sys_ops, .topics = sys_topics, .init = NULL
};
