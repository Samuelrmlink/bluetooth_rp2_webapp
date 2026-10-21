#ifndef BTGW_LHEAP_H
#define BTGW_LHEAP_H

/*
 * The Lua heap: address-ordered first fit, with boundary tags, over one
 * static arena (one word of header per block; see lheap.c).
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
#include <stdint.h>

#ifndef LHEAP_KB
#include "autoconf.h"
#define LHEAP_KB CONFIG_LUA_HEAP_KB
#endif

void  lheap_init(void);

/* Raw block operations. `want` of 0 is not meaningful; callers go through
 * lheap_realloc, which is the lua_Alloc shape. */
void *lheap_alloc(size_t want);
void  lheap_free(void *p);
/* The same, but from the top of the arena (see lheap.c): for long-lived C
 * allocations made on Lua's behalf. Freed with lheap_free. Not zeroed. */
void *lheap_alloc_high(size_t want);
/* While open (they nest), lheap_alloc -- and so every new block Lua asks for
 * -- comes from the top, as lheap_alloc_high does. lrt.c opens it around
 * require(): a module's code is loaded and dropped as a unit, and from the
 * top it never ends up interleaved with the scripts' long-lived data at the
 * bottom (which is what cut the free space into pieces: each module loaded
 * for a job and dropped after it left holes between the objects created
 * while it ran). lheap_init closes it. */
void  lheap_high_begin(void);
void  lheap_high_end(void);
/* First fit from the bottom whatever the placement: for what a unit creates
 * but does not own -- strings (interned, so a module's constant is the same
 * object as the value some long-lived table holds), userdata, threads. A
 * reallocation that has to move a block always goes here too. */
void *lheap_alloc_low(size_t want);
/* Bumped by every lheap_init (a Lua VM restart). A block from an older
 * generation is gone -- the arena was reset under it -- and must not be
 * passed to lheap_free. */
uint32_t lheap_generation(void);

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
/* Every block in address order: its payload, size, and whether it is free. */
typedef void (*lheap_walk_fn)(const void *payload, size_t size, bool free, void *ctx);
void lheap_walk(lheap_walk_fn cb, void *ctx);
/* Where the arena is, and the block header size -- for a raw dump. */
const void *lheap_base(void);
size_t lheap_header_size(void);
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

/* The same figures without the checks: one walk, for a script that wants to
 * watch its own fragmentation (sys.heap). */
void lheap_stats(lheap_stats_t *out);

#endif
