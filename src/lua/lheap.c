#include <stdint.h>
#include <string.h>

#include "lua/lheap.h"

#define ARENA_BYTES ((size_t)LHEAP_KB * 1024u)

/*
 * A first-fit free list. Lua's allocation pattern is many small, short-lived
 * blocks, which this handles adequately; the point is the hard ceiling, not
 * peak allocator performance.
 *
 * Two structural invariants make the in-place realloc below simple, and both
 * hold by induction over the only three mutators (alloc, free, realloc):
 *
 *   - The chain is a GAPLESS ADDRESS-ORDERED PARTITION of the arena, so
 *     b->next is always the physical right-hand neighbour. Absorbing it needs
 *     no address comparison.
 *   - lheap_free coalesces completely, so NO TWO ADJACENT FREE BLOCKS ever
 *     coexist. Absorbing one neighbour therefore already yields the maximum
 *     in-place growth available.
 *
 * lheap_check() asserts both, which is what makes them safe to rely on.
 */
typedef struct blk { size_t size; struct blk *next; bool free; } blk_t;
#define HDR ((size_t)((sizeof(blk_t) + 7u) & ~7u))
#define ALIGN8(n) (((n) + 7u) & ~(size_t)7u)

/* A tail smaller than this is not worth a header, so it stays with the block.
 * Handing back an over-sized block is legal (contract rule 3). */
#define MIN_SPLIT 32u

static uint8_t arena[ARENA_BYTES] __attribute__((aligned(8)));
static blk_t *heap_head;
static size_t heap_used, heap_peak;
/* The last request that found no block big enough, and how many have. With a
 * first-fit arena "not enough memory" usually means "no hole that size", not
 * "no bytes": these say which, without a debugger. */
static size_t fail_last, fail_count;

void lheap_init(void) {
    heap_head = (blk_t *)arena;
    heap_head->size = ARENA_BYTES - HDR;
    heap_head->next = NULL;
    heap_head->free = true;
    heap_used = 0;
    heap_peak = 0;
}

static blk_t *block_of(void *p) {
    return (blk_t *)((uint8_t *)p - HDR);
}

/*
 * `b` is a LIVE block with b->size >= want. Give the tail back as a free block
 * if it is big enough to carry a header, and merge it with its right-hand
 * neighbour if that is free too (preserving the no-adjacent-free invariant).
 *
 * This deliberately does NOT touch heap_used: the caller knows whether the
 * block is newly live or was already counted, and one of the two reverted
 * attempts at this fix went wrong by having both the splitter and the caller
 * adjust the same bytes. Exactly one of them may.
 */
static void split_tail(blk_t *b, size_t want) {
    if (b->size < want + HDR + MIN_SPLIT) {
        return;
    }
    blk_t *n = (blk_t *)((uint8_t *)b + HDR + want);
    n->size = b->size - want - HDR;
    n->next = b->next;
    n->free = true;
    b->size = want;
    b->next = n;
    while (n->next && n->next->free) {
        n->size += HDR + n->next->size;
        n->next = n->next->next;
    }
}

void *lheap_alloc(size_t want) {
    want = ALIGN8(want);
    for (blk_t *b = heap_head; b; b = b->next) {
        if (!b->free || b->size < want) {
            continue;
        }
        b->free = false;
        split_tail(b, want);
        heap_used += b->size;
        if (heap_used > heap_peak) { heap_peak = heap_used; }
        return (uint8_t *)b + HDR;
    }
    fail_last = want;
    fail_count++;
    return NULL;
}

void lheap_walk(lheap_walk_fn cb, void *ctx) {
    for (blk_t *b = heap_head; b; b = b->next) {
        cb((uint8_t *)b + HDR, b->size, b->free, ctx);
    }
}

const void *lheap_base(void) { return arena; }
size_t lheap_header_size(void) { return HDR; }

size_t lheap_largest_free(void) {
    size_t big = 0;
    for (blk_t *b = heap_head; b; b = b->next) {
        if (b->free && b->size > big) { big = b->size; }
    }
    return big;
}

void lheap_fail_stats(size_t *last, size_t *count) {
    if (last)  { *last = fail_last; }
    if (count) { *count = fail_count; }
}

void lheap_free(void *p) {
    if (!p) {
        return;
    }
    blk_t *b = block_of(p);
    b->free = true;
    heap_used -= b->size;
    for (blk_t *c = heap_head; c; c = c->next) {     /* coalesce forward */
        while (c->free && c->next && c->next->free) {
            c->size += HDR + c->next->size;
            c->next = c->next->next;
        }
    }
}

void *lheap_realloc(void *ptr, size_t osize, size_t nsize) {
    (void)osize;   /* a TYPE TAG when ptr is NULL; the header is truth */

    if (nsize == 0) { lheap_free(ptr); return NULL; }
    if (!ptr)       { return lheap_alloc(nsize); }

    blk_t *b = block_of(ptr);
    const size_t old  = b->size;
    const size_t want = ALIGN8(nsize);

    if (want == old) {
        return ptr;
    }

    if (want < old) {
        /* Shrink in place. This is a CORRECTNESS fix as much as a footprint
         * one: the old allocate-copy-free path made shrinking 32 KB to 20 KB
         * require 20 KB free, so a shrink could fail with out-of-memory while
         * releasing memory -- and Lua assumes shrinks succeed
         * (luaM_shrinkvector_ raises on NULL). */
        split_tail(b, want);
        heap_used -= (old - b->size);
        return ptr;
    }

    /* Grow. The right-hand neighbour is b->next (gapless partition), and if
     * it is free it is the ONLY free space adjacent to us (no two adjacent
     * free blocks), so this is the whole of the in-place opportunity. */
    blk_t *n = b->next;
    if (n && n->free && (old + HDR + n->size) >= want) {
        b->size = old + HDR + n->size;
        b->next = n->next;
        split_tail(b, want);
        heap_used += (b->size - old);
        if (heap_used > heap_peak) { heap_peak = heap_used; }
        return ptr;
    }

    /* Allocate before freeing -- see rule 2 in lheap.h. */
    void *np = lheap_alloc(nsize);
    if (!np) {
        return NULL;
    }
    memcpy(np, ptr, old < nsize ? old : nsize);
    lheap_free(ptr);
    return np;
}

size_t lheap_used(void)     { return heap_used; }
size_t lheap_peak(void)     { return heap_peak; }
size_t lheap_capacity(void) { return ARENA_BYTES; }

bool lheap_check(lheap_stats_t *out, const char **why) {
    lheap_stats_t s;
    memset(&s, 0, sizeof s);
    const char *dummy = NULL;
    if (!why) { why = &dummy; }

    uint8_t *walk = arena;
    blk_t *prev = NULL;
    for (blk_t *c = heap_head; c; prev = c, c = c->next) {
        if ((uint8_t *)c != walk) {
            *why = "chain does not tile the arena (gap or overlap)";
            return false;
        }
        if (c->size & 7u) {
            *why = "block size is not a multiple of 8";
            return false;
        }
        if ((uint8_t *)c + HDR + c->size > arena + ARENA_BYTES) {
            *why = "block runs past the end of the arena";
            return false;
        }
        if (c->free && prev && prev->free) {
            *why = "two adjacent free blocks";
            return false;
        }
        s.blocks++;
        if (c->free) {
            s.free_blocks++;
            s.free_bytes += c->size;
            if (c->size > s.largest_free) { s.largest_free = c->size; }
        } else {
            s.live_bytes += c->size;
        }
        walk += HDR + c->size;
    }
    if (walk != arena + ARENA_BYTES) {
        *why = "chain ends before the end of the arena";
        return false;
    }
    if (s.live_bytes != heap_used) {
        *why = "heap_used disagrees with the sum of live payloads";
        return false;
    }
    if (out) { *out = s; }
    return true;
}
