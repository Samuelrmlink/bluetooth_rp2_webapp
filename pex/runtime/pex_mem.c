/*
 * The PEX pool, separated by lifetime (spec 7.2):
 *
 *   base                                                         end
 *   | long-term blocks and scratch arenas ->  ...gap...  | RW suffix |
 *   ^base                                            ^ord ^end-suffix ^end
 *
 * Long-term blocks (module images, RW, pex_alloc_long, pools, malloc) and
 * scratch arenas grow up from the base, first-fit with coalescing. Every
 * block carries its owner tag so a module's leftovers are freed at unload.
 * Scratch arenas are ordinary blocks flagged BLK_SCRATCH: they count as
 * scratch, not as long-term blocks, and they end in reverse order (LIFO).
 * An alloc while a scratch arena is open takes a free hole or the next bytes
 * above that arena, still below the gap.
 *
 * The model-A RW reservation is not a block. It is a suffix of the pool,
 * [end - suffix, end). pex__pool_end() stays the real end, so a relocated RW
 * address (pool end - aoff - size) does not move when the suffix grows down
 * or shrinks from its low end. The suffix counts as used.
 */
#include <string.h>
#include "pex_internal.h"

#define BLK_MAGIC 0xB10Cu
#define BLK_FREE 1u
#define BLK_SYS 2u                /* runtime data for a module: not in its accounting */
#define BLK_SCRATCH 4u            /* a scratch arena: scratch, not a long-term block */
#define ARENA_MAGIC 0x5C7A7C40u
#define POOL_MAGIC 0x9001F00Du
#define SPLIT_MIN 24u             /* a remainder smaller than this stays with the block */

typedef struct {
    uint32_t size;                /* incl. this header, multiple of 8 */
    uint8_t owner, flags;
    uint16_t magic;
} blk_t;

struct pex_arena {
    uint32_t magic, cap, used;
    uint8_t owner, dead;
    uint16_t pad;
    struct pex_arena *prev;
    uint32_t pad2;
};
#define ARENA_HDR ((uint32_t)sizeof(struct pex_arena))

struct pex_pool {
    uint32_t magic, obj, count, used;
    void *free;
    uint8_t *objs;
};

static uint8_t *s_base, *s_end, *s_ord;
static uint32_t s_suffix;
static struct pex_arena *s_last;
static uint32_t s_scratch_now, s_scratch_peak;

#define AL8(x) (((x) + 7u) & ~7u)

_Static_assert(sizeof(blk_t) == 8, "block header is 8 bytes");
_Static_assert(sizeof(struct pex_arena) % 8 == 0, "arena header is 8-aligned");

void pex__mem_init(void *pool, uint32_t size) {
    uintptr_t b = ((uintptr_t)pool + 7u) & ~(uintptr_t)7u;
    uintptr_t e = ((uintptr_t)pool + size) & ~(uintptr_t)7u;
    s_base = s_ord = (uint8_t *)b;
    s_end = (uint8_t *)e;
    s_suffix = 0;
    s_last = NULL;
    s_scratch_now = s_scratch_peak = 0;
}

uint8_t *pex__pool_end(void) { return s_end; }

bool pex__in_pool(const void *p) {
    return (const uint8_t *)p >= s_base && (const uint8_t *)p < s_end;
}

uint32_t pex__pool_gap(void) {
    if (!s_base) { return 0; }
    uint8_t *ge = s_end - s_suffix;
    return s_ord < ge ? (uint32_t)(ge - s_ord) : 0;
}

/* n is rounded up to 8. Growing needs that many bytes of gap. Shrinking only
 * releases the low end of the tail; nothing else moves. */
int pex__pool_set_suffix(uint32_t n) {
    if (!s_base) { return -1; }
    uint32_t aligned = AL8(n);
    if (aligned < n) { return -1; }
    uint32_t pool = (uint32_t)(s_end - s_base);
    if (aligned > pool) { return -1; }
    if (s_ord > s_end - aligned) { return -1; }
    s_suffix = aligned;
    return 0;
}

static void account(uint8_t owner, uint8_t flags, int32_t bytes, int32_t blocks) {
    if (owner == PEX_OWNER_HOST || owner == PEX_OWNER_RT || (flags & (BLK_SYS | BLK_SCRATCH))) { return; }
    pex_mod_t *m = pex__mod_by_id(owner);
    if (m) {
        m->long_bytes += (uint32_t)bytes;
        m->long_blocks += (uint32_t)blocks;
    }
}

static int block_ok(const blk_t *b) {
    const uint8_t *p = (const uint8_t *)b;
    return p >= s_base && b->size >= sizeof(blk_t) && (b->size & 7u) == 0 && p + b->size <= s_ord;
}

static void coalesce(void) {
    uint8_t *p = s_base;
    while (p + sizeof(blk_t) <= s_ord) {
        blk_t *b = (blk_t *)p;
        if (!block_ok(b)) { break; }
        uint8_t *np = p + b->size;
        if (np < s_ord) {
            blk_t *n = (blk_t *)np;
            if ((b->flags & BLK_FREE) && block_ok(n) && (n->flags & BLK_FREE)) {
                b->size += n->size;
                n->magic = 0;
                continue;
            }
        }
        p = np;
    }
    p = s_base;
    blk_t *last = NULL;
    while (p + sizeof(blk_t) <= s_ord) {
        blk_t *b = (blk_t *)p;
        if (!block_ok(b)) { break; }
        last = b;
        if (p + b->size == s_ord) { break; }
        p += b->size;
    }
    if (last && (last->flags & BLK_FREE) && (uint8_t *)last + last->size == s_ord) {
        last->magic = 0;
        s_ord = (uint8_t *)last;
    }
}

static void stamp(blk_t *b, uint32_t size, uint8_t owner, uint8_t flags) {
    b->size = size;
    b->owner = owner;
    b->flags = flags;
    b->magic = BLK_MAGIC;
}

/* Place an `align`-aligned payload in [start, start+fsz). pad is the free
 * bytes before the header (0, or at least SPLIT_MIN). *blk includes a tail
 * smaller than SPLIT_MIN so it is not left as its own block. */
static int fit_aligned(uint8_t *start, uint32_t fsz, uint32_t payload, uint32_t align,
                       uint32_t *pad_out, uint32_t *blk_out) {
    uintptr_t base = (uintptr_t)start;
    uintptr_t limit = base + fsz;
    uintptr_t data = (base + sizeof(blk_t) + (align - 1u)) & ~(uintptr_t)(align - 1u);
    uintptr_t hdr = data - sizeof(blk_t);
    if (hdr < base) { return 0; }
    uint32_t pad = (uint32_t)(hdr - base);
    if (pad > 0 && pad < SPLIT_MIN) {
        if (align > limit - base) { return 0; }
        data += align;
        hdr += align;
        pad += align;
    }
    if (hdr < base || hdr + sizeof(blk_t) + payload > limit) { return 0; }
    uint32_t blk = (uint32_t)sizeof(blk_t) + payload;
    uint32_t tail = (uint32_t)(limit - (hdr + blk));
    if (tail > 0 && tail < SPLIT_MIN) { blk += tail; }
    if ((pad & 7u) || (blk & 7u) || pad + blk > fsz) { return 0; }
    *pad_out = pad;
    *blk_out = blk;
    return 1;
}

static blk_t *take_hole(uint32_t payload, uint32_t align, uint8_t owner, uint8_t flags) {
    uint32_t need = payload + (uint32_t)sizeof(blk_t);
    for (uint8_t *p = s_base; p + sizeof(blk_t) <= s_ord;) {
        blk_t *f = (blk_t *)p;
        if (!block_ok(f)) { break; }
        uint8_t *next = p + f->size;
        if (!(f->flags & BLK_FREE)) { p = next; continue; }
        if (align <= 8) {
            if (f->size < need) { p = next; continue; }
            if (f->size - need >= SPLIT_MIN) {
                blk_t *rest = (blk_t *)(p + need);
                stamp(rest, f->size - need, 0, BLK_FREE);
                f->size = need;
            }
            stamp(f, f->size, owner, flags);
            return f;
        }
        uint32_t pad = 0, blk = 0, orig = f->size;
        if (!fit_aligned(p, orig, payload, align, &pad, &blk)) { p = next; continue; }
        if (pad) {
            f->size = pad;
            blk_t *b = (blk_t *)(p + pad);
            stamp(b, blk, owner, flags);
            uint32_t tail = orig - pad - blk;
            if (tail) { stamp((blk_t *)((uint8_t *)b + blk), tail, 0, BLK_FREE); }
            return b;
        }
        if (orig - blk >= SPLIT_MIN) { stamp((blk_t *)(p + blk), orig - blk, 0, BLK_FREE); }
        stamp(f, orig - blk >= SPLIT_MIN ? blk : orig, owner, flags);
        return f;
    }
    return NULL;
}

static blk_t *bump(uint32_t payload, uint32_t align, uint8_t owner, uint8_t flags) {
    uint8_t *ge = s_end - s_suffix;
    if (align <= 8) {
        uint32_t need = payload + (uint32_t)sizeof(blk_t);
        if (s_ord > ge || (uint32_t)(ge - s_ord) < need) { return NULL; }
        blk_t *b = (blk_t *)s_ord;
        stamp(b, need, owner, flags);
        s_ord += need;
        return b;
    }
    uintptr_t base = (uintptr_t)s_ord;
    uintptr_t data = (base + sizeof(blk_t) + (align - 1u)) & ~(uintptr_t)(align - 1u);
    uintptr_t hdr = data - sizeof(blk_t);
    if (hdr < base) { return NULL; }
    uint32_t pad = (uint32_t)(hdr - base);
    if (pad > 0 && pad < SPLIT_MIN) {
        data += align;
        hdr += align;
        pad += align;
    }
    uint32_t blk = (uint32_t)sizeof(blk_t) + payload;
    if ((uint8_t *)hdr + blk > ge || (pad & 7u)) { return NULL; }
    if (pad) { stamp((blk_t *)s_ord, pad, 0, BLK_FREE); }
    blk_t *b = (blk_t *)hdr;
    stamp(b, blk, owner, flags);
    s_ord = (uint8_t *)hdr + blk;
    return b;
}

static void *alloc_block(uint32_t size, uint32_t align, uint8_t owner, uint8_t flags) {
    if (!s_base) { return NULL; }
    if (!size) { size = 1; }
    if (align < 8) { align = 8; }
    if (size > (uint32_t)(s_end - s_base)) { return NULL; }
    uint32_t payload = AL8(size);
    if (payload < size) { return NULL; }
    if (payload > (uint32_t)(s_end - s_base) - (uint32_t)sizeof(blk_t)) { return NULL; }
    blk_t *b = take_hole(payload, align, owner, flags);
    if (!b) { b = bump(payload, align, owner, flags); }
    return b ? (uint8_t *)b + sizeof(blk_t) : NULL;
}

void *pex__alloc(uint32_t size, uint32_t align, uint8_t owner, uint8_t sys) {
    uint8_t flags = sys ? BLK_SYS : 0;
    void *p = alloc_block(size, align, owner, flags);
    if (!p) { return NULL; }
    blk_t *b = (blk_t *)((uint8_t *)p - sizeof(blk_t));
    account(owner, flags, (int32_t)b->size, 1);
    return p;
}

static blk_t *blk_of(void *p) {
    if (!p || ((uintptr_t)p & 7u)) { return NULL; }
    if ((uint8_t *)p < s_base + sizeof(blk_t) || (uint8_t *)p >= s_ord) { return NULL; }
    blk_t *b = (blk_t *)((uint8_t *)p - sizeof(blk_t));
    if ((uint8_t *)b < s_base || b->magic != BLK_MAGIC || (b->flags & BLK_FREE)) { return NULL; }
    if (!block_ok(b)) { return NULL; }
    return b;
}

void pex__free(void *p) {
    if (!p) { return; }
    blk_t *b = blk_of(p);
    if (!b) {
        pex_mod_t *m = pex__current();
        pex__logf(PEX_LOG_ERR, "pex: %s freed %p which is not a live PEX block (ignored)",
                  m ? m->name : "host", p);
        return;
    }
    if (b->flags & BLK_SCRATCH) {
        s_scratch_now = s_scratch_now >= b->size ? s_scratch_now - b->size : 0;
    } else {
        account(b->owner, b->flags, -(int32_t)b->size, -1);
    }
    b->flags = BLK_FREE;
    b->owner = 0;
    coalesce();
}

uint32_t pex__free_owner(uint8_t owner, uint32_t *bytes) {
    uint32_t n = 0, by = 0;
    for (uint8_t *p = s_base; p + sizeof(blk_t) <= s_ord;) {
        blk_t *b = (blk_t *)p;
        if (!block_ok(b)) { break; }
        uint8_t *next = p + b->size;
        /* Scratch stays until its arena ends. free_owner runs after
         * scratch_release_owner, and a buried dead arena is still on s_last. */
        if (!(b->flags & (BLK_FREE | BLK_SCRATCH)) && b->owner == owner) {
            if (!(b->flags & BLK_SYS)) { n++; by += b->size; }
            account(b->owner, b->flags, -(int32_t)b->size, -1);
            b->flags = BLK_FREE;
            b->owner = 0;
        }
        p = next;
    }
    coalesce();
    if (bytes) { *bytes = by; }
    return n;
}

static uint32_t blk_payload(void *p) {
    blk_t *b = blk_of(p);
    return b ? b->size - (uint32_t)sizeof(blk_t) : 0;
}

/* ---- scratch -------------------------------------------------------------- */
pex_arena_t *pex__scratch_begin(uint32_t cap, uint8_t owner) {
    if (!s_base || cap > (uint32_t)(s_end - s_base)) { return NULL; }
    uint32_t need = ARENA_HDR + AL8(cap);
    void *p = alloc_block(need, 8, owner, BLK_SCRATCH);
    if (!p) { return NULL; }
    blk_t *b = (blk_t *)((uint8_t *)p - sizeof(blk_t));
    s_scratch_now += b->size;
    if (s_scratch_now > s_scratch_peak) { s_scratch_peak = s_scratch_now; }
    struct pex_arena *a = p;
    a->magic = ARENA_MAGIC;
    a->cap = AL8(cap);
    a->used = 0;
    a->owner = owner;
    a->dead = 0;
    a->prev = s_last;
    s_last = a;
    pex_mod_t *m = owner != PEX_OWNER_HOST ? pex__mod_by_id(owner) : NULL;
    if (m) {
        m->scratch_bytes += need;
        if (m->scratch_bytes > m->scratch_peak) { m->scratch_peak = m->scratch_bytes; }
    }
    return a;
}

void *pex__scratch_alloc(pex_arena_t *a, uint32_t size) {
    if (!a || a->magic != ARENA_MAGIC || a->dead) { return NULL; }
    uint32_t off = AL8(a->used);
    if (size > a->cap || off > a->cap - size) { return NULL; }
    a->used = off + size;
    return (uint8_t *)a + ARENA_HDR + off;
}

static void arena_unaccount(struct pex_arena *a) {
    pex_mod_t *m = a->owner != PEX_OWNER_HOST ? pex__mod_by_id(a->owner) : NULL;
    if (m) { m->scratch_bytes -= ARENA_HDR + a->cap; }
}

static void arena_drop_dead(void) {
    while (s_last && s_last->dead) {
        struct pex_arena *a = s_last;
        s_last = a->prev;
        a->magic = 0;
        pex__free(a);
    }
}

int pex__scratch_end(pex_arena_t *a) {
    if (!a || !pex__in_pool(a) || a->magic != ARENA_MAGIC || a->dead) { return PEX_EINVAL; }
    if (a != s_last) {
        pex_mod_t *m = pex__current();
        pex__logf(PEX_LOG_ERR, "pex: %s: scratch arenas must end in reverse order (LIFO): refused",
                  m ? m->name : "host");
        return PEX_EORDER;
    }
    arena_unaccount(a);
    s_last = a->prev;
    a->magic = 0;
    pex__free(a);
    arena_drop_dead();
    return PEX_OK;
}

uint32_t pex__scratch_release_owner(uint8_t owner) {
    uint32_t n = 0;
    for (struct pex_arena *a = s_last; a; a = a->prev) {
        if (!a->dead && a->owner == owner) {
            arena_unaccount(a);
            a->dead = 1;
            n++;
        }
    }
    arena_drop_dead();
    return n;
}

/* ---- pool-wide stats ------------------------------------------------------ */
int pex_pool_stats(pex_mem_stats_t *out) {
    memset(out, 0, sizeof *out);
    uint32_t gap = pex__pool_gap(), fr = gap, largest = gap;
    for (uint8_t *p = s_base; s_base && p + sizeof(blk_t) <= s_ord;) {
        blk_t *b = (blk_t *)p;
        if (!block_ok(b)) { break; }
        if (b->flags & BLK_FREE) {
            fr += b->size;
            if (b->size > largest) { largest = b->size; }
        } else if (!(b->flags & BLK_SCRATCH)) {
            out->long_bytes += b->size;
            out->long_blocks++;
        }
        p += b->size;
    }
    out->long_bytes += s_suffix;
    out->scratch_bytes = s_scratch_now;
    out->scratch_peak = s_scratch_peak;
    out->pool_free = fr;
    out->pool_largest = largest;
    out->pool_size = s_base ? (uint32_t)(s_end - s_base) : 0;
    return PEX_OK;
}

/* ---- the mem group (module-facing) ----------------------------------------- */
void *pex_alloc_long(size_t size) { return pex__alloc((uint32_t)size, 8, pex__current_owner(), 0); }
void *pex_alloc_ex(size_t size, uint32_t flags) {
    return pex__alloc((uint32_t)size, (flags & PEX_MEM_DMA) ? 32 : 8, pex__current_owner(), 0);
}
void pex_free(void *p) { pex__free(p); }
void *pex_alloc_host(size_t size) { return pex__alloc((uint32_t)size, 8, PEX_OWNER_HOST, 0); }
void *pex_malloc(size_t size) { return pex_alloc_long(size); }
void *pex_calloc(size_t n, size_t size) {
    if (size && n > 0xFFFFFFFFu / size) { return NULL; }
    void *p = pex_alloc_long(n * size);
    if (p) { memset(p, 0, n * size); }
    return p;
}
void *pex_realloc(void *p, size_t size) {
    if (!p) { return pex_malloc(size); }
    uint32_t old = blk_payload(p);
    if (!old) { return NULL; }
    if (size <= old) { return p; }
    void *n = pex_malloc(size);
    if (!n) { return NULL; }
    memcpy(n, p, old);
    pex__free(p);
    return n;
}
pex_arena_t *pex_scratch_begin(size_t cap) { return pex__scratch_begin((uint32_t)cap, pex__current_owner()); }
void *pex_scratch_alloc(pex_arena_t *a, size_t size) { return pex__scratch_alloc(a, (uint32_t)size); }
int pex_scratch_end(pex_arena_t *a) { return pex__scratch_end(a); }
size_t pex_scratch_avail(void) {
    /* Largest 8-aligned cap whose arena block fits in the gap. Holes are not
     * counted: begin(av) fits, begin(av + 1) does not (AL8 rounds up by 8). */
    uint32_t gap = pex__pool_gap();
    uint32_t overhead = ARENA_HDR + (uint32_t)sizeof(blk_t);
    if (gap <= overhead) { return 0; }
    return (gap - overhead) & ~7u;
}

pex_pool_t *pex_pool_new(size_t obj_size, uint32_t count) {
    if (!count || obj_size > 0x10000u) { return NULL; }
    uint32_t obj = AL8(obj_size < 8 ? 8 : (uint32_t)obj_size);
    uint32_t hdr = AL8((uint32_t)sizeof(struct pex_pool));
    if (count > (0xFFFFFFFFu - hdr) / obj) { return NULL; }
    struct pex_pool *p = pex__alloc(hdr + obj * count, 8, pex__current_owner(), 0);
    if (!p) { return NULL; }
    p->magic = POOL_MAGIC;
    p->obj = obj;
    p->count = count;
    p->used = 0;
    p->objs = (uint8_t *)p + hdr;
    p->free = NULL;
    for (uint32_t i = count; i-- > 0;) {
        void **o = (void **)(p->objs + i * obj);
        *o = p->free;
        p->free = o;
    }
    return p;
}
void *pex_pool_get(pex_pool_t *p) {
    if (!p || p->magic != POOL_MAGIC || !p->free) { return NULL; }
    void **o = p->free;
    p->free = *o;
    p->used++;
    return o;
}
void pex_pool_put(pex_pool_t *p, void *obj) {
    if (!p || p->magic != POOL_MAGIC || !obj) { return; }
    uint8_t *o = obj;
    if (o < p->objs || o >= p->objs + p->obj * p->count || (uint32_t)(o - p->objs) % p->obj) {
        pex__logf(PEX_LOG_ERR, "pex: pool_put of an object not from this pool (ignored)");
        return;
    }
    *(void **)o = p->free;
    p->free = o;
    p->used--;
}
void pex_pool_delete(pex_pool_t *p) {
    if (!p || p->magic != POOL_MAGIC) { return; }
    p->magic = 0;
    pex__free(p);
}

int pex_mem_stats(pex_mem_stats_t *out) {
    if (!out) { return PEX_EINVAL; }
    pex_pool_stats(out);
    pex_mod_t *m = pex__current();
    if (m) {
        out->long_bytes = m->long_bytes;
        out->long_blocks = m->long_blocks;
        out->scratch_bytes = m->scratch_bytes;
        out->scratch_peak = m->scratch_peak;
    }
    return PEX_OK;
}
