/* Host checks for the pure driver code. The module's anker_tlv_emit goes
 * through the codec host group; here it is anker_tlv_build_payload. */
#include "codec/anker_tlv.h"
#include "crypto/ghash.h"
#include "anker_adv.h"
#include "anker_wire.h"
#include "anker_models.h"
#include "pinecil_live.h"
#include <stdio.h>
#include <string.h>

static int g_fail;

static void expect(int cond, const char *msg) {
    if (cond) { return; }
    fprintf(stderr, "FAIL %s\n", msg);
    g_fail++;
}

int anker_tlv_emit(const anker_item *it, size_t n, uint8_t *out, size_t cap) {
    anker_tlv_spec_t sp[12];
    if (!it || !out || n > 12) { return -1; }
    memset(sp, 0, sizeof sp);
    for (size_t i = 0; i < n; i++) {
        sp[i].tag = it[i].tag;
        sp[i].enc = (anker_enc_t)it[i].enc;
        sp[i].type_byte = it[i].type_byte;
        sp[i].ival = it[i].ival;
        sp[i].fval = 0;
        sp[i].data = it[i].data;
        sp[i].data_len = it[i].data_len;
    }
    return anker_tlv_build_payload(sp, n, out, cap);
}

static void hex_dump(const uint8_t *p, int n) {
    for (int i = 0; i < n; i++) { fprintf(stderr, "%02x", p[i]); }
    fputc('\n', stderr);
}

static int adv_ok(const uint8_t *mfg, size_t n, const uint8_t addr[6], int ff09, anker_adv_t *a) {
    const char *why = NULL;
    int rc = anker_adv_identify(mfg, n, addr, ff09, a, &why);
    if (!rc) { fprintf(stderr, "  adv: %s\n", why ? why : "?"); }
    return rc;
}

static void test_adv(void) {
    uint8_t addr[6] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66 };
    uint8_t v1[16] = {
        0xff, 0xff, 0x01, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x02, 0xb1, 0x05, 'A', 'B', 'C', 0x04
    };
    anker_adv_t a;
    expect(adv_ok(v1, sizeof v1, addr, 1, &a), "v1 identify");
    expect(a.version == 1 && a.bound && a.gcm_hint, "v1 fields");
    expect(strcmp(a.product_type, "B105") == 0 && strcmp(a.sku, "ABC") == 0, "v1 type and sku");

    expect(!anker_adv_identify(v1, 8, addr, 1, &a, NULL), "short payload");
    uint8_t badco[16];
    memcpy(badco, v1, sizeof v1);
    badco[0] = 0x00;
    expect(!anker_adv_identify(badco, sizeof badco, addr, 1, &a, NULL), "wrong company");
    uint8_t other[6] = { 1, 2, 3, 4, 5, 6 };
    expect(!anker_adv_identify(v1, sizeof v1, other, 1, &a, NULL), "mac mismatch");
    expect(!anker_adv_identify(v1, sizeof v1, addr, 0, &a, NULL), "missing ff09");

    /* v2 sku is 4 bytes. Capability is the byte after it, so DJW6's '6'
     * (0x36, GCM bit set) is not the capability. */
    uint8_t v2[17] = {
        0xff, 0xff, 0x02, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x01, 0xb4, 0x05,
        'D', 'J', 'W', '6', 0x00
    };
    expect(adv_ok(v2, sizeof v2, addr, 1, &a), "v2 identify");
    expect(a.version == 2 && strcmp(a.sku, "DJW6") == 0, "v2 sku DJW6");
    expect(a.gcm_hint == 0 && a.capability == 0, "v2 gcm hint stays clear");
}

static void test_country(void) {
    const char *cc = anker_country("A1761", "5YX");
    expect(cc && strcmp(cc, "us") == 0, "country 5YX");
    expect(anker_country("A2687", "DJW6") == NULL, "DJW6 has no country row");
}

static void test_keystream(void) {
    const uint8_t *ks = anker_keystream();
    expect(ks[0] == 0xab && ks[1] == 0x86 && ks[2] == 0x3f && ks[3] == 0xd2, "keystream head");
    anker_set_ghash(ghash_aad_ct);
    uint8_t pt[4] = { 1, 2, 3, 4 };
    uint8_t ct[80];
    int n = anker_neg_encrypt(pt, 4, ct, sizeof ct);
    expect(n == 20, "neg encrypt length");
    uint8_t back[8];
    int m = anker_neg_decrypt(ct, (size_t)n, back, sizeof back);
    expect(m == 4 && memcmp(back, pt, 4) == 0, "neg round trip");
    expect(anker_neg_encrypt(pt, 68, ct, sizeof ct) < 0, "keystream refuses n > 67");
}

static void test_setter_bytes(void) {
    const anker_setter *s = anker_setter_by_name(ANKER_MODEL_A1761, "ac_charge_power_w");
    expect(s && s->opcode == 0x0044 && s->fe == ANKER_FE_TYPED, "700 W setter shape");
    anker_val v;
    memset(&v, 0, sizeof v);
    v.is_int = 1;
    v.i = 700;
    anker_job job;
    const char *err = NULL;
    expect(s && anker_setter_check(s, &v, &job, &err) == 0, "700 W accepted");
    uint8_t out[32];
    int n = anker_setter_payload(&job, 0x01020304u, out, sizeof out);
    static const uint8_t want[] = {
        0xa1, 0x01, 0x21, 0xa2, 0x03, 0x02, 0xbc, 0x02, 0xfe, 0x05, 0x03, 0x04, 0x03, 0x02, 0x01
    };
    if (n != (int)sizeof want || memcmp(out, want, sizeof want) != 0) {
        fprintf(stderr, "FAIL 700 W payload (%d)\n", n);
        if (n > 0) { hex_dump(out, n); }
        g_fail++;
    }

    n = anker_history_request(0, 0x01020304u, out, sizeof out);
    static const uint8_t hist[] = {
        0xa1, 0x01, 0x21, 0xa2, 0x02, 0x01, 0x00, 0xfe, 0x04, 0x04, 0x03, 0x02, 0x01
    };
    if (n != (int)sizeof hist || memcmp(out, hist, sizeof hist) != 0) {
        fprintf(stderr, "FAIL history request (%d)\n", n);
        if (n > 0) { hex_dump(out, n); }
        g_fail++;
    }
}

static void test_checks(void) {
    const anker_setter *s = anker_setter_by_name(ANKER_MODEL_A2687, "custom_mode");
    anker_val v;
    memset(&v, 0, sizeof v);
    v.is_obj = 1;
    v.have_c1 = v.have_c2 = v.have_c3 = 1;
    v.c1 = 150;
    v.c2 = 15;
    v.c3 = 0;
    anker_job job;
    const char *err = NULL;
    expect(s && anker_setter_check(s, &v, &job, &err) != 0, "150+15+0 exceeds 160 W");
    v.c1 = 140;
    expect(anker_setter_check(s, &v, &job, &err) == 0, "140+15+0 is inside 160 W");

    s = anker_setter_by_name(ANKER_MODEL_A2687, "protocols");
    memset(&v, 0, sizeof v);
    v.is_obj = 1;
    v.have_c1 = v.have_c2 = v.have_c3 = 1;
    v.c1 = 0x3c;
    expect(s && anker_setter_check(s, &v, &job, &err) != 0, "four PD bits refused");
    v.c1 = 0x1c;
    expect(anker_setter_check(s, &v, &job, &err) == 0, "three PD bits accepted");
}

typedef struct {
    const uint8_t *body;
    size_t n, off;
} walk_t;

typedef struct {
    int have_ac;
    int64_t ac_w;
    int have_c1;
    double c1_w;
} got_t;

static int next_host(void *st, anker_in *item) {
    walk_t *w = st;
    anker_tlv_item_t it;
    int nx = anker_tlv_next(w->body, w->n, w->off, &it);
    if (nx < 0) { return 0; }
    w->off = (size_t)nx;
    memset(item, 0, sizeof *item);
    item->tag = it.tag;
    item->raw = anker_tlv_raw(&it, w->body);
    item->raw_len = anker_tlv_raw_len(&it);
    if (it.val_len) {
        item->val = w->body + it.val_off;
        item->val_len = it.val_len;
    }
    if (it.type == ANKER_TLV_U8 || it.type == ANKER_TLV_U16 || it.type == ANKER_TLV_U32) {
        item->has_num = 1;
        item->ival = it.ival;
    }
    return 1;
}

static void eg_str(void *c, const char *k, const char *v) { (void)c; (void)k; (void)v; }
static void eg_i64(void *c, const char *k, int64_t v) {
    got_t *g = c;
    if (k && strcmp(k, "ac_output_power_w") == 0) { g->have_ac = 1; g->ac_w = v; }
}
static void eg_num(void *c, const char *k, double v) {
    got_t *g = c;
    if (k && strcmp(k, "c1_w") == 0) { g->have_c1 = 1; g->c1_w = v; }
}
static void eg_bool(void *c, const char *k, int v) { (void)c; (void)k; (void)v; }
static void eg_obj(void *c, const char *k) { (void)c; (void)k; }
static void eg_end(void *c) { (void)c; }

static void test_decode(void) {
    anker_tlv_spec_t sp;
    memset(&sp, 0, sizeof sp);
    sp.tag = 0xa6;
    sp.enc = ANKER_ENC_U16;
    sp.type_byte = -1;
    sp.ival = 250;
    uint8_t body[32];
    int n = anker_tlv_build_payload(&sp, 1, body, sizeof body);
    expect(n > 0, "a6 built");
    walk_t w = { body, (size_t)n, 0 };
    got_t g;
    memset(&g, 0, sizeof g);
    anker_emit em = { &g, eg_str, eg_i64, eg_num, eg_bool, eg_obj, eg_end, NULL };
    expect(anker_decode(ANKER_MODEL_A1761, next_host, &w, 0, &em, NULL) == 0, "a1761 decode");
    expect(g.have_ac && g.ac_w == 250, "a6 is ac_output_power_w");

    uint8_t port[7] = { 1, 0x10, 0x27, 0x00, 0x00, 0xdc, 0x05 };
    memset(&sp, 0, sizeof sp);
    sp.tag = 0xa5;
    sp.enc = ANKER_ENC_BYTES;
    sp.type_byte = -1;
    sp.data = port;
    sp.data_len = 7;
    n = anker_tlv_build_payload(&sp, 1, body, sizeof body);
    expect(n > 0, "port power built");
    w.body = body;
    w.n = (size_t)n;
    w.off = 0;
    memset(&g, 0, sizeof g);
    expect(anker_decode(ANKER_MODEL_A2687, next_host, &w, 0, &em, NULL) == 0, "a2687 decode");
    expect(g.have_c1 && g.c1_w > 14.9 && g.c1_w < 15.1, "c1_w from port power");
}

static void test_pinecil(void) {
    uint8_t b[56];
    memset(b, 0, sizeof b);
    b[8] = 50;
    b[48] = 1;
    pinecil_live_t t;
    expect(pinecil_decode(b, 56, &t) == 1, "pinecil decode");
    expect(t.input_v == 5.0, "input_v is raw/10");
    expect(pinecil_mode_name(t.mode) && strcmp(pinecil_mode_name(t.mode), "soldering") == 0, "mode name");
    expect(pinecil_decode(b, 55, &t) == 0, "short live rejected");
}

int main(void) {
    test_adv();
    test_country();
    test_keystream();
    test_setter_bytes();
    test_checks();
    test_decode();
    test_pinecil();
    if (g_fail) {
        fprintf(stderr, "%d failed\n", g_fail);
        return 1;
    }
    printf("ok\n");
    return 0;
}
