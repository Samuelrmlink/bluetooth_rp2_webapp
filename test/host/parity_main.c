/*
 * Parity test: the ported C modules must reproduce the JS reference
 * byte-for-byte. The vectors are FROZEN -- vectors_data.h was generated once
 * by solix-esp32's tools/gen_vectors.mjs and is carried as data. We do not
 * carry the JS oracle: a second implementation of the protocol is exactly what
 * this project exists to avoid.
 *
 * Two assertions here are regression LOCKS and must never be "fixed":
 *   - the lenient PKCS7 unpad (a CBC case whose padding is malformed and whose
 *     expected result is the buffer unchanged)
 *   - GCM decrypt with no tag verification, under a reused nonce
 * Both encode a device behaviour. Tightening either stops the charger talking.
 *
 * Differences from the solix-esp32 original, all consequences of the C/Lua
 * split (see the plan's "Phase 8 -- the C/Lua line"):
 *
 *   - The negotiation cipher is no longer a C module. Its keystream and
 *     forgery constants are device data and live in the Lua profile. What
 *     remains testable in C is that the GENERALISED ghash_aad_ct() still
 *     reproduces the original's tags, so that is what the NEG section now
 *     checks, using test/host/neg_fixture.h.
 *   - anker_crypto no longer hardcodes the app private key, so the ECDH cases
 *     need one supplied. It is read at runtime from the reference workspace
 *     and the section SKIPS when that is not present -- the key is never
 *     copied into this repository.
 *   - The "ECIES" KDF, the account-key derivation and ECDH mode 0 need
 *     HMAC-SHA256, which serves the C1000 only and is out of scope. Those
 *     cases report SKIP rather than passing silently.
 *
 * Output: one "OK <case>" / "FAIL <case>" / "SKIP <case>" line per assertion,
 * non-zero exit on any failure. Skips never fail the run but are counted and
 * printed in the summary, so a shrinking suite is visible.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <ctype.h>
#include <stdlib.h>

#include "codec/anker_frame.h"
#include "codec/anker_tlv.h"
#include "crypto/anker_crypto.h"
#include "crypto/ghash.h"

#include "neg_fixture.h"

/* vectors_data.h gates its crypto vectors behind HAVE_MBEDTLS -- the name they
 * carried when they were generated, back when mbedTLS computed them. The
 * vectors are values, not an implementation, and are just as valid against
 * uECC + tiny-AES. Define the macro rather than edit the file: keeping
 * vectors_data.h byte-identical to what generated it is the whole point of
 * freezing it. */
#define HAVE_MBEDTLS 1
#include "vectors_data.h"

static int g_fails = 0;
static int g_skips = 0;

static void skip(const char *name, const char *why) {
    printf("SKIP %s  (%s)\n", name, why);
    g_skips++;
}

static void hexdump(const char *label, const uint8_t *b, int n) {
    fprintf(stderr, "        %s ", label);
    for (int i = 0; i < n; i++) fprintf(stderr, "%02x", b[i]);
    fprintf(stderr, "\n");
}

static void ok(const char *name, int cond) {
    printf("%s %s\n", cond ? "OK  " : "FAIL", name);
    if (!cond) g_fails++;
}

static int eq_bytes(const char *name, const uint8_t *got, int gn,
                    const uint8_t *want, int wn) {
    int cond = (gn == wn) && (wn == 0 || memcmp(got, want, wn) == 0);
    printf("%s %s\n", cond ? "OK  " : "FAIL", name);
    if (!cond) {
        g_fails++;
        fprintf(stderr, "        got  %d bytes, want %d bytes\n", gn, wn);
        hexdump("got ", got, gn);
        hexdump("want", want, wn);
    }
    return cond;
}

// ---------------------------------------------------------------------------
static void test_frame_build(void) {
    puts("\n== anker_frame_build ==");
    uint8_t buf[600];
    for (int i = 0; i < FB_N; i++) {
        const fb_case_t *c = &FB_CASES[i];
        int n = anker_frame_build(c->ft, c->opcode, c->enc, c->slave,
                                  c->payload_len ? c->payload : NULL,
                                  (size_t)c->payload_len, buf, sizeof buf);
        if (n < 0) { ok(c->name, 0); fprintf(stderr, "        build returned %d\n", n); continue; }
        eq_bytes(c->name, buf, n, c->want, c->want_len);
    }
}

static void test_frame_parse(void) {
    puts("\n== anker_frame_parse ==");
    for (int i = 0; i < FP_N; i++) {
        const fp_case_t *c = &FP_CASES[i];
        anker_frame_t f;
        bool got = anker_frame_parse(c->frame, (size_t)c->frame_len, &f);

        if (!c->want_ok) { ok(c->name, !got); continue; }
        if (!got) { ok(c->name, 0); fprintf(stderr, "        parse returned false\n"); continue; }

        int fields_ok =
            f.function_type == c->ft &&
            f.opcode == c->opcode &&
            (int)f.encrypted == c->enc &&
            (int)f.fragmented == c->frag &&
            (int)f.frag_index == c->fidx &&
            (int)f.frag_count == c->fcnt &&
            (int)f.is_response == c->is_resp &&
            (int)f.length_ok == c->len_ok &&
            (int)f.checksum_ok == c->cksum_ok;

        if (!fields_ok) {
            ok(c->name, 0);
            fprintf(stderr,
                "        got  ft=%02x op=%04x enc=%d frag=%d idx=%d cnt=%d resp=%d len_ok=%d ck_ok=%d\n"
                "        want ft=%02x op=%04x enc=%d frag=%d idx=%d cnt=%d resp=%d len_ok=%d ck_ok=%d\n",
                f.function_type, f.opcode, f.encrypted, f.fragmented, f.frag_index,
                f.frag_count, f.is_response, f.length_ok, f.checksum_ok,
                c->ft, c->opcode, c->enc, c->frag, c->fidx, c->fcnt, c->is_resp,
                c->len_ok, c->cksum_ok);
            continue;
        }
        eq_bytes(c->name, f.payload, (int)f.payload_len, c->payload, c->payload_len);
    }
}

// ---------------------------------------------------------------------------
static void test_neg_cipher(void) {
    puts("\n== negotiation cipher (keystream in Lua, GHASH in C) ==");
    uint8_t out[128], back[128];
    for (int i = 0; i < NEG_N; i++) {
        const neg_case_t *c = &NEG_CASES[i];
        if (!c->want_enc_ok) {
            /* The only way to fail an encrypt was to exceed the keystream, a
             * bound the Lua side now owns. Assert the bound is what we think. */
            char nm[160];
            snprintf(nm, sizeof nm, "%s (over-long, rejected)", c->name);
            ok(nm, c->pt_len > NEG_KEYSTREAM_LEN);
            continue;
        }
        if (c->pt_len > (int)sizeof out - 16) { ok(c->name, 0); continue; }

        /* encrypt = XOR keystream, then tag = GHASH(H, AAD, ct) ^ E_K(J0) */
        for (int k = 0; k < c->pt_len; k++) { out[k] = c->pt[k] ^ NEG_KEYSTREAM[k]; }
        uint8_t s[16];
        ghash_aad_ct(NEG_GHASH_H, ANKER_GCM_AAD, 16, out, (size_t)c->pt_len, s);
        for (int k = 0; k < 16; k++) { out[c->pt_len + k] = (uint8_t)(s[k] ^ NEG_EJ0[k]); }
        if (!eq_bytes(c->name, out, c->pt_len + 16, c->body, c->body_len)) { continue; }

        int pt_len = c->body_len >= 16 ? c->body_len - 16 : c->body_len;
        for (int k = 0; k < pt_len; k++) { back[k] = c->body[k] ^ NEG_KEYSTREAM[k]; }
        char nm[160];
        snprintf(nm, sizeof nm, "%s (decrypt)", c->name);
        eq_bytes(nm, back, pt_len, c->pt, c->pt_len);
    }
}

// ---------------------------------------------------------------------------
static void hex_lower(const uint8_t *b, int n, char *out, size_t cap) {
    static const char H[] = "0123456789abcdef";
    int o = 0;
    for (int i = 0; i < n && o + 2 < (int)cap; i++) {
        out[o++] = H[b[i] >> 4];
        out[o++] = H[b[i] & 0xf];
    }
    out[o] = 0;
}

// Rebuild the JS `decoded`-as-string form: the literal string for STRING items,
// lowercase hex for BYTES / UNKNOWN, empty for numeric.
static void item_sval(const anker_tlv_item_t *it, const uint8_t *body, char *out,
                      size_t cap) {
    out[0] = 0;
    if (it->type == ANKER_TLV_STRING) {
        int n = it->val_len;
        if (n >= (int)cap) n = (int)cap - 1;
        memcpy(out, body + it->val_off, (size_t)n);
        out[n] = 0;
    } else if (it->type == ANKER_TLV_BYTES || it->type == ANKER_TLV_UNKNOWN) {
        hex_lower(body + it->val_off, it->val_len, out, cap);
    }
}

static void check_items(const char *name, const uint8_t *body,
                        const anker_tlv_item_t *items, size_t n,
                        const tlv_item_exp_t *exp, int n_exp) {
    char nm[192];
    if ((int)n != n_exp) {
        snprintf(nm, sizeof nm, "%s: item count", name);
        ok(nm, 0);
        fprintf(stderr, "        got %d items, want %d\n", (int)n, n_exp);
        return;
    }
    for (int i = 0; i < n_exp; i++) {
        const anker_tlv_item_t *it = &items[i];
        const tlv_item_exp_t *e = &exp[i];
        char sval[600], raw[600];
        item_sval(it, body, sval, sizeof sval);
        hex_lower(anker_tlv_raw(it, body), anker_tlv_raw_len(it), raw, sizeof raw);
        int fld_ok = it->tag == e->tag &&
                     strcmp(anker_tlv_type_name(it->type), e->type) == 0 &&
                     it->len == e->len && it->type_byte == e->type_byte &&
                     strcmp(sval, e->sval) == 0 && strcmp(raw, e->raw_hex) == 0;
        if (!strcmp(e->type, "u8") || !strcmp(e->type, "u16") || !strcmp(e->type, "u32"))
            fld_ok = fld_ok && it->ival == e->ival;
        if (!strcmp(e->type, "f32")) {
            double d = it->fval - e->fval;
            fld_ok = fld_ok && d > -1e-9 && d < 1e-9;
        }
        snprintf(nm, sizeof nm, "%s: item[%d] %02X", name, i, e->tag);
        ok(nm, fld_ok);
        if (!fld_ok)
            fprintf(stderr,
                "        got  tag=%02X type=%s len=%d tb=%d ival=%lld fval=%g sval='%s' raw=%s\n"
                "        want tag=%02X type=%s len=%d tb=%d ival=%lld fval=%g sval='%s' raw=%s\n",
                it->tag, anker_tlv_type_name(it->type), it->len, it->type_byte,
                (long long)it->ival, it->fval, sval, raw, e->tag, e->type, e->len,
                e->type_byte, e->ival, e->fval, e->sval, e->raw_hex);
    }
}

static void test_tlv(void) {
    anker_tlv_item_t items[200];
    const size_t CAP = sizeof items / sizeof items[0];

    puts("\n== anker_tlv parse ==");
    for (int i = 0; i < TLVP_N; i++) {
        const tlv_parse_case_t *c = &TLVP_CASES[i];
        const uint8_t *b = c->body;
        size_t len = (size_t)c->body_len;
        anker_tlv_status_strip(&b, &len, c->strip_mode);
        size_t n = 0;
        int consumed = anker_tlv_parse(b, len, items, CAP, &n);
        int trailing = (int)(len - (size_t)consumed);
        int anom = anker_tlv_looks_anomalous(items, n) ? 1 : 0;
        char nm[192];
        int meta_ok = consumed == c->consumed && trailing == c->trailing &&
                      anom == c->anomalous;
        snprintf(nm, sizeof nm, "%s: consumed/trailing/anomaly", c->name);
        ok(nm, meta_ok);
        if (!meta_ok)
            fprintf(stderr,
                "        got  consumed=%d trailing=%d anom=%d\n"
                "        want consumed=%d trailing=%d anom=%d\n",
                consumed, trailing, anom, c->consumed, c->trailing, c->anomalous);
        check_items(c->name, b, items, n, c->items, c->n_items);
    }

    puts("\n== anker_tlv negotiation parse ==");
    for (int i = 0; i < TLVN_N; i++) {
        const tlv_neg_case_t *c = &TLVN_CASES[i];
        size_t n = 0;
        int consumed =
            anker_tlv_parse_negotiation(c->body, (size_t)c->body_len, items, CAP, &n);
        int trailing = (int)((size_t)c->body_len - (size_t)consumed);
        char nm[192];
        int meta_ok = consumed == c->consumed && trailing == c->trailing;
        snprintf(nm, sizeof nm, "%s: consumed/trailing", c->name);
        ok(nm, meta_ok);
        check_items(c->name, c->body, items, n, c->items, c->n_items);
    }

    puts("\n== anker_tlv build ==");
    for (int i = 0; i < TLVB_N; i++) {
        const tlv_build_case_t *c = &TLVB_CASES[i];
        anker_tlv_spec_t specs[8];
        int nsp = c->n <= 8 ? c->n : 8;
        for (int k = 0; k < nsp; k++) {
            const tlv_build_spec_t *s = &c->specs[k];
            specs[k].tag = (uint8_t)s->tag;
            specs[k].enc = (anker_enc_t)s->enc;
            specs[k].type_byte = s->type_byte;
            specs[k].ival = s->ival;
            specs[k].fval = s->fval;
            specs[k].data = s->data_len ? s->data : NULL;
            specs[k].data_len = (size_t)s->data_len;
        }
        uint8_t out[512];
        int w = anker_tlv_build_payload(specs, (size_t)nsp, out, sizeof out);
        eq_bytes(c->name, out, w < 0 ? -1 : w, c->want, c->want_len);
    }

    puts("\n== anker_tlv_find_tag ==");
    {
        // a1 01 21 (unknown "21") | a2 02 01 05 (u8=5) | a3 03 02 3412 (u16=0x1234)
        uint8_t b[] = {0xa1, 0x01, 0x21, 0xa2, 0x02, 0x01, 0x05,
                       0xa3, 0x03, 0x02, 0x34, 0x12};
        anker_tlv_item_t it;
        bool f1 = anker_tlv_find_tag(b, sizeof b, 0xa3, &it);
        ok("find_tag: A3 found", f1 && it.type == ANKER_TLV_U16 && it.ival == 0x1234);
        bool f2 = anker_tlv_find_tag(b, sizeof b, 0xa2, &it);
        ok("find_tag: A2 found", f2 && it.type == ANKER_TLV_U8 && it.ival == 5);
        bool f3 = anker_tlv_find_tag(b, sizeof b, 0x99, &it);
        ok("find_tag: absent tag", !f3);
    }

    puts("\n== anker_tlv_stamp_fe ==");
    {
        // typed fe 05 03 <e32>
        uint8_t a[] = {0xa1, 0x01, 0x21, 0xa2, 0x02, 0x01, 0x01,
                       0xfe, 0x05, 0x03, 0x00, 0x00, 0x00, 0x00};
        uint8_t a_want[] = {0xa1, 0x01, 0x21, 0xa2, 0x02, 0x01, 0x01,
                            0xfe, 0x05, 0x03, 0x78, 0x56, 0x34, 0x12};
        int s = anker_tlv_stamp_fe(a, sizeof a, 0x12345678);
        ok("stamp typed fe 05 03 (count)", s == 1);
        eq_bytes("stamp typed fe 05 03 (bytes)", a, sizeof a, a_want, sizeof a_want);
    }
    {
        // bare fe 04 <e32>
        uint8_t b[] = {0xa2, 0x02, 0x01, 0x00, 0xfe, 0x04, 0xaa, 0xbb, 0xcc, 0xdd};
        uint8_t b_want[] = {0xa2, 0x02, 0x01, 0x00, 0xfe, 0x04, 0x01, 0xef, 0xcd, 0xab};
        int s = anker_tlv_stamp_fe(b, sizeof b, 0xabcdef01);
        ok("stamp bare fe 04 (count)", s == 1);
        eq_bytes("stamp bare fe 04 (bytes)", b, sizeof b, b_want, sizeof b_want);
    }
    {
        // no FE item -> untouched, count 0
        uint8_t c[] = {0xa1, 0x01, 0x21, 0xa2, 0x05, 0x03, 0x01, 0x02, 0x03, 0x04};
        uint8_t c_want[10];
        memcpy(c_want, c, sizeof c);
        int s = anker_tlv_stamp_fe(c, sizeof c, 0x99999999);
        ok("stamp no-FE (count)", s == 0);
        eq_bytes("stamp no-FE (untouched)", c, sizeof c, c_want, sizeof c_want);
    }
}

/* The app private key is never committed. It is read at run time from the
 * reference workspace, whose path comes from $ANKER_REF_CONSTANTS (default:
 * solix-esp32's constants.js). Absent -> the ECDH cases SKIP. */
static int load_private_key(uint8_t d[32]) {
    static char path[512];
    const char *env = getenv("ANKER_REF_CONSTANTS");
    if (env) {
        snprintf(path, sizeof path, "%s", env);
    } else {
        const char *home = getenv("HOME");
        if (!home) { return -1; }
        snprintf(path, sizeof path, "%s/Work/solix-esp32/web/js/protocol/constants.js", home);
    }
    FILE *f = fopen(path, "r");
    if (!f) { return -1; }

    /* Slurp: the value sits on the line AFTER the identifier in the reference
     * file, so a line-at-a-time scan misses it. */
    static char src[1 << 16];
    size_t n = fread(src, 1, sizeof src - 1, f);
    fclose(f);
    src[n] = 0;

    const char *k = strstr(src, "ECDH_PRIVATE_KEY_HEX");
    if (!k) { return -1; }
    /* First run of >= 64 hex digits after the identifier. */
    for (const char *p = k; *p; p++) {
        int run = 0;
        while (isxdigit((unsigned char)p[run])) { run++; }
        if (run >= 64) {
            for (int i = 0; i < 32; i++) {
                unsigned v;
                if (sscanf(p + 2 * i, "%2x", &v) != 1) { return -1; }
                d[i] = (uint8_t)v;
            }
            return 0;
        }
        p += run;
        if (!*p) { break; }
    }
    return -1;
}

static void test_crypto(void) {
    puts("\n== anker_crypto (uECC + tiny-AES, no mbedTLS) ==");

    uint8_t d[32];
    int have_d = load_private_key(d) == 0;

    if (!have_d) {
        skip("pubkey X||Y", "private key not in the reference workspace");
    } else {
        uint8_t xy[64];
        if (anker_pubkey_xy(d, xy) == 0) {
            eq_bytes("pubkey X||Y", xy, 64, V_pubkey_xy, PUBKEY_XY_LEN);
        } else {
            ok("pubkey X||Y", 0);
        }
    }

    for (int i = 0; i < ECDH_N; i++) {
        const ecdh_case_t *c = &ECDH_CASES[i];
        if (!have_d) { skip(c->name, "private key not in the reference workspace"); continue; }
        /* mode 0 in the vector table is the app's default "ecdh" -- the C1000's
         * AES-256-CBC path. It skipped only while the KDF was missing. */
        anker_mode_t m = (c->mode == 0) ? ANKER_MODE_ECDH : (anker_mode_t)c->mode;

        uint8_t Z[32], key[32], iv[16];
        size_t kl = 0, il = 0;
        char nm[192];
        if (anker_ecdh_z(d, c->peer, (size_t)c->peer_len, Z) != 0) { ok(c->name, 0); continue; }
        snprintf(nm, sizeof nm, "%s : Z", c->name);
        if (!eq_bytes(nm, Z, 32, c->Z, 32)) { continue; }
        if (anker_derive_keys(Z, m, key, &kl, iv, &il) != 0) {
            ok(c->name, 0); continue;
        }
        snprintf(nm, sizeof nm, "%s : key", c->name);
        eq_bytes(nm, key, (int)kl, c->key, c->keylen);
        snprintf(nm, sizeof nm, "%s : iv", c->name);
        eq_bytes(nm, iv, (int)il, c->iv, c->ivlen);
    }

    for (int i = 0; i < KDF_N; i++) {
        const kdf_case_t *c = &KDF_CASES[i];
        uint8_t out[64];
        if (anker_kdf(c->Z, out, (size_t)c->outlen) != 0) { ok(c->name, 0); continue; }
        eq_bytes(c->name, out, c->outlen, c->want, c->outlen);
    }
    for (int i = 0; i < ACCT_N; i++) {
        const acct_case_t *c = &ACCT_CASES[i];
        uint8_t key[32], iv[16];
        if (anker_derive_account_key(c->id, key, iv) != 0) { ok(c->name, 0); continue; }
        eq_bytes(c->name, key, 32, c->key, 32);
    }

    for (int i = 0; i < CBC_N; i++) {
        const cbc_case_t *c = &CBC_CASES[i];
        uint8_t out[256];
        size_t ol = 0;
        int rc = anker_aes_cbc(c->op == 0, c->key, (size_t)c->keylen, c->iv,
                               c->in, (size_t)c->in_len, out, sizeof out, &ol);
        if (!c->want_ok) { ok(c->name, rc != 0); continue; }
        if (rc != 0) { ok(c->name, 0); continue; }
        eq_bytes(c->name, out, (int)ol, c->want, c->want_len);
    }

    for (int i = 0; i < GCM_N; i++) {
        const gcm_case_t *c = &GCM_CASES[i];
        uint8_t body[256], back[256];
        int bl = anker_aes_gcm_encrypt(c->key, (size_t)c->keylen, c->iv12,
                                       ANKER_GCM_AAD, 16, c->pt, (size_t)c->pt_len,
                                       body, sizeof body);
        if (!eq_bytes(c->name, body, bl < 0 ? -1 : bl, c->body, c->body_len)) { continue; }
        int pl = anker_aes_gcm_decrypt(c->key, (size_t)c->keylen, c->iv12,
                                       c->body, (size_t)c->body_len, back, sizeof back);
        char nm[160];
        snprintf(nm, sizeof nm, "%s (decrypt)", c->name);
        eq_bytes(nm, back, pl < 0 ? -1 : pl, c->pt, c->pt_len);
    }
}

/* FIPS-197 known answers for the runtime-key-length patch we carry against
 * tiny-AES-c. Upstream picks one key size per build; we made it a parameter,
 * so the three appendix-C vectors are the thing that proves the patch. */
static void test_aes_fips197(void) {
    puts("\n== tiny-AES-c runtime key length (FIPS-197 C.1-C.3) ==");
    static const struct { size_t kl; const char *want; } cases[] = {
        { 16, "69c4e0d86a7b0430d8cdb78070b4c55a" },
        { 24, "dda97ca4864cdfe06eaf70a0ec0d7191" },
        { 32, "8ea2b7ca516745bfeafc49904b496089" },
    };
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        uint8_t key[32];
        for (int k = 0; k < 32; k++) { key[k] = (uint8_t)k; }
        /* One AES-CBC block with a zero IV is ECB, so this needs no extra API. */
        static const uint8_t zero_iv[16] = { 0 };
        static const uint8_t pt[16] = { 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
                                        0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff };
        uint8_t out[64];
        size_t ol = 0;
        char got[33], nm[64];
        snprintf(nm, sizeof nm, "AES-%zu single block", cases[i].kl * 8);
        if (anker_aes_cbc(true, key, cases[i].kl, zero_iv, pt, 16, out, sizeof out, &ol) != 0) {
            ok(nm, 0); continue;
        }
        for (int k = 0; k < 16; k++) { snprintf(got + 2 * k, 3, "%02x", out[k]); }
        ok(nm, strcmp(got, cases[i].want) == 0);
        if (strcmp(got, cases[i].want) != 0) {
            fprintf(stderr, "        got  %s\n        want %s\n", got, cases[i].want);
        }
    }
}

// ---------------------------------------------------------------------------
int main(void) {
    test_frame_build();
    test_frame_parse();
    test_neg_cipher();
    test_tlv();
    test_aes_fips197();
    test_crypto();

    if (g_skips) {
        printf("\n%d assertion(s) skipped -- see the SKIP lines above\n", g_skips);
    }
    printf("\n%s\n", g_fails ? "PARITY FAILED" : "all parity vectors matched");
    return g_fails ? 1 : 0;
}
