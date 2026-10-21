/*
 * The loader (spec 4, 6, 7, 8): validate, place, relocate, link, start, call,
 * unload, introspect, attribute faults. RAM mode: code and RW both come from
 * the long-term side of the PEX pool; temporary buffers from the scratch side.
 */
#include <stdio.h>
#include <string.h>
#include "pex_internal.h"

pex_mod_t *pex__mods;
pex_mod_t *pex__stack[PEX_MAX_DEPTH];
uint32_t pex__depth;
static pex_fault_t s_fault;

#define CRC_OFF 72u

pex_mod_t *pex__current(void) { return pex__depth ? pex__stack[pex__depth - 1] : NULL; }
uint8_t pex__current_owner(void) {
    pex_mod_t *m = pex__current();
    return m ? m->id : PEX_OWNER_HOST;
}
pex_mod_t *pex__mod_by_id(uint8_t id) {
    for (pex_mod_t *m = pex__mods; m; m = m->next) {
        if (m->id == id) { return m; }
    }
    return NULL;
}
static bool is_mod(pex_mod_t *x) {
    for (pex_mod_t *m = pex__mods; m; m = m->next) {
        if (m == x) { return true; }
    }
    return false;
}

static uint32_t rd32(const void *p) {
    const uint8_t *b = p;
    return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
}
static uint16_t rd16(const void *p) {
    const uint8_t *b = p;
    return (uint16_t)(b[0] | b[1] << 8);
}

static const char *tname(uint32_t t) {
    switch (t) {
    case PEX_TARGET_ARMV6M: return "m0p";
    case PEX_TARGET_ARMV8M_MAIN: return "m33";
    case PEX_TARGET_ARMV8M_BASE: return "m23";
    case PEX_TARGET_ARMV7EM_FP: return "m4f";
    }
    return "?";
}

static int rread(const pex_reader_t *rd, uint32_t off, void *dst, uint32_t n) {
    if (!n) { return 0; }
    if (rd->size && (off > rd->size || n > rd->size - off)) { return PEX_EIO; }
    return rd->read(rd->ctx, off, dst, n) == (int)n ? 0 : PEX_EIO;
}

/* ---- header ---------------------------------------------------------------- */
typedef struct { uint16_t type, flags; uint32_t off, size; } sec_t;
typedef struct {
    uint16_t fmaj, fmin, hsize, target, api_major, api_minor;
    uint32_t file_size, features, flags, version, image_size, text, ro, data, bss, stack, entry;
    uint32_t nsec, sec_off, image_off, crc;
    uint8_t build_id[16];
    sec_t sec[PEX_MAX_SECTIONS];
    uint32_t sig_off;
    int sig;
} hdr_t;

static const sec_t *find_sec(const hdr_t *h, uint16_t type) {
    for (uint32_t i = 0; i < h->nsec; i++) {
        if (h->sec[i].type == type) { return &h->sec[i]; }
    }
    return NULL;
}

static int fail(int code, const char *msg) {
    pex__err(code, "%s", msg);
    return code;
}

static int parse_header(const pex_reader_t *rd, hdr_t *h) {
    uint8_t b[PEX_HEADER_SIZE];
    memset(h, 0, sizeof *h);
    if (rread(rd, 0, b, 16)) { return fail(PEX_EIO, "cannot read the file"); }
    if (rd32(b) != PEX_MAGIC) { return fail(PEX_EFORMAT, "not a .pex (bad magic)"); }
    h->fmaj = rd16(b + 4); h->fmin = rd16(b + 6); h->hsize = rd16(b + 8); h->target = rd16(b + 10);
    h->file_size = rd32(b + 12);
    if (h->fmaj != PEX_FORMAT_MAJOR) {
        pex__err(PEX_EVERSION, "format %u.%u, this loader reads %u.x", h->fmaj, h->fmin, PEX_FORMAT_MAJOR);
        return PEX_EVERSION;
    }
    if (h->target != pex__host_target()) {
        pex__err(PEX_ETARGET, "built for %s (0x%04x), this host is %s (0x%04lx)", tname(h->target), h->target,
                 tname(pex__host_target()), (unsigned long)pex__host_target());
        return PEX_ETARGET;
    }
    if (h->hsize < PEX_HEADER_SIZE) { return fail(PEX_EFORMAT, "header too small"); }
    if (rd->size && h->file_size != rd->size) {
        pex__err(PEX_EFORMAT, "file is %lu bytes, header says %lu (truncated?)", (unsigned long)rd->size,
                 (unsigned long)h->file_size);
        return PEX_EFORMAT;
    }
    if (rread(rd, 0, b, PEX_HEADER_SIZE)) { return fail(PEX_EIO, "cannot read the header"); }
    h->features = rd32(b + 16); h->api_major = rd16(b + 20); h->api_minor = rd16(b + 22);
    h->flags = rd32(b + 24); h->version = rd32(b + 28); h->image_size = rd32(b + 32);
    h->text = rd32(b + 36); h->ro = rd32(b + 40); h->data = rd32(b + 44); h->bss = rd32(b + 48);
    h->stack = rd32(b + 52); h->entry = rd32(b + 56); h->nsec = rd32(b + 60); h->sec_off = rd32(b + 64);
    h->image_off = rd32(b + 68); h->crc = rd32(b + 72);
    memcpy(h->build_id, b + 76, 16);

    if (h->api_major != PEX_API_MAJOR || h->api_minor > PEX_API_MINOR) {
        pex__err(PEX_EAPI, "built against host API %u.%u, this host has %u.%u", h->api_major, h->api_minor,
                 PEX_API_MAJOR, PEX_API_MINOR);
        return PEX_EAPI;
    }
    uint32_t missing = h->features & ~pex__host_features();
    if (missing) {
        pex__err(PEX_ETARGET, "needs CPU features 0x%lx this host lacks", (unsigned long)missing);
        return PEX_ETARGET;
    }
    if (h->nsec > PEX_MAX_SECTIONS) { return fail(PEX_EFORMAT, "too many sections"); }
    if (h->sec_off < h->hsize || h->sec_off > h->file_size || h->nsec * 12u > h->file_size - h->sec_off) {
        return fail(PEX_EFORMAT, "section table out of bounds");
    }
    uint8_t t[12];
    h->sig_off = h->file_size;
    h->sig = -1;
    for (uint32_t i = 0; i < h->nsec; i++) {
        if (rread(rd, h->sec_off + 12 * i, t, 12)) { return fail(PEX_EIO, "cannot read the section table"); }
        sec_t *s = &h->sec[i];
        s->type = rd16(t); s->flags = rd16(t + 2); s->off = rd32(t + 4); s->size = rd32(t + 8);
        if (s->off > h->file_size || s->size > h->file_size - s->off || (s->off & 3)) {
            pex__err(PEX_EFORMAT, "section %lu (type %u) out of bounds", (unsigned long)i, s->type);
            return PEX_EFORMAT;
        }
        bool known = (s->type >= PEX_SEC_RELOC && s->type <= PEX_SEC_FINI) || s->type == PEX_SEC_SIG;
        if (!known && !(s->flags & PEX_SECF_OPTIONAL)) {
            pex__err(PEX_EFORMAT, "unknown required section type 0x%x (newer format minor?)", s->type);
            return PEX_EFORMAT;
        }
        if (s->type == PEX_SEC_DEPS) { return fail(PEX_ENOTSUP, "DEPS (inter-module dependencies) not supported"); }
        if (s->type == PEX_SEC_SIG) {
            if (s->off + s->size != h->file_size || s->size < 4) { return fail(PEX_EFORMAT, "SIG must be last"); }
            h->sig_off = s->off;
            h->sig = (int)i;
        }
    }
    if (h->text + h->ro + h->data != h->image_size || h->image_off > h->sig_off ||
        h->image_size > h->sig_off - h->image_off || (h->image_off & 3) || (h->text & 3) || (h->ro & 3)) {
        return fail(PEX_EFORMAT, "image layout inconsistent");
    }
    if (h->entry && (!(h->entry & 1) || (h->entry & ~1u) >= h->text)) {
        return fail(PEX_EFORMAT, "entry outside the code");
    }
    if (pex__cfg.max_stack && h->stack > pex__cfg.max_stack) {
        pex__err(PEX_ENOMEM, "stack hint %lu exceeds the host's limit %lu", (unsigned long)h->stack,
                 (unsigned long)pex__cfg.max_stack);
        return PEX_ENOMEM;
    }
    if ((h->flags & PEX_F_REENTRANT) && !(h->flags & PEX_F_PIC)) {
        return fail(PEX_EFORMAT, "REENTRANT without PIC");
    }
    return PEX_OK;
}

/* CRC over [0, sig_off) with the crc field as zero; SHA-256 of the same bytes;
 * then the signature and the policy (spec 10). */
static int integrity(const pex_reader_t *rd, const hdr_t *h) {
    uint8_t buf[128];
    uint32_t crc = 0;
    pex_sha256_t sha;
    bool signed_ = h->sig >= 0;
    if (signed_) { pex_sha256_init(&sha); }
    for (uint32_t off = 0; off < h->sig_off;) {
        uint32_t n = h->sig_off - off < sizeof buf ? h->sig_off - off : (uint32_t)sizeof buf;
        if (rread(rd, off, buf, n)) { return fail(PEX_EIO, "read error"); }
        if (signed_) { pex_sha256_update(&sha, buf, n); }
        for (uint32_t k = 0; k < 4; k++) {
            if (off <= CRC_OFF + k && CRC_OFF + k < off + n) { buf[CRC_OFF + k - off] = 0; }
        }
        crc = pex_crc32_update(crc, buf, n);
        off += n;
    }
    if (crc != h->crc) {
        pex__err(PEX_ECRC, "CRC 0x%08lx, header says 0x%08lx: the file is corrupted", (unsigned long)crc,
                 (unsigned long)h->crc);
        return PEX_ECRC;
    }
    if (signed_) {
        const sec_t *s = &h->sec[h->sig];
        uint8_t sig[4 + 64];
        if (s->size != sizeof sig || rread(rd, s->off, sig, sizeof sig)) {
            return fail(PEX_ESIG, "malformed SIG section");
        }
        uint8_t digest[32];
        pex_sha256_final(&sha, digest);
        if (!pex__cfg.verify) { return fail(PEX_ESIG, "signed, but this host has no signature verifier"); }
        int rc = pex__cfg.verify(rd16(sig), rd16(sig + 2), digest, sig + 4, 64);
        if (rc != 0) {
            pex__err(PEX_ESIG, "signature does not verify (alg %u, key %u)", rd16(sig), rd16(sig + 2));
            return PEX_ESIG;
        }
    } else {
        if (h->flags & PEX_F_NEEDS_SIG) { return fail(PEX_EUNSIGNED, "module requires a signature and has none"); }
        if (!(pex__cfg.policy & PEX_POLICY_ALLOW_UNSIGNED)) {
            return fail(PEX_EUNSIGNED, "unsigned, and this host loads signed modules only");
        }
    }
    return PEX_OK;
}

int pex_check(const pex_reader_t *rd) {
    hdr_t h;
    int rc = parse_header(rd, &h);
    return rc ? rc : integrity(rd, &h);
}

/* An import's name from the optional DEBUG section, for messages. */
static void debug_name(const pex_reader_t *rd, const hdr_t *h, uint32_t hash, char *out, size_t cap) {
    snprintf(out, cap, "0x%08lx", (unsigned long)hash);
    const sec_t *s = find_sec(h, PEX_SEC_DEBUG);
    if (!s) { return; }
    uint8_t r[8];
    for (uint32_t i = 0; i + 8 <= s->size;) {
        if (rread(rd, s->off + i, r, 8)) { return; }
        uint16_t kind = rd16(r + 4), len = rd16(r + 6);
        if (kind == 1 && rd32(r) == hash) {
            uint32_t n = len < cap - 1 ? len : (uint32_t)cap - 1;
            if (rread(rd, s->off + i + 8, out, n) == 0) { out[n] = 0; }
            return;
        }
        i += (8u + len + 3u) & ~3u;
    }
}

static const void *lookup(const pex_group_t *g, uint32_t hash) {
    uint32_t lo = 0, hi = g->count;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (g->syms[mid].hash == hash) { return g->syms[mid].addr; }
        if (g->syms[mid].hash < hash) { lo = mid + 1; } else { hi = mid; }
    }
    return NULL;
}

/* ---- calls ---------------------------------------------------------------- */
uint64_t pex_call(pex_mod_t *m, uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3) {
    if (!m || m->state == PEX_ST_FAULTED || !fn) { return (uint64_t)(int64_t)PEX_EFAULT; }
    if (pex__depth >= PEX_MAX_DEPTH) {
        pex__logf(PEX_LOG_ERR, "pex: call depth %u exceeded calling %s", PEX_MAX_DEPTH, m->name);
        return (uint64_t)(int64_t)PEX_EBUSY;
    }
    pex__stack[pex__depth++] = m;
    m->active++;
    uint32_t prev = 0;
    if (pex__cfg.trace) {
        prev = *pex__cfg.trace;
        *pex__cfg.trace = rd32(m->build_id);
    }
    uint64_t r = pex_tramp((uint32_t)(uintptr_t)m->rw, fn, a0, a1, a2, a3);
    if (pex__cfg.trace) { *pex__cfg.trace = prev; }
    m->active--;
    pex__depth--;
    return r;
}

uint32_t pex_depth(void) { return pex__depth; }

void pex_unwind(uint32_t depth) {
    while (pex__depth > depth) {
        pex_mod_t *m = pex__stack[--pex__depth];
        if (m->active > 0) { m->active--; }
    }
    if (pex__cfg.trace) { *pex__cfg.trace = pex__depth ? rd32(pex__stack[pex__depth - 1]->build_id) : 0; }
}

int pex_entry(pex_mod_t *m, uint32_t reason, void *arg) {
    if (!m || !m->entry) { return PEX_ENOENT; }
    m->host.self = m;
    return (int)(uint32_t)pex_call(m, (uint32_t)(uintptr_t)m->code->base + m->entry,
                                   (uint32_t)(uintptr_t)&m->host, reason, (uint32_t)(uintptr_t)arg, 0);
}

static uint32_t fnv1a(const char *s) {
    uint32_t h = 0x811C9DC5u;
    while (*s) { h ^= (uint8_t)*s++; h *= 0x01000193u; }
    return h;
}

uint32_t pex_sym(pex_mod_t *m, const char *name) {
    if (!m || !name) { return 0; }
    uint32_t h = fnv1a(name);
    for (uint32_t i = 0; i < m->n_exports; i++) {
        if (m->exports[i].hash == h && strcmp(m->strings + m->exports[i].name_off, name) == 0) {
            return (uint32_t)(uintptr_t)m->code->base + m->exports[i].value;
        }
    }
    return 0;
}

const char *pex_export_name(pex_mod_t *m, uint32_t i) {
    return (m && i < m->n_exports) ? m->strings + m->exports[i].name_off : NULL;
}

/* ---- load ------------------------------------------------------------------ */
static uint8_t new_id(void) {
    for (uint32_t id = 1; id < PEX_OWNER_RT; id++) {
        if (!pex__mod_by_id((uint8_t)id)) { return (uint8_t)id; }
    }
    return 0;
}

static void release(pex_mod_t *m) {
    /* everything tagged with the module's id, then its share of the code */
    pex_code_t *c = m->code;
    uint8_t id = m->id;
    pex__sched_cancel_owner(m);
    pex__scratch_release_owner(id);
    pex__free_owner(id, NULL);
    if (c && --c->users == 0) {
        pex__free(c->base);
        pex__free(c);
    }
}

static void unlink_mod(pex_mod_t *m) {
    for (pex_mod_t **pp = &pex__mods; *pp; pp = &(*pp)->next) {
        if (*pp == m) { *pp = m->next; return; }
    }
}

pex_mod_t *pex_load(const pex_reader_t *rd, const pex_load_opts_t *opts, int *errp) {
    hdr_t *h = NULL;
    pex_arena_t *tmp = NULL, *tmp2 = NULL;
    pex_mod_t *m = NULL;
    int rc;
    char nm[48];
#define FAIL(code) do { rc = (code); goto out; } while (0)
    pex__err(PEX_OK, "ok");
    if (!rd || !rd->read) { pex__err(PEX_EINVAL, "no reader"); rc = PEX_EINVAL; goto out; }
    tmp = pex__scratch_begin(sizeof(hdr_t) + 512, PEX_OWNER_HOST);
    if (!tmp) { pex__err(PEX_ENOMEM, "PEX pool full (no room for the load buffer)"); FAIL(PEX_ENOMEM); }
    h = pex__scratch_alloc(tmp, sizeof *h);
    if ((rc = parse_header(rd, h)) != 0) { goto out; }
    if ((rc = integrity(rd, h)) != 0) { goto out; }

    /* ---- imports: resolve every symbol before allocating anything */
    const sec_t *si = find_sec(h, PEX_SEC_IMPORTS);
    uint32_t ng = 0, ni = 0;
    uint8_t *imp = NULL;
    uint32_t *addrs = NULL;
    if (si) {
        uint8_t b4[4];
        if (si->size < 4 || rread(rd, si->off, b4, 4)) { pex__err(PEX_EFORMAT, "bad IMPORTS"); FAIL(PEX_EFORMAT); }
        ng = rd16(b4);
        ni = rd16(b4 + 2);
        if (4 + 4 * ng + 12 * ni > si->size) { pex__err(PEX_EFORMAT, "IMPORTS truncated"); FAIL(PEX_EFORMAT); }
        /* a second arena, nested inside the load: the imports and their addresses */
        tmp2 = pex__scratch_begin(si->size + 4 * ni + 16, PEX_OWNER_HOST);
        if (!tmp2) { pex__err(PEX_ENOMEM, "PEX pool full (imports)"); FAIL(PEX_ENOMEM); }
        imp = pex__scratch_alloc(tmp2, si->size);
        addrs = pex__scratch_alloc(tmp2, 4 * ni + 4);
        if (!imp || !addrs || rread(rd, si->off, imp, si->size)) { pex__err(PEX_EIO, "cannot read IMPORTS"); FAIL(PEX_EIO); }
        for (uint32_t gi = 0; gi < ng; gi++) {
            const uint8_t *gr = imp + 4 + 4 * gi;
            uint16_t gid = rd16(gr);
            uint8_t maj = gr[2], mnr = gr[3];
            const pex_group_t *g = pex_find_group(gid);
            bool ok = g && g->major == maj && mnr <= g->minor;
            if (!ok) {
                /* acceptable only when every import from the group is weak */
                for (uint32_t k = 0; k < ni; k++) {
                    const uint8_t *e = imp + 4 + 4 * ng + 12 * k;
                    if (rd16(e + 4) == gid && !(rd16(e + 6) & PEX_IMP_WEAK)) {
                        debug_name(rd, h, rd32(e), nm, sizeof nm);
                        if (!g) {
                            pex__err(PEX_EAPI, "needs API group %u (for '%s'), which this host does not provide", gid, nm);
                        } else {
                            pex__err(PEX_EAPI, "needs group '%s' %u.%u, this host has %u.%u", g->name, maj, mnr,
                                     g->major, g->minor);
                        }
                        FAIL(PEX_EAPI);
                    }
                }
            }
        }
        for (uint32_t k = 0; k < ni; k++) {
            const uint8_t *e = imp + 4 + 4 * ng + 12 * k;
            uint32_t hash = rd32(e), slot = rd32(e + 8);
            uint16_t gid = rd16(e + 4), fl = rd16(e + 6);
            const pex_group_t *g = pex_find_group(gid);
            const void *a = NULL;
            bool group_ok = false;
            for (uint32_t gi = 0; gi < ng; gi++) {
                const uint8_t *gr = imp + 4 + 4 * gi;
                if (rd16(gr) == gid) { group_ok = g && g->major == gr[2] && gr[3] <= g->minor; }
            }
            if (group_ok) { a = lookup(g, hash); }
            if (!a && !(fl & PEX_IMP_WEAK)) {
                debug_name(rd, h, hash, nm, sizeof nm);
                pex__err(PEX_EAPI, "host does not export '%s' (group %u)", nm, gid);
                FAIL(PEX_EAPI);
            }
            if ((slot & 3) || slot + 4 > h->data) { pex__err(PEX_EFORMAT, "GOT slot outside data"); FAIL(PEX_EFORMAT); }
            addrs[k] = (uint32_t)(uintptr_t)a;
        }
    }

    /* ---- the module record */
    uint8_t id = new_id();
    if (!id) { pex__err(PEX_ENOMEM, "too many modules"); FAIL(PEX_ENOMEM); }
    m = pex__alloc(sizeof *m, 8, id, 1);
    if (!m) { pex__err(PEX_ENOMEM, "PEX pool full (module record)"); FAIL(PEX_ENOMEM); }
    memset(m, 0, sizeof *m);
    m->id = id;
    m->flags = h->flags;
    m->version = h->version;
    m->target = h->target;
    memcpy(m->build_id, h->build_id, 16);
    m->text_size = h->text;
    m->code_size = h->text + h->ro;
    m->data_size = h->data;
    m->rw_size = h->data + h->bss;
    m->entry = h->entry;

    /* ---- metadata: exports, fini list, strings (one block) */
    const sec_t *se = find_sec(h, PEX_SEC_EXPORTS), *sm = find_sec(h, PEX_SEC_META), *sf = find_sec(h, PEX_SEC_FINI);
    uint32_t nexp = 0, nfini = 0, strsz = 0, descl = 0, metaname_len = 0;
    uint8_t b8[8];
    if (se) {
        if (se->size < 4 || rread(rd, se->off, b8, 4)) { pex__err(PEX_EFORMAT, "bad EXPORTS"); FAIL(PEX_EFORMAT); }
        nexp = rd32(b8);
        if (nexp > 1024 || 4 + 16 * nexp > se->size) { pex__err(PEX_EFORMAT, "bad EXPORTS"); FAIL(PEX_EFORMAT); }
        strsz = se->size - 4 - 16 * nexp;
    }
    if (sf) {
        if (sf->size < 4 || rread(rd, sf->off, b8, 4)) { pex__err(PEX_EFORMAT, "bad FINI"); FAIL(PEX_EFORMAT); }
        nfini = rd32(b8);
        if (nfini > 256 || 4 + 4 * nfini > sf->size) { pex__err(PEX_EFORMAT, "bad FINI"); FAIL(PEX_EFORMAT); }
    }
    char metaname[24] = "";
    uint32_t desc_off = 0;
    if (sm) {
        for (uint32_t i = 0; i + 4 <= sm->size;) {
            if (rread(rd, sm->off + i, b8, 4)) { break; }
            uint16_t tag = rd16(b8), len = rd16(b8 + 2);
            if (i + 4 + len > sm->size) { break; }
            if (tag == PEX_META_NAME && len) {
                metaname_len = len < sizeof metaname - 1 ? len : (uint32_t)sizeof metaname - 1;
                rread(rd, sm->off + i + 4, metaname, metaname_len);
                metaname[metaname_len] = 0;
            } else if (tag == PEX_META_DESC && len) {
                descl = len < 120 ? len : 120;
                desc_off = sm->off + i + 4;
            }
            i += (4u + len + 3u) & ~3u;
        }
    }
    uint32_t msz = 12 * nexp + 4 * nfini + strsz + descl + 2;
    m->meta = pex__alloc(msz, 8, id, 1);
    if (!m->meta) { pex__err(PEX_ENOMEM, "PEX pool full (metadata)"); FAIL(PEX_ENOMEM); }
    m->exports = (pex_export_t *)m->meta;
    m->fini = (uint32_t *)((uint8_t *)m->meta + 12 * nexp);
    m->strings = (char *)m->fini + 4 * nfini;
    m->n_exports = nexp;
    m->n_fini = nfini;
    for (uint32_t i = 0; i < nexp; i++) {
        uint8_t e[16];
        if (rread(rd, se->off + 4 + 16 * i, e, 16)) { pex__err(PEX_EIO, "read error"); FAIL(PEX_EIO); }
        m->exports[i].hash = rd32(e);
        m->exports[i].value = rd32(e + 4);
        m->exports[i].name_off = rd32(e + 12);
        if (m->exports[i].name_off >= strsz || (m->exports[i].value & ~1u) >= m->code_size) {
            pex__err(PEX_EFORMAT, "bad export %lu", (unsigned long)i);
            FAIL(PEX_EFORMAT);
        }
    }
    if (strsz && rread(rd, se->off + 4 + 16 * nexp, m->strings, strsz)) { pex__err(PEX_EIO, "read error"); FAIL(PEX_EIO); }
    m->strings[strsz ? strsz - 1 : 0] = 0;
    for (uint32_t i = 0; i < nfini; i++) {
        if (rread(rd, sf->off + 4 + 4 * i, b8, 4)) { pex__err(PEX_EIO, "read error"); FAIL(PEX_EIO); }
        m->fini[i] = rd32(b8);
        if ((m->fini[i] & ~1u) >= m->text_size || !(m->fini[i] & 1)) { pex__err(PEX_EFORMAT, "bad FINI entry"); FAIL(PEX_EFORMAT); }
    }
    char *desc = m->strings + strsz + 1;
    if (descl) { rread(rd, desc_off, desc, descl); }
    desc[descl] = 0;
    m->desc = desc;

    /* name: the caller's, else META's; a second instance gets "#2" */
    const char *base = (opts && opts->name && *opts->name) ? opts->name : (metaname[0] ? metaname : "module");
    snprintf(m->name, sizeof m->name, "%s", base);
    for (int k = 2; pex_find(m->name) && k < 100; k++) { snprintf(m->name, sizeof m->name, "%.20s#%d", base, k); }

    /* ---- code: shared with a live instance of the same build (model B, REENTRANT) */
    pex_code_t *c = NULL;
    if ((h->flags & PEX_F_PIC) && (h->flags & PEX_F_REENTRANT)) {
        for (pex_mod_t *o = pex__mods; o; o = o->next) {
            if (o->code && o->state != PEX_ST_FAULTED && memcmp(o->code->build_id, h->build_id, 16) == 0 &&
                o->code->size == m->code_size) {
                c = o->code;
                break;
            }
        }
    }
    bool shared = c != NULL;
    if (!c) {
        c = pex__alloc(sizeof *c, 8, PEX_OWNER_RT, 1);
        if (!c) { pex__err(PEX_ENOMEM, "PEX pool full"); FAIL(PEX_ENOMEM); }
        memset(c, 0, sizeof *c);
        c->base = pex__alloc(m->code_size ? m->code_size : 4, 8, PEX_OWNER_RT, 1);
        if (!c->base) {
            pex__free(c);
            pex__err(PEX_ENOMEM, "PEX pool full: %lu bytes of code", (unsigned long)m->code_size);
            FAIL(PEX_ENOMEM);
        }
        c->size = m->code_size;
        memcpy(c->build_id, h->build_id, 16);
        if (rread(rd, h->image_off, c->base, m->code_size)) {
            pex__free(c->base); pex__free(c);
            pex__err(PEX_EIO, "cannot read the image");
            FAIL(PEX_EIO);
        }
    }
    c->users++;
    m->code = c;

    /* ---- RW: data copied, bss zeroed */
    m->rw = pex__alloc(m->rw_size ? m->rw_size : 4, 8, id, 1);
    if (!m->rw) { pex__err(PEX_ENOMEM, "PEX pool full: %lu bytes of RW", (unsigned long)m->rw_size); FAIL(PEX_ENOMEM); }
    if (rread(rd, h->image_off + m->code_size, m->rw, h->data)) { pex__err(PEX_EIO, "cannot read data"); FAIL(PEX_EIO); }
    memset(m->rw + h->data, 0, h->bss);

    /* ---- relocations (spec 6): word += base of the segment it points into */
    const sec_t *sr = find_sec(h, PEX_SEC_RELOC);
    if (sr) {
        if (sr->size < 8 || rread(rd, sr->off, b8, 8)) { pex__err(PEX_EFORMAT, "bad RELOC"); FAIL(PEX_EFORMAT); }
        uint32_t nro = rd32(b8), nrw = rd32(b8 + 4);
        if (nro > sr->size / 4 || nrw > sr->size / 4 || 8 + 4 * (nro + nrw) > sr->size) {
            pex__err(PEX_EFORMAT, "RELOC truncated");
            FAIL(PEX_EFORMAT);
        }
        if (nro && (h->flags & PEX_F_PIC)) {
            pex__err(PEX_ERELOC, "position-independent module with %lu RO relocations: refused (spec 6.2)",
                     (unsigned long)nro);
            FAIL(PEX_ERELOC);
        }
        uint32_t cb = (uint32_t)(uintptr_t)c->base, rb = (uint32_t)(uintptr_t)m->rw;
        uint8_t chunk[64];
        for (uint32_t k = 0; k < nro + nrw; k += 16) {
            uint32_t n = nro + nrw - k < 16 ? nro + nrw - k : 16;
            if (rread(rd, sr->off + 8 + 4 * k, chunk, 4 * n)) { pex__err(PEX_EIO, "read error"); FAIL(PEX_EIO); }
            for (uint32_t j = 0; j < n; j++) {
                uint32_t e = rd32(chunk + 4 * j), off = e & 0x7FFFFFFFu;
                uint32_t add = (e & PEX_RELOC_TO_RW) ? rb : cb;
                bool ro = (k + j) < nro;
                if (off & 3u || off + 4 > (ro ? m->code_size : h->data)) {
                    pex__err(PEX_EFORMAT, "relocation %lu out of range", (unsigned long)(k + j));
                    FAIL(PEX_EFORMAT);
                }
                if (ro && shared) { continue; }      /* never happens: PIC has no RO list */
                uint32_t *w = (uint32_t *)((ro ? c->base : m->rw) + off);
                *w += add;
            }
        }
    }
    /* ---- the GOT */
    for (uint32_t k = 0; k < ni; k++) {
        const uint8_t *e = imp + 4 + 4 * ng + 12 * k;
        *(uint32_t *)(m->rw + rd32(e + 8)) = addrs[k];
    }
    if (!shared) { pex_port_code_sync(c->base, c->size); }

    m->host.size = sizeof m->host;
    m->host.api_major = PEX_API_MAJOR;
    m->host.api_minor = PEX_API_MINOR;
    m->host.target = (uint16_t)pex__host_target();
    m->host.format_minor = PEX_FORMAT_MINOR;
    m->host.host_name = pex__cfg.host_name;
    m->host.self = m;
    m->state = PEX_ST_LOADED;
    /* append (lists show load order) */
    pex_mod_t **pp = &pex__mods;
    while (*pp) { pp = &(*pp)->next; }
    *pp = m;

    /* the temporary buffers are no longer needed: give the scratch side back
     * before any module code runs (it may want scratch itself) */
    const sec_t *sinit = find_sec(h, PEX_SEC_INIT);
    uint32_t init_off = sinit ? sinit->off : 0, init_size = sinit ? sinit->size : 0;
    bool no_init = opts && opts->no_init;
    if (tmp2) { pex__scratch_end(tmp2); tmp2 = NULL; }
    pex__scratch_end(tmp);
    tmp = NULL;
    h = NULL;

    if (init_size >= 4) {
        if (rread(rd, init_off, b8, 4)) { pex__err(PEX_EIO, "read error"); rc = PEX_EIO; goto undo; }
        uint32_t n = rd32(b8);
        if (n > 256 || 4 + 4 * n > init_size) { pex__err(PEX_EFORMAT, "bad INIT"); rc = PEX_EFORMAT; goto undo; }
        for (uint32_t i = 0; i < n; i++) {
            if (rread(rd, init_off + 4 + 4 * i, b8, 4)) { pex__err(PEX_EIO, "read error"); rc = PEX_EIO; goto undo; }
            uint32_t f = rd32(b8);
            if (!(f & 1) || (f & ~1u) >= m->text_size) { pex__err(PEX_EFORMAT, "bad INIT entry"); rc = PEX_EFORMAT; goto undo; }
            pex_call(m, (uint32_t)(uintptr_t)c->base + f, 0, 0, 0, 0);
            if (m->state == PEX_ST_FAULTED) { pex__err(PEX_EFAULT, "a constructor faulted"); rc = PEX_EFAULT; goto undo; }
        }
    }
    if (!no_init && m->entry) {
        int r = pex_entry(m, PEX_INIT, NULL);
        if (r < 0) {
            pex__err(PEX_EINIT, "%s: pex_main(INIT) returned %d (%s)", m->name, r, pex_strerror(r));
            rc = PEX_EINIT;
            goto undo;
        }
    }
    pex__logf(PEX_LOG_INFO, "pex: loaded %s v%lu.%lu.%lu (%s, model %c%s): code %lu @%p, RW %lu @%p, %lu imports, %lu exports",
              m->name, (unsigned long)(m->version >> 24), (unsigned long)((m->version >> 16) & 0xFF),
              (unsigned long)(m->version & 0xFFFF), tname(m->target), (m->flags & PEX_F_PIC) ? 'B' : 'A',
              shared ? ", code shared" : "", (unsigned long)m->code_size, (void *)c->base,
              (unsigned long)m->rw_size, (void *)m->rw, (unsigned long)ni, (unsigned long)nexp);
    if (errp) { *errp = PEX_OK; }
    return m;

undo:
    unlink_mod(m);
    release(m);
    if (errp) { *errp = rc; }
    pex__logf(PEX_LOG_WARN, "pex: load failed: %s (%s)", pex_errmsg(), pex_strerror(rc));
    return NULL;
out:
    if (tmp2) { pex__scratch_end(tmp2); }
    if (tmp) { pex__scratch_end(tmp); }
    if (m) {
        pex_code_t *cc = m->code;
        uint8_t id2 = m->id;
        pex__free_owner(id2, NULL);
        if (cc && --cc->users == 0) { pex__free(cc->base); pex__free(cc); }
    }
    if (errp) { *errp = rc; }
    if (rc) { pex__logf(PEX_LOG_WARN, "pex: load refused: %s (%s)", pex_errmsg(), pex_strerror(rc)); }
    return NULL;
#undef FAIL
}

typedef struct { const uint8_t *p; uint32_t n; } membuf_t;
static int mem_read(void *ctx, uint32_t off, void *dst, uint32_t n) {
    membuf_t *b = ctx;
    if (off > b->n || n > b->n - off) { return -1; }
    memcpy(dst, b->p + off, n);
    return (int)n;
}

pex_mod_t *pex_load_mem(const void *buf, uint32_t len, const pex_load_opts_t *opts, int *err) {
    membuf_t b = { buf, len };
    pex_reader_t rd = { mem_read, &b, len };
    return pex_load(&rd, opts, err);
}

/* ---- lifecycle -------------------------------------------------------------- */
int pex_start(pex_mod_t *m, const char *cfg_json) {
    pex__err(PEX_OK, "ok");
    if (!is_mod(m)) { pex__err(PEX_ENOENT, "no such module"); return PEX_ENOENT; }
    if (m->state == PEX_ST_FAULTED) { pex__err(PEX_EFAULT, "%s faulted; unload it", m->name); return PEX_EFAULT; }
    if (m->tasks > 0) { pex__err(PEX_ESTATE, "%s is already running", m->name); return PEX_ESTATE; }
    if (!m->entry) { pex__err(PEX_ENOENT, "%s has no pex_main", m->name); return PEX_ENOENT; }
    m->exit_code = 0;
    int rc = pex_entry(m, PEX_APP_RUN, (void *)cfg_json);
    if (m->state == PEX_ST_FAULTED) { pex__err(PEX_EFAULT, "%s faulted starting", m->name); return PEX_EFAULT; }
    if (rc < 0) {
        pex__sched_cancel_owner(m);
        m->state = PEX_ST_EXITED;
        pex__err(rc, "%s: pex_main(APP_RUN) returned %d (%s)", m->name, rc, pex_strerror(rc));
        return rc;
    }
    m->state = m->tasks > 0 ? PEX_ST_RUNNING : PEX_ST_EXITED;
    pex__logf(PEX_LOG_INFO, "pex: started %s (%d task%s)", m->name, m->tasks, m->tasks == 1 ? "" : "s");
    return PEX_OK;
}

int pex_stop(pex_mod_t *m) {
    pex__err(PEX_OK, "ok");
    if (!is_mod(m)) { pex__err(PEX_ENOENT, "no such module"); return PEX_ENOENT; }
    if (m->tasks == 0 && m->state != PEX_ST_RUNNING) { return PEX_OK; }
    if (m->state != PEX_ST_FAULTED && m->entry) { pex_entry(m, PEX_APP_STOP, NULL); }
    uint32_t n = pex__sched_cancel_owner(m);
    if (m->state != PEX_ST_FAULTED) { m->state = PEX_ST_STOPPED; }
    pex__logf(PEX_LOG_INFO, "pex: stopped %s%s", m->name, n ? " (cancelled its remaining tasks)" : "");
    return PEX_OK;
}

static bool looks_like_return(uint32_t w, const pex_code_t *c, uint32_t text) {
    if (!(w & 1)) { return false; }
    uint32_t a = w & ~1u, base = (uint32_t)(uintptr_t)c->base;
    if (a < base + 4 || a >= base + text) { return false; }
    uint16_t h1 = *(const uint16_t *)(uintptr_t)(a - 4), h2 = *(const uint16_t *)(uintptr_t)(a - 2);
    bool bl = (h1 & 0xF800u) == 0xF000u && (h2 & 0xD000u) == 0xD000u;
    bool blx = (h2 & 0xFF87u) == 0x4780u;
    return bl || blx;
}

int pex_unload(pex_mod_t *m) {
    pex__err(PEX_OK, "ok");
    if (!is_mod(m)) { pex__err(PEX_ENOENT, "no such module"); return PEX_ENOENT; }
    if (m->active > 0) {
        pex__err(PEX_EBUSY, "%s is on a call stack (active %d)", m->name, m->active);
        return PEX_EBUSY;
    }
    if (m->refs > 0) {
        pex__err(PEX_EBUSY, "%s has %d live reference%s (Lua functions, registered callbacks)", m->name, m->refs,
                 m->refs == 1 ? "" : "s");
        return PEX_EBUSY;
    }
    if (m->tasks > 0) {
        pex__err(PEX_EBUSY, "%s is running (%d scheduled task%s): stop it first", m->name, m->tasks,
                 m->tasks == 1 ? "" : "s");
        return PEX_EBUSY;
    }
    uintptr_t lo, hi;
    if (pex__cfg.stack_scan && m->state != PEX_ST_FAULTED && m->code->users == 1 && pex_port_stack_bounds(&lo, &hi)) {
        uintptr_t sp = pex_get_sp();
        if (sp > lo && sp < hi) { lo = sp; }
        for (uintptr_t a = (lo + 3) & ~(uintptr_t)3; a + 4 <= hi; a += 4) {
            if (looks_like_return(*(const uint32_t *)a, m->code, m->text_size)) {
                pex__err(PEX_EBUSY, "a return address into %s is on the stack (at %p)", m->name, (void *)a);
                return PEX_EBUSY;
            }
        }
    }
    char name[24];
    memcpy(name, m->name, sizeof name);
    if (m->state != PEX_ST_FAULTED) {
        if (m->entry) { pex_entry(m, PEX_DEINIT, NULL); }
        for (uint32_t i = m->n_fini; i-- > 0 && m->state != PEX_ST_FAULTED;) {
            pex_call(m, (uint32_t)(uintptr_t)m->code->base + m->fini[i], 0, 0, 0, 0);
        }
        if (m->active > 0) {   /* a DEINIT that faulted and was recovered */
            pex__err(PEX_EBUSY, "%s is on a call stack", name);
            return PEX_EBUSY;
        }
    }
    uint32_t ntasks = pex__sched_cancel_owner(m);
    uint32_t nscr = pex__scratch_release_owner(m->id);
    uint32_t leak_bytes = 0, leak_blocks = m->long_blocks;
    leak_bytes = m->long_bytes;
    unlink_mod(m);
    pex_code_t *c = m->code;
    pex__free_owner(m->id, NULL);   /* the module's blocks, its RW and this record */
    bool code_freed = false;
    if (c && --c->users == 0) {
        pex__free(c->base);
        pex__free(c);
        code_freed = true;
    }
    if (leak_blocks || nscr || ntasks) {
        pex__logf(PEX_LOG_WARN, "pex: %s: unload reclaimed %lu leaked block%s (%lu bytes), %lu scratch arena%s, %lu task%s",
                  name, (unsigned long)leak_blocks, leak_blocks == 1 ? "" : "s", (unsigned long)leak_bytes,
                  (unsigned long)nscr, nscr == 1 ? "" : "s", (unsigned long)ntasks, ntasks == 1 ? "" : "s");
    }
    pex__logf(PEX_LOG_INFO, "pex: unloaded %s%s", name, code_freed ? "" : " (code kept: shared with another instance)");
    return PEX_OK;
}

int pex_unload_all(void) {
    int left = 0;
    for (int pass = 0; pass < 3 && pex__mods; pass++) {
        left = 0;
        for (pex_mod_t *m = pex__mods, *nx; m; m = nx) {
            nx = m->next;
            pex_stop(m);
            if (pex_unload(m) != PEX_OK) { left++; }
        }
    }
    return left;
}

pex_mod_t *pex_find(const char *name) {
    for (pex_mod_t *m = pex__mods; m; m = m->next) {
        if (strcmp(m->name, name) == 0) { return m; }
    }
    return NULL;
}
pex_mod_t *pex_first(void) { return pex__mods; }
pex_mod_t *pex_next(pex_mod_t *m) { return m ? m->next : NULL; }

/* ---- introspection --------------------------------------------------------- */
const char *pex_state_name(pex_state_t s) {
    static const char *const n[] = { "loaded", "running", "exited", "stopped", "faulted" };
    return (unsigned)s < 5 ? n[s] : "?";
}

int pex_info(pex_mod_t *m, pex_info_t *o) {
    if (!is_mod(m) || !o) { return PEX_ENOENT; }
    memset(o, 0, sizeof *o);
    o->name = m->name;
    o->desc = m->desc;
    o->state = (pex_state_t)m->state;
    o->flags = m->flags;
    o->version = m->version;
    o->target = m->target;
    o->build_id = m->build_id;
    o->code = (uintptr_t)m->code->base;
    o->rw = (uintptr_t)m->rw;
    o->code_size = m->code_size;
    o->rw_size = m->rw_size;
    o->code_shared = m->code->users > 1;
    o->active = m->active;
    o->refs = m->refs;
    o->tasks = m->tasks;
    o->exit_code = m->exit_code;
    o->n_exports = m->n_exports;
    pex_pool_stats(&o->mem);
    o->mem.long_bytes = m->long_bytes;
    o->mem.long_blocks = m->long_blocks;
    o->mem.scratch_bytes = m->scratch_bytes;
    o->mem.scratch_peak = m->scratch_peak;
    return PEX_OK;
}

#define APPEND(...) do { int _k = snprintf(dst + n, n < cap ? cap - n : 0, __VA_ARGS__); if (_k > 0) { n += (size_t)_k; } } while (0)

static size_t mod_json(pex_mod_t *m, char *dst, size_t cap, bool full) {
    size_t n = 0;
    APPEND("{\"name\":\"%s\",\"state\":\"%s\",\"version\":\"%lu.%lu.%lu\",\"build_id\":\"", m->name,
           pex_state_name((pex_state_t)m->state), (unsigned long)(m->version >> 24),
           (unsigned long)((m->version >> 16) & 0xFF), (unsigned long)(m->version & 0xFFFF));
    for (int i = 0; i < (full ? 16 : 8); i++) { APPEND("%02x", m->build_id[i]); }
    APPEND("\",\"model\":\"%s\",\"app\":%s,\"lua\":%s,\"code\":%lu,\"rw\":%lu,\"shared\":%s,\"active\":%d,\"refs\":%d,"
           "\"tasks\":%d,\"long\":%lu,\"blocks\":%lu,\"scratch_peak\":%lu", (m->flags & PEX_F_PIC) ? "pic" : "reloc",
           (m->flags & PEX_F_APP) ? "true" : "false", (m->flags & PEX_F_LUA_MODULE) ? "true" : "false",
           (unsigned long)m->code_size, (unsigned long)m->rw_size, m->code->users > 1 ? "true" : "false", m->active,
           m->refs, m->tasks, (unsigned long)m->long_bytes, (unsigned long)m->long_blocks,
           (unsigned long)m->scratch_peak);
    if (full) {
        APPEND(",\"target\":\"%s\",\"flags\":%lu,\"code_addr\":%lu,\"rw_addr\":%lu,\"exit_code\":%d,\"desc\":\"",
               tname(m->target), (unsigned long)m->flags, (unsigned long)(uintptr_t)m->code->base,
               (unsigned long)(uintptr_t)m->rw, m->exit_code);
        for (const char *p = m->desc; p && *p; p++) {
            if (*p == '"' || *p == '\\') { APPEND("\\%c", *p); } else if ((uint8_t)*p >= 0x20) { APPEND("%c", *p); }
        }
        APPEND("\",\"exports\":[");
        for (uint32_t i = 0; i < m->n_exports; i++) { APPEND("%s\"%s\"", i ? "," : "", pex_export_name(m, i)); }
        APPEND("]");
        if (s_fault.valid && strcmp(s_fault.name, m->name) == 0) {
            APPEND(",\"fault\":{\"offset\":%lu,\"pc\":%lu,\"lr\":%lu,\"in_host\":%s}", (unsigned long)s_fault.offset,
                   (unsigned long)s_fault.pc, (unsigned long)s_fault.lr, s_fault.in_host ? "true" : "false");
        }
    }
    APPEND("}");
    return n;
}

int pex_list_json(char *dst, size_t cap) {
    size_t n = 0;
    APPEND("[");
    for (pex_mod_t *m = pex__mods; m; m = m->next) {
        if (m != pex__mods) { APPEND(","); }
        n += mod_json(m, dst + n, n < cap ? cap - n : 0, false);
    }
    APPEND("]");
    return n < cap ? (int)n : -1;
}

int pex_info_json(pex_mod_t *m, char *dst, size_t cap) {
    if (!is_mod(m)) { return PEX_ENOENT; }
    size_t n = mod_json(m, dst, cap, true);
    return n < cap ? (int)n : -1;
}

/* ---- faults --------------------------------------------------------------- */
pex_mod_t *pex_mod_at(uint32_t addr, uint32_t *off) {
    for (pex_mod_t *m = pex__mods; m; m = m->next) {
        uint32_t b = (uint32_t)(uintptr_t)m->code->base;
        if (addr >= b && addr < b + m->code_size) {
            if (off) { *off = addr - b; }
            return m;
        }
    }
    return NULL;
}

bool pex_fault_attribute(const uint32_t *frame, pex_fault_t *out) {
    uint32_t pc = frame[6], lr = frame[5], off = 0;
    bool in_host = false;
    pex_mod_t *m = pex_mod_at(pc, &off);
    if (!m) {
        uint32_t loff = 0;
        pex_mod_t *ml = pex_mod_at(lr & ~1u, &loff);
        if (ml) {
            m = ml; off = loff; in_host = true;           /* e.g. a call through a NULL weak import */
        } else if (pex__depth) {
            m = pex__current(); in_host = true;          /* host code called from the module */
        }
    }
    if (!m) { return false; }
    pex_fault_t f;
    memset(&f, 0, sizeof f);
    f.valid = true;
    memcpy(f.name, m->name, sizeof f.name);
    memcpy(f.build_id, m->build_id, 16);
    f.pc = pc;
    f.lr = lr;
    f.offset = off;
    f.in_host = in_host;
    s_fault = f;
    if (out) { *out = f; }
    m->state = PEX_ST_FAULTED;
    char bid[33];
    for (int i = 0; i < 16; i++) { snprintf(bid + 2 * i, 3, "%02x", m->build_id[i]); }
    pex__logf(PEX_LOG_ERR, "PEX FAULT: module %s build %s %s offset 0x%lx (pc 0x%08lx lr 0x%08lx)", m->name, bid,
              in_host ? "in host code called from it, caller" : "at", (unsigned long)off, (unsigned long)pc,
              (unsigned long)lr);
    return true;
}

const pex_fault_t *pex_last_fault(void) { return s_fault.valid ? &s_fault : NULL; }
