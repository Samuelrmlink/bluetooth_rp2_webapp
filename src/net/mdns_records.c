/* mDNS record groups: table + wire format. See mdns_records.h. */
#include "net/mdns_records.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    int      id;          /* 0 = free */
    int      owner;
    uint32_t recs;        /* bit per record slot */
} mgroup_t;

static mgroup_t groups[MREC_GROUPS];
static mrec_t recs[MREC_RECORDS];
static int8_t rec_group[MREC_RECORDS];   /* group index + 1; 0 = free */
static int next_id = 1;

void mrec_reset(void) {
    memset(groups, 0, sizeof groups);
    memset(recs, 0, sizeof recs);
    memset(rec_group, 0, sizeof rec_group);
    next_id = 1;
}

static bool ends_local(const char *n) {
    size_t l = strlen(n);
    if (l < 6) { return false; }
    const char *t = n + l - 6;
    for (int i = 0; i < 6; i++) {
        if (tolower((unsigned char)t[i]) != ".local"[i]) { return false; }
    }
    return true;
}

bool mrec_name_ok(const char *name) {
    if (!name) { return false; }
    size_t l = strlen(name);
    if (l == 0 || l > MREC_NAME_MAX || !ends_local(name)) { return false; }
    size_t lab = 0;
    for (size_t i = 0; i <= l; i++) {
        if (name[i] == '.' || name[i] == 0) {
            if (lab == 0 || lab > 63) { return false; }
            lab = 0;
        } else {
            lab++;
        }
    }
    return true;
}

bool mrec_txt_pack(const char *const *items, int n, mrec_t *r) {
    size_t at = 0;
    for (int i = 0; i < n; i++) {
        size_t l = items[i] ? strlen(items[i]) : 0;
        if (!l) { continue; }
        if (l > 255 || at + 1 + l > MREC_TXT_MAX) { return false; }
        r->data[at++] = (char)l;
        memcpy(&r->data[at], items[i], l);
        at += l;
    }
    r->data_len = (uint8_t)at;
    return true;
}

static int group_index(int id) {
    for (int g = 0; g < MREC_GROUPS; g++) {
        if (groups[g].id == id && id > 0) { return g; }
    }
    return -1;
}

static int free_records(void) {
    int n = 0;
    for (int i = 0; i < MREC_RECORDS; i++) { n += rec_group[i] == 0; }
    return n;
}

int mrec_group_add(int owner, const mrec_t *in, int n, const char **why) {
    if (n <= 0) { *why = "a group needs at least one record"; return -1; }
    if (n > MREC_PER_GROUP) { *why = "too many records in one group (max 8)"; return -1; }
    for (int i = 0; i < n; i++) {
        const mrec_t *r = &in[i];
        if (r->type != MREC_A && r->type != MREC_PTR && r->type != MREC_SRV && r->type != MREC_TXT) {
            *why = "record type is A, PTR, SRV or TXT"; return -1;
        }
        if (!mrec_name_ok(r->name)) { *why = "a record name must be a .local name of up to 63 characters"; return -1; }
        if ((r->type == MREC_PTR || r->type == MREC_SRV) && !mrec_name_ok(r->data)) {
            *why = "a PTR/SRV target must be a .local name of up to 63 characters"; return -1;
        }
        if (r->type == MREC_SRV && r->port == 0) { *why = "an SRV record needs a port"; return -1; }
    }
    int g = -1;
    for (int i = 0; i < MREC_GROUPS; i++) {
        if (!groups[i].id) { g = i; break; }
    }
    if (g < 0) { *why = "no free group (max 8)"; return -1; }
    if (free_records() < n) { *why = "no room for that many records (16 in all groups)"; return -1; }
    groups[g].id = next_id++;
    if (next_id <= 0) { next_id = 1; }
    groups[g].owner = owner;
    groups[g].recs = 0;
    int placed = 0;
    for (int i = 0; i < MREC_RECORDS && placed < n; i++) {
        if (rec_group[i] != 0) { continue; }
        recs[i] = in[placed++];
        rec_group[i] = (int8_t)(g + 1);
        groups[g].recs |= 1u << i;
    }
    return groups[g].id;
}

bool mrec_group_remove(int id) {
    int g = group_index(id);
    if (g < 0) { return false; }
    for (int i = 0; i < MREC_RECORDS; i++) {
        if (rec_group[i] == g + 1) { rec_group[i] = 0; memset(&recs[i], 0, sizeof recs[i]); }
    }
    memset(&groups[g], 0, sizeof groups[g]);
    return true;
}

bool mrec_group_exists(int id) { return group_index(id) >= 0; }

int mrec_group_count(void) {
    int n = 0;
    for (int g = 0; g < MREC_GROUPS; g++) { n += groups[g].id != 0; }
    return n;
}

int mrec_groups_of(int owner, int *ids, int max) {
    int n = 0;
    for (int g = 0; g < MREC_GROUPS && n < max; g++) {
        if (groups[g].id && groups[g].owner == owner) { ids[n++] = groups[g].id; }
    }
    return n;
}

int mrec_group_owner(int id) {
    int g = group_index(id);
    return g < 0 ? -1 : groups[g].owner;
}

static const char *type_name(uint8_t t) {
    switch (t) {
        case MREC_A: return "A"; case MREC_PTR: return "PTR";
        case MREC_SRV: return "SRV"; case MREC_TXT: return "TXT";
        default: return "?";
    }
}

size_t mrec_groups_json(char *dst, size_t cap) {
    size_t n = (size_t)snprintf(dst, cap, "[");
    bool fg = true;
    for (int g = 0; g < MREC_GROUPS && n + 64 < cap; g++) {
        if (!groups[g].id) { continue; }
        n += (size_t)snprintf(dst + n, cap - n, "%s{\"id\":%d,\"owner\":%d,\"records\":[",
                              fg ? "" : ",", groups[g].id, groups[g].owner);
        fg = false;
        bool fr = true;
        for (int i = 0; i < MREC_RECORDS && n + 200 < cap; i++) {
            if (rec_group[i] != g + 1) { continue; }
            const mrec_t *r = &recs[i];
            n += (size_t)snprintf(dst + n, cap - n, "%s{\"type\":\"%s\",\"name\":\"%s\"",
                                  fr ? "" : ",", type_name(r->type), r->name);
            fr = false;
            if (r->type == MREC_A) {
                n += (size_t)snprintf(dst + n, cap - n, ",\"addr\":\"%u.%u.%u.%u\"",
                                      r->a[0], r->a[1], r->a[2], r->a[3]);
            } else if (r->type == MREC_PTR) {
                n += (size_t)snprintf(dst + n, cap - n, ",\"target\":\"%s\"", r->data);
            } else if (r->type == MREC_SRV) {
                n += (size_t)snprintf(dst + n, cap - n, ",\"target\":\"%s\",\"port\":%u,\"priority\":%u,\"weight\":%u",
                                      r->data, r->port, r->prio, r->weight);
            } else if (r->type == MREC_TXT) {
                n += (size_t)snprintf(dst + n, cap - n, ",\"txt\":[");
                for (int at = 0, k = 0; at < r->data_len && n + 80 < cap; k++) {
                    int l = (uint8_t)r->data[at++];
                    n += (size_t)snprintf(dst + n, cap - n, "%s\"%.*s\"", k ? "," : "", l, &r->data[at]);
                    at += l;
                }
                n += (size_t)snprintf(dst + n, cap - n, "]");
            }
            n += (size_t)snprintf(dst + n, cap - n, "}");
        }
        n += (size_t)snprintf(dst + n, cap - n, "]}");
    }
    return n + (size_t)snprintf(dst + n, cap - n, "]");
}

/* --- wire format ----------------------------------------------------------- */

typedef struct { uint8_t *p; size_t cap, len; bool overflow; } wbuf_t;

static void put(wbuf_t *w, const void *d, size_t n) {
    if (w->len + n > w->cap) { w->overflow = true; return; }
    memcpy(w->p + w->len, d, n);
    w->len += n;
}
static void put16(wbuf_t *w, uint16_t v) { uint8_t b[2] = { (uint8_t)(v >> 8), (uint8_t)v }; put(w, b, 2); }
static void put32(wbuf_t *w, uint32_t v) {
    uint8_t b[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v };
    put(w, b, 4);
}

static void put_name(wbuf_t *w, const char *name) {
    const char *s = name;
    while (*s) {
        const char *dot = strchr(s, '.');
        size_t l = dot ? (size_t)(dot - s) : strlen(s);
        uint8_t lb = (uint8_t)l;
        put(w, &lb, 1);
        put(w, s, l);
        s += l;
        if (*s == '.') { s++; }
    }
    uint8_t z = 0;
    put(w, &z, 1);
}

static size_t name_wire_len(const char *name) { return strlen(name) + 2; }

static void put_rr(wbuf_t *w, const mrec_t *r, uint32_t ttl, bool legacy) {
    put_name(w, r->name);
    put16(w, r->type);
    bool unique = r->type != MREC_PTR;
    put16(w, (uint16_t)((unique && !legacy) ? 0x8001 : 0x0001));
    put32(w, ttl);
    switch (r->type) {
    case MREC_A:
        put16(w, 4);
        put(w, r->a, 4);
        break;
    case MREC_PTR:
        put16(w, (uint16_t)name_wire_len(r->data));
        put_name(w, r->data);
        break;
    case MREC_SRV:
        put16(w, (uint16_t)(6 + name_wire_len(r->data)));
        put16(w, r->prio);
        put16(w, r->weight);
        put16(w, r->port);
        put_name(w, r->data);
        break;
    case MREC_TXT:
        if (r->data_len) {
            put16(w, r->data_len);
            put(w, r->data, r->data_len);
        } else {
            uint8_t z = 0;
            put16(w, 1);
            put(w, &z, 1);              /* an empty TXT is one empty string */
        }
        break;
    }
}

static uint32_t ttl_for(const mrec_t *r, bool legacy) {
    if (legacy) { return MREC_TTL_LEGACY; }
    return (r->type == MREC_A || r->type == MREC_SRV) ? MREC_TTL_HOST : MREC_TTL_OTHER;
}

/* Read a (possibly compressed) name at *off into out as dotted text.
 * Advances *off past it. False on anything malformed. */
static bool read_name(const uint8_t *q, size_t qlen, size_t *off, char *out, size_t cap) {
    size_t at = *off, n = 0;
    bool jumped = false;
    int hops = 0;
    for (;;) {
        if (at >= qlen) { return false; }
        uint8_t l = q[at];
        if ((l & 0xC0) == 0xC0) {
            if (at + 1 >= qlen || ++hops > 8) { return false; }
            size_t ptr = ((size_t)(l & 0x3F) << 8) | q[at + 1];
            if (!jumped) { *off = at + 2; }
            jumped = true;
            at = ptr;
            continue;
        }
        if (l & 0xC0) { return false; }
        at++;
        if (l == 0) { break; }
        if (at + l > qlen) { return false; }
        if (n && n + 1 < cap) { out[n++] = '.'; }
        for (uint8_t i = 0; i < l; i++) {
            if (n + 1 < cap) { out[n++] = (char)q[at + i]; }
        }
        at += l;
    }
    out[n] = 0;
    if (!jumped) { *off = at; }
    return true;
}

static bool name_eq(const char *a, const char *b) {
    for (; *a && *b; a++, b++) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) { return false; }
    }
    return *a == *b;
}

size_t mrec_build_answer(const uint8_t *q, size_t qlen, bool legacy,
                         uint8_t *out, size_t cap, bool *unicast) {
    if (unicast) { *unicast = false; }
    if (qlen < 12 || mrec_group_count() == 0) { return 0; }
    if (q[2] & 0x80) { return 0; }                    /* a response, not a query */
    uint16_t qd = (uint16_t)((q[4] << 8) | q[5]);
    size_t off = 12;
    uint32_t ans = 0, add = 0;
    for (uint16_t i = 0; i < qd; i++) {
        char name[MREC_NAME_MAX + 64];
        if (!read_name(q, qlen, &off, name, sizeof name) || off + 4 > qlen) { break; }
        uint16_t qt = (uint16_t)((q[off] << 8) | q[off + 1]);
        uint16_t qc = (uint16_t)((q[off + 2] << 8) | q[off + 3]);
        off += 4;
        if ((qc & 0x7FFF) != 1 && (qc & 0x7FFF) != 255) { continue; }
        bool hit = false;
        for (int r = 0; r < MREC_RECORDS; r++) {
            if (rec_group[r] == 0) { continue; }
            if ((qt == 255 || qt == recs[r].type) && name_eq(name, recs[r].name)) {
                ans |= 1u << r;
                hit = true;
            }
        }
        if (hit && (qc & 0x8000) && unicast) { *unicast = true; }
    }
    size_t qend = off;
    if (!ans) { return 0; }

    /* Additionals (RFC 6763 12): a PTR brings its instance's SRV and TXT,
     * an SRV the A of its target. */
    for (int pass = 0; pass < 2; pass++) {
        for (int r = 0; r < MREC_RECORDS; r++) {
            if (!((ans | add) & (1u << r))) { continue; }
            uint8_t want1 = recs[r].type == MREC_PTR ? MREC_SRV : recs[r].type == MREC_SRV ? MREC_A : 0;
            uint8_t want2 = recs[r].type == MREC_PTR ? MREC_TXT : 0;
            if (!want1) { continue; }
            for (int s = 0; s < MREC_RECORDS; s++) {
                if (rec_group[s] == 0 || ((ans | add) & (1u << s))) { continue; }
                if ((recs[s].type == want1 || recs[s].type == want2) && name_eq(recs[s].name, recs[r].data)) {
                    add |= 1u << s;
                }
            }
        }
    }

    wbuf_t w = { out, cap, 0, false };
    put16(&w, legacy ? (uint16_t)((q[0] << 8) | q[1]) : 0);
    put16(&w, 0x8400);                                /* response, authoritative */
    put16(&w, legacy ? qd : 0);
    put16(&w, (uint16_t)__builtin_popcount(ans));
    put16(&w, 0);
    put16(&w, (uint16_t)__builtin_popcount(add));
    if (legacy) { put(&w, q + 12, qend - 12); }
    for (int r = 0; r < MREC_RECORDS; r++) {
        if (ans & (1u << r)) { put_rr(&w, &recs[r], ttl_for(&recs[r], legacy), legacy); }
    }
    for (int r = 0; r < MREC_RECORDS; r++) {
        if (add & (1u << r)) { put_rr(&w, &recs[r], ttl_for(&recs[r], legacy), legacy); }
    }
    return w.overflow ? 0 : w.len;
}

size_t mrec_build_announce(int id, uint32_t ttl, uint8_t *out, size_t cap) {
    int g = group_index(id);
    if (g < 0) { return 0; }
    wbuf_t w = { out, cap, 0, false };
    put16(&w, 0);
    put16(&w, 0x8400);
    put16(&w, 0);
    put16(&w, (uint16_t)__builtin_popcount(groups[g].recs));
    put16(&w, 0);
    put16(&w, 0);
    for (int r = 0; r < MREC_RECORDS; r++) {
        if (rec_group[r] == g + 1) {
            put_rr(&w, &recs[r], ttl == MREC_TTL_DEFAULT ? ttl_for(&recs[r], false) : ttl, false);
        }
    }
    return w.overflow ? 0 : w.len;
}
