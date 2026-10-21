/* gwbus: the bus, json and sys groups -- a native module serving the web UI.
 *   pextest.echo {..}  -> {"echo":<params>,"calls":n}
 *   pextest.sys        -> {"time":..,"host":..,"version":..,"heap":..,"pex_free":..}
 *   pextest.emit {"n":k} -> emits event "pextest.event" {"k":k}, -> {"emitted":true}
 *   pextest.fail       -> an error reply */
#include "pex.h"

static int calls;

static int op_echo(void *arg, const char *params, char *reply, size_t cap) {
    pex_jw_t w;
    pex_jw_init(&w, reply, cap);
    pex_jw_obj(&w, NULL);
    pex_jw_raw(&w, "echo", params);
    pex_jw_int(&w, "calls", ++calls);
    pex_jw_end(&w);
    return pex_jw_finish(&w);
}

static int op_sys(void *arg, const char *params, char *reply, size_t cap) {
    pex_sys_mem_t m;
    pex_sys_mem(&m);
    pex_jw_t w;
    pex_jw_init(&w, reply, cap);
    pex_jw_obj(&w, NULL);
    pex_jw_int(&w, "time", pex_sys_time());
    pex_jw_str(&w, "host", pex_sys_hostname());
    pex_jw_str(&w, "version", pex_sys_version());
    pex_jw_int(&w, "heap", m.c_heap_free);
    pex_jw_int(&w, "lua_size", m.lua_size);
    pex_jw_int(&w, "pex_size", m.pex_size);
    pex_jw_int(&w, "pex_free", m.pex_free);
    pex_jw_str(&w, "board", pex_board_name());
    pex_jw_end(&w);
    return pex_jw_finish(&w);
}

static int op_emit(void *arg, const char *params, char *reply, size_t cap) {
    int64_t k = 0;
    pex_json_int(params, "n", &k);
    char ev[32];
    pex_snprintf(ev, sizeof ev, "{\"k\":%d}", (int)k);
    pex_bus_emit("pextest.event", ev);
    return pex_snprintf(reply, cap, "{\"emitted\":true}");
}

static int op_fail(void *arg, const char *params, char *reply, size_t cap) {
    pex_snprintf(reply, cap, "deliberate failure");
    return PEX_EINVAL;
}

int pex_main(const pex_host_t *host, uint32_t reason, void *arg) {
    if (reason == PEX_INIT) {
        if (pex_bus_op("pextest.echo", op_echo, 0) || pex_bus_op("pextest.sys", op_sys, 0) ||
            pex_bus_op("pextest.emit", op_emit, 0) || pex_bus_op("pextest.fail", op_fail, 0)) {
            return PEX_EINIT;
        }
        /* a built-in op name must be refused */
        if (pex_bus_op("sys.info", op_echo, 0) != PEX_EEXIST) { return PEX_EINIT; }
    }
    return PEX_OK;
}
