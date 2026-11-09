#include <stdint.h>
#include <string.h>

#include "lua/lheap.h"

#define ARENA_BYTES ((size_t)LHEAP_KB * 1024u)
#if LHEAP_KB == 0
/* Lua is off. The TLS pool further down is the region this file owns.
 * Eight bytes keeps the arena symbol real; nothing calls lheap_init. */
#undef ARENA_BYTES
#define ARENA_BYTES 8
#endif

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
 *     objects was 21 KB of the 176 KB arena (measured in mcusim with a large BLE
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

/* A region managed by the allocator: the arena itself, or a scratch page
 * (below) carved from it. The block operations work on the CURRENT region,
 * G: every public entry point sets it (single-threaded, never re-entered
 * from an interrupt), and the scratch code switches it for its pages. */
typedef struct rg { uint8_t *base, *end; fblk_t *head, *tail; size_t used, peak; } rg_t;
static rg_t g_main;
static rg_t *G = &g_main;
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
    if (n >= G->end) { return; }
    if (free) { *hdr(n) |= F_PREV; } else { *hdr(n) &= ~F_PREV; }
}

/* --- the free list --------------------------------------------------------- */
static void fl_remove(fblk_t *f) {
    if (f->prev) { f->prev->next = f->next; } else { G->head = f->next; }
    if (f->next) { f->next->prev = f->prev; } else { G->tail = f->prev; }
}

/* `nu` takes `old`'s place: nothing free lies between them. The two may
 * overlap (a block split a few bytes into its free neighbour), so the links
 * are read before any is written. */
static void fl_replace(fblk_t *old, fblk_t *nu) {
    fblk_t *p = old->prev, *n = old->next;
    nu->prev = p;
    nu->next = n;
    if (nu->prev) { nu->prev->next = nu; } else { G->head = nu; }
    if (nu->next) { nu->next->prev = nu; } else { G->tail = nu; }
}

/* In address order: from whichever end of the region it is nearer. */
static void fl_insert(fblk_t *f) {
    fblk_t *before = NULL, *at;   /* insert between `before` and `at` */
    if ((uint8_t *)f - G->base < G->end - (uint8_t *)f) {
        for (at = G->head; at && at < f; at = at->next) { before = at; }
    } else {
        for (before = G->tail; before && before > f; before = before->prev) { }
        at = before ? before->next : G->head;
    }
    f->prev = before;
    f->next = at;
    if (before) { before->next = f; } else { G->head = f; }
    if (at) { at->prev = f; } else { G->tail = f; }
}

/* Mark `b` (not on the list) free with this size, and tell its neighbour. */
static void make_free(void *b, size_t size, word_t prevflag) {
    *hdr(b) = (word_t)size | F_FREE | prevflag;
    put_footer(b, size);
    tell_next(b, true);
}

static void rg_init(rg_t *g, uint8_t *base, size_t size) {
    rg_t *saved = G;
    G = g;
    g->base = base;
    g->end = base + (size & ~(ALIGN - 1u));
    fblk_t *f = (fblk_t *)base;
    f->h = (word_t)(g->end - base) | F_FREE;
    put_footer(f, bsize(f));
    f->next = f->prev = NULL;
    g->head = g->tail = f;
    g->used = 0;
    g->peak = 0;
    G = saved;
}

static void scratch_forget_all(void);

void lheap_init(void) {
    heap_gen++;
    rg_init(&g_main, arena, ARENA_BYTES);
    G = &g_main;
    s_high = 0;
    scratch_forget_all();
}

/* The whole block a request of `want` payload bytes needs; 0 if impossible. */
static size_t need_of(size_t want) {
    /* The region being allocated from, not the arena: the TLS pool is its
     * own region, and on a no-Lua image the arena is 8 bytes, which refused
     * every pool allocation larger than that. */
    if (want > (size_t)(G->end - G->base)) { return 0; }
    size_t n = RUP(want + HDR);
    return n < MINBLK ? MINBLK : n;
}

static void count_live(size_t payload) {
    G->used += payload;
    if (G->used > G->peak) { G->peak = G->used; }
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

/* First fit in the current region, or NULL -- not counted as a failure. */
static void *try_low(size_t want) {
    size_t need = need_of(want);
    if (need) {
        for (fblk_t *f = G->head; f; f = f->next) {
            if (bsize(f) >= need) { return take_low(f, need); }
        }
    }
    return NULL;
}

static void *alloc_low(size_t want) {
    void *p = try_low(want);
    return p ? p : fail(want);
}

static void *rg_alloc_high(size_t want) {
    size_t need = need_of(want);
    if (need) {
        for (fblk_t *f = G->tail; f; f = f->prev) {
            if (bsize(f) >= need) { return take_high(f, need); }
        }
    }
    return fail(want);
}

void *lheap_alloc_high(size_t want) { G = &g_main; return rg_alloc_high(want); }

void *lheap_alloc(size_t want) {
    G = &g_main;
    return s_high ? rg_alloc_high(want) : alloc_low(want);
}

void *lheap_alloc_low(size_t want) { G = &g_main; return alloc_low(want); }

void lheap_high_begin(void) { s_high++; }
void lheap_high_end(void) { if (s_high > 0) { s_high--; } }
int  lheap_high_suspend(void) { int d = s_high; s_high = 0; return d; }
void lheap_high_resume(int depth) { s_high = depth; }

static void rg_free(void *p) {
    if (!p) {
        return;
    }
    uint8_t *b = (uint8_t *)p - HDR;
    size_t size = bsize(b);
    word_t prevflag = *hdr(b) & F_PREV;
    G->used -= size - HDR;

    fblk_t *r = (fblk_t *)(b + size);
    bool r_free = (uint8_t *)r < G->end && is_free(r);
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

void lheap_free(void *p) { G = &g_main; rg_free(p); }

/*
 * `b` is live and at least `want` bytes: give its tail back as a free block
 * if that is big enough to be one, merged with a free right-hand neighbour.
 * Leaves G->used alone: the caller knows how the block was counted.
 */
static void split_tail(uint8_t *b, size_t want) {
    size_t size = bsize(b);
    fblk_t *r = (fblk_t *)(b + size);
    bool r_free = (uint8_t *)r < G->end && is_free(r);
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
    G = &g_main;

    uint8_t *b = (uint8_t *)ptr - HDR;
    const size_t old = bsize(b);
    const size_t want = need_of(nsize);
    if (!want) { return fail(nsize); }

    if (want <= old) {
        /* Shrink in place. A CORRECTNESS matter as much as a footprint one:
         * Lua assumes a shrink succeeds (luaM_shrinkvector_ raises on NULL),
         * and an allocate-copy-free shrink can fail for want of room. */
        split_tail(b, want);
        G->used -= old - bsize(b);
        return ptr;
    }

    /* Grow into a free right-hand neighbour: the whole of the in-place room,
     * since no two free blocks are ever adjacent. */
    fblk_t *r = (fblk_t *)(b + old);
    if ((uint8_t *)r < G->end && is_free(r) && old + bsize(r) >= want) {
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
    rg_free(ptr);
    return np;
}

void lheap_walk(lheap_walk_fn cb, void *ctx) {
    G = &g_main;
    for (uint8_t *b = G->base; b < G->end; b = after(b)) {
        cb(b + HDR, bsize(b) - HDR, is_free(b), ctx);
    }
}

const void *lheap_base(void) { return arena; }
size_t lheap_header_size(void) { return HDR; }

size_t lheap_largest_free(void) {
    G = &g_main;
    size_t big = 0;
    for (fblk_t *f = G->head; f; f = f->next) {
        if (bsize(f) > big) { big = bsize(f); }
    }
    return big ? big - HDR : 0;
}

void lheap_fail_stats(size_t *last, size_t *count) {
    if (last)  { *last = fail_last; }
    if (count) { *count = fail_count; }
}

size_t lheap_used(void)     { return g_main.used; }
size_t lheap_peak(void)     { return g_main.peak; }
size_t lheap_capacity(void) { return ARENA_BYTES; }

void lheap_stats(lheap_stats_t *out) {
    G = &g_main;
    memset(out, 0, sizeof *out);
    uint8_t *gap = NULL;
    for (uint8_t *b = G->base; b < G->end; b = after(b)) {
        size_t pay = bsize(b) - HDR;
        out->blocks++;
        if (is_free(b)) {
            out->free_blocks++;
            out->free_bytes += pay;
            if (pay > out->largest_free) { out->largest_free = pay; gap = b; }
        } else {
            out->live_bytes += pay;
        }
    }
    /* The largest free block is the gap between the two sides (the scripts'
     * data packed from the bottom, modules and long-lived buffers from the
     * top); what is free on either side of it are holes in that side. */
    for (fblk_t *f = G->head; f; f = f->next) {
        size_t pay = bsize(f) - HDR;
        if ((uint8_t *)f == gap) { continue; }
        if ((uint8_t *)f < gap) { if (pay > out->largest_free_low) { out->largest_free_low = pay; } out->free_low += pay; }
        else { if (pay > out->largest_free_high) { out->largest_free_high = pay; } out->free_high += pay; }
    }
    out->gap_offset = gap ? (size_t)(gap - G->base) : 0;
}

/* --- the pool: a second region, apart from the arena ----------------------
 * The same blocks and free list, over memory of its own (LHEAP_POOL_KB, the
 * HTTP client's TLS sessions: CONFIG_NET_TLS_POOL_KB). lheap_init does not
 * touch it -- a Lua VM restart leaves it alone -- and nothing Lua allocates
 * ever lands in it, so a TLS session can neither cut the scripts' space
 * into pieces nor be refused because the scripts already did. */
#if LHEAP_POOL_KB > 0
static uint8_t pool_mem[(size_t)LHEAP_POOL_KB * 1024u] __attribute__((aligned(8)));
static rg_t g_pool;
static bool pool_ready;
static size_t pool_fail_last;
static uint32_t pool_fails;

static void pool_on(void) {
    if (!pool_ready) { rg_init(&g_pool, pool_mem, sizeof pool_mem); pool_ready = true; }
    G = &g_pool;
}

void *lheap_pool_alloc(size_t want) {
    pool_on();
    void *p = try_low(want);
    G = &g_main;
    if (!p) { pool_fail_last = want; pool_fails++; }
    return p;
}

void lheap_pool_free(void *p) {
    if (!p) { return; }
    pool_on();
    rg_free(p);
    G = &g_main;
}

bool lheap_pool_owns(const void *p) {
    return (const uint8_t *)p >= pool_mem && (const uint8_t *)p < pool_mem + sizeof pool_mem;
}

void lheap_pool_stats(lheap_pool_stats_t *out) {
    memset(out, 0, sizeof *out);
    pool_on();
    out->capacity = sizeof pool_mem;
    out->used = g_pool.used;
    out->peak = g_pool.peak;
    for (fblk_t *f = G->head; f; f = f->next) {
        size_t pay = bsize(f) - HDR;
        out->free_blocks++;
        if (pay > out->largest_free) { out->largest_free = pay; }
    }
    for (uint8_t *b = G->base; b < G->end; b = after(b)) { if (!is_free(b)) { out->blocks++; } }
    out->fails = pool_fails;
    out->fail_last = pool_fail_last;
    G = &g_main;
}

void lheap_pool_reset_peak(void) { pool_on(); g_pool.peak = g_pool.used; G = &g_main; }
#else
void *lheap_pool_alloc(size_t want) { (void)want; return NULL; }
void  lheap_pool_free(void *p) { (void)p; }
bool  lheap_pool_owns(const void *p) { (void)p; return false; }
void  lheap_pool_stats(lheap_pool_stats_t *out) { memset(out, 0, sizeof *out); }
void  lheap_pool_reset_peak(void) { }
#endif

bool lheap_check(lheap_stats_t *out, const char **why) {
    G = &g_main;
    lheap_stats_t s;
    memset(&s, 0, sizeof s);
    const char *dummy = NULL;
    if (!why) { why = &dummy; }

    bool prev_free = false;
    uint8_t *b = G->base;
    while (b < G->end) {
        size_t size = bsize(b);
        if (size < MINBLK || (size & (ALIGN - 1u))) {
            *why = "block size is below the minimum or not aligned";
            return false;
        }
        if (b + size > G->end) {
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
    if (b != G->end) {
        *why = "chain does not tile the arena (gap or overlap)";
        return false;
    }
    size_t n = 0;
    fblk_t *last = NULL;
    for (fblk_t *f = G->head; f; last = f, f = f->next) {
        if ((uint8_t *)f < G->base || (uint8_t *)f >= G->end || !is_free(f)) {
            *why = "the free list holds a block that is not free";
            return false;
        }
        if (f->prev != last || (last && last >= f)) {
            *why = "the free list is not in address order, or its links disagree";
            return false;
        }
        if (++n > s.free_blocks) { break; }
    }
    if (n != s.free_blocks || G->tail != last) {
        *why = "the free list does not hold exactly the free blocks";
        return false;
    }
    if (s.live_bytes != G->used) {
        *why = "G->used disagrees with the sum of live payloads";
        return false;
    }
    if (out) { *out = s; }
    return true;
}

/* --- scratch: short-lived C memory, in pages, freed whole --------------------
 *
 * What one HTTP request allocates -- its URL and headers, the response, the
 * TLS session's state, mbedTLS's handshake temporaries -- lives exactly as
 * long as the request, and in the arena it used to be dozens of separate
 * blocks placed wherever there was room, between blocks that outlive it.
 * A scratch gathers them into pages (2 KB by default, bigger for one bigger
 * allocation) carved from the top of the arena; inside a page the same
 * allocator runs, so mbedTLS's alloc-free-alloc during a handshake reuses
 * the page. A page that empties goes back at once (the last one stays), and
 * lheap_scratch_end gives all of them back together. */
typedef struct spage { struct spage *next; rg_t rg; } spage_t;
struct lheap_scratch {
    spage_t *pages;
    struct lheap_scratch *next_active;
    size_t page;                 /* default page size */
    size_t bytes;                /* in its pages */
    size_t live;                 /* handed out */
    uint32_t gen;
};
#define SPAGE_HDR RUP(sizeof(spage_t))
static lheap_scratch_t *s_active, *s_cur;
static size_t s_sc_live, s_sc_live_peak, s_sc_bytes, s_sc_bytes_peak;
static uint32_t s_sc_page_fails, s_sc_runs;

static void scratch_forget_all(void) { s_active = NULL; s_cur = NULL; s_sc_live = 0; s_sc_bytes = 0; }

lheap_scratch_t *lheap_scratch_begin(size_t page) {
    G = &g_main;
    lheap_scratch_t *s = rg_alloc_high(sizeof *s);
    if (!s) { return NULL; }
    memset(s, 0, sizeof *s);
    s->page = page ? page : 2048;
    s->gen = heap_gen;
    s->next_active = s_active;
    s_active = s;
    s_sc_runs++;
    return s;
}

static void sc_account(lheap_scratch_t *s, long dlive, long dbytes) {
    s->live = (size_t)((long)s->live + dlive);
    s->bytes = (size_t)((long)s->bytes + dbytes);
    s_sc_live = (size_t)((long)s_sc_live + dlive);
    s_sc_bytes = (size_t)((long)s_sc_bytes + dbytes);
    if (s_sc_live > s_sc_live_peak) { s_sc_live_peak = s_sc_live; }
    if (s_sc_bytes > s_sc_bytes_peak) { s_sc_bytes_peak = s_sc_bytes; }
}

void *lheap_scratch_alloc(lheap_scratch_t *s, size_t n) {
    if (!s || s->gen != heap_gen) { return NULL; }
    for (spage_t *pg = s->pages; pg; pg = pg->next) {
        G = &pg->rg;
        void *p = try_low(n);
        if (p) {
            sc_account(s, (long)(bsize((uint8_t *)p - HDR) - HDR), 0);
            G = &g_main;
            return p;
        }
    }
    /* A new page: the default size, or what this one allocation needs. The
     * page comes from the arena, so the arena is what bounds the request. */
    G = &g_main;
    size_t need = need_of(n);
    if (!need) { return fail(n); }
    size_t want = SPAGE_HDR + need + MINBLK;
    if (want < s->page) { want = s->page; }
    uint8_t *blk = rg_alloc_high(want);
    if (!blk && want > SPAGE_HDR + need) { blk = rg_alloc_high(SPAGE_HDR + need); }
    if (!blk) { s_sc_page_fails++; return NULL; }
    size_t pay = bsize(blk - HDR) - HDR;
    spage_t *pg = (spage_t *)blk;
    pg->next = s->pages;
    s->pages = pg;
    rg_init(&pg->rg, blk + SPAGE_HDR, pay - SPAGE_HDR);
    sc_account(s, 0, (long)pay);
    G = &pg->rg;
    void *p = try_low(n);
    if (p) { sc_account(s, (long)(bsize((uint8_t *)p - HDR) - HDR), 0); }
    G = &g_main;
    return p;
}

/* The scratch and page `p` is in, if any. */
static spage_t *page_of(const void *p, lheap_scratch_t **owner) {
    for (lheap_scratch_t *s = s_active; s; s = s->next_active) {
        for (spage_t *pg = s->pages; pg; pg = pg->next) {
            if ((const uint8_t *)p >= pg->rg.base && (const uint8_t *)p < pg->rg.end) { *owner = s; return pg; }
        }
    }
    return NULL;
}

static void page_release(lheap_scratch_t *s, spage_t *pg) {
    spage_t **pp = &s->pages;
    while (*pp && *pp != pg) { pp = &(*pp)->next; }
    if (*pp) { *pp = pg->next; }
    G = &g_main;
    sc_account(s, -(long)pg->rg.used, -(long)(bsize((uint8_t *)pg - HDR) - HDR));
    rg_free(pg);
}

void lheap_free_any(void *p) {
    if (!p) { return; }
    lheap_scratch_t *s = NULL;
    spage_t *pg = page_of(p, &s);
    if (!pg) { G = &g_main; rg_free(p); return; }
    G = &pg->rg;
    size_t pay = bsize((uint8_t *)p - HDR) - HDR;
    rg_free(p);
    G = &g_main;
    sc_account(s, -(long)pay, 0);
    /* An emptied page goes back now, unless it is the scratch's only one. */
    if (pg->rg.used == 0 && (s->pages != pg || pg->next)) { page_release(s, pg); }
}

void lheap_scratch_end(lheap_scratch_t *s) {
    if (!s) { return; }
    if (s_cur == s) { s_cur = NULL; }
    lheap_scratch_t **pp = &s_active;
    while (*pp && *pp != s) { pp = &(*pp)->next_active; }
    if (!*pp) { return; }                    /* not (any more) a live scratch */
    *pp = s->next_active;
    if (s->gen != heap_gen) { return; }      /* the arena was reset under it */
    while (s->pages) { page_release(s, s->pages); }   /* and whatever is still in them */
    G = &g_main;
    rg_free(s);
}

lheap_scratch_t *lheap_scratch_use(lheap_scratch_t *s) {
    lheap_scratch_t *prev = s_cur;
    s_cur = s;
    return prev;
}

lheap_scratch_t *lheap_scratch_current(void) { return s_cur; }

void lheap_scratch_stats(lheap_scratch_stats_t *out) {
    memset(out, 0, sizeof *out);
    for (lheap_scratch_t *s = s_active; s; s = s->next_active) {
        out->active++;
        for (spage_t *pg = s->pages; pg; pg = pg->next) { out->pages++; }
    }
    out->bytes = s_sc_bytes;
    out->live = s_sc_live;
    out->bytes_peak = s_sc_bytes_peak;
    out->live_peak = s_sc_live_peak;
    out->page_fails = s_sc_page_fails;
    out->runs = s_sc_runs;
}
