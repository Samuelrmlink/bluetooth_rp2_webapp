/*
 * Module registry. The table is static and built at compile time from whatever
 * Kconfig enabled -- there is no runtime registration in C, because everything
 * is known at link time. Lua's ops.register() (phase 6) adds a second, dynamic
 * tier on top of this one.
 */
#include <stdio.h>
#include <string.h>

#include "api/ops.h"
#include "autoconf.h"

extern const op_module_t op_module_sys;
#if CONFIG_BT_LE
extern const op_module_t op_module_bt;
#endif
#if CONFIG_LUA_ENABLE
extern const op_module_t op_module_lua;
#endif
#if CONFIG_IO_PINS
extern const op_module_t op_module_io;
#endif
#if CONFIG_NET_WIFI
extern const op_module_t op_module_wifi;
#endif

static const op_module_t *const modules[] = {
    &op_module_sys,
#if CONFIG_BT_LE
    &op_module_bt,
#endif
#if CONFIG_LUA_ENABLE
    &op_module_lua,
#endif
#if CONFIG_IO_PINS
    &op_module_io,
#endif
#if CONFIG_NET_WIFI
    &op_module_wifi,
#endif
};
#define N_MODULES (sizeof(modules) / sizeof(modules[0]))

bool ops_init_all(void) {
    bool ok = true;
    for (size_t i = 0; i < N_MODULES; i++) {
        if (modules[i]->init && !modules[i]->init()) {
            printf("ops: module %s failed to init\n", modules[i]->ns);
            ok = false;
        }
    }
    return ok;
}

const op_def_t *ops_lookup(const char *name) {
    for (size_t i = 0; i < N_MODULES; i++) {
        for (const op_def_t *d = modules[i]->ops; d && d->name; d++) {
            if (strcmp(d->name, name) == 0) {
                return d;
            }
        }
    }
    return NULL;
}

size_t ops_modules_json(char *dst, size_t cap) {
    size_t n = (size_t)snprintf(dst, cap, "[");
    for (size_t i = 0; i < N_MODULES && n + 32 < cap; i++) {
        n += (size_t)snprintf(dst + n, cap - n, "%s\"%s\"", i ? "," : "", modules[i]->ns);
    }
    if (n + 2 < cap) {
        n += (size_t)snprintf(dst + n, cap - n, "]");
    }
    return n;
}
