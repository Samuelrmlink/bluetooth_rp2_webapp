/*
 * The bus group: native modules serve the web UI. An op a module registers
 * is found after the built-in ones (ops_lookup) and before Lua's (api.c:
 * pex_gw_dispatch ahead of lbind_dispatch); it replies synchronously -- a
 * long job replies {"pending":...} and reports completion with an event,
 * exactly as the built-in ops do.
 */
#include <stdio.h>
#include <string.h>
#include "pex/pex_gw.h"
#include "api/ops.h"

#define BUS_OPS 16
typedef struct { char name[40]; pex_mod_t *mod; pex_op_fn fn; void *arg; } bus_op_t;
static bus_op_t s_ops[BUS_OPS];

static bool valid_name(const char *n) {
    const char *dot = strchr(n, '.');
    if (!dot || dot == n || !dot[1] || strlen(n) >= sizeof s_ops[0].name) { return false; }
    for (const char *p = n; *p; p++) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_' || *p == '.')) { return false; }
    }
    return strncmp(n, "pex.", 4) != 0;
}

int pex_bus_op(const char *name, pex_op_fn fn, void *arg) {
    pex_mod_t *m = pex_self();
    if (!name || !fn || !valid_name(name)) { return PEX_EINVAL; }
    if (ops_lookup(name)) { return PEX_EEXIST; }          /* never shadow a built-in */
    bus_op_t *free_slot = NULL;
    for (int i = 0; i < BUS_OPS; i++) {
        if (s_ops[i].fn && strcmp(s_ops[i].name, name) == 0) {
            if (s_ops[i].mod != m) { return PEX_EEXIST; }
            s_ops[i].fn = fn;
            s_ops[i].arg = arg;
            return PEX_OK;
        }
        if (!s_ops[i].fn && !free_slot) { free_slot = &s_ops[i]; }
    }
    if (!free_slot) { return PEX_ENOMEM; }
    snprintf(free_slot->name, sizeof free_slot->name, "%s", name);
    free_slot->mod = m;
    free_slot->fn = fn;
    free_slot->arg = arg;
    return PEX_OK;
}

int pex_bus_op_remove(const char *name) {
    pex_mod_t *m = pex_self();
    for (int i = 0; i < BUS_OPS; i++) {
        if (s_ops[i].fn && strcmp(s_ops[i].name, name) == 0 && s_ops[i].mod == m) {
            memset(&s_ops[i], 0, sizeof s_ops[i]);
            return PEX_OK;
        }
    }
    return PEX_ENOENT;
}

int pex_bus_emit(const char *topic, const char *json) {
    if (!topic || !*topic) { return PEX_EINVAL; }
    api_emit(topic, json && *json ? json : "null");
    return PEX_OK;
}

void pex_gw_bus_unload(pex_mod_t *m) {
    for (int i = 0; i < BUS_OPS; i++) {
        if (s_ops[i].fn && s_ops[i].mod == m) { memset(&s_ops[i], 0, sizeof s_ops[i]); }
    }
}

size_t pex_gw_bus_ops_json(char *dst, size_t cap) {
    size_t n = (size_t)snprintf(dst, cap, "[");
    bool first = true;
    for (int i = 0; i < BUS_OPS && n < cap; i++) {
        if (!s_ops[i].fn) { continue; }
        n += (size_t)snprintf(dst + n, cap - n, "%s{\"op\":\"%s\",\"module\":\"%s\"}", first ? "" : ",", s_ops[i].name,
                              pex_name(s_ops[i].mod));
        first = false;
    }
    if (n < cap) { n += (size_t)snprintf(dst + n, cap - n, "]"); }
    return n;
}

bool pex_gw_dispatch(const char *name, const char *params, api_reply_fn reply, void *ctx, long id) {
    bus_op_t *o = NULL;
    for (int i = 0; i < BUS_OPS; i++) {
        if (s_ops[i].fn && strcmp(s_ops[i].name, name) == 0) { o = &s_ops[i]; break; }
    }
    if (!o) { return false; }
    /* the params value alone, NUL-terminated (the request envelope follows it) */
    size_t pl = 0;
    const char *pv = pex_json_find(params, "", &pl);
    char *buf = api_scratch();
    size_t cap = api_scratch_size();
    size_t pcap = cap / 4;
    if (!pv || pl >= pcap) { pv = "{}"; pl = 2; }
    memcpy(buf, pv, pl);
    buf[pl] = 0;
    char *out = buf + pcap;
    out[0] = 0;
    int r = (int)(uint32_t)pex_call(o->mod, (uint32_t)(uintptr_t)o->fn, (uint32_t)(uintptr_t)o->arg,
                                    (uint32_t)(uintptr_t)buf, (uint32_t)(uintptr_t)out, (uint32_t)(cap - pcap - 1));
    if (r < 0) {
        out[cap - pcap - 1] = 0;
        api_reply_err(reply, ctx, id, r == PEX_EFAULT ? "module_faulted" : pex_strerror(r), out[0] ? out : pex_strerror(r));
        return true;
    }
    if ((size_t)r >= cap - pcap || r == 0) {
        api_reply_err(reply, ctx, id, "bad_reply", "the module's op returned no JSON or too much");
        return true;
    }
    out[r] = 0;
    api_reply_ok(reply, ctx, id, out);
    return true;
}
