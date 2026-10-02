/* gzip inflate into a flat buffer -- after Mark Adler's puff.c (zlib/contrib).
 *
 * The board only ever DEcompresses: the browser compresses (CompressionStream)
 * and the board inflates a compressed Lua source when it must compile it.
 * The whole output lands in one caller buffer (lrt.c's 32 KB source buffer),
 * and that buffer is also the window -- a back-reference simply reads what
 * was already written -- so no 32 KB window of its own is needed. Input is
 * pulled through a callback in small pieces.
 *
 * Checked: the gzip header (any FEXTRA / FNAME / FCOMMENT / FHCRC), every
 * block type, distances never before the start, output never past the end,
 * and the trailer's CRC-32 and length. Anything wrong is an error, never a
 * crash or a partial "success". Host-tested against zlib on random and real
 * inputs, corrupt ones included (test/host/inflate_test.c). */

#include <setjmp.h>
#include <string.h>

#include "util/inflate.h"

#define MAXBITS 15
#define MAXLCODES 286
#define MAXDCODES 30
#define FIXLCODES 288

typedef struct {
    inflate_read_fn rd;
    void *ctx;
    uint8_t in[256];
    size_t in_len, in_pos;
    uint32_t bitbuf;
    int bitcnt;
    uint8_t *out;
    size_t cap, len;
    jmp_buf env;
} state_t;

typedef struct {
    short count[MAXBITS + 1];
    short symbol[FIXLCODES];
} huff_t;

static void fail(state_t *s, int err) { longjmp(s->env, err); }

static int byte_in(state_t *s) {
    if (s->in_pos == s->in_len) {
        int n = s->rd(s->ctx, s->in, sizeof s->in);
        if (n <= 0) { fail(s, n < 0 ? INFLATE_EREAD : INFLATE_ETRUNC); }
        s->in_len = (size_t)n;
        s->in_pos = 0;
    }
    return s->in[s->in_pos++];
}

static int bits(state_t *s, int need) {
    long val = s->bitbuf;
    while (s->bitcnt < need) {
        val |= (long)byte_in(s) << s->bitcnt;
        s->bitcnt += 8;
    }
    s->bitbuf = (uint32_t)(val >> need);
    s->bitcnt -= need;
    return (int)(val & ((1L << need) - 1));
}

static void put(state_t *s, uint8_t b) {
    if (s->len >= s->cap) { fail(s, INFLATE_ETOOBIG); }
    s->out[s->len++] = b;
}

static void stored(state_t *s) {
    s->bitbuf = 0;
    s->bitcnt = 0;                                   /* to a byte boundary */
    unsigned len = (unsigned)byte_in(s);
    len |= (unsigned)byte_in(s) << 8;
    unsigned nlen = (unsigned)byte_in(s);
    nlen |= (unsigned)byte_in(s) << 8;
    if (len != (~nlen & 0xffffu)) { fail(s, INFLATE_EDATA); }
    while (len--) { put(s, (uint8_t)byte_in(s)); }
}

static int decode(state_t *s, const huff_t *h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= MAXBITS; len++) {
        code |= bits(s, 1);
        int count = h->count[len];
        if (code - count < first) { return h->symbol[index + (code - first)]; }
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    fail(s, INFLATE_EDATA);
    return -1;
}

/* 0 for a complete code, > 0 incomplete (allowed only for a single
 * distance code), < 0 over-subscribed. */
static int construct(huff_t *h, const short *length, int n) {
    short offs[MAXBITS + 1];
    for (int len = 0; len <= MAXBITS; len++) { h->count[len] = 0; }
    for (int sym = 0; sym < n; sym++) { h->count[length[sym]]++; }
    if (h->count[0] == n) { return 0; }
    int left = 1;
    for (int len = 1; len <= MAXBITS; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0) { return left; }
    }
    offs[1] = 0;
    for (int len = 1; len < MAXBITS; len++) { offs[len + 1] = (short)(offs[len] + h->count[len]); }
    for (int sym = 0; sym < n; sym++) {
        if (length[sym] != 0) { h->symbol[offs[length[sym]]++] = (short)sym; }
    }
    return left;
}

static const short LBASE[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const short LEXT[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const short DBASE[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
static const short DEXT[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

static void codes(state_t *s, const huff_t *lc, const huff_t *dc) {
    for (;;) {
        int sym = decode(s, lc);
        if (sym < 256) { put(s, (uint8_t)sym); continue; }
        if (sym == 256) { return; }
        sym -= 257;
        if (sym >= 29) { fail(s, INFLATE_EDATA); }
        int len = LBASE[sym] + bits(s, LEXT[sym]);
        int d = decode(s, dc);
        if (d < 0 || d >= 30) { fail(s, INFLATE_EDATA); }
        size_t dist = (size_t)(DBASE[d] + bits(s, DEXT[d]));
        if (dist > s->len) { fail(s, INFLATE_EDATA); }      /* before the start */
        while (len--) { put(s, s->out[s->len - dist]); }
    }
}

static void fixed(state_t *s) {
    static huff_t lc, dc;
    static int built;
    if (!built) {
        short len[FIXLCODES];
        int sym = 0;
        for (; sym < 144; sym++) { len[sym] = 8; }
        for (; sym < 256; sym++) { len[sym] = 9; }
        for (; sym < 280; sym++) { len[sym] = 7; }
        for (; sym < FIXLCODES; sym++) { len[sym] = 8; }
        construct(&lc, len, FIXLCODES);
        for (sym = 0; sym < MAXDCODES; sym++) { len[sym] = 5; }
        construct(&dc, len, MAXDCODES);
        built = 1;
    }
    codes(s, &lc, &dc);
}

static void dynamic(state_t *s) {
    static const short ORDER[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
    short len[MAXLCODES + MAXDCODES];
    huff_t lc, dc;
    int nlen = bits(s, 5) + 257;
    int ndist = bits(s, 5) + 1;
    int ncode = bits(s, 4) + 4;
    if (nlen > MAXLCODES || ndist > MAXDCODES) { fail(s, INFLATE_EDATA); }
    int i = 0;
    for (; i < ncode; i++) { len[ORDER[i]] = (short)bits(s, 3); }
    for (; i < 19; i++) { len[ORDER[i]] = 0; }
    if (construct(&lc, len, 19) != 0) { fail(s, INFLATE_EDATA); }
    i = 0;
    while (i < nlen + ndist) {
        int sym = decode(s, &lc);
        if (sym < 16) { len[i++] = (short)sym; continue; }
        int rep;
        short val = 0;
        if (sym == 16) {
            if (i == 0) { fail(s, INFLATE_EDATA); }
            val = len[i - 1];
            rep = 3 + bits(s, 2);
        } else if (sym == 17) {
            rep = 3 + bits(s, 3);
        } else {
            rep = 11 + bits(s, 7);
        }
        if (i + rep > nlen + ndist) { fail(s, INFLATE_EDATA); }
        while (rep--) { len[i++] = val; }
    }
    if (len[256] == 0) { fail(s, INFLATE_EDATA); }       /* no end-of-block code */
    int err = construct(&lc, len, nlen);
    if (err < 0 || (err > 0 && nlen - lc.count[0] != 1)) { fail(s, INFLATE_EDATA); }
    err = construct(&dc, len + nlen, ndist);
    if (err < 0 || (err > 0 && ndist - dc.count[0] != 1)) { fail(s, INFLATE_EDATA); }
    codes(s, &lc, &dc);
}

uint32_t inflate_crc32(const uint8_t *p, size_t n) {
    uint32_t c = 0xffffffffu;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++) { c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1u))); }
    }
    return ~c;
}

long gz_inflate(inflate_read_fn rd, void *ctx, uint8_t *out, size_t cap) {
    static state_t s;                    /* ~300 B; off the (small) C stack */
    memset(&s, 0, sizeof s);
    s.rd = rd;
    s.ctx = ctx;
    s.out = out;
    s.cap = cap;
    int err = setjmp(s.env);
    if (err) { return err; }

    if (byte_in(&s) != 0x1f || byte_in(&s) != 0x8b || byte_in(&s) != 8) { return INFLATE_ENOTGZ; }
    int flg = byte_in(&s);
    for (int i = 0; i < 6; i++) { (void)byte_in(&s); }    /* mtime, xfl, os */
    if (flg & 4) {                                         /* FEXTRA */
        unsigned xlen = (unsigned)byte_in(&s);
        xlen |= (unsigned)byte_in(&s) << 8;
        while (xlen--) { (void)byte_in(&s); }
    }
    if (flg & 8) { while (byte_in(&s) != 0) {} }           /* FNAME */
    if (flg & 16) { while (byte_in(&s) != 0) {} }          /* FCOMMENT */
    if (flg & 2) { (void)byte_in(&s); (void)byte_in(&s); } /* FHCRC */

    int last;
    do {
        last = bits(&s, 1);
        int type = bits(&s, 2);
        if (type == 0) { stored(&s); }
        else if (type == 1) { fixed(&s); }
        else if (type == 2) { dynamic(&s); }
        else { return INFLATE_EDATA; }
    } while (!last);

    s.bitbuf = 0;
    s.bitcnt = 0;
    uint32_t crc = 0, isize = 0;
    for (int i = 0; i < 4; i++) { crc |= (uint32_t)byte_in(&s) << (8 * i); }
    for (int i = 0; i < 4; i++) { isize |= (uint32_t)byte_in(&s) << (8 * i); }
    if (isize != (uint32_t)s.len || crc != inflate_crc32(out, s.len)) { return INFLATE_ECHECK; }
    return (long)s.len;
}
