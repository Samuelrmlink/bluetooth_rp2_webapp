#include <stdint.h>
#include <string.h>

#include "lua/lheap.h"

#define ARENA_BYTES ((size_t)LHEAP_KB * 1024u)

/*
 * Address-ordered first fit with boundary tags.
 *
 * The arena is a gapless run of blocks. Each starts with ONE word: the
 * block's whole size (header included, a multiple of ALIGN) and two flags in
 * its low bits -- this block is free, the block before it is free. A free
 * block also keeps its size in its last word (the footer) and two links in
 * its payload, which chain every free block in ADDRESS ORDER. So:
 *
 *   - a live block costs one word (4 bytes on the RP2350) rather than the
 *     16-byte header of the first version, which on a heap of ~1300 Lua
 *     objects was 21 KB of the 176 KB arena (measured in mcusim with the iTi
 *     backend's two scripts running);
 *   - free coalesces with both neighbours in O(1) (the flags and the
 *     footer say where they are), instead of walking every block in the
 *     arena on every free;
 *   - allocation scans only the free blocks, lowest address first: the same
 *     placement as before (address-ordered first fit keeps live data packed
 *     at the bottom, which is what keeps the free space in one piece), at a
 *     fraction of the walk.
 *
 * Invariants (lheap_check asserts every one):
 *   - the blocks tile the arena exactly;
 *   - no two free blocks are adjacent (free coalesces completely), so a free
 *     right-hand neighbour is the whole of a block's in-place growth room;
 *   - a block's PREV flag is set exactly when the block before it is free,
 *     and a free block's footer holds its size;
 *   - the free list holds exactly the free blocks, in address order;
 *   - heap_used is the sum of the live blocks' payloads.
 *
 * Payloads are 4-byte aligned on the RP2350 (Cortex-M33 has no access that
 * needs more: LDRD, LDM and VLDR.64 want word alignment) and pointer-aligned
 * on a host.
 *
 * Two placements:
 *   low   first fit from the bottom: Lua's objects;
 *   high  last fit, carved from the top of the highest free block that fits:
 *         what is allocated and freed as a unit -- a module's code while it
 *         loads (lheap_high_begin/end), the HTTP client's TLS buffers. It then
 *         sits above everything Lua packs from the bottom, and when it goes
 *         its space rejoins the free space in the middle in one piece, rather
 *         than leaving holes between long-lived objects.
 */
typedef uintptr_t word_t;
#define WSZ ((size_t)sizeof(word_t))
#if defined(__arm__)
#define ALIGN ((size_t)4u)
#else
#define ALIGN ((size_t)sizeof(void *))
#endif
#define HDR WSZ
#define F_FREE ((word_t)1u)
#define F_PREV ((word_t)2u)
#define F_MASK ((word_t)3u)

typedef struct fblk { word_t h; struct fblk *next, *prev; } fblk_t;
#define RUP(n) (((n) + ALIGN - 1u) & ~(ALIGN - 1u))
/* The smallest block: a free one needs its header, two links and a footer. */
#define MINBLK RUP(sizeof(fblk_t) + WSZ)

static uint8_t arena[ARENA_BYTES] __attribute__((aligned(8)));
#define ARENA_END (arena + (ARENA_BYTES & ~(ALIGN - 1u)))
static fblk_t *fl_head, *fl_tail;
static size_t heap_used, heap_peak;
/* The last request that found no block big enough, and how many have. With a
 * first-fit arena "not enough memory" usually means "no hole that size", not
 * "no bytes": these say which, without a debugger. */
static size_t fail_last, fail_count;
static int s_high;

static uint32_t heap_gen;
uint32_t lheap_generation(void) { return heap_gen; }

static inline word_t *hdr(void *b) { return (word_t *)b; }
static inline size_t bsize(const void *b) { return (size_t)(*(const word_t *)b & ~F_MASK); }
static inline bool is_free(const void *b) { return (*(const word_t *)b & F_FREE) != 0; }
static inline uint8_t *after(void *b) { return (uint8_t *)b + bsize(b); }
static inline void put_footer(void *b, size_t size) { *(word_t *)((uint8_t *)b + size - WSZ) = (word_t)size; }
static inline void *prev_of(void *b) { return (uint8_t *)b - *(word_t *)((uint8_t *)b - WSZ); }

/* The block after `b`, if there is one, learns whether `b` is free. */
static inline void tell_next(void *b, bool free) {
    uint8_t *n = after(b);
    if (n >= ARENA_END) { return; }
    if (free) { *hdr(n) |= F_PREV; } else { *hdr(n) &= ~F_PREV; }
}

/* --- the free list --------------------------------------------------------- */
static void fl_remove(fblk_t *f) {
    if (f->prev) { f->prev->next = f->next; } else { fl_head = f->next; }
    if (f->next) { f->next->prev = f->prev; } else { fl_tail = f->prev; }
}

/* `nu` takes `old`'s place: nothing free lies between them. The two may
 * overlap (a block split a few bytes into its free neighbour), so the links
 * are read before any is written. */
static void fl_replace(fblk_t *old, fblk_t *nu) {
    fblk_t *p = old->prev, *n = old->next;
    nu->prev = p;
    nu->next = n;
    if (nu->prev) { nu->prev->next = nu; } else { fl_head = nu; }
    if (nu->next) { nu->next->prev = nu; } else { fl_tail = nu; }
}

/* In address order: from whichever end of the arena it is nearer. */
static void fl_insert(fblk_t *f) {
    fblk_t *before = NULL, *at;   /* insert between `before` and `at` */
    if ((uint8_t *)f - arena < ARENA_END - (uint8_t *)f) {
        for (at = fl_head; at && at < f; at = at->next) { before = at; }
    } else {
        for (before = fl_tail; before && before > f; before = before->prev) { }
        at = before ? before->next : fl_head;
    }
    f->prev = before;
    f->next = at;
    if (before) { before->next = f; } else { fl_head = f; }
    if (at) { at->prev = f; } else { fl_tail = f; }
}

/* Mark `b` (not on the list) free with this size, and tell its neighbour. */
static void make_free(void *b, size_t size, word_t prevflag) {
    *hdr(b) = (word_t)size | F_FREE | prevflag;
    put_footer(b, size);
    tell_next(b, true);
}

void lheap_init(void) {
    heap_gen++;
    fblk_t *f = (fblk_t *)arena;
    f->h = (word_t)(ARENA_END - arena) | F_FREE;
    put_footer(f, bsize(f));
    f->next = f->prev = NULL;
    fl_head = fl_tail = f;
    heap_used = 0;
    heap_peak = 0;
    s_high = 0;
}

/* The whole block a request of `want` payload bytes needs; 0 if impossible. */
static size_t need_of(size_t want) {
    if (want > ARENA_BYTES) { return 0; }
    size_t n = RUP(want + HDR);
    return n < MINBLK ? MINBLK : n;
}

static void count_live(size_t payload) {
    heap_used += payload;
    if (heap_used > heap_peak) { heap_peak = heap_used; }
}

static void *fail(size_t want) {
    fail_last = want;
    fail_count++;
    return NULL;
}

/* Free block `f` becomes live: its bottom `need` bytes, the rest stays free. */
static void *take_low(fblk_t *f, size_t need) {
    size_t size = bsize(f);
    if (size - need >= MINBLK) {
        fblk_t *rest = (fblk_t *)((uint8_t *)f + need);
        fl_replace(f, rest);
        rest->h = (word_t)(size - need) | F_FREE;     /* f, before it, is live */
        put_footer(rest, size - need);
        *hdr(f) = (word_t)need;                       /* a free block's PREV is never set */
    } else {
        fl_remove(f);
        *hdr(f) = (word_t)size;
        tell_next(f, false);
    }
    count_live(bsize(f) - HDR);
    return (uint8_t *)f + HDR;
}

/* ... or its TOP `need` bytes. */
static void *take_high(fblk_t *f, size_t need) {
    size_t size = bsize(f);
    if (size - need >= MINBLK) {
        f->h = (word_t)(size - need) | F_FREE;        /* keeps its place in the list */
        put_footer(f, size - need);
        uint8_t *n = (uint8_t *)f + (size - need);
        *hdr(n) = (word_t)need | F_PREV;
        tell_next(n, false);
        count_live(need - HDR);
        return n + HDR;
    }
    fl_remove(f);
    *hdr(f) = (word_t)size;
    tell_next(f, false);
    count_live(size - HDR);
    return (uint8_t *)f + HDR;
}

static void *alloc_low(size_t want) {
    size_t need = need_of(want);
    if (need) {
        for (fblk_t *f = fl_head; f; f = f->next) {
            if (bsize(f) >= need) { return take_low(f, need); }
        }
    }
    return fail(want);
}

void *lheap_alloc_high(size_t want) {
    size_t need = need_of(want);
    if (need) {
        for (fblk_t *f = fl_tail; f; f = f->prev) {
            if (bsize(f) >= need) { return take_high(f, need); }
        }
    }
    return fail(want);
}

void *lheap_alloc(size_t want) {
    return s_high ? lheap_alloc_high(want) : alloc_low(want);
}

void *lheap_alloc_low(size_t want) { return alloc_low(want); }

void lheap_high_begin(void) { s_high++; }
void lheap_high_end(void) { if (s_high > 0) { s_high--; } }

void lheap_free(void *p) {
    if (!p) {
        return;
    }
    uint8_t *b = (uint8_t *)p - HDR;
    size_t size = bsize(b);
    word_t prevflag = *hdr(b) & F_PREV;
    heap_used -= size - HDR;

    fblk_t *r = (fblk_t *)(b + size);
    bool r_free = (uint8_t *)r < ARENA_END && is_free(r);
    if (prevflag) {
        /* The free block on the left absorbs this one (and the right one). */
        fblk_t *l = prev_of(b);
        size_t total = bsize(l) + size;
        if (r_free) { total += bsize(r); fl_remove(r); }
        l->h = (word_t)total | F_FREE;
        put_footer(l, total);
        tell_next(l, true);
        return;
    }
    fblk_t *f = (fblk_t *)b;
    if (r_free) {
        size_t total = size + bsize(r);
        fl_replace(r, f);
        make_free(f, total, 0);
        return;
    }
    make_free(f, size, 0);
    fl_insert(f);
}

/*
 * `b` is live and at least `want` bytes: give its tail back as a free block
 * if that is big enough to be one, merged with a free right-hand neighbour.
 * Leaves heap_used alone: the caller knows how the block was counted.
 */
static void split_tail(uint8_t *b, size_t want) {
    size_t size = bsize(b);
    fblk_t *r = (fblk_t *)(b + size);
    bool r_free = (uint8_t *)r < ARENA_END && is_free(r);
    size_t tail = size - want;
    if (tail == 0 || (!r_free && tail < MINBLK)) { return; }
    *hdr(b) = (word_t)want | (*hdr(b) & F_PREV);
    fblk_t *t = (fblk_t *)(b + want);
    if (r_free) {
        size_t total = tail + bsize(r);
        fl_replace(r, t);
        make_free(t, total, 0);
    } else {
        make_free(t, tail, 0);
        fl_insert(t);
    }
}

void *lheap_realloc(void *ptr, size_t osize, size_t nsize) {
    (void)osize;   /* a TYPE TAG when ptr is NULL; the header is truth */

    if (nsize == 0) { lheap_free(ptr); return NULL; }
    if (!ptr)       { return lheap_alloc(nsize); }

    uint8_t *b = (uint8_t *)ptr - HDR;
    const size_t old = bsize(b);
    const size_t want = need_of(nsize);
    if (!want) { return fail(nsize); }

    if (want <= old) {
        /* Shrink in place. A CORRECTNESS matter as much as a footprint one:
         * Lua assumes a shrink succeeds (luaM_shrinkvector_ raises on NULL),
         * and an allocate-copy-free shrink can fail for want of room. */
        split_tail(b, want);
        heap_used -= old - bsize(b);
        return ptr;
    }

    /* Grow into a free right-hand neighbour: the whole of the in-place room,
     * since no two free blocks are ever adjacent. */
    fblk_t *r = (fblk_t *)(b + old);
    if ((uint8_t *)r < ARENA_END && is_free(r) && old + bsize(r) >= want) {
        size_t total = old + bsize(r);
        if (total - want >= MINBLK) {
            fblk_t *t = (fblk_t *)(b + want);
            fl_replace(r, t);
            t->h = (word_t)(total - want) | F_FREE;
            put_footer(t, total - want);
            *hdr(b) = (word_t)want | (*hdr(b) & F_PREV);
        } else {
            fl_remove(r);
            *hdr(b) = (word_t)total | (*hdr(b) & F_PREV);
            tell_next(b, false);
        }
        count_live(bsize(b) - old);
        return ptr;
    }

    /* Allocate before freeing -- see rule 2 in lheap.h. From the bottom even
     * while high placement is open: a block that has to move to grow existed
     * before (a thread's stack, the string table, a table's array) and lives
     * on after the unit being loaded is dropped. */
    void *np = alloc_low(nsize);
    if (!np) {
        return NULL;
    }
    memcpy(np, ptr, old - HDR < nsize ? old - HDR : nsize);
    lheap_free(ptr);
    return np;
}

void lheap_walk(lheap_walk_fn cb, void *ctx) {
    for (uint8_t *b = arena; b < ARENA_END; b = after(b)) {
        cb(b + HDR, bsize(b) - HDR, is_free(b), ctx);
    }
}

const void *lheap_base(void) { return arena; }
size_t lheap_header_size(void) { return HDR; }

size_t lheap_largest_free(void) {
    size_t big = 0;
    for (fblk_t *f = fl_head; f; f = f->next) {
        if (bsize(f) > big) { big = bsize(f); }
    }
    return big ? big - HDR : 0;
}

void lheap_fail_stats(size_t *last, size_t *count) {
    if (last)  { *last = fail_last; }
    if (count) { *count = fail_count; }
}

size_t lheap_used(void)     { return heap_used; }
size_t lheap_peak(void)     { return heap_peak; }
size_t lheap_capacity(void) { return ARENA_BYTES; }

void lheap_stats(lheap_stats_t *out) {
    memset(out, 0, sizeof *out);
    for (uint8_t *b = arena; b < ARENA_END; b = after(b)) {
        size_t pay = bsize(b) - HDR;
        out->blocks++;
        if (is_free(b)) {
            out->free_blocks++;
            out->free_bytes += pay;
            if (pay > out->largest_free) { out->largest_free = pay; }
        } else {
            out->live_bytes += pay;
        }
    }
}

bool lheap_check(lheap_stats_t *out, const char **why) {
    lheap_stats_t s;
    memset(&s, 0, sizeof s);
    const char *dummy = NULL;
    if (!why) { why = &dummy; }

    bool prev_free = false;
    uint8_t *b = arena;
    while (b < ARENA_END) {
        size_t size = bsize(b);
        if (size < MINBLK || (size & (ALIGN - 1u))) {
            *why = "block size is below the minimum or not aligned";
            return false;
        }
        if (b + size > ARENA_END) {
            *why = "block runs past the end of the arena";
            return false;
        }
        if (((*hdr(b) & F_PREV) != 0) != prev_free) {
            *why = "a block's PREV flag disagrees with the block before it";
            return false;
        }
        bool fr = is_free(b);
        if (fr && prev_free) {
            *why = "two adjacent free blocks";
            return false;
        }
        if (fr && *(word_t *)(b + size - WSZ) != (word_t)size) {
            *why = "a free block's footer disagrees with its size";
            return false;
        }
        s.blocks++;
        if (fr) {
            s.free_blocks++;
            s.free_bytes += size - HDR;
            if (size - HDR > s.largest_free) { s.largest_free = size - HDR; }
        } else {
            s.live_bytes += size - HDR;
        }
        prev_free = fr;
        b += size;
    }
    if (b != ARENA_END) {
        *why = "chain does not tile the arena (gap or overlap)";
        return false;
    }
    size_t n = 0;
    fblk_t *last = NULL;
    for (fblk_t *f = fl_head; f; last = f, f = f->next) {
        if ((uint8_t *)f < arena || (uint8_t *)f >= ARENA_END || !is_free(f)) {
            *why = "the free list holds a block that is not free";
            return false;
        }
        if (f->prev != last || (last && last >= f)) {
            *why = "the free list is not in address order, or its links disagree";
            return false;
        }
        if (++n > s.free_blocks) { break; }
    }
    if (n != s.free_blocks || fl_tail != last) {
        *why = "the free list does not hold exactly the free blocks";
        return false;
    }
    if (s.live_bytes != heap_used) {
        *why = "heap_used disagrees with the sum of live payloads";
        return false;
    }
    if (out) { *out = s; }
    return true;
}
