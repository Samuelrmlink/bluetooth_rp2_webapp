#include "net/ws_link.h"

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

size_t ws_b64_encode(const uint8_t *in, size_t n, char *out, size_t cap) {
    size_t need = ((n + 2u) / 3u) * 4u;
    size_t i = 0;
    size_t o = 0;
    if (!out || cap < need + 1u) {
        return 0;
    }
    if (!in && n) {
        return 0;
    }
    while (i + 3u <= n) {
        unsigned v = ((unsigned)in[i] << 16) | ((unsigned)in[i + 1] << 8) | in[i + 2];
        out[o++] = B64[(v >> 18) & 63u];
        out[o++] = B64[(v >> 12) & 63u];
        out[o++] = B64[(v >> 6) & 63u];
        out[o++] = B64[v & 63u];
        i += 3u;
    }
    if (i < n) {
        unsigned v = (unsigned)in[i] << 16;
        int two = (i + 1u < n);
        if (two) {
            v |= (unsigned)in[i + 1] << 8;
        }
        out[o++] = B64[(v >> 18) & 63u];
        out[o++] = B64[(v >> 12) & 63u];
        out[o++] = two ? B64[(v >> 6) & 63u] : '=';
        out[o++] = '=';
    }
    out[o] = 0;
    return o;
}

static int b64_val(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

int ws_b64_decode(const char *in, size_t n, uint8_t *out, size_t cap) {
    size_t o = 0;
    size_t i;
    if (!in || (n % 4u) != 0u) {
        return -1;
    }
    if (!out && n) {
        return -1;
    }
    for (i = 0; i < n; i += 4u) {
        int v[4];
        int pad = 0;
        int k;
        size_t need;
        for (k = 0; k < 4; k++) {
            unsigned char c = (unsigned char)in[i + (size_t)k];
            if (c == '=') {
                if (k < 2) {
                    return -1;
                }
                v[k] = 0;
                pad++;
            } else {
                if (pad) {
                    return -1;
                }
                v[k] = b64_val(c);
                if (v[k] < 0) {
                    return -1;
                }
            }
        }
        if (pad > 2) {
            return -1;
        }
        need = (size_t)(3 - pad);
        if (o + need > cap) {
            return -1;
        }
        out[o++] = (uint8_t)((v[0] << 2) | (v[1] >> 4));
        if (pad < 2) {
            out[o++] = (uint8_t)(((v[1] << 4) | (v[2] >> 2)) & 0xff);
        }
        if (pad < 1) {
            out[o++] = (uint8_t)(((v[2] << 6) | v[3]) & 0xff);
        }
    }
    return (int)o;
}
