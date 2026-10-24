/* The sys and board groups on the gateway: thin calls into existing code. */
#include <malloc.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "autoconf.h"
#include "pex/pex_gw.h"
#include "fs/fs.h"
#include "pico/unique_id.h"
#include "hardware/clocks.h"
#include "net/mdns.h"
#include "net/sntp_time.h"
#include "util/stackmark.h"
#if CONFIG_LUA_ENABLE
#include "lua/lrt.h"
#endif

int64_t pex_sys_time(void) { return sntp_time_unix(); }
const char *pex_sys_hostname(void) { return mdns_hostname(); }
const char *pex_sys_version(void) {
#ifdef BTGW_VERSION_STR
    return BTGW_VERSION_STR;
#else
    return "dev";
#endif
}

int pex_sys_mem(pex_sys_mem_t *o) {
    if (!o) { return PEX_EINVAL; }
    memset(o, 0, sizeof *o);
    extern char __StackLimit;
    char *brk = sbrk(0);
    size_t top = (brk != (char *)-1 && brk < &__StackLimit) ? (size_t)(&__StackLimit - brk) : 0;
    struct mallinfo mi = mallinfo();
    o->c_heap_free = (uint32_t)(top + (size_t)mi.fordblks);
#if CONFIG_LUA_ENABLE
    lrt_stats_t ls;
    lrt_stats(&ls);
    o->lua_used = ls.used;
    o->lua_size = ls.arena_bytes;
#endif
    pex_mem_stats_t ps;
    pex_pool_stats(&ps);
    o->pex_free = ps.pool_free;
    o->pex_size = ps.pool_size;
    o->stack_max = stackmark_max_used();
    return PEX_OK;
}

/* One top-level string from cfg/<name>.json. Same rule as Lua sys.secret:
 * the name cannot steer the path, escapes are not decoded, and there is no
 * write. The scan buffer is static, so this is main-loop only, as the Lua
 * binding is. */
int pex_sys_secret(const char *name, const char *key, char *out, size_t cap) {
    if (!name || !name[0] || !key || !out || cap == 0) { return PEX_EINVAL; }
    for (const char *p = name; *p; p++) {
        if (*p == '/' || *p == '\\' || *p == '.') { return PEX_EINVAL; }
    }
    char path[48];
    snprintf(path, sizeof path, "cfg/%s.json", name);
    static char buf[1024];
    size_t sz = 0;
    if (!fs_stat(path, &sz) || sz == 0 || sz >= sizeof buf) { return PEX_ENOENT; }
    int h = fs_begin_read(path, NULL);
    if (h < 0) { return PEX_ENOENT; }
    int n = fs_read(h, (uint8_t *)buf, sz);
    fs_end_read(h);
    if (n < 0) { return PEX_EIO; }
    buf[n] = 0;
    char needle[64];
    snprintf(needle, sizeof needle, "\"%s\"", key);
    const char *k = strstr(buf, needle);
    if (!k) { return PEX_ENOENT; }
    const char *v = strchr(k + strlen(needle), ':');
    if (!v) { return PEX_ENOENT; }
    v++;
    while (*v == ' ' || *v == '\t' || *v == '\n' || *v == '\r') { v++; }
    if (*v != '"') { return PEX_ENOENT; }
    v++;
    const char *end = strchr(v, '"');
    if (!end) { return PEX_ENOENT; }
    size_t len = (size_t)(end - v);
    if (len + 1 > cap) { return PEX_ENOSPC; }
    memcpy(out, v, len);
    out[len] = 0;
    return (int)len;
}

const char *pex_board_name(void) { return PICO_BOARD; }
uint32_t pex_board_cpu_hz(void) { return clock_get_hz(clk_sys); }
int pex_board_uid(uint8_t *out, size_t cap) {
    pico_unique_board_id_t id;
    pico_get_unique_board_id(&id);
    size_t n = cap < sizeof id.id ? cap : sizeof id.id;
    memcpy(out, id.id, n);
    return (int)n;
}
