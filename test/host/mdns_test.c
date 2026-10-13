/*
 * src/net/mdns_records.c: the record-group table, the wire encoding, and
 * answering queries -- checked by parsing what it produces with an
 * independent little DNS reader below.
 *
 *   gcc -Isrc test/host/mdns_test.c src/net/mdns_records.c
 */
#include <stdio.h>
#include <string.h>

#include "net/mdns_records.h"

static int checks, failed;
#define CHECK(c, msg) do { checks++; if (!(c)) { failed++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, msg); } } while (0)

/* --- a query builder --------------------------------------------------------- */
static size_t q_name(uint8_t *b, const char *name) {
    size_t n = 0;
    const char *s = name;
    while (*s) {
        const char *d = strchr(s, '.');
        size_t l = d ? (size_t)(d - s) : strlen(s);
        b[n++] = (uint8_t)l;
        memcpy(b + n, s, l); n += l;
        s += l; if (*s == '.') { s++; }
    }
    b[n++] = 0;
    return n;
}
static size_t mkquery(uint8_t *b, uint16_t id, const char *name, uint16_t type, bool qu) {
    memset(b, 0, 12);
    b[0] = (uint8_t)(id >> 8); b[1] = (uint8_t)id;
    b[5] = 1;
    size_t n = 12 + q_name(b + 12, name);
    b[n++] = (uint8_t)(type >> 8); b[n++] = (uint8_t)type;
    b[n++] = qu ? 0x80 : 0; b[n++] = 1;
    return n;
}

/* --- an independent reader ------------------------------------------------------ */
typedef struct { char name[128]; uint16_t type, cls; uint32_t ttl; uint8_t rd[256]; uint16_t rdlen; } rr_t;
static size_t rd_name(const uint8_t *p, size_t len, size_t off, char *out) {
    size_t n = 0, end = 0; int jumps = 0;
    for (;;) {
        uint8_t l = p[off];
        if ((l & 0xC0) == 0xC0) { if (!jumps++) { end = off + 2; } off = ((l & 0x3F) << 8) | p[off + 1]; continue; }
        off++;
        if (!l) { break; }
        if (n) { out[n++] = '.'; }
        memcpy(out + n, p + off, l); n += l; off += l;
        if (off > len) { break; }
    }
    out[n] = 0;
    return jumps ? end : off;
}
static int parse(const uint8_t *p, size_t len, uint16_t *id, uint16_t *flags, int *qd, rr_t *rrs, int max, int *an) {
    *id = (uint16_t)((p[0] << 8) | p[1]); *flags = (uint16_t)((p[2] << 8) | p[3]);
    *qd = (p[4] << 8) | p[5];
    int ancount = (p[6] << 8) | p[7], arcount = (p[10] << 8) | p[11];
    *an = ancount;
    size_t off = 12;
    char tmp[128];
    for (int i = 0; i < *qd; i++) { off = rd_name(p, len, off, tmp) + 4; }
    int n = 0;
    for (int i = 0; i < ancount + arcount && n < max; i++, n++) {
        rr_t *r = &rrs[n];
        off = rd_name(p, len, off, r->name);
        r->type = (uint16_t)((p[off] << 8) | p[off + 1]);
        r->cls = (uint16_t)((p[off + 2] << 8) | p[off + 3]);
        r->ttl = ((uint32_t)p[off + 4] << 24) | ((uint32_t)p[off + 5] << 16) | ((uint32_t)p[off + 6] << 8) | p[off + 7];
        r->rdlen = (uint16_t)((p[off + 8] << 8) | p[off + 9]);
        off += 10;
        memcpy(r->rd, p + off, r->rdlen);
        off += r->rdlen;
    }
    return off == len ? n : -1;
}
static const rr_t *find(const rr_t *rrs, int n, uint16_t type) {
    for (int i = 0; i < n; i++) { if (rrs[i].type == type) { return &rrs[i]; } }
    return NULL;
}

static mrec_t rec_a(const char *name, int a, int b, int c, int d) {
    mrec_t r; memset(&r, 0, sizeof r);
    r.type = MREC_A; snprintf(r.name, sizeof r.name, "%s", name);
    r.a[0] = (uint8_t)a; r.a[1] = (uint8_t)b; r.a[2] = (uint8_t)c; r.a[3] = (uint8_t)d;
    return r;
}
static mrec_t rec_ptr(const char *name, const char *target) {
    mrec_t r; memset(&r, 0, sizeof r);
    r.type = MREC_PTR; snprintf(r.name, sizeof r.name, "%s", name); snprintf(r.data, sizeof r.data, "%s", target);
    return r;
}
static mrec_t rec_srv(const char *name, const char *target, uint16_t port) {
    mrec_t r; memset(&r, 0, sizeof r);
    r.type = MREC_SRV; snprintf(r.name, sizeof r.name, "%s", name); snprintf(r.data, sizeof r.data, "%s", target);
    r.port = port; r.prio = 0; r.weight = 0;
    return r;
}
static mrec_t rec_txt(const char *name, const char *const *items, int n) {
    mrec_t r; memset(&r, 0, sizeof r);
    r.type = MREC_TXT; snprintf(r.name, sizeof r.name, "%s", name);
    mrec_txt_pack(items, n, &r);
    return r;
}

int main(void) {
    const char *why = NULL;
    uint8_t q[256], out[512];
    rr_t rrs[16];
    uint16_t id, flags; int qd, an;
    bool uni;

    mrec_reset();
    /* --- validation ----------------------------------------------------------- */
    CHECK(mrec_name_ok("printer.local") && mrec_name_ok("My Printer._ipp._tcp.local"), "legal names");
    CHECK(!mrec_name_ok("printer.lan") && !mrec_name_ok("") && !mrec_name_ok("a..local") &&
          !mrec_name_ok(".local"), "illegal names");
    char longn[80]; memset(longn, 'x', 70); strcpy(longn + 70, ".local");
    CHECK(!mrec_name_ok(longn), "over 63 characters refused");
    mrec_t bad = rec_srv("x._ipp._tcp.local", "printer.local", 0);
    CHECK(mrec_group_add(3, &bad, 1, &why) < 0 && strstr(why, "port"), "SRV without a port refused");
    bad = rec_ptr("_ipp._tcp.local", "elsewhere.com");
    CHECK(mrec_group_add(3, &bad, 1, &why) < 0 && strstr(why, "target"), "PTR target off .local refused");
    { const char *big[] = { "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "bbbbbbbbbbbbbbbbbbbbbbbbbbbb" };
      mrec_t t; memset(&t, 0, sizeof t);
      CHECK(!mrec_txt_pack(big, 2, &t), "TXT over 63 bytes refused"); }

    /* --- a printer: A + PTR + SRV + TXT ----------------------------------------- */
    const char *txt[] = { "txtvers=1", "rp=ipp/print" };
    mrec_t g1[] = {
        rec_a("printer.local", 192, 168, 8, 50),
        rec_ptr("_ipp._tcp.local", "Printer._ipp._tcp.local"),
        rec_srv("Printer._ipp._tcp.local", "printer.local", 631),
        rec_txt("Printer._ipp._tcp.local", txt, 2),
    };
    int id1 = mrec_group_add(3, g1, 4, &why);
    CHECK(id1 > 0 && mrec_group_count() == 1, "group added");

    /* A query, multicast */
    size_t ql = mkquery(q, 0x1234, "PRINTER.local", MREC_A, false);
    size_t n = mrec_build_answer(q, ql, false, out, sizeof out, &uni);
    CHECK(n > 0 && !uni, "A answered, multicast");
    int k = parse(out, n, &id, &flags, &qd, rrs, 16, &an);
    CHECK(k == 1 && an == 1 && id == 0 && flags == 0x8400 && qd == 0, "one authoritative answer, id 0, no question echo");
    CHECK(rrs[0].type == MREC_A && strcmp(rrs[0].name, "printer.local") == 0, "A record for the stored (case-preserved) name");
    CHECK(rrs[0].cls == 0x8001 && rrs[0].ttl == 120 && rrs[0].rdlen == 4 &&
          rrs[0].rd[0] == 192 && rrs[0].rd[3] == 50, "cache-flush, TTL 120, 192.168.8.50");

    /* PTR browse: answer + SRV, TXT and A as additionals */
    ql = mkquery(q, 7, "_ipp._tcp.local", MREC_PTR, false);
    n = mrec_build_answer(q, ql, false, out, sizeof out, &uni);
    k = parse(out, n, &id, &flags, &qd, rrs, 16, &an);
    CHECK(k == 4 && an == 1, "PTR answer with 3 additionals");
    const rr_t *ptr = find(rrs, k, MREC_PTR), *srv = find(rrs, k, MREC_SRV), *tx = find(rrs, k, MREC_TXT), *a = find(rrs, k, MREC_A);
    CHECK(ptr && ptr->cls == 0x0001 && ptr->ttl == 4500, "PTR is shared (no cache-flush), TTL 4500");
    CHECK(srv && srv->rdlen == 6 + strlen("printer.local") + 2 && srv->rd[4] == 0x02 && srv->rd[5] == 0x77,
          "SRV: port 631, target encoded");
    CHECK(tx && tx->rdlen == 1 + 9 + 1 + 12 && tx->rd[0] == 9 && memcmp(tx->rd + 1, "txtvers=1", 9) == 0,
          "TXT items length-prefixed");
    CHECK(a != NULL, "A of the SRV target included");

    /* ANY */
    ql = mkquery(q, 7, "Printer._ipp._tcp.local", 255, false);
    n = mrec_build_answer(q, ql, false, out, sizeof out, &uni);
    k = parse(out, n, &id, &flags, &qd, rrs, 16, &an);
    CHECK(an == 2 && find(rrs, k, MREC_SRV) && find(rrs, k, MREC_TXT), "ANY returns SRV and TXT");

    /* QU bit */
    ql = mkquery(q, 7, "printer.local", MREC_A, true);
    n = mrec_build_answer(q, ql, false, out, sizeof out, &uni);
    CHECK(n > 0 && uni, "QU asks for a unicast reply");

    /* legacy unicast (dig @224.0.0.251 -p 5353 from an ephemeral port) */
    ql = mkquery(q, 0xBEEF, "printer.local", MREC_A, false);
    n = mrec_build_answer(q, ql, true, out, sizeof out, &uni);
    k = parse(out, n, &id, &flags, &qd, rrs, 16, &an);
    CHECK(k == 1 && id == 0xBEEF && qd == 1, "legacy: id and question echoed");
    CHECK(rrs[0].ttl == 10 && rrs[0].cls == 0x0001, "legacy: TTL 10, no cache-flush bit");

    /* not ours */
    ql = mkquery(q, 7, "other.local", MREC_A, false);
    CHECK(mrec_build_answer(q, ql, false, out, sizeof out, &uni) == 0, "other names are not answered");
    ql = mkquery(q, 7, "printer.local", MREC_TXT, false);
    CHECK(mrec_build_answer(q, ql, false, out, sizeof out, &uni) == 0, "wrong type is not answered");
    q[2] = 0x84;
    CHECK(mrec_build_answer(q, ql, false, out, sizeof out, &uni) == 0, "responses are not answered");

    /* compressed question names (two questions, the second points into the first) */
    {
        uint8_t b[128]; memset(b, 0, 12); b[5] = 2;
        size_t o = 12 + q_name(b + 12, "printer.local");
        b[o++] = 0; b[o++] = 1; b[o++] = 0; b[o++] = 1;
        /* "_ipp._tcp" + pointer to "local" at 12 + 8 */
        b[o++] = 4; memcpy(b + o, "_ipp", 4); o += 4; b[o++] = 4; memcpy(b + o, "_tcp", 4); o += 4;
        b[o++] = 0xC0; b[o++] = 12 + 8;
        b[o++] = 0; b[o++] = 12; b[o++] = 0; b[o++] = 1;
        n = mrec_build_answer(b, o, false, out, sizeof out, &uni);
        k = parse(out, n, &id, &flags, &qd, rrs, 16, &an);
        CHECK(an == 2 && find(rrs, k, MREC_A) && find(rrs, k, MREC_PTR), "both questions answered through a compression pointer");
        uint8_t loop[20] = { 0, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0xC0, 12, 0, 1, 0, 1 };
        CHECK(mrec_build_answer(loop, 18, false, out, sizeof out, &uni) == 0, "a name that points at itself is rejected, not followed forever");
        uint8_t trunc[16] = { 0, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 7, 'p', 'r' };
        CHECK(mrec_build_answer(trunc, 15, false, out, sizeof out, &uni) == 0, "a truncated name is rejected");
    }

    /* announce / goodbye */
    n = mrec_build_announce(id1, MREC_TTL_DEFAULT, out, sizeof out);
    k = parse(out, n, &id, &flags, &qd, rrs, 16, &an);
    CHECK(k == 4 && an == 4 && find(rrs, k, MREC_A)->ttl == 120 && find(rrs, k, MREC_PTR)->ttl == 4500,
          "announce carries the whole group at normal TTLs");
    n = mrec_build_announce(id1, 0, out, sizeof out);
    k = parse(out, n, &id, &flags, &qd, rrs, 16, &an);
    CHECK(k == 4 && rrs[0].ttl == 0 && rrs[3].ttl == 0, "goodbye: TTL 0");

    /* --- ownership and limits ---------------------------------------------------- */
    mrec_t one = rec_a("cam.local", 10, 0, 0, 9);
    int id2 = mrec_group_add(5, &one, 1, &why);
    int ids[8];
    CHECK(mrec_groups_of(3, ids, 8) == 1 && ids[0] == id1, "owner 3 holds group 1");
    CHECK(mrec_groups_of(5, ids, 8) == 1 && ids[0] == id2, "owner 5 holds group 2");
    CHECK(mrec_group_remove(id1) && !mrec_group_exists(id1) && mrec_group_count() == 1, "removed");
    ql = mkquery(q, 7, "printer.local", MREC_A, false);
    CHECK(mrec_build_answer(q, ql, false, out, sizeof out, &uni) == 0, "a removed group is not answered");
    /* fill to 16 records */
    mrec_t eight[8];
    for (int i = 0; i < 8; i++) { char nm[32]; snprintf(nm, sizeof nm, "h%d.local", i); eight[i] = rec_a(nm, 10, 0, 0, i); }
    int a1 = mrec_group_add(1, eight, 8, &why);
    CHECK(a1 > 0, "8 more records");
    CHECK(mrec_group_add(1, eight, 8, &why) < 0 && strstr(why, "16"), "a 17th record is refused");
    mrec_t nine[9];
    for (int i = 0; i < 9; i++) { nine[i] = eight[0]; }
    CHECK(mrec_group_add(1, nine, 9, &why) < 0 && strstr(why, "max 8"), "9 in one group refused");
    char js[2048];
    mrec_groups_json(js, sizeof js);
    CHECK(strstr(js, "\"name\":\"cam.local\",\"addr\":\"10.0.0.9\"") != NULL, "json lists records");

    printf("%d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
