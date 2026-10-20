/*
 * Host tests for the Lua arena (src/lua/lheap.c).
 *
 * This file exists because two attempts at the in-place realloc shipped to
 * hardware and had to be reverted. Each assertion below names the failure it
 * would have caught:
 *
 *   attempt (a)  "if (b->size >= nsize) return ptr;" with no shrink path, so
 *                every buffer's high-water mark became permanent. Resting
 *                heap went 20 KB -> 126 KB.  -> t_drain_to_empty, t_shrink
 *   attempt (b)  split the tail and handed it to free(), which subtracts a
 *                size that was never added, so the same bytes were subtracted
 *                twice. heap_used wrapped to 4294882968 (2^32 - 84328).
 *                -> lheap_check()'s live_bytes == heap_used, after EVERY op.
 *
 * HDR is 16 on-device and 24 on x86-64, so nothing here asserts byte totals
 * that depend on it.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lua/lheap.h"

static int failures;
static const char *current;

static void ck(void) {
    const char *why = NULL;
    if (!lheap_check(NULL, &why)) {
        printf("  FAIL [%s]: %s\n", current, why);
        failures++;
        abort();   /* every later assertion would be noise */
    }
}

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("  FAIL [%s]: %s\n", current, (msg)); failures++; } \
} while (0)

/* Fill a payload with a byte pattern derived from a seed, and verify it. */
static void paint(void *p, size_t n, unsigned seed) {
    uint8_t *b = p;
    for (size_t i = 0; i < n; i++) { b[i] = (uint8_t)(seed * 31u + i); }
}
static int painted(const void *p, size_t n, unsigned seed) {
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) {
        if (b[i] != (uint8_t)(seed * 31u + i)) { return 0; }
    }
    return 1;
}

/* --- the arena starts and returns to one coalesced block ----------------- */
static void t_empty(void) {
    current = "empty arena";
    lheap_init();
    ck();
    lheap_stats_t s;
    lheap_check(&s, NULL);
    CHECK(s.blocks == 1, "a fresh arena should be exactly one block");
    CHECK(s.free_blocks == 1, "that block should be free");
    CHECK(lheap_used() == 0, "used should be 0");
}

/*
 * Allocate a mixed set, free it all, and require the arena back as ONE block
 * with used == 0. This is what attempt (a) could never do: without a shrink
 * path the reclaimed bytes stayed attached to whichever block had once grown.
 */
static void t_drain_to_empty(void) {
    current = "drain to empty";
    lheap_init();
    void *p[64];
    for (int i = 0; i < 64; i++) {
        p[i] = lheap_alloc((size_t)(8 + i * 13));
        CHECK(p[i] != NULL, "allocation failed in a fresh arena");
        ck();
    }
    /* Free out of order: coalescing must not depend on release order. */
    for (int i = 1; i < 64; i += 2) { lheap_free(p[i]); ck(); }
    for (int i = 0; i < 64; i += 2) { lheap_free(p[i]); ck(); }

    lheap_stats_t s;
    lheap_check(&s, NULL);
    CHECK(lheap_used() == 0, "used must return to 0 once everything is freed");
    CHECK(s.blocks == 1, "the arena must coalesce back to a single block");
    CHECK(s.live_bytes == 0, "no live bytes should remain");
}

/* --- shrink is in place, and actually gives the bytes back --------------- */
static void t_shrink(void) {
    current = "shrink in place";
    lheap_init();
    void *big = lheap_alloc(8192);
    CHECK(big != NULL, "8 KB should fit");
    paint(big, 8192, 7);
    size_t used_big = lheap_used();

    void *small = lheap_realloc(big, 8192, 1024);
    ck();
    CHECK(small == big, "a shrink must not move the block");
    CHECK(painted(small, 1024, 7), "shrink corrupted the surviving payload");
    CHECK(lheap_used() < used_big,
          "a shrink must RELEASE bytes -- attempt (a) kept them forever");

    lheap_free(small);
    ck();
    CHECK(lheap_used() == 0, "used must return to 0");
}

/*
 * A shrink must SUCCEED even when the arena has no room for a copy. The old
 * allocate-copy-free path could fail here, and Lua does not tolerate that:
 * luaM_shrinkvector_ raises on NULL.
 */
static void t_shrink_when_full(void) {
    current = "shrink with a full arena";
    lheap_init();
    size_t cap = lheap_capacity();
    void *big = lheap_alloc(cap / 2);
    CHECK(big != NULL, "half the arena should fit");
    /* Soak up everything that is left. */
    void *rest = NULL;
    for (size_t n = cap / 2; n >= 64; n /= 2) {
        void *q = lheap_alloc(n);
        if (q) { rest = q; break; }
    }
    (void)rest;
    void *shrunk = lheap_realloc(big, cap / 2, 64);
    ck();
    CHECK(shrunk != NULL, "a shrink must never fail for want of memory");
    CHECK(shrunk == big, "a shrink must not move the block");
}

/* --- grow absorbs the free neighbour instead of copying ------------------ */
static void t_grow_in_place(void) {
    current = "grow into the free neighbour";
    lheap_init();
    void *a = lheap_alloc(1024);
    void *b = lheap_alloc(1024);
    void *c = lheap_alloc(1024);
    CHECK(a && b && c, "three 1 KB blocks should fit");
    paint(a, 1024, 3);
    lheap_free(b);          /* leaves a free hole immediately after `a` */
    ck();

    void *grown = lheap_realloc(a, 1024, 1900);
    ck();
    CHECK(grown == a, "growing into an adjacent free block must not move it");
    CHECK(painted(grown, 1024, 3), "grow corrupted the payload");
    lheap_free(grown);
    lheap_free(c);
    ck();
    CHECK(lheap_used() == 0, "used must return to 0");
}

/* --- a failed grow leaves the original completely untouched -------------- */
static void t_failed_grow_is_inert(void) {
    current = "failed grow leaves the old block intact";
    lheap_init();
    void *p = lheap_alloc(4096);
    CHECK(p != NULL, "4 KB should fit");
    paint(p, 4096, 11);
    size_t used_before = lheap_used();

    /* Larger than the arena: cannot possibly be satisfied. */
    void *q = lheap_realloc(p, 4096, lheap_capacity() * 2);
    ck();
    CHECK(q == NULL, "an impossible grow must return NULL");
    CHECK(lheap_used() == used_before, "a failed grow must not change used");
    CHECK(painted(p, 4096, 11),
          "a failed grow must leave the old block byte-identical: Lua runs an "
          "emergency GC and retries with the SAME pointer");
    lheap_free(p);
    ck();
}

/* --- the (NULL, tag, n) call shape --------------------------------------- */
static void t_null_ptr_osize_is_a_tag(void) {
    current = "osize is a type tag when ptr is NULL";
    lheap_init();
    /* Lua passes the type tag (small ints like LUA_TSTRING) as osize here. If
     * anything treated that as a length, this would misbehave. */
    void *p = lheap_realloc(NULL, 4 /* LUA_TSTRING */, 300);
    ck();
    CHECK(p != NULL, "a fresh allocation through realloc should succeed");
    paint(p, 300, 5);
    CHECK(painted(p, 300, 5), "payload not writable for the full length");
    p = lheap_realloc(p, 300, 0);
    ck();
    CHECK(p == NULL, "nsize 0 must free and return NULL");
    CHECK(lheap_used() == 0, "used must return to 0");
}

/* --- payload survives every realloc path, in place or not ---------------- */
static void t_payload_integrity(void) {
    current = "payload integrity across reallocs";
    lheap_init();
    size_t n = 64;
    void *p = lheap_alloc(n);
    paint(p, n, 21);
    /* A ladder of grows and shrinks, with a live block on either side so the
     * grow path sometimes finds no room and has to relocate. */
    void *guard = lheap_alloc(512);
    (void)guard;
    const size_t steps[] = { 128, 96, 4096, 512, 8192, 32, 16384, 8 };
    for (size_t i = 0; i < sizeof steps / sizeof steps[0]; i++) {
        size_t m = steps[i];
        void *q = lheap_realloc(p, n, m);
        ck();
        CHECK(q != NULL, "realloc in the ladder failed");
        if (!q) { return; }
        size_t keep = n < m ? n : m;
        CHECK(painted(q, keep, 21), "realloc lost the overlapping payload");
        if (m > keep) { paint((uint8_t *)q + keep, m - keep, 21 + (unsigned)i); }
        /* Re-paint the whole thing so the next step has a known pattern. */
        paint(q, m, 21);
        p = q; n = m;
    }
    lheap_free(p);
    ck();
}

/* --- randomised, with the real call shapes ------------------------------- */
static void t_fuzz(void) {
    current = "randomised fuzz";
    lheap_init();
    enum { SLOTS = 96 };
    void  *p[SLOTS];
    size_t sz[SLOTS];
    unsigned seed[SLOTS];
    memset(p, 0, sizeof p);
    memset(sz, 0, sizeof sz);

    unsigned rng = 20250926u;   /* fixed seed: a failure must be replayable */
    for (int step = 0; step < 40000; step++) {
        rng = rng * 1103515245u + 12345u;
        int i = (int)((rng >> 8) % SLOTS);
        int op = (int)((rng >> 20) % 10);
        /* Lua's sizes are overwhelmingly small, with occasional big buffers. */
        size_t n = ((rng >> 4) % 8 == 0) ? (size_t)((rng >> 12) % 6000u) + 1u
                                         : (size_t)((rng >> 12) % 200u) + 1u;

        if (!p[i]) {
            /* Through realloc, with a type tag in osize, as Lua does --
             * and now and then from the top, as the HTTP client does. */
            void *q = (op == 9) ? lheap_alloc_high(n) : lheap_realloc(NULL, 4, n);
            if (q) { p[i] = q; sz[i] = n; seed[i] = (unsigned)step;
                     paint(q, n, seed[i]); }
        } else if (op < 3) {
            if (!painted(p[i], sz[i], seed[i])) {
                CHECK(0, "payload corrupted while merely resident");
                return;
            }
            lheap_free(p[i]); p[i] = NULL; sz[i] = 0;
        } else {
            if (!painted(p[i], sz[i], seed[i])) {
                CHECK(0, "payload corrupted before realloc");
                return;
            }
            void *q = lheap_realloc(p[i], sz[i], n);
            if (q) {
                size_t keep = sz[i] < n ? sz[i] : n;
                if (!painted(q, keep, seed[i])) {
                    CHECK(0, "realloc lost the overlapping payload");
                    return;
                }
                p[i] = q; sz[i] = n; seed[i] = (unsigned)step;
                paint(q, n, seed[i]);
            } else {
                /* A refused grow must have changed nothing at all. */
                if (!painted(p[i], sz[i], seed[i])) {
                    CHECK(0, "a failed realloc damaged the old block");
                    return;
                }
            }
        }
        ck();
    }
    for (int i = 0; i < SLOTS; i++) {
        if (p[i]) {
            CHECK(painted(p[i], sz[i], seed[i]), "payload corrupted by the end");
            lheap_free(p[i]);
        }
    }
    ck();
    lheap_stats_t s;
    lheap_check(&s, NULL);
    CHECK(lheap_used() == 0, "used must return to 0 after the fuzz drains");
    CHECK(s.blocks == 1, "the arena must coalesce back to one block");
}

/* lheap_alloc_high: from the top, below nothing Lua packs at the bottom, and
 * back to one block when it goes. */
static void t_alloc_high(void) {
    current = "alloc from the top";
    lheap_init();
    void *lo = lheap_alloc(100);
    void *hi = lheap_alloc_high(16384);
    void *lo2 = lheap_alloc(200);
    CHECK(lo && hi && lo2, "allocations succeed");
    CHECK((uint8_t *)hi > (uint8_t *)lo2 && (uint8_t *)hi + 16384 == (uint8_t *)lheap_base() + lheap_capacity(),
          "the high block ends at the end of the arena, above the low ones");
    ck();
    paint(hi, 16384, 7);
    void *hi2 = lheap_alloc_high(4096);
    CHECK((uint8_t *)hi2 + 4096 + lheap_header_size() == (uint8_t *)hi, "the next high block sits right under it");
    lheap_free(hi);
    ck();
    void *hi3 = lheap_alloc_high(8000);
    CHECK((uint8_t *)hi3 > (uint8_t *)hi2, "a freed top block is reused from the top");
    lheap_free(hi2); lheap_free(hi3); lheap_free(lo); lheap_free(lo2);
    ck();
    lheap_stats_t st;
    lheap_check(&st, NULL);
    CHECK(st.blocks == 1 && lheap_used() == 0, "coalesces back to one block");
    CHECK(lheap_alloc_high(lheap_capacity()) == NULL, "too large fails");
    ck();
}

/*
 * The headline number: peak usage while a buffer grows the way Lua grows one.
 * The old allocator needed old+new resident at once, so doubling a 32 KB
 * buffer took 96 KB. In place, the same ladder should stay near the final
 * size. Not a hard threshold -- the assertion is simply that the peak is well
 * under what copy-every-time would force.
 */
static void t_growth_peak(void) {
    current = "growth peak";
    lheap_init();
    size_t n = 512;
    void *p = lheap_alloc(n);
    paint(p, n, 1);
    while (n < lheap_capacity() / 4) {
        size_t m = n * 2;
        void *q = lheap_realloc(p, n, m);
        if (!q) { break; }
        p = q; n = m;
        ck();
    }
    printf("  growth: final %zu B, peak %zu B, arena %zu B\n",
           n, lheap_peak(), lheap_capacity());
    CHECK(lheap_peak() < n * 2,
          "doubling in place should never need two copies resident");
    lheap_free(p);
}

int main(void) {
    /* Line-buffered: ck() aborts on a structural failure, and an abort
     * discards a buffered explanation -- which is the one line worth having. */
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("lheap: arena %zu B\n", lheap_capacity());
    t_empty();
    t_drain_to_empty();
    t_shrink();
    t_shrink_when_full();
    t_grow_in_place();
    t_failed_grow_is_inert();
    t_null_ptr_osize_is_a_tag();
    t_payload_integrity();
    t_fuzz();
    t_alloc_high();
    t_growth_peak();
    if (failures) {
        printf("heap_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("heap_test: all checks passed\n");
    return 0;
}
