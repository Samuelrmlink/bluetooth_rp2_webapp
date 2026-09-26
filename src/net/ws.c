/* WebSocket handshake (SHA-1 + base64) and minimal text-frame codec. See
 * ws.h for scope/rationale — new code, no external crypto dependency (a
 * full mbedtls link just for one hash used once per connection isn't
 * worth it), so a small compact SHA-1 is included directly below. */
#include "net/ws.h"

#include <string.h>

/* --- SHA-1 (public-domain-style compact implementation; RFC 3174) ----- */

typedef struct {
    uint32_t h[5];
    uint64_t len;
    uint8_t buf[64];
    size_t buf_len;
} sha1_ctx_t;

static uint32_t rol32(uint32_t v, int n) {
    return (v << n) | (v >> (32 - n));
}

static void sha1_block(sha1_ctx_t *c, const uint8_t *p) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
               ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
    }
    for (int i = 16; i < 80; i++) {
        w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }
    uint32_t a = c->h[0], b = c->h[1], cc = c->h[2], d = c->h[3], e = c->h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) {
            f = (b & cc) | ((~b) & d);
            k = 0x5A827999u;
        } else if (i < 40) {
            f = b ^ cc ^ d;
            k = 0x6ED9EBA1u;
        } else if (i < 60) {
            f = (b & cc) | (b & d) | (cc & d);
            k = 0x8F1BBCDCu;
        } else {
            f = b ^ cc ^ d;
            k = 0xCA62C1D6u;
        }
        uint32_t tmp = rol32(a, 5) + f + e + k + w[i];
        e = d;
        d = cc;
        cc = rol32(b, 30);
        b = a;
        a = tmp;
    }
    c->h[0] += a;
    c->h[1] += b;
    c->h[2] += cc;
    c->h[3] += d;
    c->h[4] += e;
}

static void sha1_init(sha1_ctx_t *c) {
    c->h[0] = 0x67452301u;
    c->h[1] = 0xEFCDAB89u;
    c->h[2] = 0x98BADCFEu;
    c->h[3] = 0x10325476u;
    c->h[4] = 0xC3D2E1F0u;
    c->len = 0;
    c->buf_len = 0;
}

static void sha1_update(sha1_ctx_t *c, const uint8_t *data, size_t len) {
    c->len += len;
    while (len > 0) {
        size_t n = sizeof(c->buf) - c->buf_len;
        if (n > len) {
            n = len;
        }
        memcpy(c->buf + c->buf_len, data, n);
        c->buf_len += n;
        data += n;
        len -= n;
        if (c->buf_len == sizeof(c->buf)) {
            sha1_block(c, c->buf);
            c->buf_len = 0;
        }
    }
}

static void sha1_final(sha1_ctx_t *c, uint8_t out[20]) {
    uint64_t bit_len = c->len * 8;
    uint8_t pad = 0x80;
    sha1_update(c, &pad, 1);
    uint8_t zero = 0;
    while (c->buf_len != 56) {
        sha1_update(c, &zero, 1);
    }
    uint8_t len_be[8];
    for (int i = 0; i < 8; i++) {
        len_be[i] = (uint8_t)(bit_len >> (56 - 8 * i));
    }
    /* Bypass sha1_update's own padding recursion for the length field —
     * write it straight into the block buffer, which is already at 56
     * bytes used from the loop above. */
    memcpy(c->buf + 56, len_be, 8);
    sha1_block(c, c->buf);
    for (int i = 0; i < 5; i++) {
        out[i * 4] = (uint8_t)(c->h[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(c->h[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(c->h[i] >> 8);
        out[i * 4 + 3] = (uint8_t)c->h[i];
    }
}

/* --- base64 (encode-only) --------------------------------------------- */

static const char b64_tab[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static size_t base64_encode(const uint8_t *in, size_t len, char *out, size_t out_cap) {
    size_t need = ((len + 2) / 3) * 4;
    if (out_cap < need + 1) {
        return 0;
    }
    size_t o = 0;
    size_t i = 0;
    while (i + 3 <= len) {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8) | in[i + 2];
        out[o++] = b64_tab[(v >> 18) & 0x3F];
        out[o++] = b64_tab[(v >> 12) & 0x3F];
        out[o++] = b64_tab[(v >> 6) & 0x3F];
        out[o++] = b64_tab[v & 0x3F];
        i += 3;
    }
    size_t rem = len - i;
    if (rem == 1) {
        uint32_t v = (uint32_t)in[i] << 16;
        out[o++] = b64_tab[(v >> 18) & 0x3F];
        out[o++] = b64_tab[(v >> 12) & 0x3F];
        out[o++] = '=';
        out[o++] = '=';
    } else if (rem == 2) {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8);
        out[o++] = b64_tab[(v >> 18) & 0x3F];
        out[o++] = b64_tab[(v >> 12) & 0x3F];
        out[o++] = b64_tab[(v >> 6) & 0x3F];
        out[o++] = '=';
    }
    out[o] = 0;
    return o;
}

/* --- handshake ---------------------------------------------------------*/

#define WS_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

void ws_accept_key(const char *key, size_t key_len, char *out, size_t out_cap) {
    sha1_ctx_t c;
    sha1_init(&c);
    sha1_update(&c, (const uint8_t *)key, key_len);
    sha1_update(&c, (const uint8_t *)WS_GUID, strlen(WS_GUID));
    uint8_t digest[20];
    sha1_final(&c, digest);
    base64_encode(digest, sizeof(digest), out, out_cap);
}

/* --- frame encode (server -> client, text, unmasked) ------------------ */

size_t ws_encode_text_frame(const uint8_t *payload, size_t len, uint8_t *out, size_t out_cap) {
    size_t hdr_len;
    if (len <= 125) {
        hdr_len = 2;
    } else if (len <= 0xFFFF) {
        hdr_len = 4;
    } else {
        return 0; /* not needed for one protocol line at a time */
    }
    if (out_cap < hdr_len + len) {
        return 0;
    }
    out[0] = 0x81; /* FIN=1, opcode=1 (text) */
    if (len <= 125) {
        out[1] = (uint8_t)len;
    } else {
        out[1] = 126;
        out[2] = (uint8_t)(len >> 8);
        out[3] = (uint8_t)len;
    }
    memcpy(out + hdr_len, payload, len);
    return hdr_len + len;
}

/* --- frame parse (client -> server, always masked) --------------------- */

void ws_parser_reset(ws_parser_t *p) {
    memset(p, 0, sizeof(*p));
    p->hdr_need = 2;
}

size_t ws_parser_feed(ws_parser_t *p, const uint8_t *data, size_t len, ws_on_text_fn on_text,
                      void *user, bool *closed) {
    *closed = false;
    size_t consumed = 0;
    while (consumed < len) {
        if (!p->in_payload) {
            size_t want = p->hdr_need - p->hdr_got;
            size_t take = len - consumed;
            if (take > want) {
                take = want;
            }
            memcpy(p->hdr + p->hdr_got, data + consumed, take);
            p->hdr_got += take;
            consumed += take;
            if (p->hdr_got < p->hdr_need) {
                break; /* header still incomplete, wait for more data */
            }
            /* Have at least the first 2 bytes; hdr_need may grow once we
             * see the length field, in which case loop again next byte. */
            if (p->hdr_need == 2) {
                uint8_t len7 = p->hdr[1] & 0x7F;
                if (len7 == 126) {
                    p->hdr_need = 2 + 2 + 4;
                    continue;
                } else if (len7 == 127) {
                    p->hdr_need = 2 + 8 + 4;
                    continue;
                } else {
                    p->hdr_need = 2 + 4;
                    continue;
                }
            }
            /* Full header (including mask) now in p->hdr. */
            bool masked = (p->hdr[1] & 0x80) != 0;
            uint8_t len7 = p->hdr[1] & 0x7F;
            size_t mask_off;
            if (len7 == 126) {
                p->payload_len = ((size_t)p->hdr[2] << 8) | p->hdr[3];
                mask_off = 4;
            } else if (len7 == 127) {
                /* Only the low 32 bits are used — a single protocol line
                 * will never approach 4GiB; anything with high bits set
                 * is treated as oversized below. */
                p->payload_len = ((size_t)p->hdr[6] << 24) | ((size_t)p->hdr[7] << 16) |
                                 ((size_t)p->hdr[8] << 8) | p->hdr[9];
                mask_off = 10;
            } else {
                p->payload_len = len7;
                mask_off = 2;
            }
            uint8_t opcode = p->hdr[0] & 0x0F;
            if (opcode == 0x8 /* close */) {
                *closed = true;
                return consumed;
            }
            if (!masked || p->payload_len > sizeof(p->payload)) {
                /* Spec violation (client frames must be masked) or a
                 * payload bigger than any protocol line we'd ever send —
                 * simplest safe response is to close the connection. */
                *closed = true;
                return consumed;
            }
            memcpy(p->mask, p->hdr + mask_off, 4);
            p->payload_got = 0;
            p->in_payload = (p->payload_len > 0);
            if (p->payload_len == 0 && (opcode == 0x1 || opcode == 0x2)) {
                on_text(p->payload, 0, user);
            }
            /* Stash the opcode in hdr[0]'s low nibble for when the
             * payload finishes below (hdr is otherwise done with). */
            p->hdr[0] = opcode;
            if (!p->in_payload) {
                ws_parser_reset(p);
            }
            continue;
        }

        /* Reading (and unmasking) the payload. */
        size_t want = p->payload_len - p->payload_got;
        size_t take = len - consumed;
        if (take > want) {
            take = want;
        }
        for (size_t i = 0; i < take; i++) {
            p->payload[p->payload_got + i] =
                data[consumed + i] ^ p->mask[(p->payload_got + i) % 4];
        }
        p->payload_got += take;
        consumed += take;
        if (p->payload_got >= p->payload_len) {
            uint8_t opcode = p->hdr[0];
            if (opcode == 0x1 || opcode == 0x2) {
                on_text(p->payload, p->payload_len, user);
            }
            ws_parser_reset(p);
        }
    }
    return consumed;
}
