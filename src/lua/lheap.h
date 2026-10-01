#ifndef BTGW_LHEAP_H
#define BTGW_LHEAP_H

/*
 * The Lua heap: a first-fit free list over one static arena.
 *
 * A dedicated block, not the system heap. A runaway script then hits its own
 * ceiling and gets an ordinary Lua out-of-memory error, instead of starving
 * lwIP or BTstack and taking the whole device down. Static so the link-time
 * RAM check can see every byte of it.
 *
 * This lives apart from lrt.c for one practical reason: lrt.c cannot be
 * compiled on a host (it pulls in pico/time.h, fs/fs.h, api/api.h, net/ws.h),
 * so while the allocator was buried in it the only way to test a change was to
 * flash it. Two attempts at the in-place realloc below shipped and had to be
 * reverted -- one leaked every buffer's high-water mark, the other corrupted
 * the byte accounting -- and a host test would have caught both in a second.
 * See test/host/heap_test.c, which asserts the invariants each one broke.
 *
 * LHEAP_KB lets that test build a small arena without autoconf.h.
 */
#include <stdbool.h>
#include <stddef.h>

#ifndef LHEAP_KB
#include "autoconf.h"
#define LHEAP_KB CONFIG_LUA_HEAP_KB
#endif

void  lheap_init(void);

/* Raw block operations. `want` of 0 is not meaningful; callers go through
 * lheap_realloc, which is the lua_Alloc shape. */
void *lheap_alloc(size_t want);
void  lheap_free(void *p);

/*
 * The allocator Lua sees, minus the unused `ud`. Three rules from Lua's
 * lua_Alloc contract shape this, and all three have bitten:
 *
 *  1. When ptr == NULL, `osize` is a TYPE TAG, not a length (lmem.c). It must
 *     never be used as a size. The block header is authoritative.
 *  2. A failed grow must leave the old block byte-identical: luaM_realloc_
 *     runs an emergency GC and retries with the SAME pointer. So the fallback
 *     path allocates before it frees, which is a requirement, not a style.
 *  3. Returning the same pointer, or a block larger than nsize, is legal.
 */
void *lheap_realloc(void *ptr, size_t osize, size_t nsize);

size_t lheap_used(void);
size_t lheap_peak(void);
size_t lheap_capacity(void);
/* The largest single block that could be handed out now (walks the chain). */
size_t lheap_largest_free(void);
/* The size of the last request that failed, and how many have failed. */
void  lheap_fail_stats(size_t *last, size_t *count);

/* Structural self-check, for tests and for anything that wants to assert the
 * heap is still sane. Walks the whole chain; not cheap. Returns true when
 * every invariant holds, and otherwise sets *why to a static description.
 *
 * `live_bytes` is recomputed from the chain rather than read from the running
 * counter, which is precisely how a desynchronised lheap_used() gets caught. */
typedef struct {
    size_t blocks;
    size_t free_blocks;
    size_t live_bytes;      /* recomputed sum of live payloads */
    size_t free_bytes;
    size_t largest_free;
} lheap_stats_t;

bool lheap_check(lheap_stats_t *out, const char **why);

#endif
