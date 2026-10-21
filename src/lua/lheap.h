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
#if defined(CONFIG_NET_HTTP_TLS) && CONFIG_NET_HTTP_TLS && defined(CONFIG_NET_TLS_POOL_KB)
#define LHEAP_POOL_KB CONFIG_NET_TLS_POOL_KB
#endif
#endif
#ifndef LHEAP_POOL_KB
#define LHEAP_POOL_KB 0
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
/* Close it for a moment, whatever the nesting (returns what to resume): for a
 * long-lived structure touched while a unit loads -- require()'s module cache
 * gets the NESTED module's entry while the outer one's main chunk runs. */
int   lheap_high_suspend(void);
void  lheap_high_resume(int depth);
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
    /* lheap_stats only: the largest free block is the gap between the two
     * sides; the free space below it (the scripts' data side) and above it
     * (modules, long-lived buffers, scratch pages) is in holes. */
    size_t largest_free_low, largest_free_high;
    size_t free_low, free_high;
    size_t gap_offset;
} lheap_stats_t;

bool lheap_check(lheap_stats_t *out, const char **why);

/* The same figures without the checks (and the two sides): one walk, for a
 * script that wants to watch its own fragmentation (sys.heap). */
void lheap_stats(lheap_stats_t *out);

/*
 * The pool: a second region with the same allocator, in memory of its own
 * (LHEAP_POOL_KB; 0 = none, and every call fails). Not the Lua arena: Lua
 * never allocates from it and lheap_init (a VM restart) leaves it alone.
 * The HTTP client keeps its TLS session and its requests' bookkeeping here
 * (src/net/httpc.c), so HTTPS costs the scripts nothing and cannot be
 * starved by them. Not zeroed. lheap_pool_owns says whether a block is the
 * pool's (for a free that may be given either kind).
 */
void *lheap_pool_alloc(size_t want);
void  lheap_pool_free(void *p);
bool  lheap_pool_owns(const void *p);
typedef struct {
    size_t capacity, used, peak, largest_free;
    size_t blocks, free_blocks;
    uint32_t fails;              /* requests that found no room */
    size_t fail_last;
} lheap_pool_stats_t;
void  lheap_pool_stats(lheap_pool_stats_t *out);
void  lheap_pool_reset_peak(void);

/*
 * Scratch: short-lived C memory, grouped and freed whole (lheap.c). Memory is
 * kept apart BY LIFETIME rather than made movable: what lives as long as the
 * system (the TLS record buffers) and what lives as long as a module sit at
 * the top of the arena; the scripts' objects pack from the bottom; and what
 * lives as long as one request or one job comes from a scratch, whose pages
 * (carved from the top, LIFO with the job's modules) all go back when it
 * ends -- so a request never leaves holes between blocks that outlive it.
 *
 *   lheap_scratch_begin(page)    a scratch (page 0 = 2 KB pages)
 *   lheap_scratch_alloc(s, n)    from its pages; NULL when the arena is out
 *   lheap_free_any(p)            frees a scratch block or an arena block
 *   lheap_scratch_end(s)         every page back, whatever is still in them
 *   lheap_scratch_use(s)         the CURRENT scratch (returns the previous):
 *                                where C code that allocates on behalf of a
 *                                request (the HTTP client, mbedTLS) puts it
 *
 * A scratch from before an lheap_init (a VM restart) is dead: its alloc
 * returns NULL and its end only forgets it.
 */
typedef struct lheap_scratch lheap_scratch_t;
lheap_scratch_t *lheap_scratch_begin(size_t page);
void *lheap_scratch_alloc(lheap_scratch_t *s, size_t n);
void  lheap_free_any(void *p);
void  lheap_scratch_end(lheap_scratch_t *s);
lheap_scratch_t *lheap_scratch_use(lheap_scratch_t *s);
lheap_scratch_t *lheap_scratch_current(void);
typedef struct {
    size_t active, pages;        /* scratches alive now, their pages */
    size_t bytes, live;          /* in their pages / handed out, now */
    size_t bytes_peak, live_peak;
    uint32_t page_fails;         /* a page wanted and the arena had no room */
    uint32_t runs;               /* scratches begun since boot */
} lheap_scratch_stats_t;
void lheap_scratch_stats(lheap_scratch_stats_t *out);

#endif
