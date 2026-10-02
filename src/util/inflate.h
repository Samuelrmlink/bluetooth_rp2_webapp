#ifndef BTGW_INFLATE_H
#define BTGW_INFLATE_H

#include <stddef.h>
#include <stdint.h>

/* A gzip stream decompressed into one flat buffer (inflate.c). The board
 * never compresses; the browser does. */

/* Fill buf with up to cap input bytes: how many, 0 at the end, < 0 on error. */
typedef int (*inflate_read_fn)(void *ctx, uint8_t *buf, size_t cap);

enum {
    INFLATE_ENOTGZ = -1,   /* not a gzip stream (or not deflate) */
    INFLATE_ETRUNC = -2,   /* input ended early */
    INFLATE_EREAD = -3,    /* the read callback failed */
    INFLATE_EDATA = -4,    /* invalid deflate data */
    INFLATE_ETOOBIG = -5,  /* the output does not fit cap */
    INFLATE_ECHECK = -6,   /* CRC-32 or length in the trailer wrong */
};

/* The decompressed length, or a negative INFLATE_E*. Not reentrant. */
long gz_inflate(inflate_read_fn rd, void *ctx, uint8_t *out, size_t cap);

uint32_t inflate_crc32(const uint8_t *p, size_t n);

#endif
