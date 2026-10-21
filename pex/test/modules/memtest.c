/* memtest: the mem group (spec 7.2) -- long-term blocks, LIFO scratch arenas,
 * fixed-size pools, malloc fallback, accounting -- and, with mem_leak(), a
 * module that leaves everything allocated for the unload to reclaim. */
#include "pex.h"

static void *kept[4];
static pex_pool_t *kept_pool;

/* 0 = all good, else the first failing check. */
int mem_run(void) {
    pex_mem_stats_t s0, s1;
    if (pex_mem_stats(&s0) != 0) { return 1; }
    void *a = pex_alloc_long(100), *b = pex_alloc_long(2000), *c = pex_alloc_long(10);
    if (!a || !b || !c || ((uintptr_t)a & 7) || ((uintptr_t)b & 7)) { return 2; }
    memset(a, 1, 100); memset(b, 2, 2000); memset(c, 3, 10);
    pex_free(b);
    void *d = pex_alloc_long(1500);              /* reuses the freed hole */
    if (!d) { return 3; }
    pex_mem_stats(&s1);
    if (s1.long_blocks != s0.long_blocks + 3) { return 4; }
    pex_free(a); pex_free(c); pex_free(d);
    pex_mem_stats(&s1);
    if (s1.long_blocks != s0.long_blocks || s1.long_bytes != s0.long_bytes) { return 5; }

    void *dma = pex_alloc_ex(64, PEX_MEM_DMA);
    if (!dma || ((uintptr_t)dma & 31)) { return 6; }
    pex_free(dma);

    char *m = pex_malloc(16);
    if (!m) { return 7; }
    for (int i = 0; i < 15; i++) { m[i] = (char)('a' + i); }
    m[15] = 0;
    m = pex_realloc(m, 4000);
    if (!m || m[14] != 'o') { return 8; }
    int *z = pex_calloc(50, sizeof(int));
    for (int i = 0; i < 50; i++) { if (z[i]) { return 9; } }
    pex_free(m); pex_free(z);

    /* scratch: nested, LIFO */
    size_t av = pex_scratch_avail();
    pex_arena_t *outer = pex_scratch_begin(1024);
    if (!outer) { return 10; }
    char *p1 = pex_scratch_alloc(outer, 100);
    char *p2 = pex_scratch_alloc(outer, 900);
    if (!p1 || !p2 || p2 < p1 + 100 || ((uintptr_t)p2 & 7)) { return 11; }
    if (pex_scratch_alloc(outer, 100) != NULL) { return 12; }     /* full */
    pex_arena_t *inner = pex_scratch_begin(256);
    if (!inner) { return 13; }
    if (pex_scratch_end(outer) != PEX_EORDER) { return 14; }      /* out of order: refused */
    if (pex_scratch_end(inner) != PEX_OK || pex_scratch_end(outer) != PEX_OK) { return 15; }
    if (pex_scratch_avail() != av) { return 16; }
    pex_mem_stats(&s1);
    if (s1.scratch_bytes != 0 || s1.scratch_peak < 1024 + 256) { return 17; }
    if (pex_scratch_begin(av + 1) != NULL) { return 18; }          /* larger than the gap */

    /* pools */
    pex_pool_t *pl = pex_pool_new(24, 8);
    if (!pl) { return 19; }
    void *o[9];
    for (int i = 0; i < 8; i++) { o[i] = pex_pool_get(pl); if (!o[i]) { return 20; } memset(o[i], i, 24); }
    if (pex_pool_get(pl) != NULL) { return 21; }
    pex_pool_put(pl, o[3]);
    o[8] = pex_pool_get(pl);
    if (o[8] != o[3]) { return 22; }
    pex_pool_delete(pl);
    pex_mem_stats(&s1);
    if (s1.long_blocks != s0.long_blocks) { return 23; }
    if (s1.pool_size == 0 || s1.pool_free > s1.pool_size) { return 24; }
    return 0;
}
PEX_EXPORT(mem_run);

/* Leave long-term blocks, a pool and an open scratch arena behind. */
int mem_leak(void) {
    for (int i = 0; i < 4; i++) { kept[i] = pex_alloc_long(300 + 100 * i); if (!kept[i]) { return 1; } }
    kept_pool = pex_pool_new(32, 16);
    if (!kept_pool || !pex_pool_get(kept_pool)) { return 2; }
    if (!pex_scratch_begin(512)) { return 3; }
    pex_mem_stats_t s;
    pex_mem_stats(&s);
    return s.long_blocks >= 5 ? 0 : 4;
}
PEX_EXPORT(mem_leak);

int pex_main(const pex_host_t *host, uint32_t reason, void *arg) { return PEX_OK; }
