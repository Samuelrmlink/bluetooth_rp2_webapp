/* The sys and board groups on the gateway: thin calls into existing code. */
#include <malloc.h>
#include <string.h>
#include <unistd.h>
#include "autoconf.h"
#include "pex/pex_gw.h"
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

const char *pex_board_name(void) { return PICO_BOARD; }
uint32_t pex_board_cpu_hz(void) { return clock_get_hz(clk_sys); }
int pex_board_uid(uint8_t *out, size_t cap) {
    pico_unique_board_id_t id;
    pico_get_unique_board_id(&id);
    size_t n = cap < sizeof id.id ? cap : sizeof id.id;
    memcpy(out, id.id, n);
    return (int)n;
}
