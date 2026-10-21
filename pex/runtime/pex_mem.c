/*
 * The PEX pool, separated by lifetime (spec 7.2):
 *
 *   base                                                              end
 *   | scratch arenas -> (LIFO, bump)   ...free gap...   <- long-term blocks |
 *                     ^top                             ^low
 *
 * Long-term blocks (module images, RW, pex_alloc_long, pools, malloc) grow
 * down from the end, first-fit with coalescing; every block carries its owner
 * tag so a module's leftovers are freed at unload. Scratch arenas grow up from
 * the base and end in reverse order. The two sides never interleave.
 */
#include <string.h>
#include "pex_internal.h"

#define BLK_MAGIC 0xB10Cu
#define BLK_FREE 1u
#define BLK_SYS 2u                /* runtime data for a module: not in its accounting */
#define ARENA_MAGIC 0x5C7A7C40u
#define POOL_MAGIC 0x9001F00Du

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

static uint8_t *s_base, *s_end, *s_top, *s_low;
static struct pex_arena *s_last;
static uint32_t s_scratch_peak;

#define AL8(x) (((x) + 7u) & ~7u)

void pex__mem_init(void *pool, uint32_t size) {
    uintptr_t b = ((uintptr_t)pool + 7u) & ~(uintptr_t)7u;
    uintptr_t e = ((uintptr_t)pool + size) & ~(uintptr_t)7u;
    s_base = s_top = (uint8_t *)b;
    s_end = s_low = (uint8_t *)e;
    s_last = NULL;
    s_scratch_peak = 0;
}

uint8_t *pex__pool_end(void) { return s_end; }

bool pex__in_pool(const void *p) {
    return (const uint8_t *)p >= s_base && (const uint8_t *)p < s_end;
}

static void account(uint8_t owner, uint8_t flags, int32_t bytes, int32_t blocks) {
    if (owner == PEX_OWNER_HOST || owner == PEX_OWNER_RT || (flags & BLK_SYS)) { return; }
    pex_mod_t *m = pex__mod_by_id(owner);
    if (m) {
        m->long_bytes += (uint32_t)bytes;
        m->long_blocks += (uint32_t)blocks;
    }
}

static void coalesce(void) {
    blk_t *b = (blk_t *)s_low;
    while ((uint8_t *)b < s_end) {
        blk_t *n = (blk_t *)((uint8_t *)b + b->size);
        if ((b->flags & BLK_FREE) && (uint8_t *)n < s_end && (n->flags & BLK_FREE)) {
            b->size += n->size;
            n->magic = 0;
            continue;
        }
        b = n;
    }
    while (s_low < s_end && (((blk_t *)s_low)->flags & BLK_FREE)) {
        blk_t *f = (blk_t *)s_low;
        f->magic = 0;
        s_low += f->size;
    }
}

void *pex__alloc(uint32_t size, uint32_t align, uint8_t owner, uint8_t sys) {
    if (!s_base || size > (uint32_t)(s_end - s_base)) { return NULL; }
    uint32_t need = AL8(size ? size : 1) + (uint32_t)sizeof(blk_t);
    uint8_t flags = sys ? BLK_SYS : 0;
    blk_t *b = NULL;
    if (align <= 8) {
        for (blk_t *f = (blk_t *)s_low; (uint8_t *)f < s_end; f = (blk_t *)((uint8_t *)f + f->size)) {
            if ((f->flags & BLK_FREE) && f->size >= need) {
                if (f->size - need >= 24) {
                    blk_t *rest = (blk_t *)((uint8_t *)f + need);
                    rest->size = f->size - need;
                    rest->flags = BLK_FREE;
                    rest->owner = 0;
                    rest->magic = BLK_MAGIC;
                    f->size = need;
                }
                b = f;
                break;
            }
        }
        if (!b) {
            if ((uint32_t)(s_low - s_top) < need) { return NULL; }
            s_low -= need;
            b = (blk_t *)s_low;
            b->size = need;
        }
    } else {
        /* aligned (DMA): carved fresh at the low end so the payload lands aligned */
        uintptr_t data = ((uintptr_t)s_low - AL8(size)) & ~(uintptr_t)(align - 1);
        uintptr_t hdr = data - sizeof(blk_t);
        if (hdr < (uintptr_t)s_top || hdr < (uintptr_t)s_base) { return NULL; }
        b = (blk_t *)hdr;
        b->size = (uint32_t)((uintptr_t)s_low - hdr);
        s_low = (uint8_t *)hdr;
    }
    b->owner = owner;
    b->flags = flags;
    b->magic = BLK_MAGIC;
    account(owner, flags, (int32_t)b->size, 1);
    return (uint8_t *)b + sizeof(blk_t);
}

static blk_t *blk_of(void *p) {
    if (!p || !pex__in_pool(p) || ((uintptr_t)p & 7u)) { return NULL; }
    blk_t *b = (blk_t *)((uint8_t *)p - sizeof(blk_t));
    if ((uint8_t *)b < s_low || b->magic != BLK_MAGIC || (b->flags & BLK_FREE)) { return NULL; }
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
    account(b->owner, b->flags, -(int32_t)b->size, -1);
    b->flags = BLK_FREE;
    b->owner = 0;
    coalesce();
}

uint32_t pex__free_owner(uint8_t owner, uint32_t *bytes) {
    uint32_t n = 0, by = 0;
    for (blk_t *b = (blk_t *)s_low; (uint8_t *)b < s_end; b = (blk_t *)((uint8_t *)b + b->size)) {
        if (!(b->flags & BLK_FREE) && b->owner == owner) {
            if (!(b->flags & BLK_SYS)) { n++; by += b->size; }
            account(b->owner, b->flags, -(int32_t)b->size, -1);
            b->flags = BLK_FREE;
            b->owner = 0;
        }
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
    if (!s_base) { return NULL; }
    uint32_t need = ARENA_HDR + AL8(cap);
    if (cap > (uint32_t)(s_end - s_base) || (uint32_t)(s_low - s_top) < need) { return NULL; }
    struct pex_arena *a = (struct pex_arena *)s_top;
    a->magic = ARENA_MAGIC;
    a->cap = AL8(cap);
    a->used = 0;
    a->owner = owner;
    a->dead = 0;
    a->prev = s_last;
    s_last = a;
    s_top += need;
    uint32_t used = (uint32_t)(s_top - s_base);
    if (used > s_scratch_peak) { s_scratch_peak = used; }
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

static void arena_drop_dead(void) {
    while (s_last && s_last->dead) {
        s_top = (uint8_t *)s_last;
        s_last->magic = 0;
        s_last = s_last->prev;
    }
}

static void arena_unaccount(struct pex_arena *a) {
    pex_mod_t *m = a->owner != PEX_OWNER_HOST ? pex__mod_by_id(a->owner) : NULL;
    if (m) { m->scratch_bytes -= ARENA_HDR + a->cap; }
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
    s_top = (uint8_t *)a;
    a->magic = 0;
    s_last = a->prev;
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
    uint32_t gap = (uint32_t)(s_low - s_top), fr = gap, largest = gap;
    for (blk_t *b = (blk_t *)s_low; (uint8_t *)b < s_end; b = (blk_t *)((uint8_t *)b + b->size)) {
        if (b->flags & BLK_FREE) {
            fr += b->size;
            if (b->size > largest) { largest = b->size; }
        } else {
            out->long_bytes += b->size;
            out->long_blocks++;
        }
    }
    out->scratch_bytes = (uint32_t)(s_top - s_base);
    out->scratch_peak = s_scratch_peak;
    out->pool_free = fr;
    out->pool_largest = largest;
    out->pool_size = (uint32_t)(s_end - s_base);
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
    uint32_t gap = (uint32_t)(s_low - s_top);
    return gap > ARENA_HDR ? gap - ARENA_HDR : 0;
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
