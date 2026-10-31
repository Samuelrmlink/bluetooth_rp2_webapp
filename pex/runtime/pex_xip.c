/*
 * XIP: installing modules into the PEX flash partition and running them in
 * place (spec 16). Portable: the host gives the partition as a pex_flash_t
 * (where it reads and executes, how a sector is erased and a page programmed).
 *
 *   sector 0, 1   the index, two copies with a sequence number. An update
 *                 writes the copy that does NOT hold the current index and
 *                 the newest valid one wins at mount: a power cut during any
 *                 update leaves the previous index in force.
 *   sector 2..    whole .pex files, each from a sector boundary.
 *
 * Every change is copy-then-commit: a file is written into free sectors,
 * verified in place, and only then entered in the index; a removal is an
 * index update; space nobody's index entry covers is free.
 *
 * Code models (spec 6.4):
 *   B (PIC)  stored exactly as signed; runs at any address; moved by a copy.
 *   A        its RO words are relocated when it is written: code words to
 *            its flash address, RW words to a FIXED RW address at the top of
 *            the pool (pool end - aoff), reserved at mount for every
 *            installed model-A module. A move (compaction) re-relocates the
 *            copy it writes. The original bytes are always recoverable: the
 *            entry records the bases the RO words carry, so a reader that
 *            subtracts them yields the signed file again (verification in
 *            place, a RAM load, the source of a move).
 *
 * Compaction slides a file DOWN, possibly overlapping its old place, one
 * sector at a time in ascending order. Writing destination sector j destroys
 * source sector j-d (d = the distance moved), which was copied at step j-d.
 * Before that step the index records a journal (entry, destination, steps
 * done = j) so that a resumed move (pex_xip_mount) redoes only steps whose
 * source is intact. Without overlap no journal is needed.
 */
#include <stdio.h>
#include <string.h>
#include "pex_internal.h"

#define SEC 4096u
#define PAGE 256u
#define IX_MAGIC 0x31495850u        /* "PXI1" */
#define IX_MAX 32
#define IX_SECTORS 2u
#define MAX_SECTORS 1024u

#define ES_SIGNED 1u
#define ES_VERIFIED 2u

#define AL8(x) (((x) + 7u) & ~7u)

typedef struct {
    uint8_t build_id[16];
    char name[24];
    uint32_t sec, file_size;
    uint32_t version, flags, target;
    uint32_t image_off, code_size, rw_size;
    uint32_t code_base, rw_base;  /* model A: the bases its RO words carry in flash (0 = model B) */
    uint32_t aoff;                /* model A: RW at pool end - aoff - AL8(rw_size) */
    uint32_t state;               /* ES_* */
} ix_ent_t;

typedef struct {
    uint32_t magic, seq;
    uint16_t n, esize;
    uint32_t sectors;
    uint32_t mv_active, mv_ent, mv_dst, mv_done, mv_code_base, mv_rw_base, mv_aoff;
    uint32_t reserved[4];
    uint32_t crc;
} ix_hdr_t;

typedef struct {
    ix_hdr_t h;
    ix_ent_t e[IX_MAX];
} ix_t;

#define IX_BYTES ((uint32_t)((sizeof(ix_t) + PAGE - 1) & ~(PAGE - 1)))
_Static_assert(sizeof(ix_ent_t) == 88, "index entry layout");
_Static_assert(sizeof(ix_hdr_t) == 64, "index header layout");
_Static_assert(IX_BYTES <= SEC, "the index fits a sector");

static const pex_flash_t *F;
static ix_t *s_ix;                  /* the committed index (IX_BYTES, pool) */
static int s_cur = -1;              /* copy holding it */
static bool s_mounted;
static uint32_t s_moves, s_recovered;
static uint8_t *s_area;             /* model A RW suffix: [pool end - s_area_size, pool end). Not a block. */
static uint32_t s_area_size;
static pex_install_t *s_open;       /* the push-install session, or NULL */
static int s_nest;                  /* the session is inside pex_xip_compact */
static int index_locked(void);

static uint32_t rd32(const void *p) {
    const uint8_t *b = p;
    return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
}
static uint16_t rd16(const void *p) {
    const uint8_t *b = p;
    return (uint16_t)(b[0] | b[1] << 8);
}
static const uint8_t *fptr(uint32_t off) { return (const uint8_t *)(F->read_base + off); }
static uint32_t nsec_total(void) { return F->size / SEC; }
static uint32_t nsec_of(const ix_ent_t *e) { return (e->file_size + SEC - 1) / SEC; }
static bool model_a(const ix_ent_t *e) { return !(e->flags & PEX_F_PIC); }
static uint32_t rwsz(uint32_t rw_size) { return AL8(rw_size ? rw_size : 4); }

/* ---- the index ---------------------------------------------------------------- */
static uint32_t ix_crc(const ix_t *x) {
    ix_hdr_t h = x->h;
    h.crc = 0;
    uint32_t c = pex_crc32_update(0, &h, sizeof h);
    return pex_crc32_update(c, x->e, (uint32_t)(x->h.n * sizeof(ix_ent_t)));
}

static bool ix_valid(const ix_t *x) {
    return x->h.magic == IX_MAGIC && x->h.n <= IX_MAX && x->h.esize == sizeof(ix_ent_t) && ix_crc(x) == x->h.crc;
}

static int fl_erase(uint32_t off) { return F->erase(F->ctx, off) ? PEX_EIO : PEX_OK; }
static int fl_prog(uint32_t off, const void *p, uint32_t n) { return F->prog(F->ctx, off, p, n) ? PEX_EIO : PEX_OK; }

/* Publish s_ix into the other sector. seq is snapshotted first because s_ix is
 * the buffer being written. On failure the current sector, which this does not
 * modify, is copied back, so a torn write leaves the RAM index unchanged too. */
static int commit(void) {
    uint32_t seq = s_ix->h.seq;
    s_ix->h.magic = IX_MAGIC;
    s_ix->h.seq = seq + 1;
    s_ix->h.esize = sizeof(ix_ent_t);
    s_ix->h.sectors = nsec_total();
    memset(&s_ix->e[s_ix->h.n], 0, IX_BYTES - sizeof(ix_hdr_t) - s_ix->h.n * sizeof(ix_ent_t));
    s_ix->h.crc = ix_crc(s_ix);
    uint32_t t = s_cur == 0 ? 1u : 0u;
    if (fl_erase(t * SEC) || fl_prog(t * SEC, s_ix, IX_BYTES) || memcmp(fptr(t * SEC), s_ix, IX_BYTES) != 0) {
        if (s_cur >= 0) { memcpy(s_ix, fptr((uint32_t)s_cur * SEC), IX_BYTES); }
        pex__err(PEX_EIO, "flash write of the PEX index failed");
        return PEX_EIO;
    }
    s_cur = (int)t;
    return PEX_OK;
}

static int find_name(const char *name) {
    for (int i = 0; i < s_ix->h.n; i++) {
        if (strncmp(s_ix->e[i].name, name, sizeof s_ix->e[i].name) == 0) { return i; }
    }
    return -1;
}

static uintptr_t code_addr(const ix_ent_t *e) { return F->exec_base + e->sec * SEC + e->image_off; }

/* A loaded module runs from this copy. */
static bool in_use(int i) {
    const ix_ent_t *e = &s_ix->e[i];
    uintptr_t a = code_addr(e);
    for (pex_mod_t *m = pex__mods; m; m = m->next) {
        if (m->code && m->code->xip && (uintptr_t)m->code->base == a) { return true; }
    }
    return false;
}

/* ---- free space ------------------------------------------------------------------ */
static void used_map(uint8_t *map, int skip) {
    memset(map, 0, MAX_SECTORS / 8);
    for (uint32_t s = 0; s < IX_SECTORS; s++) { map[s / 8] |= (uint8_t)(1u << (s % 8)); }
    for (int i = 0; i < s_ix->h.n; i++) {
        if (i == skip) { continue; }
        const ix_ent_t *e = &s_ix->e[i];
        for (uint32_t s = e->sec; s < e->sec + nsec_of(e) && s < MAX_SECTORS; s++) { map[s / 8] |= (uint8_t)(1u << (s % 8)); }
    }
}

static bool is_used(const uint8_t *map, uint32_t s) { return (map[s / 8] >> (s % 8)) & 1u; }

/* First free run of n sectors (skip: an entry whose sectors count as free), or -1. */
static int find_run(uint32_t n, int skip, uint32_t *free_total, uint32_t *largest) {
    uint8_t map[MAX_SECTORS / 8];
    used_map(map, skip);
    int found = -1;
    uint32_t run = 0, ft = 0, lg = 0;
    for (uint32_t s = 0; s < nsec_total(); s++) {
        if (is_used(map, s)) { run = 0; continue; }
        run++;
        ft++;
        if (run > lg) { lg = run; }
        if (found < 0 && run == n) { found = (int)(s + 1 - n); }
    }
    if (free_total) { *free_total = ft; }
    if (largest) { *largest = lg; }
    return found;
}

/* ---- reading an installed copy: the original bytes ---------------------------------
 * File sector k is at sec + k -- or, during a move, at alt + k for k < done.
 * Model A: the RO words in its code carry a base; subtract it. */
typedef struct {
    uint32_t sec, code_base, rw_base;
    uint32_t alt, done, alt_code_base, alt_rw_base;
    uint32_t file_size, image_off, code_size;
    uint32_t ro_off, n_ro, split;
    bool reloc;
} xf_t;

static void xf_raw(const xf_t *x, uint32_t off, uint8_t *dst, uint32_t n) {
    while (n) {
        uint32_t k = off / SEC, in = off % SEC, c = SEC - in < n ? SEC - in : n;
        uint32_t s = (k < x->done ? x->alt : x->sec) + k;
        memcpy(dst, fptr(s * SEC + in), c);
        off += c;
        dst += c;
        n -= c;
    }
}

static uint32_t xf_word(const xf_t *x, uint32_t off) {
    uint8_t b[4];
    xf_raw(x, off, b, 4);
    return rd32(b);
}

static uint32_t ro_entry(const xf_t *x, uint32_t i) { return xf_word(x, x->ro_off + 4 * i); }

/* First RO entry in [lo, hi) whose offset part is >= key. */
static uint32_t ro_lower(const xf_t *x, uint32_t lo, uint32_t hi, uint32_t key) {
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if ((ro_entry(x, mid) & 0x7FFFFFFFu) < key) { lo = mid + 1; } else { hi = mid; }
    }
    return lo;
}

/* Header facts and the RO list, read from the copy itself. */
static int xf_setup(xf_t *x) {
    uint8_t h[PEX_HEADER_SIZE];
    xf_raw(x, 0, h, sizeof h);
    if (rd32(h) != PEX_MAGIC) { return PEX_EFORMAT; }
    x->file_size = rd32(h + 12);
    x->image_off = rd32(h + 68);
    x->code_size = rd32(h + 36) + rd32(h + 40);
    uint32_t nsec = rd32(h + 60), soff = rd32(h + 64);
    x->n_ro = x->split = 0;
    x->reloc = false;
    if (nsec > PEX_MAX_SECTIONS || soff + 12 * nsec > x->file_size) { return PEX_EFORMAT; }
    for (uint32_t i = 0; i < nsec; i++) {
        uint8_t t[12];
        xf_raw(x, soff + 12 * i, t, 12);
        if (rd16(t) == PEX_SEC_RELOC && rd32(t + 8) >= 8) {
            x->ro_off = rd32(t + 4) + 8;
            x->n_ro = xf_word(x, rd32(t + 4));
        }
    }
    x->reloc = x->n_ro && (x->code_base || x->rw_base || x->alt_code_base || x->alt_rw_base);
    /* split: the first entry pointing into RW (the list is sorted as u32) */
    uint32_t lo = 0, hi = x->n_ro;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (ro_entry(x, mid) & PEX_RELOC_TO_RW) { hi = mid; } else { lo = mid + 1; }
    }
    x->split = lo;
    return PEX_OK;
}

static int xf_read(void *ctx, uint32_t off, void *dstv, uint32_t n) {
    const xf_t *x = ctx;
    uint8_t *dst = dstv;
    if (off >= x->file_size) { return 0; }
    if (n > x->file_size - off) { n = x->file_size - off; }
    xf_raw(x, off, dst, n);
    if (!x->reloc) { return (int)n; }
    uint32_t lo = off > x->image_off ? off : x->image_off;
    uint32_t hi = off + n < x->image_off + x->code_size ? off + n : x->image_off + x->code_size;
    if (lo >= hi) { return (int)n; }
    lo -= x->image_off;
    hi -= x->image_off;
    for (int run = 0; run < 2; run++) {
        uint32_t a = run ? x->split : 0, b = run ? x->n_ro : x->split;
        for (uint32_t i = ro_lower(x, a, b, lo >= 3 ? lo - 3 : 0); i < b; i++) {
            uint32_t o = ro_entry(x, i) & 0x7FFFFFFFu;
            if (o >= hi) { break; }
            uint32_t fo = x->image_off + o;
            bool alt = fo / SEC < x->done;
            uint32_t base = run ? (alt ? x->alt_rw_base : x->rw_base) : (alt ? x->alt_code_base : x->code_base);
            uint32_t w = xf_word(x, fo) - base;
            for (uint32_t k = 0; k < 4; k++) {
                uint32_t p = fo + k;
                if (p >= off && p < off + n) { dst[p - off] = (uint8_t)(w >> (8 * k)); }
            }
        }
    }
    return (int)n;
}

static void xf_of(xf_t *x, const ix_ent_t *e) {
    memset(x, 0, sizeof *x);
    x->sec = e->sec;
    x->code_base = e->code_base;
    x->rw_base = e->rw_base;
}

/* ---- writing a copy ----------------------------------------------------------------- */
typedef struct {
    const pex_reader_t *src;      /* the original bytes */
    const xf_t *ro;               /* where the RO list is read (src's own copy or the file) */
    uint32_t file_size, image_off, code_size;
    uint32_t n_ro, split;
    uint32_t code_base, rw_base;  /* model A: the bases to apply; 0/0 = as is */
    int (*ro_at)(const void *ctx, uint32_t i, uint32_t *v);
    const void *ro_ctx;
} wr_t;

/* RO entry i through a pex_reader_t (a file being installed) */
typedef struct { const pex_reader_t *rd; uint32_t off; } rorr_t;
static int ro_from_reader(const void *ctx, uint32_t i, uint32_t *v) {
    const rorr_t *r = ctx;
    uint8_t b[4];
    if (pex__rread(r->rd, r->off + 4 * i, b, 4)) { return PEX_EIO; }
    *v = rd32(b);
    return PEX_OK;
}
static int ro_from_xf(const void *ctx, uint32_t i, uint32_t *v) {
    *v = ro_entry(ctx, i);
    return PEX_OK;
}

static int wr_lower(const wr_t *w, uint32_t lo, uint32_t hi, uint32_t key, uint32_t *out) {
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2, v;
        if (w->ro_at(w->ro_ctx, mid, &v)) { return PEX_EIO; }
        if ((v & 0x7FFFFFFFu) < key) { lo = mid + 1; } else { hi = mid; }
    }
    *out = lo;
    return PEX_OK;
}

/* Erase dst_sec and write file sector k into it, relocated. */
static int write_sector(const wr_t *w, uint32_t dst_sec, uint32_t k) {
    if (fl_erase(dst_sec * SEC)) { return PEX_EIO; }
    bool reloc = w->n_ro && (w->code_base || w->rw_base);
    uint32_t cur[2] = { 0, w->split }, end[2] = { w->split, w->n_ro };
    uint32_t s_lo = k * SEC, s_hi = s_lo + SEC;
    if (reloc && s_hi > w->image_off && s_lo < w->image_off + w->code_size) {
        uint32_t key = s_lo > w->image_off ? s_lo - w->image_off : 0;
        for (int r = 0; r < 2; r++) {
            if (wr_lower(w, cur[r], end[r], key, &cur[r])) { return PEX_EIO; }
        }
    } else {
        reloc = false;
    }
    uint8_t buf[PAGE];
    for (uint32_t p = 0; p < SEC / PAGE; p++) {
        uint32_t off = s_lo + p * PAGE;
        if (off >= w->file_size) { break; }
        uint32_t n = w->file_size - off < PAGE ? w->file_size - off : PAGE;
        memset(buf, 0xFF, sizeof buf);
        if (pex__rread(w->src, off, buf, n)) { return PEX_EIO; }
        if (reloc) {
            for (int r = 0; r < 2; r++) {
                while (cur[r] < end[r]) {
                    uint32_t v;
                    if (w->ro_at(w->ro_ctx, cur[r], &v)) { return PEX_EIO; }
                    uint32_t fo = w->image_off + (v & 0x7FFFFFFFu);
                    if (fo >= off + n) { break; }
                    if (fo >= off && fo + 4 <= off + n) {
                        uint32_t x = rd32(buf + (fo - off)) + (r ? w->rw_base : w->code_base);
                        memcpy(buf + (fo - off), &x, 4);
                    }
                    cur[r]++;
                }
            }
        }
        bool blank = true;
        for (uint32_t i = 0; i < PAGE && blank; i++) { blank = buf[i] == 0xFF; }
        if (!blank && fl_prog(dst_sec * SEC + p * PAGE, buf, PAGE)) { return PEX_EIO; }
    }
    return PEX_OK;
}

/* ---- moving a copy (compaction, re-placing model A) --------------------------------- */
static int move_entry(int i, uint32_t dst, uint32_t rw_new, uint32_t aoff_new, uint32_t done0, bool resumed) {
    ix_ent_t e = s_ix->e[i];
    uint32_t n = nsec_of(&e), src = e.sec;
    bool a = model_a(&e);
    uint32_t code_new = a ? (uint32_t)(F->exec_base + dst * SEC + e.image_off) : 0;
    if (!a) { rw_new = 0; aoff_new = 0; }
    bool overlap = dst < src + n && src < dst + n;
    if (overlap && dst > src) { pex__err(PEX_EINVAL, "move up over itself"); return PEX_EINVAL; }
    xf_t x;
    xf_of(&x, &e);
    x.alt = dst;
    x.alt_code_base = code_new;
    x.alt_rw_base = rw_new;
    x.done = done0;
    if (xf_setup(&x) != PEX_OK) { pex__err(PEX_EFORMAT, "installed copy of %s unreadable", e.name); return PEX_EFORMAT; }
    pex_reader_t rd = { xf_read, &x, x.file_size };
    wr_t w = { .src = &rd, .file_size = x.file_size, .image_off = x.image_off, .code_size = x.code_size,
               .n_ro = x.n_ro, .split = x.split, .code_base = code_new, .rw_base = rw_new,
               .ro_at = ro_from_xf, .ro_ctx = &x };
    bool journal = resumed;
    uint32_t committed = done0;
    for (uint32_t j = done0; j < n; j++) {
        if (overlap) {
            int64_t destroyed = (int64_t)dst + j - src;
            if (destroyed >= 0 && destroyed < (int64_t)n && (!journal || (int64_t)committed < destroyed + 1)) {
                s_ix->h.mv_active = 1;
                s_ix->h.mv_ent = (uint32_t)i;
                s_ix->h.mv_dst = dst;
                s_ix->h.mv_done = j;
                s_ix->h.mv_code_base = code_new;
                s_ix->h.mv_rw_base = rw_new;
                s_ix->h.mv_aoff = aoff_new;
                int rc = commit();
                if (rc) { return rc; }
                journal = true;
                committed = j;
            }
        }
        x.done = j;
        if (write_sector(&w, dst + j, j)) {
            pex__err(PEX_EIO, "flash write failed moving %s", e.name);
            return PEX_EIO;
        }
    }
    s_ix->e[i].sec = dst;
    s_ix->e[i].code_base = code_new;
    s_ix->e[i].rw_base = rw_new;
    s_ix->e[i].aoff = aoff_new;
    s_ix->h.mv_active = s_ix->h.mv_ent = s_ix->h.mv_dst = s_ix->h.mv_done = 0;
    s_ix->h.mv_code_base = s_ix->h.mv_rw_base = s_ix->h.mv_aoff = 0;
    int rc = commit();
    if (rc) { return rc; }
    s_moves++;
    pex__logf(PEX_LOG_INFO, "pex: xip: moved %s from sector %lu to %lu%s", e.name, (unsigned long)src,
              (unsigned long)dst, resumed ? " (resumed after a reset)" : "");
    return PEX_OK;
}

int pex_xip_compact(void) {
    if (!s_mounted) { pex__err(PEX_ENOTSUP, "no PEX flash partition"); return PEX_ENOTSUP; }
    if (index_locked()) { return PEX_EBUSY; }
    int order[IX_MAX], n = s_ix->h.n, moved = 0;
    for (int i = 0; i < n; i++) { order[i] = i; }
    for (int i = 1; i < n; i++) {
        for (int j = i; j > 0 && s_ix->e[order[j]].sec < s_ix->e[order[j - 1]].sec; j--) {
            int t = order[j]; order[j] = order[j - 1]; order[j - 1] = t;
        }
    }
    uint32_t next = IX_SECTORS;
    for (int k = 0; k < n; k++) {
        int i = order[k];
        const ix_ent_t *e = &s_ix->e[i];
        if (e->sec > next && !in_use(i)) {
            int rc = move_entry(i, next, e->rw_base, e->aoff, 0, false);
            if (rc) { return rc; }
            moved++;
        }
        next = s_ix->e[i].sec + nsec_of(&s_ix->e[i]);
    }
    return moved;
}

/* ---- model A: the RW area at the top of the pool ------------------------------------ */
static uint32_t area_need(const ix_t *x) {
    uint32_t need = 0;
    for (int i = 0; i < x->h.n; i++) {
        const ix_ent_t *e = &x->e[i];
        if (model_a(e) && e->aoff + rwsz(e->rw_size) > need) { need = e->aoff + rwsz(e->rw_size); }
    }
    return need;
}

/* High-water mark with entry `skip` left out and one new model-A slot added.
 * skip < 0 keeps every current entry. */
static uint32_t area_need_replacing(int skip, uint32_t aoff, uint32_t rw_size) {
    uint32_t need = aoff + rwsz(rw_size);
    for (int i = 0; i < s_ix->h.n; i++) {
        const ix_ent_t *e = &s_ix->e[i];
        if (i == skip || !model_a(e)) { continue; }
        if (e->aoff + rwsz(e->rw_size) > need) { need = e->aoff + rwsz(e->rw_size); }
    }
    return need;
}

/* The suffix is not a block. Growing fails when the gap is short of `need`;
 * shrinking only releases the low end, so loaded modules keep their addresses. */
static int area_ensure(uint32_t need) {
    if (pex__pool_set_suffix(need) != 0) { return PEX_ENOMEM; }
    s_area_size = need ? (need + 7u) & ~7u : 0;
    s_area = s_area_size ? pex__pool_end() - s_area_size : NULL;
    return PEX_OK;
}

static uint8_t *rw_ptr(const ix_ent_t *e) { return pex__pool_end() - e->aoff - rwsz(e->rw_size); }
/* the address as the code's words hold it (32-bit; on a 64-bit test host only compared) */
static uint32_t rw_want(const ix_ent_t *e) { return (uint32_t)(uintptr_t)rw_ptr(e); }

static bool area_covers(const ix_ent_t *e) { return s_area && e->aoff + rwsz(e->rw_size) <= s_area_size; }

/* lowest aoff where size bytes fit among the other model-A entries */
static uint32_t assign_aoff(uint32_t size, int skip) {
    size = rwsz(size);
    uint32_t cand[IX_MAX + 1];
    int nc = 0;
    cand[nc++] = 0;
    for (int i = 0; i < s_ix->h.n; i++) {
        if (i != skip && model_a(&s_ix->e[i])) { cand[nc++] = s_ix->e[i].aoff + rwsz(s_ix->e[i].rw_size); }
    }
    uint32_t best = 0xFFFFFFFFu;
    for (int c = 0; c < nc; c++) {
        bool ok = true;
        for (int i = 0; i < s_ix->h.n && ok; i++) {
            const ix_ent_t *e = &s_ix->e[i];
            if (i == skip || !model_a(e)) { continue; }
            ok = cand[c] + size <= e->aoff || e->aoff + rwsz(e->rw_size) <= cand[c];
        }
        if (ok && cand[c] < best) { best = cand[c]; }
    }
    return best;
}

/* ---- push install --------------------------------------------------------------------
 * The caller feeds the file in order. Header and section table stay in the
 * session until they parse. RELOC and META must sit before the image (both
 * linkers already write that); a file that does not is refused here, while
 * pex_install() still accepts it. Pages are erased and programmed as they
 * fill, so a 4 KB aligned write is one sector. The RO list is read back from
 * the destination (or from the page still in hand) and is not buffered.
 * area_ensure runs once META names the module and before any image page is
 * programmed. A pre-image page can be programmed before that when RELOC is
 * large: those bytes are not relocated, and a later failure does not commit
 * the index. The suffix is restored if it was grown.
 */
enum { PH_HDR = 0, PH_BODY = 1, PH_EARLY = 2, PH_FAIL = 3 };

struct pex_install {
    uint32_t flags;
    int phase, err, maybe_same, same_hit, decided, ready, area_set, old, erased;
    int have_meta, name_set, ro_done;
    uint32_t got, prefix_got, prefix_need, meta_off, meta_end;
    uint32_t mp, mrec, mtotal, mlen;
    uint16_t mtag;
    uint8_t mhdr[4];
    uint32_t dst, nsec, code_base, rw_base, aoff, area_saved, crc_acc, n_ro, ro_off;
    uint32_t page_off, page_n, sig_have;
    char name[24];
    uint8_t sigb[68];
    uint8_t digest[32];
    uint8_t page[PAGE];
    uint8_t prefix[PEX_HEADER_SIZE + PEX_MAX_SECTIONS * 12];
    pex__hdr_t hdr;
    pex_sha256_t sha;
};
_Static_assert(sizeof(struct pex_install) <= 2048, "install session stays a small host block");

static int index_locked(void) {
    if (!s_open || s_nest) { return 0; }
    pex__err(PEX_EBUSY, "an install of %s is in progress", s_open->name[0] ? s_open->name : "a module");
    return PEX_EBUSY;
}

/* build id and file_crc32, and the installed copy is signed if the new file is.
 * file_crc32 lives in the installed header; the index format does not change.
 * SIG is outside the CRC, so another key over the same bytes still matches. */
static int same_build(const ix_ent_t *e, const uint8_t bid[16], uint32_t crc, int new_signed) {
    if (memcmp(e->build_id, bid, 16) != 0) { return 0; }
    if (rd32(fptr(e->sec * SEC + 72)) != crc) { return 0; }
    if (new_signed && !(e->state & ES_SIGNED)) { return 0; }
    return 1;
}

static int sess_fail(pex_install_t *s, int rc) {
    s->phase = PH_FAIL;
    s->err = rc;
    return rc;
}

static void sess_close(pex_install_t *s, int restore) {
    if (restore && s->area_set && area_ensure(s->area_saved) != PEX_OK) {
        pex__logf(PEX_LOG_ERR, "pex: xip: could not restore the %lu-byte RW reservation", (unsigned long)s->area_saved);
    }
    if (s_open == s) { s_open = NULL; }
    pex__free(s);
}

static void hash_bytes(pex_install_t *s, const uint8_t *p, uint32_t n, uint32_t off) {
    while (n) {
        if (off >= s->hdr.sig_off) {
            if (s->hdr.sig >= 0) {
                const pex__sec_t *sg = &s->hdr.sec[s->hdr.sig];
                if (off < sg->off + sg->size && s->sig_have == off - sg->off && s->sig_have < sizeof s->sigb) {
                    uint32_t m = n;
                    if (s->sig_have + m > sg->size) { m = sg->size - s->sig_have; }
                    if (s->sig_have + m > sizeof s->sigb) { m = (uint32_t)sizeof s->sigb - s->sig_have; }
                    memcpy(s->sigb + s->sig_have, p, m);
                    s->sig_have += m;
                }
            }
            return;
        }
        uint32_t k = n;
        if (off + k > s->hdr.sig_off) { k = s->hdr.sig_off - off; }
        uint8_t buf[128];
        uint32_t left = k;
        const uint8_t *q = p;
        uint32_t o = off;
        while (left) {
            uint32_t c = left < sizeof buf ? left : (uint32_t)sizeof buf;
            memcpy(buf, q, c);
            pex_sha256_update(&s->sha, buf, c);
            for (uint32_t i = 0; i < 4; i++) {
                if (o <= 72u + i && 72u + i < o + c) { buf[72u + i - o] = 0; }
            }
            s->crc_acc = pex_crc32_update(s->crc_acc, buf, c);
            q += c;
            o += c;
            left -= c;
        }
        p += k;
        off += k;
        n -= k;
    }
}

static void meta_byte(pex_install_t *s, uint8_t b) {
    if (s->name_set) { s->mp++; return; }
    uint32_t pos = s->mp++;
    uint32_t rel = pos - s->mrec;
    if (rel < 4) {
        s->mhdr[rel] = b;
        if (rel == 3) {
            s->mtag = rd16(s->mhdr);
            s->mlen = rd16(s->mhdr + 2);
            s->mtotal = (4u + s->mlen + 3u) & ~3u;
            /* A record that runs past META is not a name (pex__meta_name stops). */
            if (s->mrec + s->mtotal > s->meta_end - s->meta_off) { s->mtag = 0xFFFF; }
        }
        return;
    }
    if (s->mtag == PEX_META_NAME && rel - 4 < 23 && rel - 4 < s->mlen) {
        s->name[rel - 4] = (char)b;
        s->name[rel - 3] = 0;
    }
    if (rel + 1 == s->mtotal) {
        s->mrec = pos + 1;
        if (s->mtag == PEX_META_NAME) { s->name_set = 1; }
    }
}

static void meta_feed(pex_install_t *s, const uint8_t *p, uint32_t n, uint32_t off) {
    if (!s->have_meta || off + n <= s->meta_off || off >= s->meta_end) { return; }
    uint32_t skip = off < s->meta_off ? s->meta_off - off : 0;
    uint32_t m = n - skip;
    if (off + skip + m > s->meta_end) { m = s->meta_end - (off + skip); }
    for (uint32_t i = 0; i < m; i++) { meta_byte(s, p[skip + i]); }
}

static int ro_fetch(const pex_install_t *s, uint32_t off, uint8_t *dst, uint32_t n) {
    while (n) {
        if (s->page_n && off >= s->page_off && off < s->page_off + s->page_n) {
            uint32_t k = s->page_off + s->page_n - off;
            if (k > n) { k = n; }
            memcpy(dst, s->page + (off - s->page_off), k);
            off += k;
            dst += k;
            n -= k;
            continue;
        }
        if (off >= s->hdr.file_size) { return PEX_EIO; }
        uint32_t sec = off / SEC, in = off % SEC, k = SEC - in;
        if (k > n) { k = n; }
        memcpy(dst, fptr((s->dst + sec) * SEC + in), k);
        off += k;
        dst += k;
        n -= k;
    }
    return PEX_OK;
}

static int ro_word(const pex_install_t *s, uint32_t i, uint32_t *v) {
    uint8_t b[4];
    if (ro_fetch(s, s->ro_off + 4 * i, b, 4)) { return PEX_EIO; }
    *v = rd32(b);
    return PEX_OK;
}

static int ro_prepare(pex_install_t *s) {
    s->ro_done = 1;
    const pex__sec_t *sr = pex__find_sec(&s->hdr, PEX_SEC_RELOC);
    if (!sr || sr->size < 8) { s->n_ro = 0; return PEX_OK; }
    uint8_t b[8];
    if (ro_fetch(s, sr->off, b, 8)) { return PEX_EIO; }
    s->n_ro = rd32(b);
    s->ro_off = sr->off + 8;
    if (s->n_ro > (sr->size - 8) / 4) { pex__err(PEX_EFORMAT, "RELOC truncated"); return PEX_EFORMAT; }
    if (s->n_ro && (s->hdr.flags & PEX_F_PIC)) {
        pex__err(PEX_ERELOC, "position-independent module with %lu RO relocations: refused (spec 6.2)",
                 (unsigned long)s->n_ro);
        return PEX_ERELOC;
    }
    uint32_t prev = 0;
    for (uint32_t i = 0; i < s->n_ro; i++) {
        uint32_t v;
        if (ro_word(s, i, &v)) { return PEX_EIO; }
        if (i && v < prev) { pex__err(PEX_EFORMAT, "RO relocations not sorted: re-link with pexlink"); return PEX_EFORMAT; }
        prev = v;
    }
    return PEX_OK;
}

static int relocate_page(pex_install_t *s, uint32_t valid) {
    if (!s->ro_done) {
        int rc = ro_prepare(s);
        if (rc) { return rc; }
    }
    if (!s->n_ro) { return PEX_OK; }
    for (uint32_t i = 0; i < s->n_ro; i++) {
        uint32_t v;
        if (ro_word(s, i, &v)) { return PEX_EIO; }
        uint32_t fo = s->hdr.image_off + (v & 0x7FFFFFFFu);
        if (fo + 4 <= s->page_off || fo >= s->page_off + valid) { continue; }
        if (fo < s->page_off || fo + 4 > s->page_off + valid) {
            pex__err(PEX_EFORMAT, "a relocation straddles a page: re-link with pexlink");
            return PEX_EFORMAT;
        }
        uint32_t base = (v & PEX_RELOC_TO_RW) ? s->rw_base : s->code_base;
        uint32_t x = rd32(s->page + (fo - s->page_off)) + base;
        memcpy(s->page + (fo - s->page_off), &x, 4);
    }
    return PEX_OK;
}

static int emit_page(pex_install_t *s, uint32_t valid) {
    if (!valid) { return PEX_OK; }
    if (valid < PAGE) { memset(s->page + valid, 0xFF, PAGE - valid); }
    uint32_t img_lo = s->hdr.image_off;
    uint32_t img_hi = img_lo + s->hdr.image_size;
    int overlaps = s->page_off < img_hi && s->page_off + valid > img_lo;
    if (overlaps && !s->ready) { pex__err(PEX_ESTATE, "image page before the module name was known"); return PEX_ESTATE; }
    /* Same words as write_sector: RO entries point into text/ro, and that
     * range is fully received before an image page is emitted. A header page
     * in the same sector is stored raw; the list is not read early. */
    uint32_t code_lo = s->hdr.image_off;
    uint32_t code_hi = code_lo + s->hdr.text + s->hdr.ro;
    int code_page = s->page_off < code_hi && s->page_off + valid > code_lo;
    if (code_page && (s->code_base || s->rw_base)) {
        int rc = relocate_page(s, valid);
        if (rc) { return rc; }
    }
    uint32_t sec = s->page_off / SEC;
    if (s->erased != (int)sec) {
        if (fl_erase((s->dst + sec) * SEC)) {
            pex__err(PEX_EIO, "flash write failed (sector %lu)", (unsigned long)(s->dst + sec));
            return PEX_EIO;
        }
        s->erased = (int)sec;
    }
    int blank = 1;
    for (uint32_t i = 0; i < PAGE; i++) {
        if (s->page[i] != 0xFF) { blank = 0; break; }
    }
    if (!blank && fl_prog((s->dst + sec) * SEC + (s->page_off % SEC), s->page, PAGE)) {
        pex__err(PEX_EIO, "flash write failed (sector %lu)", (unsigned long)(s->dst + sec));
        return PEX_EIO;
    }
    return PEX_OK;
}

static int sess_store(pex_install_t *s, const uint8_t *p, uint32_t n) {
    while (n) {
        if (s->page_n == 0) {
            s->page_off = s->got;
            memset(s->page, 0xFF, PAGE);
        }
        uint32_t filled = s->got - s->page_off;
        uint32_t space = PAGE - filled;
        uint32_t take = n < space ? n : space;
        memcpy(s->page + filled, p, take);
        s->page_n = filled + take;
        s->got += take;
        p += take;
        n -= take;
        if (s->page_n == PAGE) {
            int rc = emit_page(s, PAGE);
            if (rc) { return rc; }
            s->page_n = 0;
        }
    }
    return PEX_OK;
}

static int sess_reserve(pex_install_t *s) {
    const char *nm = s->name[0] ? s->name : "module";
    uint32_t nsec = (s->hdr.file_size + SEC - 1) / SEC;
    if (!nsec || s->hdr.file_size < PEX_HEADER_SIZE) { pex__err(PEX_EFORMAT, "file is too small"); return PEX_EFORMAT; }
    uint32_t ft = 0, lg = 0;
    int run = find_run(nsec, -1, &ft, &lg);
    if (run < 0 && !(s->flags & PEX_INSTALL_NO_COMPACT) && ft >= nsec) {
        pex__logf(PEX_LOG_INFO, "pex: xip: %s needs %lu sectors, largest free run %lu of %lu free: compacting", nm,
                  (unsigned long)nsec, (unsigned long)lg, (unsigned long)ft);
        s_nest++;
        int moved = pex_xip_compact();
        s_nest--;
        if (moved < 0) { return moved; }
        run = find_run(nsec, -1, &ft, &lg);
    }
    if (run < 0) {
        pex__err(PEX_ENOSPC, "%s needs %lu KB; the PEX partition has %lu KB free, %lu KB in one piece", nm,
                 (unsigned long)(nsec * 4), (unsigned long)(ft * 4), (unsigned long)(lg * 4));
        return PEX_ENOSPC;
    }
    s->dst = (uint32_t)run;
    s->nsec = nsec;
    return PEX_OK;
}

static int sess_decide(pex_install_t *s) {
    s->decided = 1;
    if (!s->name_set || !s->name[0]) {
        memset(s->name, 0, sizeof s->name);
        memcpy(s->name, "module", 6);
    }
    if (s->maybe_same) {
        for (int i = 0; i < s_ix->h.n; i++) {
            if (same_build(&s_ix->e[i], s->hdr.build_id, s->hdr.crc, s->hdr.sig >= 0) &&
                strncmp(s_ix->e[i].name, s->name, sizeof s_ix->e[i].name) == 0) {
                s->same_hit = i;
                s->phase = PH_EARLY;
                return PEX_DONE;
            }
        }
        pex__err(PEX_EFORMAT, "file_crc32 matches an installed build but the name is %s", s->name);
        return sess_fail(s, PEX_EFORMAT);
    }
    s->old = find_name(s->name);
    if (s->old >= 0 && in_use(s->old)) {
        pex__err(PEX_EBUSY, "%s runs from its installed copy: unload it first", s->name);
        return sess_fail(s, PEX_EBUSY);
    }
    if (s->old < 0 && s_ix->h.n >= IX_MAX) {
        pex__err(PEX_ENOSPC, "the PEX index is full (%d modules)", IX_MAX);
        return sess_fail(s, PEX_ENOSPC);
    }
    if (!(s->hdr.flags & PEX_F_PIC)) {
        uint32_t rw_size = s->hdr.data + s->hdr.bss;
        s->aoff = assign_aoff(rw_size, s->old);
        s->rw_base = (uint32_t)(uintptr_t)(pex__pool_end() - s->aoff - rwsz(rw_size));
        s->code_base = (uint32_t)(F->exec_base + s->dst * SEC + s->hdr.image_off);
        uint32_t need = area_need_replacing(s->old, s->aoff, rw_size);
        s->area_saved = s_area_size;
        uint32_t gap = pex__pool_gap();
        if (area_ensure(need) != PEX_OK) {
            uint32_t more = need > s->area_saved ? need - s->area_saved : need;
            pex__err(PEX_ENOMEM, "this module needs %lu more bytes of RW reservation; the pool has %lu free below it",
                     (unsigned long)more, (unsigned long)gap);
            return sess_fail(s, PEX_ENOMEM);
        }
        s->area_set = 1;
    }
    s->ready = 1;
    return PEX_OK;
}

static int payload_before_image(pex_install_t *s, uint16_t type, const char *what) {
    const pex__sec_t *sec = pex__find_sec(&s->hdr, type);
    if (!sec) { return PEX_OK; }
    /* off past the image must be rejected before the subtraction underflows. */
    if (sec->off < s->prefix_need || sec->off > s->hdr.image_off ||
        sec->size > s->hdr.image_off - sec->off) {
        pex__err(PEX_EFORMAT, "%s is not before the image: re-link with pexlink", what);
        return PEX_EFORMAT;
    }
    return PEX_OK;
}

typedef struct { const uint8_t *p; uint32_t n; } pref_t;
static int pref_read(void *ctx, uint32_t off, void *dst, uint32_t n) {
    pref_t *b = ctx;
    if (n > b->n || off > b->n - n) { return -1; }
    memcpy(dst, b->p + off, n);
    return (int)n;
}

static int header_ready(pex_install_t *s) {
    pref_t b = { s->prefix, s->prefix_got };
    pex_reader_t rd = { pref_read, &b, 0 };
    int rc = pex__parse_header(&rd, &s->hdr);
    if (rc) { return sess_fail(s, rc); }
    if (s->hdr.image_off < s->prefix_need) {
        pex__err(PEX_EFORMAT, "the image overlaps the section table: re-link with pexlink");
        return sess_fail(s, PEX_EFORMAT);
    }
    if ((rc = payload_before_image(s, PEX_SEC_RELOC, "RELOC")) || (rc = payload_before_image(s, PEX_SEC_META, "META"))) {
        return sess_fail(s, rc);
    }
    if (s->hdr.flags & PEX_F_RAM_ONLY) {
        pex__err(PEX_ENOTSUP, "RAM_ONLY: not installable");
        return sess_fail(s, PEX_ENOTSUP);
    }
    const pex__sec_t *sm = pex__find_sec(&s->hdr, PEX_SEC_META);
    if (sm) {
        s->have_meta = 1;
        s->meta_off = sm->off;
        s->meta_end = sm->off + sm->size;
    } else {
        s->meta_end = s->prefix_need;
    }
    s->maybe_same = 0;
    for (int i = 0; i < s_ix->h.n; i++) {
        if (same_build(&s_ix->e[i], s->hdr.build_id, s->hdr.crc, s->hdr.sig >= 0)) { s->maybe_same = 1; break; }
    }
    s->phase = PH_BODY;
    if (s->maybe_same) {
        s->got = s->prefix_got;
        meta_feed(s, s->prefix, s->prefix_got, 0);
        if (s->got >= s->meta_end) { return sess_decide(s); }
        return PEX_OK;
    }
    rc = sess_reserve(s);
    if (rc) { return sess_fail(s, rc); }
    pex_sha256_init(&s->sha);
    hash_bytes(s, s->prefix, s->prefix_got, 0);
    rc = sess_store(s, s->prefix, s->prefix_got);
    if (rc) { return sess_fail(s, rc); }
    meta_feed(s, s->prefix, s->prefix_got, 0);
    if (s->got >= s->meta_end) { return sess_decide(s); }
    return PEX_OK;
}

static int hdr_absorb(pex_install_t *s, const uint8_t *p, uint32_t n, uint32_t *used) {
    *used = 0;
    if (s->prefix_need == 0) {
        uint32_t want = PEX_HEADER_SIZE;
        if (s->prefix_got < want) {
            uint32_t take = want - s->prefix_got;
            if (take > n) { take = n; }
            memcpy(s->prefix + s->prefix_got, p, take);
            s->prefix_got += take;
            *used = take;
            if (s->prefix_got < want) { return PEX_OK; }
            p += take;
            n -= take;
        }
        if (rd32(s->prefix) != PEX_MAGIC) { pex__err(PEX_EFORMAT, "not a .pex (bad magic)"); return sess_fail(s, PEX_EFORMAT); }
        uint16_t hsize = rd16(s->prefix + 8);
        if (hsize != PEX_HEADER_SIZE) {
            pex__err(PEX_EFORMAT, "header is %u bytes: re-link with pexlink", hsize);
            return sess_fail(s, PEX_EFORMAT);
        }
        uint32_t nsec = rd32(s->prefix + 60), soff = rd32(s->prefix + 64);
        if (soff != hsize) {
            pex__err(PEX_EFORMAT, "section table is not right after the header: re-link with pexlink");
            return sess_fail(s, PEX_EFORMAT);
        }
        if (nsec > PEX_MAX_SECTIONS) { pex__err(PEX_EFORMAT, "too many sections"); return sess_fail(s, PEX_EFORMAT); }
        uint32_t need = soff + nsec * 12u;
        if (need < soff || need > sizeof s->prefix) {
            pex__err(PEX_EFORMAT, "section table does not fit");
            return sess_fail(s, PEX_EFORMAT);
        }
        s->prefix_need = need;
    }
    if (s->prefix_got < s->prefix_need) {
        uint32_t take = s->prefix_need - s->prefix_got;
        if (take > n) { take = n; }
        memcpy(s->prefix + s->prefix_got, p, take);
        s->prefix_got += take;
        *used += take;
        if (s->prefix_got < s->prefix_need) { return PEX_OK; }
    }
    return header_ready(s);
}

static int finish_page(pex_install_t *s) {
    if (!s->page_n) { return PEX_OK; }
    int rc = emit_page(s, s->page_n);
    s->page_n = 0;
    return rc;
}

static int check_stream(pex_install_t *s) {
    pex_sha256_final(&s->sha, s->digest);
    if (s->crc_acc != s->hdr.crc) {
        pex__err(PEX_ECRC, "CRC 0x%08lx, header says 0x%08lx: the file is corrupted", (unsigned long)s->crc_acc,
                 (unsigned long)s->hdr.crc);
        return PEX_ECRC;
    }
    if (s->hdr.sig >= 0) {
        const pex__sec_t *sg = &s->hdr.sec[s->hdr.sig];
        if (sg->size != sizeof s->sigb || s->sig_have != sizeof s->sigb) {
            pex__err(PEX_ESIG, "malformed SIG section");
            return PEX_ESIG;
        }
        if (!pex__cfg.verify) { pex__err(PEX_ESIG, "signed, but this host has no signature verifier"); return PEX_ESIG; }
        int rc = pex__cfg.verify(rd16(s->sigb), rd16(s->sigb + 2), s->digest, s->sigb + 4, 64);
        if (rc) {
            pex__err(PEX_ESIG, "signature does not verify (alg %u, key %u)", rd16(s->sigb), rd16(s->sigb + 2));
            return PEX_ESIG;
        }
    } else {
        if (s->hdr.flags & PEX_F_NEEDS_SIG) { pex__err(PEX_EUNSIGNED, "module requires a signature and has none"); return PEX_EUNSIGNED; }
        if (!(pex__cfg.policy & PEX_POLICY_ALLOW_UNSIGNED)) {
            pex__err(PEX_EUNSIGNED, "unsigned, and this host loads signed modules only");
            return PEX_EUNSIGNED;
        }
    }
    return PEX_OK;
}

/* The flash copy, un-relocated, must hash to the stream. No second signature check. */
static int flash_matches(pex_install_t *s, const ix_ent_t *ne) {
    xf_t x;
    xf_of(&x, ne);
    if (xf_setup(&x) || x.file_size != ne->file_size) {
        pex__err(PEX_EIO, "%s: the installed copy does not read back", s->name);
        return PEX_EIO;
    }
    uint32_t crc = 0;
    pex_sha256_t sh;
    pex_sha256_init(&sh);
    uint8_t buf[128];
    for (uint32_t off = 0; off < s->hdr.sig_off;) {
        uint32_t n = s->hdr.sig_off - off;
        if (n > sizeof buf) { n = (uint32_t)sizeof buf; }
        if (xf_read(&x, off, buf, n) != (int)n) {
            pex__err(PEX_EIO, "%s: the installed copy does not read back", s->name);
            return PEX_EIO;
        }
        pex_sha256_update(&sh, buf, n);
        for (uint32_t i = 0; i < 4; i++) {
            if (off <= 72u + i && 72u + i < off + n) { buf[72u + i - off] = 0; }
        }
        crc = pex_crc32_update(crc, buf, n);
        off += n;
    }
    uint8_t dig[32];
    pex_sha256_final(&sh, dig);
    if (crc != s->crc_acc || memcmp(dig, s->digest, 32) != 0) {
        pex__err(PEX_ECRC, "%s: the installed copy does not match what was written", s->name);
        return PEX_ECRC;
    }
    /* SIG is outside the hash. Compare the bytes; do not verify a second time. */
    if (s->hdr.sig >= 0) {
        const pex__sec_t *sg = &s->hdr.sec[s->hdr.sig];
        uint8_t b[68];
        if (sg->size != sizeof b || ro_fetch(s, sg->off, b, sizeof b) || memcmp(b, s->sigb, sizeof b) != 0) {
            pex__err(PEX_ECRC, "%s: the installed copy does not match what was written", s->name);
            return PEX_ECRC;
        }
    }
    return PEX_OK;
}

pex_install_t *pex_install_begin(uint32_t flags) {
    pex__err(PEX_OK, "ok");
    if (!s_mounted) { pex__err(PEX_ENOTSUP, "this host has no PEX flash partition"); return NULL; }
    if (index_locked()) { return NULL; }
    pex_install_t *s = pex__alloc(sizeof *s, 8, PEX_OWNER_HOST, 1);
    if (!s) { pex__err(PEX_ENOMEM, "PEX pool full (install)"); return NULL; }
    memset(s, 0, sizeof *s);
    s->flags = flags;
    s->old = -1;
    s->erased = -1;
    s->same_hit = -1;
    s_open = s;
    return s;
}

int pex_install_write(pex_install_t *s, const void *pv, uint32_t n) {
    if (!s || s != s_open) { pex__err(PEX_EINVAL, "no install session"); return PEX_EINVAL; }
    if (s->phase == PH_EARLY) { return PEX_DONE; }
    if (s->phase == PH_FAIL) { return s->err; }
    if (!n) { return PEX_OK; }
    if (!pv) { pex__err(PEX_EINVAL, "no install bytes"); return sess_fail(s, PEX_EINVAL); }
    const uint8_t *p = pv;
    while (n && s->phase == PH_HDR) {
        uint32_t used = 0;
        hdr_absorb(s, p, n, &used);
        p += used;
        n -= used;
        if (s->phase != PH_HDR) { break; }
        if (!used) { break; }
    }
    if (s->phase == PH_EARLY) { return PEX_DONE; }
    if (s->phase == PH_FAIL) { return s->err; }
    while (n && s->phase == PH_BODY) {
        if (s->got > s->hdr.file_size) {
            pex__err(PEX_EFORMAT, "the file is longer than its header says");
            return sess_fail(s, PEX_EFORMAT);
        }
        if (s->got == s->hdr.file_size) {
            pex__err(PEX_EFORMAT, "the file is longer than its header says");
            return sess_fail(s, PEX_EFORMAT);
        }
        uint32_t lim = n;
        if (lim > s->hdr.file_size - s->got) { lim = s->hdr.file_size - s->got; }
        if (!s->decided && s->got < s->meta_end && s->got + lim > s->meta_end) { lim = s->meta_end - s->got; }
        int rc;
        if (s->maybe_same && !s->decided) {
            meta_feed(s, p, lim, s->got);
            s->got += lim;
            rc = PEX_OK;
        } else {
            hash_bytes(s, p, lim, s->got);
            meta_feed(s, p, lim, s->got);
            rc = sess_store(s, p, lim);
        }
        if (rc) { return sess_fail(s, rc); }
        p += lim;
        n -= lim;
        if (!s->decided && s->got >= s->meta_end) {
            rc = sess_decide(s);
            if (rc) { return rc; }
        }
    }
    return PEX_OK;
}

int pex_install_end(pex_install_t *s) {
    if (!s || s != s_open) { pex__err(PEX_EINVAL, "no install session"); return PEX_EINVAL; }
    if (s->phase == PH_EARLY) {
        int slot = s->same_hit;
        pex__logf(PEX_LOG_INFO, "pex: xip: %s (this build) is already installed", s->name);
        sess_close(s, 0);
        return slot;
    }
    if (s->phase == PH_FAIL) {
        int rc = s->err ? s->err : PEX_EIO;
        pex__logf(PEX_LOG_WARN, "pex: xip: install refused: %s (%s)", pex_errmsg(), pex_strerror(rc));
        sess_close(s, 1);
        return rc;
    }
    int rc = PEX_OK;
    if (s->phase != PH_BODY || s->got != s->hdr.file_size) {
        pex__err(PEX_EIO, "short read at %lu of %lu", (unsigned long)s->got, (unsigned long)s->hdr.file_size);
        rc = PEX_EIO;
    } else if (!s->decided || !s->ready) {
        pex__err(PEX_EFORMAT, "the stream ended before META");
        rc = PEX_EFORMAT;
    } else if ((rc = finish_page(s)) == PEX_OK && (rc = check_stream(s)) == PEX_OK) {
        int a = !(s->hdr.flags & PEX_F_PIC);
        ix_ent_t ne;
        memset(&ne, 0, sizeof ne);
        memcpy(ne.build_id, s->hdr.build_id, 16);
        memcpy(ne.name, s->name, sizeof ne.name);
        ne.sec = s->dst;
        ne.file_size = s->hdr.file_size;
        ne.version = s->hdr.version;
        ne.flags = s->hdr.flags;
        ne.target = s->hdr.target;
        ne.image_off = s->hdr.image_off;
        ne.code_size = s->hdr.text + s->hdr.ro;
        ne.rw_size = s->hdr.data + s->hdr.bss;
        ne.code_base = s->code_base;
        ne.rw_base = s->rw_base;
        ne.aoff = s->aoff;
        ne.state = (s->hdr.sig >= 0 ? ES_SIGNED : 0) | ES_VERIFIED;
        rc = flash_matches(s, &ne);
        if (!rc) {
            if (s->old >= 0) {
                memmove(&s_ix->e[s->old], &s_ix->e[s->old + 1], (s_ix->h.n - s->old - 1) * sizeof(ix_ent_t));
                s_ix->h.n--;
            }
            int slot = s_ix->h.n;
            s_ix->e[s_ix->h.n++] = ne;
            rc = commit();
            if (!rc) {
                pex__logf(PEX_LOG_INFO, "pex: xip: installed %s v%lu.%lu.%lu (model %c, %lu bytes) at 0x%05lx%s%s", s->name,
                          (unsigned long)(ne.version >> 24), (unsigned long)((ne.version >> 16) & 0xFF),
                          (unsigned long)(ne.version & 0xFFFF), a ? 'A' : 'B', (unsigned long)ne.file_size,
                          (unsigned long)(s->dst * SEC), s->old >= 0 ? ", replacing the previous version" : "",
                          a && !area_covers(&ne) ? " (in place after the next boot: RW address not reserved yet)" : "");
                sess_close(s, 0);
                return slot;
            }
        }
    }
    pex__logf(PEX_LOG_WARN, "pex: xip: install refused: %s (%s)", pex_errmsg(), pex_strerror(rc));
    sess_close(s, 1);
    return rc;
}

void pex_install_abort(pex_install_t *s) {
    if (!s || s != s_open) { return; }
    sess_close(s, 1);
}

/* ---- install ------------------------------------------------------------------------ */
int pex_install(const pex_reader_t *rd, uint32_t flags) {
    pex__err(PEX_OK, "ok");
    if (!s_mounted) { pex__err(PEX_ENOTSUP, "this host has no PEX flash partition"); return PEX_ENOTSUP; }
    if (index_locked()) { return PEX_EBUSY; }
    if (!rd || !rd->read) { pex__err(PEX_EINVAL, "no reader"); return PEX_EINVAL; }
    pex_arena_t *tmp = pex__scratch_begin(sizeof(pex__hdr_t), PEX_OWNER_HOST);
    pex__hdr_t *h = tmp ? pex__scratch_alloc(tmp, sizeof *h) : NULL;
    if (!h) {
        if (tmp) { pex__scratch_end(tmp); }
        pex__err(PEX_ENOMEM, "PEX pool full (install)");
        return PEX_ENOMEM;
    }
    int rc = pex__parse_header(rd, h);
    if (!rc) { rc = pex__integrity(rd, h, false); }
    char name[24];
    memset(name, 0, sizeof name);
    if (!rc) {
        pex__meta_name(rd, h, name);
        if (!name[0]) { strcpy(name, "module"); }
        if (h->flags & PEX_F_RAM_ONLY) { pex__err(PEX_ENOTSUP, "%s is RAM_ONLY: not installable", name); rc = PEX_ENOTSUP; }
    }
    int old = -1, slot = -1;
    uint32_t nsec = 0, dst = 0;
    bool a = false;
    if (!rc) {
        for (int i = 0; i < s_ix->h.n; i++) {
            if (same_build(&s_ix->e[i], h->build_id, h->crc, h->sig >= 0) &&
                strncmp(s_ix->e[i].name, name, sizeof s_ix->e[i].name) == 0) {
                pex__logf(PEX_LOG_INFO, "pex: xip: %s (this build) is already installed", name);
                pex__scratch_end(tmp);
                return i;
            }
        }
        old = find_name(name);
        if (old >= 0 && in_use(old)) {
            pex__err(PEX_EBUSY, "%s runs from its installed copy: unload it first", name);
            rc = PEX_EBUSY;
        } else if (old < 0 && s_ix->h.n >= IX_MAX) {
            pex__err(PEX_ENOSPC, "the PEX index is full (%d modules)", IX_MAX);
            rc = PEX_ENOSPC;
        }
    }
    a = !rc && !(h->flags & PEX_F_PIC);
    uint32_t ro_off = 0, n_ro = 0, split = 0;
    const pex__sec_t *sr = rc ? NULL : pex__find_sec(h, PEX_SEC_RELOC);
    if (sr && sr->size >= 8) {
        uint8_t b[8];
        if (pex__rread(rd, sr->off, b, 8)) { rc = PEX_EIO; }
        n_ro = rd32(b);
        ro_off = sr->off + 8;
        if (!rc && n_ro > (sr->size - 8) / 4) { pex__err(PEX_EFORMAT, "RELOC truncated"); rc = PEX_EFORMAT; }
        if (!rc && n_ro && (h->flags & PEX_F_PIC)) {
            pex__err(PEX_ERELOC, "position-independent module with %lu RO relocations: refused (spec 6.2)", (unsigned long)n_ro);
            rc = PEX_ERELOC;
        }
        /* the RO list must be sorted (as pexlink writes it): moves and verification search it */
        uint32_t prev = 0;
        for (uint32_t i = 0; !rc && i < n_ro; i++) {
            uint8_t c[4];
            if (pex__rread(rd, ro_off + 4 * i, c, 4)) { rc = PEX_EIO; break; }
            uint32_t v = rd32(c);
            if (i && v < prev) { pex__err(PEX_EFORMAT, "RO relocations not sorted: re-link with pexlink"); rc = PEX_EFORMAT; }
            if (!(v & PEX_RELOC_TO_RW)) { split = i + 1; }
            prev = v;
        }
    }
    if (!rc) {
        nsec = (h->file_size + SEC - 1) / SEC;
        uint32_t ft = 0, lg = 0;
        int run = find_run(nsec, -1, &ft, &lg);
        if (run < 0 && !(flags & PEX_INSTALL_NO_COMPACT) && ft >= nsec) {
            pex__logf(PEX_LOG_INFO, "pex: xip: %s needs %lu sectors, largest free run %lu of %lu free: compacting", name,
                      (unsigned long)nsec, (unsigned long)lg, (unsigned long)ft);
            int moved = pex_xip_compact();
            if (moved < 0) { rc = moved; }
            run = rc ? -1 : find_run(nsec, -1, &ft, &lg);
        }
        if (!rc && run < 0) {
            pex__err(PEX_ENOSPC, "%s needs %lu KB; the PEX partition has %lu KB free, %lu KB in one piece%s", name,
                     (unsigned long)(nsec * 4), (unsigned long)(ft * 4), (unsigned long)(lg * 4),
                     old >= 0 ? " (the old copy stays until the new one is in)" : "");
            rc = PEX_ENOSPC;
        }
        dst = (uint32_t)run;
    }
    uint32_t area_saved = s_area_size;
    int area_set = 0;
    uint32_t code_base = 0, rw_base = 0, aoff = 0, rw_size = rc ? 0 : h->data + h->bss;
    if (!rc && a) {
        code_base = (uint32_t)(F->exec_base + dst * SEC + h->image_off);
        aoff = assign_aoff(rw_size, old);
        rw_base = (uint32_t)(uintptr_t)(pex__pool_end() - aoff - rwsz(rw_size));
        /* Before any sector write: a failed install must not have moved the suffix. */
        uint32_t need = area_need_replacing(old, aoff, rw_size);
        uint32_t gap = pex__pool_gap();
        if (area_ensure(need) != PEX_OK) {
            uint32_t more = need > area_saved ? need - area_saved : need;
            pex__err(PEX_ENOMEM, "this module needs %lu more bytes of RW reservation; the pool has %lu free below it",
                     (unsigned long)more, (unsigned long)gap);
            rc = PEX_ENOMEM;
        } else {
            area_set = 1;
        }
    }
    if (!rc) {
        rorr_t rr = { rd, ro_off };
        wr_t w = { .src = rd, .file_size = h->file_size, .image_off = h->image_off, .code_size = h->text + h->ro,
                   .n_ro = n_ro, .split = split, .code_base = code_base, .rw_base = rw_base,
                   .ro_at = ro_from_reader, .ro_ctx = &rr };
        for (uint32_t k = 0; k < nsec && !rc; k++) {
            if (write_sector(&w, dst + k, k)) { pex__err(PEX_EIO, "flash write failed (sector %lu)", (unsigned long)(dst + k)); rc = PEX_EIO; }
        }
    }
    if (!rc) {
        /* verify in place: the bytes in flash give back the signed file */
        ix_ent_t ne;
        memset(&ne, 0, sizeof ne);
        memcpy(ne.build_id, h->build_id, 16);
        memcpy(ne.name, name, sizeof ne.name);
        ne.sec = dst;
        ne.file_size = h->file_size;
        ne.version = h->version;
        ne.flags = h->flags;
        ne.target = h->target;
        ne.image_off = h->image_off;
        ne.code_size = h->text + h->ro;
        ne.rw_size = rw_size;
        ne.code_base = code_base;
        ne.rw_base = rw_base;
        ne.aoff = aoff;
        ne.state = (h->sig >= 0 ? ES_SIGNED : 0) | ES_VERIFIED;
        xf_t x;
        xf_of(&x, &ne);
        pex__hdr_t *h2 = h;           /* reuse: the source header is no longer needed */
        uint8_t bid[16];
        memcpy(bid, h->build_id, 16);
        if (xf_setup(&x) || x.file_size != ne.file_size) {
            pex__err(PEX_EIO, "%s: the installed copy does not read back", name);
            rc = PEX_EIO;
        } else {
            pex_reader_t xr = { xf_read, &x, x.file_size };
            rc = pex__parse_header(&xr, h2);
            if (!rc) { rc = pex__integrity(&xr, h2, false); }
            if (!rc && memcmp(h2->build_id, bid, 16) != 0) { rc = PEX_ECRC; }
            if (rc) { pex__err(rc, "%s: verification of the installed copy failed (%s)", name, pex_strerror(rc)); }
        }
        if (!rc) {
            if (old >= 0) {
                memmove(&s_ix->e[old], &s_ix->e[old + 1], (s_ix->h.n - old - 1) * sizeof(ix_ent_t));
                s_ix->h.n--;
            }
            slot = s_ix->h.n;
            s_ix->e[s_ix->h.n++] = ne;
            rc = commit();
        }
        if (!rc) {
            pex__logf(PEX_LOG_INFO, "pex: xip: installed %s v%lu.%lu.%lu (model %c, %lu bytes) at 0x%05lx%s%s", name,
                      (unsigned long)(ne.version >> 24), (unsigned long)((ne.version >> 16) & 0xFF),
                      (unsigned long)(ne.version & 0xFFFF), a ? 'A' : 'B', (unsigned long)ne.file_size,
                      (unsigned long)(dst * SEC), old >= 0 ? ", replacing the previous version" : "",
                      a && !area_covers(&ne) ? " (in place after the next boot: RW address not reserved yet)" : "");
        }
    }
    pex__scratch_end(tmp);
    if (rc && area_set && area_ensure(area_saved) != PEX_OK) {
        pex__logf(PEX_LOG_ERR, "pex: xip: could not restore the %lu-byte RW reservation", (unsigned long)area_saved);
    }
    if (rc) {
        pex__logf(PEX_LOG_WARN, "pex: xip: install refused: %s (%s)", pex_errmsg(), pex_strerror(rc));
        return rc;
    }
    return slot;
}

typedef struct { const uint8_t *p; uint32_t n; } membuf_t;
static int mem_read(void *ctx, uint32_t off, void *dst, uint32_t n) {
    membuf_t *b = ctx;
    if (off > b->n || n > b->n - off) { return -1; }
    memcpy(dst, b->p + off, n);
    return (int)n;
}

int pex_install_mem(const void *buf, uint32_t len, uint32_t flags) {
    membuf_t b = { buf, len };
    pex_reader_t rd = { mem_read, &b, len };
    return pex_install(&rd, flags);
}

int pex_uninstall(const char *name) {
    pex__err(PEX_OK, "ok");
    if (!s_mounted) { pex__err(PEX_ENOTSUP, "this host has no PEX flash partition"); return PEX_ENOTSUP; }
    if (index_locked()) { return PEX_EBUSY; }
    int i = name ? find_name(name) : -1;
    if (i < 0) { pex__err(PEX_ENOENT, "%s is not installed", name ? name : "?"); return PEX_ENOENT; }
    if (in_use(i)) { pex__err(PEX_EBUSY, "%s runs from its installed copy: unload it first", name); return PEX_EBUSY; }
    memmove(&s_ix->e[i], &s_ix->e[i + 1], (s_ix->h.n - i - 1) * sizeof(ix_ent_t));
    s_ix->h.n--;
    int rc = commit();
    if (!rc) {
        /* After the index commit: a crash before this still remounts the old suffix.
         * A hole in the middle stays; only the unused tail comes back. */
        uint32_t need = area_need(s_ix);
        if (area_ensure(need) != PEX_OK) {
            pex__logf(PEX_LOG_WARN, "pex: xip: uninstalled %s, but %lu bytes of RW reservation stay until the next boot",
                      name, (unsigned long)(s_area_size - need));
        }
        pex__logf(PEX_LOG_INFO, "pex: xip: uninstalled %s", name);
    }
    return rc;
}

/* ---- loading from flash ---------------------------------------------------------------- */
pex_mod_t *pex_load_installed(const char *name, const pex_load_opts_t *opts, int *errp) {
    int e_ = 0;
    int *err = errp ? errp : &e_;
    pex__err(PEX_OK, "ok");
    if (!s_mounted) { pex__err(PEX_ENOTSUP, "this host has no PEX flash partition"); *err = PEX_ENOTSUP; return NULL; }
    int i = name ? find_name(name) : -1;
    if (i < 0) { pex__err(PEX_ENOENT, "%s is not installed", name ? name : "?"); *err = PEX_ENOENT; return NULL; }
    pex__xload_t xl = { 0 };
    bool ram = false;
    const char *why = "";
    if (model_a(&s_ix->e[i])) {
        if (in_use(i)) {
            ram = true;
            why = "its flash copy is bound to the RW of the instance already running";
        } else if (!area_covers(&s_ix->e[i])) {
            ram = true;
            why = "its RW address is reserved from the next boot on";
        } else if (s_ix->e[i].code_base != code_addr(&s_ix->e[i]) || s_ix->e[i].rw_base != rw_want(&s_ix->e[i])) {
            /* the pool or the partition moved (a new firmware): relocate a fresh copy */
            uint32_t want = rw_want(&s_ix->e[i]);
            uint8_t map[MAX_SECTORS / 8];
            used_map(map, i);
            /* a run that does not overlap the copy itself */
            int dst = -1;
            uint32_t n = nsec_of(&s_ix->e[i]), src = s_ix->e[i].sec;
            for (uint32_t s = IX_SECTORS; s + n <= nsec_total() && dst < 0; s++) {
                bool ok = !(s < src + n && src < s + n);
                for (uint32_t k = 0; k < n && ok; k++) { ok = !is_used(map, s + k); }
                if (ok) { dst = (int)s; }
            }
            if (dst < 0 || move_entry(i, (uint32_t)dst, want, s_ix->e[i].aoff, 0, false) != PEX_OK) {
                ram = true;
                why = "it must be relocated for this firmware and there is no room to do it";
            }
        }
        if (!ram) { xl.rw = (uintptr_t)rw_ptr(&s_ix->e[i]); }
    }
    const ix_ent_t *e = &s_ix->e[i];
    xl.code = code_addr(e);
    xl.skip_sig = (e->state & ES_VERIFIED) != 0;
    xf_t x;
    xf_of(&x, e);
    if (xf_setup(&x)) { pex__err(PEX_EFORMAT, "the installed copy of %s is unreadable", e->name); *err = PEX_EFORMAT; return NULL; }
    pex_reader_t rd = { xf_read, &x, x.file_size };
    if (ram) {
        pex__logf(PEX_LOG_WARN, "pex: xip: %s loads into RAM: %s", e->name, why);
    } else {
        pex__xload = &xl;
    }
    pex_load_opts_t o = { 0 };
    if (opts) { o = *opts; }
    if (!o.name) { o.name = e->name; }
    pex_mod_t *m = pex_load(&rd, &o, err);
    pex__xload = NULL;
    return m;
}

int pex_xip_verify(const char *name) {
    pex__err(PEX_OK, "ok");
    if (!s_mounted) { pex__err(PEX_ENOTSUP, "this host has no PEX flash partition"); return PEX_ENOTSUP; }
    int i = name ? find_name(name) : -1;
    if (i < 0) { pex__err(PEX_ENOENT, "%s is not installed", name ? name : "?"); return PEX_ENOENT; }
    xf_t x;
    xf_of(&x, &s_ix->e[i]);
    if (xf_setup(&x)) { pex__err(PEX_EFORMAT, "unreadable"); return PEX_EFORMAT; }
    pex_reader_t rd = { xf_read, &x, x.file_size };
    pex_arena_t *a = pex__scratch_begin(sizeof(pex__hdr_t), PEX_OWNER_HOST);
    pex__hdr_t *h = a ? pex__scratch_alloc(a, sizeof *h) : NULL;
    if (!h) { if (a) { pex__scratch_end(a); } return PEX_ENOMEM; }
    int rc = pex__parse_header(&rd, h);
    if (!rc) { rc = pex__integrity(&rd, h, false); }
    if (!rc && memcmp(h->build_id, s_ix->e[i].build_id, 16) != 0) { pex__err(PEX_ECRC, "build id differs"); rc = PEX_ECRC; }
    pex__scratch_end(a);
    return rc;
}

/* ---- introspection --------------------------------------------------------------------- */
static void fill(int i, pex_installed_t *o) {
    const ix_ent_t *e = &s_ix->e[i];
    memset(o, 0, sizeof *o);
    memcpy(o->name, e->name, sizeof o->name);
    o->name[sizeof o->name - 1] = 0;
    memcpy(o->build_id, e->build_id, 16);
    o->version = e->version;
    o->flags = e->flags;
    o->target = e->target;
    o->offset = e->sec * SEC;
    o->file_size = e->file_size;
    o->code_size = e->code_size;
    o->rw_size = e->rw_size;
    /* Where the installed copy executes. Model A stores this same address as
     * code_base. Model B leaves code_base at 0; the bytes still run here. */
    o->code = code_addr(e);
    o->signed_ = (e->state & ES_SIGNED) != 0;
    o->loaded = in_use(i);
    o->xip_ready = !model_a(e) || area_covers(e);
}

int pex_installed(int i, pex_installed_t *out) {
    if (!s_mounted || i < 0 || i >= s_ix->h.n) { return PEX_ENOENT; }
    if (out) { fill(i, out); }
    return PEX_OK;
}

int pex_installed_find(const char *name, pex_installed_t *out) {
    if (!s_mounted || !name) { return PEX_ENOENT; }
    int i = find_name(name);
    if (i < 0) { return PEX_ENOENT; }
    if (out) { fill(i, out); }
    return i;
}

int pex_xip_stats(pex_xip_stats_t *o) {
    memset(o, 0, sizeof *o);
    if (!s_mounted) { return PEX_ENOTSUP; }
    o->mounted = true;
    o->size = F->size;
    o->data_size = F->size - IX_SECTORS * SEC;
    uint32_t ft = 0, lg = 0;
    find_run(0xFFFFFFFFu, -1, &ft, &lg);
    o->free = ft * SEC;
    o->largest = lg * SEC;
    o->used = o->data_size - o->free;
    o->entries = s_ix->h.n;
    o->seq = s_ix->h.seq;
    o->moves = s_moves;
    o->recovered = s_recovered;
    o->rw_area = s_area ? s_area_size : 0;
    o->rw_room = pex__pool_gap();
    return PEX_OK;
}

#define APPEND(...) do { int _k = snprintf(dst + n, n < cap ? cap - n : 0, __VA_ARGS__); if (_k > 0) { n += (size_t)_k; } } while (0)

int pex_installed_json(char *dst, size_t cap) {
    size_t n = 0;
    pex_xip_stats_t st;
    pex_xip_stats(&st);
    APPEND("{\"mounted\":%s,\"size\":%lu,\"free\":%lu,\"largest\":%lu,\"seq\":%lu,\"moves\":%lu,\"recovered\":%lu,"
           "\"rw_area\":%lu,\"rw_room\":%lu,\"modules\":[", st.mounted ? "true" : "false", (unsigned long)st.size,
           (unsigned long)st.free, (unsigned long)st.largest, (unsigned long)st.seq, (unsigned long)st.moves,
           (unsigned long)st.recovered, (unsigned long)st.rw_area, (unsigned long)st.rw_room);
    for (int i = 0; s_mounted && i < s_ix->h.n; i++) {
        pex_installed_t in;
        fill(i, &in);
        APPEND("%s{\"name\":\"%s\",\"version\":\"%lu.%lu.%lu\",\"build_id\":\"", i ? "," : "", in.name,
               (unsigned long)(in.version >> 24), (unsigned long)((in.version >> 16) & 0xFF),
               (unsigned long)(in.version & 0xFFFF));
        for (int k = 0; k < 8; k++) { APPEND("%02x", in.build_id[k]); }
        APPEND("\",\"model\":\"%s\",\"offset\":%lu,\"code_addr\":%lu,\"size\":%lu,\"code\":%lu,\"rw\":%lu,\"signed\":%s,\"loaded\":%s,"
               "\"xip_ready\":%s}", (in.flags & PEX_F_PIC) ? "pic" : "reloc", (unsigned long)in.offset,
               (unsigned long)in.code, (unsigned long)in.file_size, (unsigned long)in.code_size, (unsigned long)in.rw_size,
               in.signed_ ? "true" : "false", in.loaded ? "true" : "false", in.xip_ready ? "true" : "false");
    }
    APPEND("]}");
    return n < cap ? (int)n : -1;
}

/* ---- mount ------------------------------------------------------------------------------ */
static int mount(void) {
    s_mounted = false;
    s_cur = -1;
    s_moves = s_recovered = 0;
    if (!F || F->size < 4 * SEC || (F->size % SEC) || F->size / SEC > MAX_SECTORS || !F->erase || !F->prog) {
        pex__logf(PEX_LOG_ERR, "pex: xip: unusable flash partition description");
        return PEX_EINVAL;
    }
    /* the newest valid copy (seq compared with wrap-around) */
    const ix_t *c0 = (const ix_t *)fptr(0), *c1 = (const ix_t *)fptr(SEC);
    bool v0 = ix_valid(c0), v1 = ix_valid(c1);
    const ix_t *src = NULL;
    if (v0 && v1) {
        src = (int32_t)(c1->h.seq - c0->h.seq) > 0 ? c1 : c0;
    } else if (v0 || v1) {
        src = v0 ? c0 : c1;
    }
    /* The suffix is reserved before the index block, which sits on the low side. */
    uint32_t need = src ? area_need(src) : 0;
    if (need && area_ensure(need) != PEX_OK) {
        pex__logf(PEX_LOG_WARN, "pex: xip: could not reserve %lu bytes at the top of the pool: model A modules load into RAM",
                  (unsigned long)need);
        s_area = NULL;
        s_area_size = 0;
    }
    s_ix = pex__alloc(IX_BYTES, 8, PEX_OWNER_RT, 1);
    if (!s_ix) {
        pex__pool_set_suffix(0);
        s_area = NULL;
        s_area_size = 0;
        pex__logf(PEX_LOG_ERR, "pex: xip: no pool memory for the index");
        return PEX_ENOMEM;
    }
    memset(s_ix, 0, IX_BYTES);
    if (src) {
        memcpy(s_ix, src, IX_BYTES);
        s_cur = src == c0 ? 0 : 1;
    }
    /* drop entries that make no sense (never written by this code) */
    for (int i = 0; i < s_ix->h.n;) {
        ix_ent_t *e = &s_ix->e[i];
        bool ok = e->file_size > 0 && e->sec >= IX_SECTORS && e->sec + nsec_of(e) <= nsec_total();
        for (int k = 0; k < i && ok; k++) {
            const ix_ent_t *o = &s_ix->e[k];
            ok = e->sec + nsec_of(e) <= o->sec || o->sec + nsec_of(o) <= e->sec;
        }
        if (!ok) {
            pex__logf(PEX_LOG_ERR, "pex: xip: index entry %d (%.24s) is inconsistent: ignored", i, e->name);
            memmove(e, e + 1, (s_ix->h.n - i - 1) * sizeof *e);
            s_ix->h.n--;
            continue;
        }
        i++;
    }
    s_mounted = true;
    if (s_ix->h.mv_active) {
        uint32_t i = s_ix->h.mv_ent;
        if (i >= s_ix->h.n) {
            pex__logf(PEX_LOG_ERR, "pex: xip: the move journal names no entry: dropped");
        } else {
            pex__logf(PEX_LOG_WARN, "pex: xip: resuming the move of %s interrupted at step %lu", s_ix->e[i].name,
                      (unsigned long)s_ix->h.mv_done);
            int rc = move_entry((int)i, s_ix->h.mv_dst, s_ix->h.mv_rw_base, s_ix->h.mv_aoff, s_ix->h.mv_done, true);
            if (rc) {
                pex__logf(PEX_LOG_ERR, "pex: xip: resuming the move failed: %s", pex_errmsg());
            } else {
                s_recovered = 1;
            }
        }
    }
    pex_xip_stats_t st;
    pex_xip_stats(&st);
    pex__logf(PEX_LOG_INFO, "pex: xip: partition %lu KB, %lu module%s installed, %lu KB free (index #%lu)",
              (unsigned long)(F->size / 1024), (unsigned long)st.entries, st.entries == 1 ? "" : "s",
              (unsigned long)(st.free / 1024), (unsigned long)st.seq);
    return PEX_OK;
}

int pex__xip_mount_cfg(void) {
    /* pex_init has already reset the pool, so a session block is gone. */
    s_open = NULL;
    s_nest = 0;
    F = pex__cfg.flash;
    s_ix = NULL;
    pex__pool_set_suffix(0);
    s_area = NULL;
    s_area_size = 0;
    return mount();
}

int pex_xip_mount(void) {
    if (!pex__cfg.flash) { return PEX_ENOTSUP; }
    for (pex_mod_t *m = pex__mods; m; m = m->next) {
        if (m->code && m->code->xip) { pex__err(PEX_EBUSY, "%s runs from flash", m->name); return PEX_EBUSY; }
    }
    if (s_open) { pex__free(s_open); s_open = NULL; }
    s_nest = 0;
    if (s_ix) { pex__free(s_ix); s_ix = NULL; }
    pex__pool_set_suffix(0);
    s_area = NULL;
    s_area_size = 0;
    F = pex__cfg.flash;
    return mount();
}
