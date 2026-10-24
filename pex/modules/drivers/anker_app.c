/* Anker driver as one PEX app. Two sessions share it: slot 0 always, slot 1
 * only when target2 is set. Credentials come from cfg/anker.json through
 * pex_sys_secret, never from the start JSON and never from this file.
 *
 * The Lua profiles are the behavior spec. A plaintext 0x0021 is trap 1.
 * The link epoch base is 1700000000. Bind 0x0200's FE is bare, as the code
 * sends it. */
#include "pex.h"
#include "anker_adv.h"
#include "anker_wire.h"
#include "anker_models.h"
#include <string.h>

PEX_WEAK_IMPORT(pexlua_newtable);
PEX_WEAK_IMPORT(pexlua_register);
PEX_WEAK_IMPORT(pexlua_setfield);
PEX_WEAK_IMPORT(pexlua_getfield);
PEX_WEAK_IMPORT(pexlua_geti);
PEX_WEAK_IMPORT(pexlua_settop);
PEX_WEAK_IMPORT(pexlua_type);
PEX_WEAK_IMPORT(pexlua_checkstr);
PEX_WEAK_IMPORT(pexlua_checknum);
PEX_WEAK_IMPORT(pexlua_toboolean);
PEX_WEAK_IMPORT(pexlua_pushnum);
PEX_WEAK_IMPORT(pexlua_pushstr);
PEX_WEAK_IMPORT(pexlua_pushbool);
PEX_WEAK_IMPORT(pexlua_pushnil);
PEX_WEAK_IMPORT(pexlua_rawlen);

#define FT_ZX 0x01
#define FT_CH 0x0F
#define SVC_UUID "8c850001-0302-41c5-b46e-cf057c562025"
#define CMD_UUID "8c850002-0302-41c5-b46e-cf057c562025"
#define TEL_UUID "8c850003-0302-41c5-b46e-cf057c562025"

enum {
    ST_OFF = 0, ST_HUNT, ST_IDLE, ST_LOCK, ST_CONN, ST_DISC, ST_GAP,
    ST_NEG, ST_STREAM, ST_BACKOFF
};
enum { PH_IDLE = 0, PH_WAIT = 1 };
enum {
    NP_0001 = 0, NP_0003, NP_0029, NP_0005, NP_0021, NP_PROOF, NP_AUTH,
    NP_B0, NP_B1, NP_B2, NP_B3
};
enum { SUB_IDLE = 0, SUB_SET, SUB_POLL, SUB_SS, SUB_SS_NA, SUB_HIST };
enum { CIP_NONE = 0, CIP_GCM, CIP_ECDH, CIP_LEGACY, CIP_ACCOUNT };
enum { WHY_TIMEOUT = 1, WHY_LINK, WHY_STATUS };

typedef struct {
    uint8_t step, ack, mode, window;
    uint32_t id, crc, size;
    uint16_t n, plen, count, dlen;
    uint8_t data[200];
} ss_job_t;

typedef struct {
    anker_job job;
    uint16_t opcode;
    uint8_t no_readback, busy_ok, rb_kind;
    char field[24];
    int32_t rb_i, rb_c1, rb_c2, rb_c3;
} q_job_t;

typedef struct {
    char name[24];
    double v;
    uint8_t used;
} samp_t;

typedef struct {
    uint8_t on, st, gcm_hint, attempts, saw_link;
    uint8_t ev_up, ev_down, ev_fail, ev_ready;
    uint8_t fam_gcm, fam_try, neg_on, have_session, have_z;
    uint8_t cipher, keylen, cbc_cand;
    uint8_t phase, np, sub, fail_why;
    int8_t result;
    uint8_t check_nak, busy_ok, busy_used, accept_status;
    uint8_t got, got_status, streaming, first_report, logged_chr;
    uint8_t polling, tx_busy, no_wait, opaque;
    uint8_t qn, qh, ssn, ssh;
    uint8_t hist_arm, hist_nudges, hist_packets;
    uint16_t hist_records;
    uint32_t hist_seq;
    int cmd_chr, tel_chr;
    uint16_t want_op, body_len, tx_len;
    uint32_t t0, hunt_t0, deadline, wait_ms, tx_busy_t0;
    uint32_t next_poll, poll_period, link_check, sample_ms, last_look;
    uint16_t poll_ft, poll_op, poll_want;
    uint8_t addr[6];
    char target[24], addr_str[18], product_type[5], sku[8], code[8];
    char model_name[64], region[8], chip[32], fw[32], serial[32], serial_number[40];
    int model_id, inferred;
    uint8_t Z[32], key[32], iv[16];
    uint8_t peer[80];
    uint8_t peer_len;
    uint8_t reasm[PEX_CODEC_REASM_SIZE] __attribute__((aligned(8)));
    uint8_t body[640], pt[320], ct[420], tx[480];
    q_job_t q[4];
    ss_job_t ss[4];
    samp_t samp[64];
    uint8_t have_sample;
    struct { char field[24]; int32_t v, c1, c2, c3; uint8_t kind, used; } rb[4];
} sess_t;

static sess_t S[2];
static pex_bt_dev_t s_devs[8];
static uint8_t s_dec[640];
static char s_emit[3072];
static uint8_t g_priv[32];
static char g_user[48], g_tz[48], g_client[24], g_region[8];
static char g_target[24], g_target2[24];
static uint32_t g_tz_neg;
static int g_have_key, s_running, s_stop, s_task, s_listen = -1;
static int s_hs = -1;
static uint32_t s_hs_at;
static sess_t *s_pub;
static int s_depth;

int anker_tlv_emit(const anker_item *it, size_t n, uint8_t *out, size_t cap) {
    if (!it || !out || n > 12) { return -1; }
    pex_codec_spec_t sp[12];
    memset(sp, 0, sizeof sp);
    for (size_t i = 0; i < n; i++) {
        sp[i].tag = it[i].tag;
        sp[i].enc = it[i].enc;
        sp[i].type_byte = it[i].type_byte;
        sp[i].ival = it[i].ival;
        sp[i].data = it[i].data;
        sp[i].data_len = it[i].data_len;
    }
    return pex_codec_tlv_build(sp, n, out, cap);
}

static void ghash_wrap(const uint8_t H[16], const uint8_t *aad, size_t aad_len,
                        const uint8_t *ct, size_t ct_len, uint8_t out[16]) {
    pex_codec_ghash(H, aad, aad_len, ct, ct_len, out);
}

static int slot_of(const sess_t *s) { return (int)(s - S); }

static void copy_str(char *d, size_t cap, const char *s) {
    size_t i = 0;
    if (!s) { s = ""; }
    while (s[i] && i + 1 < cap) { d[i] = s[i]; i++; }
    d[i] = 0;
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') { return c - '0'; }
    if (c >= 'a' && c <= 'f') { return c - 'a' + 10; }
    if (c >= 'A' && c <= 'F') { return c - 'A' + 10; }
    return -1;
}

static int parse_key(const char *hex, uint8_t out[32]) {
    if (!hex || strlen(hex) != 64) { return -1; }
    for (int i = 0; i < 32; i++) {
        int h = hexval(hex[i * 2]), l = hexval(hex[i * 2 + 1]);
        if (h < 0 || l < 0) { return -1; }
        out[i] = (uint8_t)((h << 4) | l);
    }
    return 0;
}

static int parse_addr(const char *str, uint8_t out[6]) {
    int n = 0;
    if (!str) { return 0; }
    while (*str && n < 6) {
        if (*str == ':' || *str == '-' || *str == ' ') { str++; continue; }
        int h = hexval(*str++);
        int l = *str ? hexval(*str++) : -1;
        if (h < 0 || l < 0) { return 0; }
        out[n++] = (uint8_t)((h << 4) | l);
    }
    while (*str == ':' || *str == '-' || *str == ' ') { str++; }
    return n == 6 && *str == 0;
}

static int addr_eq(const char *a, const char *b) {
    uint8_t x[6], y[6];
    if (!parse_addr(a, x) || !parse_addr(b, y)) { return 0; }
    return memcmp(x, y, 6) == 0;
}

static int eq_ci(const char *a, const char *b) {
    if (!a || !b) { return 0; }
    while (*a && *b) {
        unsigned char ca = (unsigned char)*a, cb = (unsigned char)*b;
        if (ca >= 'a' && ca <= 'z') { ca = (unsigned char)(ca - 32); }
        if (cb >= 'a' && cb <= 'z') { cb = (unsigned char)(cb - 32); }
        if (ca != cb) { return 0; }
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

static int32_t parse_i32(const char *s) {
    if (!s || !*s) { return 0; }
    int neg = 0;
    if (*s == '-') { neg = 1; s++; }
    int32_t v = 0;
    while (*s >= '0' && *s <= '9') { v = (int32_t)(v * 10 + (*s++ - '0')); }
    return neg ? -v : v;
}

static const char *cip_name(int c) {
    switch (c) {
    case CIP_GCM: return "gcm";
    case CIP_ECDH: return "ecdh";
    case CIP_LEGACY: return "ecdh-legacy";
    case CIP_ACCOUNT: return "account";
    default: return "";
    }
}

static int hs_try(int slot) {
    if (s_hs == slot) { return 1; }
    if (s_hs >= 0) {
        if (pex_ms() - s_hs_at < 45000u) { return 0; }
        pex_log(PEX_LOG_WARN, "anker: handshake lock expired");
        s_hs = -1;
    }
    s_hs = slot;
    s_hs_at = pex_ms();
    return 1;
}

static void hs_release(int slot) {
    if (s_hs == slot) { s_hs = -1; }
}

static int match_target(const char *target, const uint8_t addr[6], const char *pt) {
    if (!target || !target[0]) { return 1; }
    uint8_t want[6];
    if (parse_addr(target, want)) { return memcmp(want, addr, 6) == 0; }
    return eq_ci(target, pt);
}

static int claimed_by_other(int self, const uint8_t addr[6]) {
    int o = self ^ 1;
    return S[o].on && S[o].st != ST_OFF && S[o].st != ST_HUNT && S[o].st != ST_IDLE
        && S[o].st != ST_BACKOFF && memcmp(S[o].addr, addr, 6) == 0;
}

static void samp_put(sess_t *s, const char *name, double v) {
    if (!name) { return; }
    for (int i = 0; i < 64; i++) {
        if (s->samp[i].used && strcmp(s->samp[i].name, name) == 0) {
            s->samp[i].v = v;
            return;
        }
    }
    for (int i = 0; i < 64; i++) {
        if (!s->samp[i].used) {
            copy_str(s->samp[i].name, sizeof s->samp[i].name, name);
            s->samp[i].v = v;
            s->samp[i].used = 1;
            return;
        }
    }
}

static int samp_get(const sess_t *s, const char *name, double *out) {
    for (int i = 0; i < 64; i++) {
        if (s->samp[i].used && strcmp(s->samp[i].name, name) == 0) {
            *out = s->samp[i].v;
            return 1;
        }
    }
    return 0;
}

static void em_str(void *ctx, const char *key, const char *v) {
    pex_jw_str(ctx, key, v);
    if (s_pub && key && strcmp(key, "serial_number") == 0) {
        copy_str(s_pub->serial_number, sizeof s_pub->serial_number, v);
    }
}
static void em_i64(void *ctx, const char *key, int64_t v) {
    pex_jw_int(ctx, key, v);
    if (s_depth == 0 && s_pub) { samp_put(s_pub, key, (double)v); }
}
static void em_num(void *ctx, const char *key, double v) {
    pex_jw_num(ctx, key, v);
    if (s_depth == 0 && s_pub) { samp_put(s_pub, key, v); }
}
static void em_bool(void *ctx, const char *key, int v) { pex_jw_bool(ctx, key, v); }
static void em_obj(void *ctx, const char *key) { s_depth++; pex_jw_obj(ctx, key); }
static void em_end(void *ctx) { if (s_depth) { s_depth--; } pex_jw_end(ctx); }
static void em_hex(void *ctx, const char *key, const void *p, size_t n) { pex_jw_hex(ctx, key, p, n); }

typedef struct { const uint8_t *p; size_t n; } walk_t;

static int next_pex(void *st, anker_in *item) {
    walk_t *w = st;
    pex_codec_tlv_t t;
    if (pex_codec_tlv_next(&w->p, &w->n, &t) != 1) { return 0; }
    memset(item, 0, sizeof *item);
    item->tag = t.tag;
    item->raw = t.raw;
    item->raw_len = t.raw_len;
    item->val = t.val;
    item->val_len = t.val_len;
    if (t.type == PEX_CODEC_TLV_U8 || t.type == PEX_CODEC_TLV_U16 || t.type == PEX_CODEC_TLV_U32) {
        item->has_num = 1;
        item->ival = t.ival;
    }
    return 1;
}

static void add_identity(pex_jw_t *w, sess_t *s, int with_hex, const uint8_t *body, size_t n) {
    pex_jw_str(w, "addr", s->addr_str);
    pex_jw_str(w, "product_type", s->product_type);
    if (s->code[0]) { pex_jw_str(w, "product_code", s->code); }
    if (s->model_name[0]) { pex_jw_str(w, "model_name", s->model_name); }
    if (s->region[0]) { pex_jw_str(w, "region", s->region); }
    if (s->cipher) { pex_jw_str(w, "cipher", cip_name(s->cipher)); }
    if (s->serial_number[0]) { pex_jw_str(w, "serial", s->serial_number); }
    else if (s->serial[0]) { pex_jw_str(w, "serial", s->serial); }
    for (int i = 0; i < 4; i++) {
        if (!s->rb[i].used) { continue; }
        if (s->rb[i].kind == ANKER_JOB_PROTO) {
            pex_jw_obj(w, s->rb[i].field);
            pex_jw_int(w, "c1", s->rb[i].c1);
            pex_jw_int(w, "c2", s->rb[i].c2);
            pex_jw_int(w, "c3", s->rb[i].c3);
            pex_jw_end(w);
        } else {
            pex_jw_int(w, s->rb[i].field, s->rb[i].v);
        }
    }
    if (with_hex && s->first_report && body && n) {
        size_t m = n > 256 ? 256 : n;
        pex_jw_hex(w, "body_hex", body, m);
    }
}

static void publish(sess_t *s, const uint8_t *body, size_t n) {
    anker_emit em = { NULL, em_str, em_i64, em_num, em_bool, em_obj, em_end, em_hex };
    for (int pass = 0; pass < 2; pass++) {
        pex_jw_t w;
        pex_jw_init(&w, s_emit, sizeof s_emit);
        pex_jw_obj(&w, NULL);
        em.ctx = &w;
        s_pub = s;
        s_depth = 0;
        s->serial_number[0] = 0;
        if (s->model_id && body) {
            walk_t wk = { body, n };
            int count = 0;
            anker_decode(s->model_id, next_pex, &wk, s->first_report && pass == 0, &em, &count);
            pex_jw_int(&w, "tlv_count", count);
        } else if (body) {
            size_t m = n > 128 ? 128 : n;
            pex_jw_hex(&w, "raw", body, m);
        }
        add_identity(&w, s, pass == 0, body, n);
        pex_jw_end(&w);
        int len = pex_jw_finish(&w);
        if (len >= 0) {
            pex_bus_emit("profile.anker.telemetry", s_emit);
            s->sample_ms = pex_ms();
            s->have_sample = 1;
            s->first_report = 0;
            return;
        }
    }
    pex_log(PEX_LOG_WARN, "anker: telemetry did not fit");
}

static int pump(sess_t *s) {
    if (!s->tx_len) { return 1; }
    int rc = pex_bt_write(s->addr, s->cmd_chr, s->tx, s->tx_len, 1, 0);
    if (rc == PEX_EBUSY) {
        if (!s->tx_busy) { s->tx_busy = 1; s->tx_busy_t0 = pex_ms(); }
        if (pex_ms() - s->tx_busy_t0 > 2000u) {
            s->tx_len = 0;
            s->tx_busy = 0;
            return -1;
        }
        return 0;
    }
    s->tx_busy = 0;
    s->tx_len = 0;
    return rc < 0 ? -1 : 1;
}

static int seal(sess_t *s, uint8_t ft, uint16_t op, const uint8_t *pt, size_t n) {
    const uint8_t *pay = pt;
    size_t pn = n;
    int enc = 0;
    if (s->have_session) {
        int rc;
        if (s->cipher == CIP_GCM) {
            rc = pex_codec_gcm_encrypt(s->key, 16, s->iv, anker_aad(), 16, pt, n, s->ct, sizeof s->ct);
        } else {
            size_t ol = 0;
            rc = pex_codec_cbc(1, s->key, s->keylen, s->iv, pt, n, s->ct, sizeof s->ct, &ol);
            if (rc >= 0) { rc = (int)ol; }
        }
        if (rc < 0) { return -1; }
        pay = s->ct;
        pn = (size_t)rc;
        enc = 1;
    } else if (s->neg_on) {
        int rc = anker_neg_encrypt(pt, n, s->ct, sizeof s->ct);
        if (rc < 0) { return -1; }
        pay = s->ct;
        pn = (size_t)rc;
        enc = 1;
    }
    int fl = pex_codec_frame_build(ft, op, enc, 0, pay, pn, s->tx, sizeof s->tx);
    if (fl < 0) { return -1; }
    s->tx_len = (uint16_t)fl;
    s->tx_busy = 0;
    return pump(s);
}

static int launch(sess_t *s, uint8_t ft, uint16_t op, const uint8_t *pt, size_t n,
                   uint16_t want, uint32_t ms, int nak, int busy, int accept) {
    s->want_op = want;
    s->wait_ms = ms;
    s->check_nak = (uint8_t)nak;
    s->busy_ok = (uint8_t)busy;
    s->busy_used = 0;
    s->accept_status = (uint8_t)accept;
    s->got = 0;
    s->no_wait = 0;
    int rc = seal(s, ft, op, pt, n);
    if (rc < 0) { return -1; }
    if (rc == 0) { s->phase = PH_IDLE; return 0; }
    s->deadline = pex_ms() + ms;
    s->phase = PH_WAIT;
    return 0;
}

static int poll_wait(sess_t *s) {
    if (s->ev_fail || (s->ev_down && s->saw_link)) {
        s->ev_down = 0;
        s->ev_fail = 0;
        s->fail_why = WHY_LINK;
        return -1;
    }
    if (!s->got) {
        if ((int32_t)(pex_ms() - s->deadline) >= 0) {
            s->fail_why = WHY_TIMEOUT;
            return -1;
        }
        return 0;
    }
    if (s->check_nak && anker_nak_poison(s->body, s->body_len)) {
        pex_log(PEX_LOG_ERR, "anker: nak poison (0x0021 was plaintext)");
        s->got = 0;
        s->fail_why = WHY_STATUS;
        return -1;
    }
    if (s->got_status == 4 && s->busy_ok && !s->busy_used) {
        s->busy_used = 1;
        s->got = 0;
        s->deadline = pex_ms() + 3000u;
        return 0;
    }
    if (!s->accept_status && s->got_status != 0) {
        pex_log(PEX_LOG_WARN, "anker: status %u on %04x", s->got_status, s->want_op);
        s->got = 0;
        s->fail_why = WHY_STATUS;
        return -1;
    }
    s->got = 0;
    return 1;
}

static int service_tx(sess_t *s) {
    if (s->phase == PH_WAIT) {
        int r = poll_wait(s);
        if (r == 0) { return 1; }
        s->phase = PH_IDLE;
        s->result = (int8_t)(r < 0 ? -1 : 1);
        return 0;
    }
    if (s->tx_len) {
        int p = pump(s);
        if (p == 0) { return 1; }
        if (p < 0) {
            s->phase = PH_IDLE;
            s->result = -1;
            s->fail_why = WHY_STATUS;
            return 0;
        }
        if (s->no_wait) {
            s->no_wait = 0;
            s->phase = PH_IDLE;
            s->result = 1;
            return 0;
        }
        s->got = 0;
        s->busy_used = 0;
        s->deadline = pex_ms() + s->wait_ms;
        s->phase = PH_WAIT;
        return 1;
    }
    return 0;
}

static void take_neg_str(char *dst, size_t cap, const pex_codec_tlv_t *it) {
    size_t n = it->val_len;
    if (n >= cap) { n = cap - 1; }
    if (it->val && n) { memcpy(dst, it->val, n); }
    dst[n] = 0;
}

static void take_info(sess_t *s) {
    const uint8_t *p = s->body;
    size_t n = s->body_len;
    pex_codec_tlv_t it;
    while (pex_codec_tlv_neg(&p, &n, &it) == 1) {
        if (it.tag == 0xa2) { take_neg_str(s->chip, sizeof s->chip, &it); }
        else if (it.tag == 0xa3) { take_neg_str(s->fw, sizeof s->fw, &it); }
        else if (it.tag == 0xa4) { take_neg_str(s->serial, sizeof s->serial, &it); }
    }
}

static int take_peer(sess_t *s) {
    const uint8_t *p = s->body;
    size_t n = s->body_len;
    pex_codec_tlv_t it;
    while (pex_codec_tlv_neg(&p, &n, &it) == 1) {
        if (it.tag == 0xa1 && it.val && it.val_len >= 64) {
            size_t m = it.val_len > sizeof s->peer ? sizeof s->peer : it.val_len;
            memcpy(s->peer, it.val, m);
            s->peer_len = (uint8_t)m;
            return 0;
        }
    }
    return -1;
}

static int item_bytes(anker_item *it, uint8_t tag, const uint8_t *d, uint16_t n) {
    memset(it, 0, sizeof *it);
    it->tag = tag;
    it->enc = ANKER_ENC_BYTES;
    it->type_byte = -2;
    it->data = d;
    it->data_len = n;
    return 0;
}

static int emit_launch(sess_t *s, uint8_t ft, uint16_t op, anker_item *it, size_t ni,
                        uint16_t want, uint32_t ms, int nak, int busy, int accept) {
    int n = anker_tlv_emit(it, ni, s->pt, sizeof s->pt);
    if (n < 0) { return -1; }
    return launch(s, ft, op, s->pt, (size_t)n, want, ms, nak, busy, accept);
}

static void enter_stream(sess_t *s) {
    hs_release(slot_of(s));
    s->st = ST_STREAM;
    s->sub = SUB_IDLE;
    s->phase = PH_IDLE;
    s->streaming = 1;
    s->first_report = 1;
    s->want_op = 0;
    s->result = 0;
    s->tx_len = 0;
    s->link_check = pex_ms();
    if (s->polling) { s->next_poll = pex_ms(); }
    if (s->model_id && !anker_model_verified(s->model_id)) {
        pex_log(PEX_LOG_INFO, "anker: schema %s loaded (UNVERIFIED -- field names are guesses)", s->code);
    }
    pex_log(PEX_LOG_INFO, "anker: streaming from %s (%s) cipher %s", s->addr_str,
            s->code[0] ? s->code : "?", cip_name(s->cipher));
}

static void drop_link(sess_t *s) {
    pex_bt_disconnect(s->addr);
    hs_release(slot_of(s));
    s->have_session = 0;
    s->neg_on = 0;
    s->streaming = 0;
    s->tx_len = 0;
    s->phase = PH_IDLE;
    s->st = ST_BACKOFF;
    s->t0 = pex_ms();
    s->saw_link = 0;
    pex_log(PEX_LOG_WARN, "anker: link dropped, looking again");
}

static void fail_attempt(sess_t *s) {
    pex_bt_disconnect(s->addr);
    s->attempts++;
    s->saw_link = 0;
    s->tx_len = 0;
    s->have_session = 0;
    s->neg_on = 0;
    if (s->attempts < 4) {
        s->st = ST_GAP;
        s->t0 = pex_ms();
        return;
    }
    hs_release(slot_of(s));
    s->attempts = 0;
    s->st = ST_BACKOFF;
    s->t0 = pex_ms();
    pex_log(PEX_LOG_WARN, "anker: connect failed, backing off");
}

static void family_fail(sess_t *s) {
    s->have_session = 0;
    s->neg_on = 0;
    s->tx_len = 0;
    pex_codec_reasm_reset(s->reasm);
    if (s->fam_try == 0) {
        int was_gcm = s->fam_gcm;
        s->fam_try = 1;
        s->fam_gcm = (uint8_t)!s->fam_gcm;
        s->neg_on = s->fam_gcm;
        s->np = NP_0001;
        s->cbc_cand = 0;
        s->phase = PH_IDLE;
        s->result = 0;
        pex_log(PEX_LOG_WARN, "anker: %s did not take, trying %s",
                was_gcm ? "gcm" : "cbc", s->fam_gcm ? "gcm" : "cbc");
        return;
    }
    pex_log(PEX_LOG_ERR, "anker: no cipher produced a session");
    fail_attempt(s);
}

static int install_cbc(sess_t *s) {
    while (s->cbc_cand < 3) {
        if (s->cbc_cand == 0) {
            uint8_t kdf[64];
            if (pex_codec_kdf(s->Z, kdf, 64) >= 0) {
                memcpy(s->key, kdf, 32);
                memcpy(s->iv, kdf + 32, 16);
                s->keylen = 32;
                s->cipher = CIP_ECDH;
                s->have_session = 1;
                return 1;
            }
            s->cbc_cand++;
            continue;
        }
        if (s->cbc_cand == 1) {
            memcpy(s->key, s->Z, 16);
            memcpy(s->iv, s->Z + 16, 16);
            s->keylen = 16;
            s->cipher = CIP_LEGACY;
            s->have_session = 1;
            return 1;
        }
        if (!g_user[0] || pex_codec_account_key(g_user, s->key, s->iv) < 0) { return 0; }
        s->keylen = 32;
        s->cipher = CIP_ACCOUNT;
        s->have_session = 1;
        return 1;
    }
    return 0;
}

static int probe_body_ok(const sess_t *s) {
    if (s->body_len < 2) { return 0; }
    uint8_t tag = s->body[0];
    return tag >= 0xA0 || tag == 0x00;
}

static void after_auth(sess_t *s) {
    if (anker_model_post_auth(s->model_id) && g_user[0]) {
        s->np = NP_B0;
        s->phase = PH_IDLE;
        return;
    }
    if (anker_model_post_auth(s->model_id)) {
        pex_log(PEX_LOG_WARN, "anker: this model needs the bind burst and cfg has no user_id");
    }
    enter_stream(s);
}

static void on_neg_ok(sess_t *s) {
    uint8_t epb[4];
    anker_le32(anker_epoch(pex_ms()), epb);
    switch (s->np) {
    case NP_0029:
        take_info(s);
        s->np = NP_0005;
        break;
    case NP_0021:
        if (take_peer(s) < 0 || pex_codec_ecdh(g_priv, s->peer, s->peer_len, s->Z) != 32) {
            pex_log(PEX_LOG_WARN, "anker: no device public key");
            family_fail(s);
            return;
        }
        s->have_z = 1;
        if (s->fam_gcm) {
            memcpy(s->key, s->Z, 16);
            memset(s->iv, 0, sizeof s->iv);
            memcpy(s->iv, s->Z + 16, 12);
            s->keylen = 16;
            s->cipher = CIP_GCM;
            s->have_session = 1;
        } else {
            s->cbc_cand = 0;
            s->have_session = 0;
        }
        s->np = NP_PROOF;
        break;
    case NP_PROOF:
        if (!s->fam_gcm && !probe_body_ok(s)) {
            s->have_session = 0;
            s->cbc_cand++;
            if (!install_cbc(s)) { family_fail(s); return; }
            break;
        }
        pex_log(PEX_LOG_INFO, "anker: session cipher is %s (%u-byte key)", cip_name(s->cipher), s->keylen);
        s->np = NP_AUTH;
        break;
    case NP_AUTH:
        s->np = NP_AUTH;
        after_auth(s);
        return;
    case NP_B0: s->np = NP_B1; break;
    case NP_B1: s->np = NP_B2; break;
    case NP_B2: s->np = NP_B3; break;
    case NP_B3:
        pex_log(PEX_LOG_INFO, "anker: bind burst accepted");
        enter_stream(s);
        return;
    default:
        s->np++;
        break;
    }
    (void)epb;
    s->phase = PH_IDLE;
}

static void on_neg_fail(sess_t *s) {
    if (s->np == NP_PROOF && !s->fam_gcm) {
        s->have_session = 0;
        s->cbc_cand++;
        if (install_cbc(s)) { s->phase = PH_IDLE; return; }
        family_fail(s);
        return;
    }
    if (s->np == NP_AUTH) {
        pex_log(PEX_LOG_WARN, "anker: 0x0027 was not accepted; the device may refuse to report");
        after_auth(s);
        return;
    }
    if (s->np >= NP_B0) {
        pex_log(PEX_LOG_WARN, "anker: bind burst step failed");
        enter_stream(s);
        return;
    }
    family_fail(s);
}

static int build_neg(sess_t *s) {
    uint32_t now = anker_epoch(pex_ms());
    uint8_t ep[4], ep2[4], tzv[4];
    anker_le32(now, ep);
    anker_le32(now + 2, ep2);
    anker_le32(g_tz_neg, tzv);
    anker_item it[6];
    size_t ni = 0;
    if (s->np == NP_0001) {
        item_bytes(&it[ni++], 0xa1, ep, 4);
        if (!s->fam_gcm) { item_bytes(&it[ni++], 0xa2, (const uint8_t *)g_client, (uint16_t)strlen(g_client)); }
        return emit_launch(s, FT_ZX, 0x0001, it, ni, 0x0801, 3000, 0, 0, 0);
    }
    if (s->np == NP_0003) {
        static const uint8_t a3[] = { 0x20 }, a4[] = { 0x00, 0xf0 };
        item_bytes(&it[ni++], 0xa1, ep, 4);
        item_bytes(&it[ni++], 0xa3, a3, 1);
        item_bytes(&it[ni++], 0xa4, a4, 2);
        return emit_launch(s, FT_ZX, 0x0003, it, ni, 0x0803, 3000, 0, 0, 0);
    }
    if (s->np == NP_0029) {
        item_bytes(&it[ni++], 0xa1, ep, 4);
        return emit_launch(s, FT_ZX, 0x0029, it, ni, 0x0829, 3000, 0, 0, 0);
    }
    if (s->np == NP_0005) {
        static const uint8_t a3[] = { 0x20 }, a4[] = { 0x29, 0x01 }, a5[] = { 0x44 }, a6[] = { 0x02 };
        item_bytes(&it[ni++], 0xa1, ep, 4);
        item_bytes(&it[ni++], 0xa3, a3, 1);
        item_bytes(&it[ni++], 0xa4, a4, 2);
        item_bytes(&it[ni++], 0xa5, a5, 1);
        if (s->fam_gcm) { item_bytes(&it[ni++], 0xa6, a6, 1); }
        return emit_launch(s, FT_ZX, 0x0005, it, ni, 0x0805, 3000, 0, 0, 0);
    }
    if (s->np == NP_0021) {
        uint8_t xy[64];
        if (pex_codec_pubkey(g_priv, xy) != 64) { return -1; }
        item_bytes(&it[0], 0xa1, xy, 64);
        return emit_launch(s, FT_ZX, 0x0021, it, 1, 0x0821, 8000, 0, 0, 0);
    }
    if (s->np == NP_PROOF && s->fam_gcm) {
        item_bytes(&it[ni++], 0xa1, ep2, 4);
        item_bytes(&it[ni++], 0xa3, tzv, 4);
        item_bytes(&it[ni++], 0xa5, (const uint8_t *)g_tz, (uint16_t)strlen(g_tz));
        return emit_launch(s, FT_ZX, 0x0022, it, ni, 0x0822, 4000, 1, 0, 0);
    }
    if (s->np == NP_PROOF) {
        if (!s->have_session && !install_cbc(s)) { return -1; }
        static const uint8_t sel[] = { 0x21 };
        item_bytes(&it[0], 0xa1, sel, 1);
        return emit_launch(s, FT_CH, 0x0040, it, 1, 0x0840, 4000, 0, 0, 0);
    }
    if (s->np == NP_AUTH) {
        if (!g_user[0]) { after_auth(s); return 0; }
        item_bytes(&it[ni++], 0xa1, ep2, 4);
        item_bytes(&it[ni++], 0xa2, (const uint8_t *)g_user, (uint16_t)strlen(g_user));
        return emit_launch(s, FT_ZX, 0x0027, it, ni, 0x0827, 4000, 0, 0, 0);
    }
    if (s->np == NP_B0) {
        anker_item b[3];
        memset(b, 0, sizeof b);
        b[0].tag = 0xa1; b[0].enc = ANKER_ENC_U8; b[0].type_byte = -2; b[0].ival = 0x21;
        b[1].tag = 0xf0; b[1].enc = ANKER_ENC_U32; b[1].type_byte = 0x03; b[1].ival = 1;
        item_bytes(&b[2], 0xfe, ep, 4);
        return emit_launch(s, FT_CH, 0x0200, b, 3, 0x0A00, 4000, 0, 0, 0);
    }
    if (s->np == NP_B1) {
        item_bytes(&it[0], 0xa1, ep, 4);
        item_bytes(&it[1], 0xa2, (const uint8_t *)g_user, (uint16_t)strlen(g_user));
        return emit_launch(s, FT_CH, 0x0030, it, 2, 0x0830, 4000, 0, 0, 0);
    }
    if (s->np == NP_B2) {
        anker_item b[5];
        memset(b, 0, sizeof b);
        b[0].tag = 0xa1; b[0].enc = ANKER_ENC_U8; b[0].type_byte = -2; b[0].ival = 0x21;
        b[1].tag = 0xa2; b[1].enc = ANKER_ENC_STRING; b[1].type_byte = 0x04;
        b[1].data = (const uint8_t *)g_region; b[1].data_len = (uint16_t)strlen(g_region);
        b[2].tag = 0xa3; b[2].enc = ANKER_ENC_STRING; b[2].type_byte = 0x04;
        b[2].data = (const uint8_t *)g_user; b[2].data_len = (uint16_t)strlen(g_user);
        b[3].tag = 0xa5; b[3].enc = ANKER_ENC_U8; b[3].type_byte = 0x01; b[3].ival = 1;
        item_bytes(&b[4], 0xfe, ep, 4);
        return emit_launch(s, FT_CH, 0x020A, b, 5, 0x0A0A, 4000, 0, 0, 0);
    }
    if (s->np == NP_B3) {
        static const uint8_t timers[] = { 0x3b, 0x00, 0x3b, 0x00, 0x3b, 0x00 };
        anker_item b[2];
        memset(b, 0, sizeof b);
        b[0].tag = 0xa1; b[0].enc = ANKER_ENC_U8; b[0].type_byte = -2; b[0].ival = 0x21;
        b[1].tag = 0xa2; b[1].enc = ANKER_ENC_BYTES; b[1].type_byte = 0x04;
        b[1].data = timers; b[1].data_len = sizeof timers;
        return emit_launch(s, FT_CH, 0x021D, b, 2, 0x0A1D, 4000, 0, 0, 0);
    }
    return -1;
}

static void begin_neg(sess_t *s) {
    s->st = ST_NEG;
    s->np = NP_0001;
    s->phase = PH_IDLE;
    s->result = 0;
    s->tx_len = 0;
    s->fam_try = 0;
    s->fam_gcm = s->gcm_hint ? 1 : 0;
    s->neg_on = s->fam_gcm;
    s->have_session = 0;
    s->cbc_cand = 0;
    pex_codec_reasm_reset(s->reasm);
    pex_log(PEX_LOG_INFO, "anker: link up, negotiating %s first", s->fam_gcm ? "gcm" : "cbc");
}

static void start_connect(sess_t *s) {
    pex_bt_disconnect(s->addr);
    s->ev_up = s->ev_down = s->ev_fail = s->ev_ready = 0;
    s->saw_link = 0;
    s->cmd_chr = s->tel_chr = -1;
    int rc = pex_bt_connect(s->addr);
    if (rc == PEX_OK) { s->saw_link = 1; }
    else if (rc < 0 && rc != PEX_EPENDING) { fail_attempt(s); return; }
    s->st = ST_CONN;
    s->t0 = pex_ms();
}

static void hunt_look(sess_t *s) {
    int n = pex_bt_devices(s_devs, 8, NULL, NULL);
    if (n < 0) { n = 0; }
    if (n > 8) { n = 8; }
    for (int i = 0; i < n; i++) {
        int ff09 = 0;
        for (uint8_t u = 0; u < s_devs[i].n_uuid16 && u < 4; u++) {
            if (s_devs[i].uuid16[u] == 0xFF09) { ff09 = 1; }
        }
        anker_adv_t a;
        if (!anker_adv_identify(s_devs[i].mfg, s_devs[i].mfg_len, s_devs[i].addr, ff09, &a, NULL)) {
            continue;
        }
        if (claimed_by_other(slot_of(s), s_devs[i].addr)) { continue; }
        if (!match_target(s->target, s_devs[i].addr, a.product_type)) { continue; }
        memcpy(s->addr, s_devs[i].addr, 6);
        pex_bt_addr_str(s->addr, s->addr_str);
        copy_str(s->product_type, sizeof s->product_type, a.product_type);
        copy_str(s->sku, sizeof s->sku, a.sku);
        s->gcm_hint = (uint8_t)(a.gcm_hint ? 1 : 0);
        s->code[0] = s->model_name[0] = s->region[0] = 0;
        s->inferred = 0;
        if (anker_lookup(a.product_type, s->code, s->model_name, &s->inferred)) {
            const char *cc = anker_country(s->code, s->sku);
            if (cc) { copy_str(s->region, sizeof s->region, cc); }
            pex_log(PEX_LOG_INFO, "anker: %s is %s%s%s", a.product_type, s->code,
                    s->model_name[0] ? " " : "", s->model_name);
            if (s->inferred) { pex_log(PEX_LOG_WARN, "anker: that mapping is marked inferred"); }
        } else {
            pex_log(PEX_LOG_WARN, "anker: productType %s is not in the table", a.product_type);
        }
        s->model_id = anker_model_id(s->code);
        s->polling = (uint8_t)anker_model_poll(s->model_id, &s->poll_ft, &s->poll_op, &s->poll_want, &s->poll_period);
        if (!s->model_id && s->code[0]) {
            pex_log(PEX_LOG_WARN, "anker: no schema for %s; reporting raw tags", s->code);
        }
        memset(s->samp, 0, sizeof s->samp);
        s->have_sample = 0;
        s->st = ST_LOCK;
        return;
    }
}

static void note_readback(sess_t *s, const q_job_t *j) {
    if (!j->no_readback) { return; }
    int slot = -1;
    for (int i = 0; i < 4; i++) {
        if (s->rb[i].used && strcmp(s->rb[i].field, j->field) == 0) { slot = i; break; }
        if (slot < 0 && !s->rb[i].used) { slot = i; }
    }
    if (slot < 0) { return; }
    copy_str(s->rb[slot].field, sizeof s->rb[slot].field, j->field);
    s->rb[slot].kind = j->rb_kind;
    s->rb[slot].v = j->rb_i;
    s->rb[slot].c1 = j->rb_c1;
    s->rb[slot].c2 = j->rb_c2;
    s->rb[slot].c3 = j->rb_c3;
    s->rb[slot].used = 1;
}

static void hist_emit(sess_t *s, int done, const char *err, const uint8_t *rec, size_t rn,
                       int n, int last, int span) {
    pex_jw_t w;
    pex_jw_init(&w, s_emit, sizeof s_emit);
    pex_jw_obj(&w, NULL);
    pex_jw_str(&w, "addr", s->addr_str);
    pex_jw_int(&w, "id", (int64_t)s->hist_seq);
    if (err) {
        pex_jw_str(&w, "error", err);
        pex_jw_bool(&w, "done", 1);
        if (done) {
            pex_jw_int(&w, "packets", s->hist_packets);
            pex_jw_int(&w, "records", s->hist_records);
        }
    } else if (done) {
        pex_jw_bool(&w, "done", 1);
        pex_jw_int(&w, "packets", s->hist_packets);
        pex_jw_int(&w, "records", s->hist_records);
    } else {
        pex_jw_int(&w, "seq", s->hist_packets);
        pex_jw_int(&w, "n", n);
        pex_jw_bool(&w, "last", last);
        pex_jw_int(&w, "span_min", span);
        pex_jw_int(&w, "interval_s", 300);
        if (rec && rn) { pex_jw_hex(&w, "hex", rec, rn); }
    }
    pex_jw_end(&w);
    if (pex_jw_finish(&w) >= 0) { pex_bus_emit("profile.anker.history", s_emit); }
}

static int hist_parse(const uint8_t *body, size_t len, int *last, int *n, int *span,
                       const uint8_t **rec, size_t *rn) {
    const uint8_t *p = body;
    size_t left = len;
    pex_codec_tlv_t it;
    int have_n = 0, have_span = 0, lv = 0;
    const uint8_t *data = NULL;
    size_t dn = 0;
    while (pex_codec_tlv_next(&p, &left, &it) == 1) {
        if (it.tag == 0xa2 && it.type == PEX_CODEC_TLV_U8) { lv = (int)it.ival; }
        else if (it.tag == 0xa3 && (it.type == PEX_CODEC_TLV_U8 || it.type == PEX_CODEC_TLV_U16)) {
            have_n = 1; *n = (int)it.ival;
        } else if (it.tag == 0xa4) {
            have_span = 1; *span = (int)it.ival;
        } else if (it.tag == 0xa5 && it.val) {
            data = it.val; dn = it.val_len;
        }
    }
    if (!have_n || !data) { return -1; }
    if (*n < 0) { *n = 0; }
    if ((size_t)*n > dn / 12) { *n = (int)(dn / 12); }
    *last = lv == 1;
    if (!have_span) { *span = 0; }
    *rec = data;
    *rn = (size_t)*n * 12;
    return 0;
}

static void hist_arm_wait(sess_t *s) {
    s->want_op = 0x0A0C;
    s->wait_ms = 400;
    s->check_nak = 0;
    s->busy_ok = 0;
    s->accept_status = 0;
    s->got = 0;
    s->deadline = pex_ms() + 400;
    s->phase = PH_WAIT;
    s->sub = SUB_HIST;
}

static int hist_send(sess_t *s, int more) {
    int n = anker_history_request(more, anker_epoch(pex_ms()), s->pt, sizeof s->pt);
    if (n < 0) { return -1; }
    s->sub = SUB_HIST;
    return launch(s, FT_CH, 0x020C, s->pt, (size_t)n, 0x0A0C, 400, 0, 0, 0);
}

static void on_hist(sess_t *s, int ok) {
    if (!ok) {
        if (s->fail_why != WHY_TIMEOUT) {
            hist_emit(s, 1, "link", NULL, 0, 0, 0, 0);
            s->hist_arm = 0;
            s->sub = SUB_IDLE;
            if (s->fail_why == WHY_LINK) { drop_link(s); }
            return;
        }
        s->hist_nudges++;
        if (s->hist_nudges > 4) {
            hist_emit(s, 1, "no more packets", NULL, 0, 0, 0, 0);
            s->hist_arm = 0;
            s->sub = SUB_IDLE;
            return;
        }
        if (hist_send(s, 1) < 0) {
            hist_emit(s, 1, "write refused", NULL, 0, 0, 0, 0);
            s->hist_arm = 0;
            s->sub = SUB_IDLE;
        }
        return;
    }
    int last = 0, n = 0, span = 0;
    const uint8_t *rec = NULL;
    size_t rn = 0;
    if (hist_parse(s->body, s->body_len, &last, &n, &span, &rec, &rn) == 0) {
        s->hist_packets++;
        s->hist_records = (uint16_t)(s->hist_records + n);
        s->hist_nudges = 0;
        hist_emit(s, 0, NULL, rec, rn, n, last, span);
        if (last || s->hist_packets >= 40) {
            hist_emit(s, 1, s->hist_packets >= 40 ? "too many packets" : NULL, NULL, 0, 0, 0, 0);
            s->hist_arm = 0;
            s->sub = SUB_IDLE;
            return;
        }
    }
    hist_arm_wait(s);
}

static const char *ss_step_name(uint8_t step) {
    if (step == 1) { return "select"; }
    if (step == 2) { return "begin"; }
    return "data";
}

static void ss_emit(sess_t *s, const ss_job_t *j, int ok) {
    pex_jw_t w;
    pex_jw_init(&w, s_emit, sizeof s_emit);
    pex_jw_obj(&w, NULL);
    pex_jw_str(&w, "addr", s->addr_str);
    pex_jw_str(&w, "step", ss_step_name(j->step));
    if (j->step != 3) { pex_jw_int(&w, "id", j->id); }
    if (j->step == 3) { pex_jw_int(&w, "n", j->n); }
    if (ok) {
        pex_jw_int(&w, "status", s->got_status);
        if (j->step == 3) {
            int rec = anker_ss_received(s->body, s->body_len);
            if (rec >= 0) { pex_jw_int(&w, "received", rec); }
        }
    } else {
        pex_jw_str(&w, "error", s->fail_why == WHY_TIMEOUT ? "timeout" : "no reply");
    }
    pex_jw_end(&w);
    if (pex_jw_finish(&w) >= 0) { pex_bus_emit("profile.anker.screensaver", s_emit); }
}

static void dequeue_ss(sess_t *s) {
    if (s->ssn) {
        s->ssh = (uint8_t)((s->ssh + 1) % 4);
        s->ssn--;
    }
    s->sub = SUB_IDLE;
}

static void on_stream_result(sess_t *s, int ok) {
    if (s->sub == SUB_HIST) { on_hist(s, ok); return; }
    if (s->sub == SUB_SET) {
        q_job_t *j = &s->q[s->qh];
        if (ok) {
            pex_log(PEX_LOG_INFO, "anker: set %s accepted", j->field);
            note_readback(s, j);
        } else {
            pex_log(PEX_LOG_WARN, "anker: set %s failed", j->field);
        }
        if (s->qn) {
            s->qh = (uint8_t)((s->qh + 1) % 4);
            s->qn--;
        }
    } else if (s->sub == SUB_POLL) {
        if (ok) { publish(s, s->body, s->body_len); }
        else {
            pex_log(PEX_LOG_WARN, "anker: poll failed");
            int ready = 0;
            if (s->fail_why == WHY_LINK || pex_bt_state(s->addr, &ready) != PEX_BT_CONNECTED) {
                drop_link(s);
                return;
            }
        }
        s->next_poll = pex_ms() + (s->poll_period ? s->poll_period : 5000u);
    } else if (s->sub == SUB_SS) {
        if (s->ssn) { ss_emit(s, &s->ss[s->ssh], ok); }
        dequeue_ss(s);
        return;
    } else if (s->sub == SUB_SS_NA) {
        dequeue_ss(s);
        return;
    }
    s->sub = SUB_IDLE;
}

static int build_stream(sess_t *s) {
    if (s->ssn && anker_model_screensaver(s->model_id)) {
        ss_job_t *j = &s->ss[s->ssh];
        uint32_t ep = anker_epoch(pex_ms());
        int n = -1;
        uint16_t op = 0x021F;
        uint32_t ms = 5000;
        int ack = 1;
        if (j->step == 1) { n = anker_ss_select(j->id, j->crc, j->mode, ep, s->pt, sizeof s->pt); op = 0x021F; }
        else if (j->step == 2) {
            n = anker_ss_begin(j->id, j->crc, j->size, j->count, j->plen, j->window, ep, s->pt, sizeof s->pt);
            op = 0x0220;
        } else {
            n = anker_ss_data(j->n, j->data, j->dlen, s->pt, sizeof s->pt);
            op = 0x0221;
            ms = 8000;
            ack = j->ack;
        }
        if (n < 0) { dequeue_ss(s); return 0; }
        if (!ack) {
            s->sub = SUB_SS_NA;
            s->no_wait = 1;
            s->want_op = 0;
            int rc = seal(s, FT_CH, op, s->pt, (size_t)n);
            if (rc < 0) { dequeue_ss(s); return 0; }
            if (rc > 0) { s->no_wait = 0; dequeue_ss(s); }
            return 0;
        }
        s->sub = SUB_SS;
        return launch(s, FT_CH, op, s->pt, (size_t)n, (uint16_t)(op | 0x0800), ms, 0, 0, 1);
    }
    if (s->qn) {
        q_job_t *j = &s->q[s->qh];
        int n = anker_setter_payload(&j->job, anker_epoch(pex_ms()), s->pt, sizeof s->pt);
        if (n < 0) {
            pex_log(PEX_LOG_WARN, "anker: set %s: payload", j->field);
            s->qh = (uint8_t)((s->qh + 1) % 4);
            s->qn--;
            return 0;
        }
        uint16_t ft = anker_model_setter_ft(s->model_id);
        s->sub = SUB_SET;
        return launch(s, (uint8_t)ft, j->opcode, s->pt, (size_t)n, (uint16_t)(j->opcode | 0x0800),
                      4000, 0, j->busy_ok, 0);
    }
    if (s->hist_arm && !s->hist_packets && s->sub != SUB_HIST && anker_model_history(s->model_id)) {
        s->hist_packets = 0;
        s->hist_records = 0;
        s->hist_nudges = 0;
        s->hist_seq++;
        if (hist_send(s, 0) < 0) {
            hist_emit(s, 1, "write refused", NULL, 0, 0, 0, 0);
            s->hist_arm = 0;
        }
        return 0;
    }
    if (s->polling && (int32_t)(pex_ms() - s->next_poll) >= 0) {
        int n = anker_poll_payload(anker_epoch(pex_ms()), s->pt, sizeof s->pt);
        if (n < 0) { return 0; }
        s->sub = SUB_POLL;
        return launch(s, (uint8_t)s->poll_ft, s->poll_op, s->pt, (size_t)n, s->poll_want, 6000, 0, 0, 0);
    }
    if (pex_ms() - s->link_check > 10000u) {
        s->link_check = pex_ms();
        int ready = 0;
        if (pex_bt_state(s->addr, &ready) != PEX_BT_CONNECTED) { drop_link(s); }
    }
    return 0;
}

static void slot_tick(sess_t *s) {
    if (!s->on || s->st == ST_OFF) { return; }
    if (s->st == ST_HUNT) {
        uint32_t age = pex_ms() - s->hunt_t0;
        if (age >= 34000u) { s->st = ST_IDLE; s->t0 = pex_ms(); return; }
        if (pex_ms() - s->last_look >= 500u) {
            s->last_look = pex_ms();
            hunt_look(s);
        }
        return;
    }
    if (s->st == ST_IDLE) {
        if (pex_ms() - s->t0 >= 11000u) {
            s->st = ST_HUNT;
            s->hunt_t0 = pex_ms();
            s->last_look = 0;
        }
        return;
    }
    if (s->st == ST_LOCK) {
        if (hs_try(slot_of(s))) { start_connect(s); }
        return;
    }
    if (s->st == ST_GAP) {
        if (pex_ms() - s->t0 >= 1500u) { start_connect(s); }
        return;
    }
    if (s->st == ST_BACKOFF) {
        if (pex_ms() - s->t0 >= 5000u) {
            s->st = ST_HUNT;
            s->hunt_t0 = pex_ms();
            s->last_look = 0;
            memset(s->samp, 0, sizeof s->samp);
            s->have_sample = 0;
        }
        return;
    }
    if (s->st == ST_CONN) {
        if (s->ev_up) { s->saw_link = 1; s->ev_up = 0; }
        if (s->ev_fail || (s->ev_down && s->saw_link)) { fail_attempt(s); return; }
        s->ev_down = 0;
        if (s->saw_link) {
            s->st = ST_DISC;
            s->t0 = pex_ms();
            s->cmd_chr = s->tel_chr = -1;
            return;
        }
        if (pex_ms() - s->t0 > 12000u) { fail_attempt(s); }
        return;
    }
    if (s->st == ST_DISC) {
        int ready = 0;
        int stt = pex_bt_state(s->addr, &ready);
        if (s->ev_ready) { ready = 1; s->ev_ready = 0; }
        if (s->ev_fail || s->ev_down || stt == PEX_BT_DISCONNECTED) { fail_attempt(s); return; }
        if (!ready) {
            if (pex_ms() - s->t0 > 20000u) { fail_attempt(s); }
            return;
        }
        if (s->cmd_chr < 0) {
            int c = pex_bt_find(s->addr, SVC_UUID, CMD_UUID, 0);
            if (c >= 0) { s->cmd_chr = c; }
        }
        if (s->tel_chr < 0) {
            int c = pex_bt_find(s->addr, SVC_UUID, TEL_UUID, 0);
            if (c >= 0) { s->tel_chr = c; }
        }
        if (s->cmd_chr < 0 || s->tel_chr < 0) {
            if (pex_ms() - s->t0 > 20000u) { fail_attempt(s); }
            return;
        }
        int rc = pex_bt_subscribe(s->addr, s->tel_chr, 1);
        if (rc == PEX_EBUSY) { return; }
        if (rc < 0) { fail_attempt(s); return; }
        begin_neg(s);
        return;
    }
    if (s->st == ST_NEG) {
        if (s_hs == slot_of(s) && pex_ms() - s_hs_at > 45000u) {
            pex_log(PEX_LOG_WARN, "anker: handshake took more than 45 s");
            fail_attempt(s);
            return;
        }
        if (service_tx(s)) { return; }
        if (s->result) {
            int r = s->result;
            s->result = 0;
            if (r > 0) { on_neg_ok(s); }
            else { on_neg_fail(s); }
            return;
        }
        if (s->st == ST_NEG && build_neg(s) < 0) { on_neg_fail(s); }
        return;
    }
    if (s->st == ST_STREAM) {
        if (s->ev_down || s->ev_fail) { drop_link(s); return; }
        if (service_tx(s)) { return; }
        if (s->result) {
            int r = s->result;
            s->result = 0;
            on_stream_result(s, r > 0);
            return;
        }
        if (s->st == ST_STREAM) { build_stream(s); }
    }
}

static void on_bt(void *arg, const pex_bt_event_t *ev) {
    (void)arg;
    if (!ev) { return; }
    sess_t *s = NULL;
    for (int i = 0; i < 2; i++) {
        if (S[i].on && S[i].st != ST_OFF && S[i].st != ST_HUNT && S[i].st != ST_IDLE
            && memcmp(S[i].addr, ev->addr, 6) == 0) {
            s = &S[i];
            break;
        }
    }
    if (!s) { return; }
    if (ev->kind == PEX_BT_LINK_UP) { s->ev_up = 1; s->saw_link = 1; return; }
    if (ev->kind == PEX_BT_LINK_DOWN) { s->ev_down = 1; return; }
    if (ev->kind == PEX_BT_FAILED) { s->ev_fail = 1; return; }
    if (ev->kind == PEX_BT_READY) { s->ev_ready = 1; return; }
    if (ev->kind != PEX_BT_NOTIFY && ev->kind != PEX_BT_INDICATE) { return; }
    if (s->tel_chr >= 0 && ev->chr != s->tel_chr && !s->logged_chr) {
        s->logged_chr = 1;
        pex_log(PEX_LOG_INFO, "anker: notifications arrive on chr %d (tel %d)", ev->chr, s->tel_chr);
    }
    pex_codec_frame_t f;
    if (pex_codec_frame_parse(ev->data, ev->len, &f) != 1) { return; }
    if (!f.checksum_ok) {
        pex_log(PEX_LOG_WARN, "anker: bad checksum on %04x", f.opcode);
        return;
    }
    const uint8_t *pay = NULL;
    size_t pn = 0;
    int rr = pex_codec_reasm_push(s->reasm, &f, &pay, &pn);
    if (rr <= 0 || !pay) { return; }
    if (pn > sizeof s_dec) { pn = sizeof s_dec; }
    memcpy(s_dec, pay, pn);
    int encrypted = f.encrypted;
    int outn = (int)pn;
    s->opaque = 0;
    if (encrypted) {
        if (s->have_session) {
            if (s->cipher == CIP_GCM) {
                outn = pex_codec_gcm_decrypt(s->key, 16, s->iv, s_dec, pn, s->body, sizeof s->body);
            } else {
                size_t ol = 0;
                int rc = pex_codec_cbc(0, s->key, s->keylen, s->iv, s_dec, pn, s->body, sizeof s->body, &ol);
                outn = rc < 0 ? rc : (int)ol;
            }
        } else if (s->neg_on) {
            outn = anker_neg_decrypt(s_dec, pn, s->body, sizeof s->body);
        } else {
            s->opaque = 1;
            if (pn > sizeof s->body) { pn = sizeof s->body; }
            memcpy(s->body, s_dec, pn);
            outn = (int)pn;
        }
    } else {
        if (pn > sizeof s->body) { pn = sizeof s->body; }
        memcpy(s->body, s_dec, pn);
        outn = (int)pn;
    }
    if (outn < 0) { return; }
    s->body_len = (uint16_t)outn;
    s->got_status = 0;
    if (!s->opaque && f.is_response && s->body_len >= 1) {
        s->got_status = s->body[0];
        memmove(s->body, s->body + 1, (size_t)s->body_len - 1);
        s->body_len--;
    }
    if (s->want_op && f.opcode == s->want_op) {
        s->got = 1;
        return;
    }
    if (s->streaming && s->want_op == 0 && (s->model_id == 0 || anker_is_report(s->model_id, f.opcode))) {
        publish(s, s->body, s->body_len);
    } else if (s->streaming && s->want_op && anker_is_report(s->model_id, f.opcode) && f.opcode != s->want_op) {
        publish(s, s->body, s->body_len);
    }
}

static int tick(void *arg) {
    (void)arg;
    if (s_stop) { return PEX_TASK_DONE; }
    for (int i = 0; i < 2; i++) { slot_tick(&S[i]); }
    int want = 0;
    for (int i = 0; i < 2; i++) {
        if (S[i].on && S[i].st == ST_HUNT) { want = 1; }
    }
    if (want) {
        if (!pex_bt_scanning()) { pex_bt_scan(0, 1); }
    } else if (pex_bt_scanning()) {
        pex_bt_scan_stop();
    }
    return PEX_TASK_AGAIN;
}

static sess_t *find_sess(const char *device) {
    sess_t *one = NULL;
    int n = 0;
    for (int i = 0; i < 2; i++) {
        if (!S[i].on || S[i].st == ST_OFF || S[i].st == ST_HUNT || S[i].st == ST_IDLE) { continue; }
        n++;
        one = &S[i];
        if (device && device[0] && (addr_eq(device, S[i].addr_str) || eq_ci(device, S[i].product_type))) {
            return &S[i];
        }
    }
    if ((!device || !device[0]) && n == 1) { return one; }
    return NULL;
}

static int reply_obj(char *reply, size_t cap, const char *err, const char *msg) {
    pex_jw_t w;
    pex_jw_init(&w, reply, cap);
    pex_jw_obj(&w, NULL);
    if (err) {
        pex_jw_str(&w, "error", err);
        pex_jw_str(&w, "message", msg ? msg : err);
    }
    pex_jw_end(&w);
    int n = pex_jw_finish(&w);
    return n < 0 ? PEX_ENOSPC : n;
}

static int key_int(const char *obj, const char *key, int32_t *out) {
    size_t k = strlen(key);
    for (const char *p = obj; *p; p++) {
        if (*p != '"' || strncmp(p + 1, key, k) != 0 || p[1 + k] != '"') { continue; }
        p += k + 2;
        while (*p == ' ' || *p == ':' || *p == '\t') { p++; }
        if (strncmp(p, "true", 4) == 0) { *out = 1; return 1; }
        if (strncmp(p, "false", 5) == 0) { *out = 0; return 1; }
        int neg = 0;
        if (*p == '-') { neg = 1; p++; }
        if (*p < '0' || *p > '9') { return 0; }
        int32_t v = 0;
        while (*p >= '0' && *p <= '9') { v = (int32_t)(v * 10 + (*p++ - '0')); }
        *out = neg ? -v : v;
        return 1;
    }
    return 0;
}

static void val_from_json(const char *json, anker_val *v) {
    memset(v, 0, sizeof *v);
    int64_t n = 0;
    int b = 0;
    if (pex_json_int(json, "value", &n)) { v->is_int = 1; v->i = (int32_t)n; return; }
    if (pex_json_bool(json, "value", &b)) { v->is_int = 1; v->i = b ? 1 : 0; return; }
    if (pex_json_str(json, "value", v->text, sizeof v->text)) { v->is_text = 1; return; }
    size_t ln = 0;
    const char *raw = pex_json_find(json, "value", &ln);
    if (!raw || ln < 2) { return; }
    v->is_obj = 1;
    if (raw[0] == '{') {
        int32_t x;
        if (key_int(raw, "mode", &x) || key_int(raw, "mode_number", &x)) { v->mode = x; v->have_mode = 1; }
        if (key_int(raw, "auto_exit", &x)) { v->auto_exit = x; v->have_auto = 1; }
        if (key_int(raw, "c1_w", &x) || key_int(raw, "c1", &x)) { v->c1 = x; v->have_c1 = 1; }
        if (key_int(raw, "c2_w", &x) || key_int(raw, "c2", &x)) { v->c2 = x; v->have_c2 = 1; }
        if (key_int(raw, "c3_w", &x) || key_int(raw, "c3", &x)) { v->c3 = x; v->have_c3 = 1; }
        return;
    }
    if (raw[0] == '[') {
        int32_t num[5];
        int nn = 0;
        const char *p = raw + 1;
        while (*p && nn < 5) {
            while (*p == ' ' || *p == ',') { p++; }
            if (*p == ']') { break; }
            int neg = 0;
            if (*p == '-') { neg = 1; p++; }
            if (*p < '0' || *p > '9') { break; }
            int32_t x = 0;
            while (*p >= '0' && *p <= '9') { x = (int32_t)(x * 10 + (*p++ - '0')); }
            num[nn++] = neg ? -x : x;
        }
        if (nn >= 1) { v->mode = num[0]; v->have_mode = 1; }
        if (nn >= 3) { v->c1 = num[2]; v->have_c1 = 1; }
        if (nn >= 4) { v->c2 = num[3]; v->have_c2 = 1; }
        if (nn >= 5) { v->c3 = num[4]; v->have_c3 = 1; }
        if (nn >= 1 && nn <= 3) {
            v->have_c1 = nn >= 1; v->c1 = num[0];
            v->have_c2 = nn >= 2; v->c2 = nn >= 2 ? num[1] : 0;
            v->have_c3 = nn >= 3; v->c3 = nn >= 3 ? num[2] : 0;
            v->have_mode = 0;
        }
    }
}

static int enqueue(sess_t *s, const char *field, const anker_val *v, const char **err, const char **msg) {
    if (!s_running || s->st != ST_STREAM) { *err = "not_running"; *msg = "not running"; return -1; }
    const anker_setter *set = anker_setter_by_name(s->model_id, field);
    if (!set) { *err = "unknown_field"; *msg = "field has no captured setter"; return -1; }
    anker_job job;
    const char *why = NULL;
    if (anker_setter_check(set, v, &job, &why) != 0) {
        *err = "bad_value";
        *msg = why ? why : "bad value";
        return -1;
    }
    if (s->qn >= 4) { *err = "busy"; *msg = "setter queue full"; return -1; }
    q_job_t *j = &s->q[(s->qh + s->qn) % 4];
    memset(j, 0, sizeof *j);
    j->job = job;
    j->opcode = set->opcode;
    j->no_readback = set->no_readback;
    j->busy_ok = set->busy_ok;
    j->rb_kind = set->kind;
    copy_str(j->field, sizeof j->field, field);
    j->rb_i = job.value;
    j->rb_c1 = job.extra[0];
    j->rb_c2 = job.extra[1];
    j->rb_c3 = job.extra[2];
    s->qn++;
    return 0;
}

static int op_set(void *arg, const char *params, char *reply, size_t cap) {
    (void)arg;
    if (!s_running) { return reply_obj(reply, cap, "not_running", "not running"); }
    char dev[24], field[24];
    dev[0] = field[0] = 0;
    pex_json_str(params, "device", dev, sizeof dev);
    pex_json_str(params, "field", field, sizeof field);
    sess_t *s = find_sess(dev);
    if (!s) { return reply_obj(reply, cap, "not_this_device", "no Anker session holds that device"); }
    anker_val v;
    val_from_json(params, &v);
    const char *err = NULL, *msg = NULL;
    if (enqueue(s, field, &v, &err, &msg) != 0) { return reply_obj(reply, cap, err, msg); }
    pex_jw_t w;
    pex_jw_init(&w, reply, cap);
    pex_jw_obj(&w, NULL);
    pex_jw_bool(&w, "queued", 1);
    pex_jw_str(&w, "field", field);
    if (v.is_int) { pex_jw_int(&w, "value", v.i); }
    pex_jw_end(&w);
    int n = pex_jw_finish(&w);
    return n < 0 ? PEX_ENOSPC : n;
}

static int op_setters(void *arg, const char *params, char *reply, size_t cap) {
    (void)arg;
    char dev[24];
    dev[0] = 0;
    pex_json_str(params, "device", dev, sizeof dev);
    sess_t *s = find_sess(dev);
    if (!s) { return reply_obj(reply, cap, "not_this_device", "no Anker session holds that device"); }
    pex_jw_t w;
    pex_jw_init(&w, reply, cap);
    pex_jw_obj(&w, NULL);
    pex_jw_str(&w, "device", s->addr_str);
    pex_jw_obj(&w, "setters");
    int n = 0;
    const anker_setter *tab = anker_setters(s->model_id, &n);
    for (int i = 0; tab && i < n; i++) {
        const anker_setter *st = &tab[i];
        pex_jw_obj(&w, st->name);
        pex_jw_int(&w, "opcode", st->opcode);
        pex_jw_bool(&w, "verified", st->verified);
        pex_jw_bool(&w, "readback", !st->no_readback);
        if (st->kind == ANKER_JOB_CUSTOM) { pex_jw_str(&w, "kind", "custom"); }
        else if (st->kind == ANKER_JOB_PROTO) { pex_jw_str(&w, "kind", "protocols"); }
        if (st->maxv >= st->minv) {
            pex_jw_int(&w, "min", st->minv);
            pex_jw_int(&w, "max", st->maxv);
        }
        if (st->nvalues && st->values) {
            pex_jw_arr(&w, "values");
            for (uint8_t k = 0; k < st->nvalues; k++) { pex_jw_int(&w, NULL, st->values[k]); }
            pex_jw_end(&w);
        }
        if (st->nnames && st->name_keys && st->name_vals) {
            pex_jw_obj(&w, "names");
            for (uint8_t k = 0; k < st->nnames; k++) { pex_jw_int(&w, st->name_keys[k], st->name_vals[k]); }
            pex_jw_end(&w);
        }
        pex_jw_end(&w);
    }
    pex_jw_end(&w);
    pex_jw_end(&w);
    int ln = pex_jw_finish(&w);
    return ln < 0 ? PEX_ENOSPC : ln;
}

static int unhex(const char *a, const char *b, uint8_t *out, size_t cap, size_t *n) {
    char tmp[400];
    size_t i = 0;
    const char *p = a ? a : "";
    while (*p && i + 1 < sizeof tmp) { tmp[i++] = *p++; }
    p = b ? b : "";
    while (*p && i + 1 < sizeof tmp) { tmp[i++] = *p++; }
    tmp[i] = 0;
    if (i < 2 || (i & 1)) { return -1; }
    size_t bytes = i / 2;
    if (bytes > cap) { return -1; }
    for (size_t k = 0; k < bytes; k++) {
        int h = hexval(tmp[k * 2]), l = hexval(tmp[k * 2 + 1]);
        if (h < 0 || l < 0) { return -1; }
        out[k] = (uint8_t)((h << 4) | l);
    }
    *n = bytes;
    return 0;
}

static int op_ss(void *arg, const char *params, char *reply, size_t cap) {
    (void)arg;
    if (!s_running) { return reply_obj(reply, cap, "not_running", "not running"); }
    char dev[24], step[16];
    dev[0] = step[0] = 0;
    pex_json_str(params, "device", dev, sizeof dev);
    pex_json_str(params, "step", step, sizeof step);
    sess_t *s = NULL;
    for (int i = 0; i < 2; i++) {
        if (!S[i].on || !anker_model_screensaver(S[i].model_id)) { continue; }
        if (S[i].st == ST_OFF) { continue; }
        if (!dev[0] || addr_eq(dev, S[i].addr_str) || eq_ci(dev, S[i].product_type)) { s = &S[i]; break; }
    }
    if (!s) { return reply_obj(reply, cap, "not_this_device", "no screensaver-capable device"); }
    if (s->ssn >= 4) { return reply_obj(reply, cap, "busy", "screensaver queue full -- wait for the ack"); }
    ss_job_t j;
    memset(&j, 0, sizeof j);
    int64_t id = 0, crc = 0, size = 0, count = 0, plen = 156, window = 10, mode = 3, nn = 0;
    if (strcmp(step, "select") == 0) {
        if (!pex_json_int(params, "id", &id) || !pex_json_int(params, "crc", &crc)) {
            return reply_obj(reply, cap, "bad_request", "select needs id and crc");
        }
        pex_json_int(params, "mode", &mode);
        j.step = 1; j.id = (uint32_t)id; j.crc = (uint32_t)crc; j.mode = (uint8_t)(mode & 0xff);
    } else if (strcmp(step, "begin") == 0) {
        if (!pex_json_int(params, "id", &id) || !pex_json_int(params, "crc", &crc)
            || !pex_json_int(params, "size", &size) || !pex_json_int(params, "count", &count)) {
            return reply_obj(reply, cap, "bad_request", "begin needs id, crc, size and count");
        }
        pex_json_int(params, "plen", &plen);
        pex_json_int(params, "window", &window);
        if (plen < 1 || plen > 200) { return reply_obj(reply, cap, "bad_request", "plen takes 1..200"); }
        j.step = 2; j.id = (uint32_t)id; j.crc = (uint32_t)crc; j.size = (uint32_t)size;
        j.count = (uint16_t)count; j.plen = (uint16_t)plen; j.window = (uint8_t)window;
    } else if (strcmp(step, "data") == 0) {
        if (!pex_json_int(params, "n", &nn) || nn < 0) {
            return reply_obj(reply, cap, "bad_request", "data needs n and a/b hex");
        }
        char a[200], b[200];
        a[0] = b[0] = 0;
        pex_json_str(params, "a", a, sizeof a);
        pex_json_str(params, "b", b, sizeof b);
        size_t dn = 0;
        if (unhex(a, b, j.data, sizeof j.data, &dn) != 0 || dn == 0) {
            return reply_obj(reply, cap, "bad_request", "data needs n and a/b hex");
        }
        int ack = 0;
        pex_json_bool(params, "ack", &ack);
        j.step = 3; j.n = (uint16_t)nn; j.dlen = (uint16_t)dn; j.ack = (uint8_t)(ack ? 1 : 0);
    } else {
        return reply_obj(reply, cap, "bad_request", "step is select, begin or data");
    }
    s->ss[(s->ssh + s->ssn) % 4] = j;
    s->ssn++;
    pex_jw_t w;
    pex_jw_init(&w, reply, cap);
    pex_jw_obj(&w, NULL);
    pex_jw_bool(&w, "queued", 1);
    pex_jw_str(&w, "step", step);
    pex_jw_end(&w);
    int ln = pex_jw_finish(&w);
    return ln < 0 ? PEX_ENOSPC : ln;
}

static int op_hist(void *arg, const char *params, char *reply, size_t cap) {
    (void)arg;
    if (!s_running) { return reply_obj(reply, cap, "not_running", "not running"); }
    char dev[24];
    dev[0] = 0;
    pex_json_str(params, "device", dev, sizeof dev);
    sess_t *s = NULL;
    for (int i = 0; i < 2; i++) {
        if (!S[i].on || !anker_model_history(S[i].model_id)) { continue; }
        if (!dev[0] || addr_eq(dev, S[i].addr_str) || eq_ci(dev, S[i].product_type)) { s = &S[i]; break; }
    }
    if (!s) { return reply_obj(reply, cap, "not_this_device", "no Anker session with history holds that device"); }
    if (s->hist_arm || s->sub == SUB_HIST) { return reply_obj(reply, cap, "busy", "a history read is under way"); }
    s->hist_arm = 1;
    s->hist_packets = 0;
    pex_jw_t w;
    pex_jw_init(&w, reply, cap);
    pex_jw_obj(&w, NULL);
    pex_jw_bool(&w, "queued", 1);
    pex_jw_end(&w);
    int n = pex_jw_finish(&w);
    return n < 0 ? PEX_ENOSPC : n;
}

static sess_t *pref_sample(const char *addr) {
    sess_t *any = NULL, *pps = NULL;
    for (int i = 0; i < 2; i++) {
        if (!S[i].on || !S[i].have_sample) { continue; }
        if (addr && addr[0] && !addr_eq(addr, S[i].addr_str)) { continue; }
        if (!any) { any = &S[i]; }
        if (S[i].model_id == ANKER_MODEL_A1761) { pps = &S[i]; }
    }
    return pps ? pps : any;
}

static int l_device(lua_State *L) {
    sess_t *s = pref_sample(NULL);
    if (!s) { pexlua_pushnil(L); return 1; }
    pexlua_pushstr(L, s->addr_str);
    return 1;
}

static int l_get(lua_State *L) {
    size_t n = 0;
    const char *addr = pexlua_checkstr(L, 1, &n);
    const char *field = pexlua_checkstr(L, 2, &n);
    sess_t *s = pref_sample(addr);
    double v;
    if (!s || !samp_get(s, field, &v)) { pexlua_pushnil(L); return 1; }
    pexlua_pushnum(L, v);
    return 1;
}

static int l_age(lua_State *L) {
    size_t n = 0;
    const char *addr = pexlua_checkstr(L, 1, &n);
    sess_t *s = pref_sample(addr);
    if (!s || !s->have_sample) { pexlua_pushnil(L); return 1; }
    pexlua_pushnum(L, (double)(pex_ms() - s->sample_ms));
    return 1;
}

static void lua_num_field(lua_State *L, int idx, const char *k, int32_t *out, int *have) {
    pexlua_getfield(L, idx, k);
    if (pexlua_type(L, -1) == PEXLUA_TNUMBER) {
        *out = (int32_t)pexlua_checknum(L, -1);
        *have = 1;
    } else if (pexlua_type(L, -1) == PEXLUA_TBOOLEAN) {
        *out = pexlua_toboolean(L, -1) ? 1 : 0;
        *have = 1;
    }
    pexlua_settop(L, idx);
}

static int l_set(lua_State *L) {
    size_t n = 0;
    const char *addr = pexlua_checkstr(L, 1, &n);
    const char *field = pexlua_checkstr(L, 2, &n);
    sess_t *s = NULL;
    for (int i = 0; i < 2; i++) {
        if (S[i].on && addr_eq(addr, S[i].addr_str)) { s = &S[i]; break; }
    }
    anker_val v;
    memset(&v, 0, sizeof v);
    int t = pexlua_type(L, 3);
    if (t == PEXLUA_TNUMBER) { v.is_int = 1; v.i = (int32_t)pexlua_checknum(L, 3); }
    else if (t == PEXLUA_TBOOLEAN) { v.is_int = 1; v.i = pexlua_toboolean(L, 3) ? 1 : 0; }
    else if (t == PEXLUA_TSTRING) {
        const char *ts = pexlua_checkstr(L, 3, &n);
        copy_str(v.text, sizeof v.text, ts);
        v.is_text = 1;
    } else if (t == PEXLUA_TTABLE) {
        v.is_obj = 1;
        lua_num_field(L, 3, "mode", &v.mode, &v.have_mode);
        if (!v.have_mode) { lua_num_field(L, 3, "mode_number", &v.mode, &v.have_mode); }
        lua_num_field(L, 3, "auto_exit", &v.auto_exit, &v.have_auto);
        lua_num_field(L, 3, "c1_w", &v.c1, &v.have_c1);
        if (!v.have_c1) { lua_num_field(L, 3, "c1", &v.c1, &v.have_c1); }
        lua_num_field(L, 3, "c2_w", &v.c2, &v.have_c2);
        if (!v.have_c2) { lua_num_field(L, 3, "c2", &v.c2, &v.have_c2); }
        lua_num_field(L, 3, "c3_w", &v.c3, &v.have_c3);
        if (!v.have_c3) { lua_num_field(L, 3, "c3", &v.c3, &v.have_c3); }
        size_t len = pexlua_rawlen(L, 3);
        if (len >= 1 && len <= 3 && !v.have_c1) {
            for (size_t i = 1; i <= len && i <= 3; i++) {
                pexlua_geti(L, 3, (int64_t)i);
                int32_t x = 0;
                if (pexlua_type(L, -1) == PEXLUA_TNUMBER) { x = (int32_t)pexlua_checknum(L, -1); }
                pexlua_settop(L, 3);
                if (i == 1) { v.c1 = x; v.have_c1 = 1; }
                if (i == 2) { v.c2 = x; v.have_c2 = 1; }
                if (i == 3) { v.c3 = x; v.have_c3 = 1; }
            }
        }
    }
    const char *err = "not_this_device", *msg = "no Anker session holds that device";
    int ok = s && enqueue(s, field, &v, &err, &msg) == 0;
    pexlua_newtable(L, 0, 3);
    if (ok) {
        pexlua_pushbool(L, 1);
        pexlua_setfield(L, -2, "queued");
        pexlua_pushstr(L, field);
        pexlua_setfield(L, -2, "field");
    } else {
        pexlua_pushstr(L, err);
        pexlua_setfield(L, -2, "error");
        pexlua_pushstr(L, msg);
        pexlua_setfield(L, -2, "message");
    }
    return 1;
}

static PEX_PTR_TABLE pexlua_reg_t s_regs[] = {
    { "device", l_device }, { "get", l_get }, { "age_ms", l_age }, { "set", l_set },
};

static void load_cfg(const char *json) {
    copy_str(g_client, sizeof g_client, "btgw-0000");
    copy_str(g_region, sizeof g_region, "US");
    copy_str(g_tz, sizeof g_tz, "UTC0");
    g_user[0] = g_target[0] = g_target2[0] = 0;
    g_tz_neg = 0;
    char buf[80];
    if (pex_sys_secret("anker", "user_id", g_user, sizeof g_user) < 0) { g_user[0] = 0; }
    if (pex_sys_secret("anker", "tz", g_tz, sizeof g_tz) < 0) { copy_str(g_tz, sizeof g_tz, "UTC0"); }
    if (pex_sys_secret("anker", "client_id", buf, sizeof buf) >= 0 && buf[0]) { copy_str(g_client, sizeof g_client, buf); }
    if (pex_sys_secret("anker", "region", buf, sizeof buf) >= 0 && buf[0]) { copy_str(g_region, sizeof g_region, buf); }
    if (pex_sys_secret("anker", "tz_offset_sec", buf, sizeof buf) >= 0) {
        g_tz_neg = (uint32_t)(-parse_i32(buf));
    }
    if (pex_sys_secret("anker", "target", g_target, sizeof g_target) < 0) { g_target[0] = 0; }
    if (pex_sys_secret("anker", "target2", g_target2, sizeof g_target2) < 0) { g_target2[0] = 0; }
    if (json) {
        if (pex_json_str(json, "target", buf, sizeof buf) && buf[0]) { copy_str(g_target, sizeof g_target, buf); }
        if (pex_json_str(json, "target2", buf, sizeof buf) && buf[0]) { copy_str(g_target2, sizeof g_target2, buf); }
    }
}

static void reset_slot(sess_t *s, int on, const char *target) {
    memset(s, 0, sizeof *s);
    s->on = (uint8_t)on;
    s->st = on ? ST_HUNT : ST_OFF;
    s->hunt_t0 = pex_ms();
    s->cmd_chr = s->tel_chr = -1;
    if (target) { copy_str(s->target, sizeof s->target, target); }
}

int pex_main(const pex_host_t *host, uint32_t reason, void *arg) {
    (void)host;
    switch (reason) {
    case PEX_INIT:
        anker_set_ghash(ghash_wrap);
        pex_bus_op("anker.set", op_set, NULL);
        pex_bus_op("anker.setters", op_setters, NULL);
        pex_bus_op("anker.screensaver", op_ss, NULL);
        pex_bus_op("anker.history", op_hist, NULL);
        return PEX_OK;
    case PEX_LUA_OPEN:
        if (!pex_avail(pexlua_newtable)) { return PEX_ENOTSUP; }
        pexlua_newtable(arg, 0, 4);
        pexlua_register(arg, s_regs, 4);
        return 1;
    case PEX_APP_RUN: {
        char key[80];
        int kn = pex_sys_secret("anker", "private_key", key, sizeof key);
        if (kn < 0 || parse_key(key, g_priv) != 0) {
            memset(key, 0, sizeof key);
            pex_log(PEX_LOG_ERR, "anker: cfg/anker.json has no 32-byte private_key");
            return PEX_EINVAL;
        }
        memset(key, 0, sizeof key);
        g_have_key = 1;
        load_cfg(arg);
        if (!g_user[0]) { pex_log(PEX_LOG_WARN, "anker: no user_id; the device may refuse to report"); }
        s_stop = 0;
        s_running = 1;
        reset_slot(&S[0], 1, g_target);
        reset_slot(&S[1], g_target2[0] != 0, g_target2);
        s_hs = -1;
        s_listen = pex_bt_listen(NULL, on_bt, NULL);
        if (s_listen < 0) { s_running = 0; return s_listen; }
        s_task = pex_sched_every(50, tick, NULL);
        if (s_task <= 0) { s_running = 0; return s_task; }
        pex_log(PEX_LOG_INFO, "anker: running%s", S[1].on ? " (two devices)" : "");
        return PEX_OK;
    }
    case PEX_APP_STOP:
        s_stop = 1;
        s_running = 0;
        for (int i = 0; i < 2; i++) {
            if (S[i].on && S[i].st != ST_OFF && S[i].st != ST_HUNT && S[i].st != ST_IDLE) {
                pex_bt_disconnect(S[i].addr);
            }
            S[i].st = ST_OFF;
        }
        s_hs = -1;
        if (s_listen >= 0) { pex_bt_unlisten(s_listen); s_listen = -1; }
        pex_bt_scan_stop();
        return PEX_OK;
    case PEX_DEINIT:
        s_running = 0;
        s_stop = 1;
        memset(g_priv, 0, sizeof g_priv);
        return PEX_OK;
    default:
        return PEX_OK;
    }
}
