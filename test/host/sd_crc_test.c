/*
 * src/sd/sd_crc.c against plain bit-by-bit references, including the claim
 * the SDIO driver rests on: one CRC64 over the data bytes equals the four
 * per-line CRC16s of a 4-bit block, interleaved.
 *
 *   gcc -Isrc test/host/sd_crc_test.c src/sd/sd_crc.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sd/sd_crc.h"

static int checks, failed;
#define CHECK(c, msg) do { checks++; if (!(c)) { failed++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, msg); } } while (0)

/* bit-serial CRC over `nbits` bits from a bit getter */
static uint16_t crc16_bits(const uint8_t *bits, size_t n) {
    uint16_t c = 0;
    for (size_t i = 0; i < n; i++) {
        int fb = ((c >> 15) & 1) ^ bits[i];
        c = (uint16_t)(c << 1);
        if (fb) { c ^= 0x1021; }
    }
    return c;
}
static uint8_t crc7_bits(const uint8_t *p, size_t nbytes) {
    uint8_t c = 0;
    for (size_t i = 0; i < nbytes * 8; i++) {
        int bit = (p[i / 8] >> (7 - i % 8)) & 1;
        int fb = ((c >> 6) & 1) ^ bit;
        c = (uint8_t)((c << 1) & 0x7F);
        if (fb) { c ^= 0x09; }
    }
    return c;
}

int main(void) {
    /* The spec's own examples (Physical Layer Simplified Spec 4.5). */
    uint8_t cmd0[5] = { 0x40, 0, 0, 0, 0 };
    CHECK(sd_crc7(cmd0, 5) == 0x4A, "CMD0 -> CRC7 0x4A (0x95 on the wire)");
    uint8_t cmd17[5] = { 0x51, 0, 0, 0, 0 };
    CHECK(sd_crc7(cmd17, 5) == 0x2A, "CMD17 arg 0 -> 0x2A");
    uint8_t resp17[5] = { 0x11, 0, 0, 0x09, 0 };
    CHECK(sd_crc7(resp17, 5) == 0x33, "R1 of CMD17 -> 0x33");
    uint8_t cmd8[5] = { 0x48, 0, 0, 0x01, 0xAA };
    CHECK(((sd_crc7(cmd8, 5) << 1) | 1) == 0x87, "CMD8 0x1AA -> 0x87 on the wire");
    uint8_t ff[512];
    memset(ff, 0xFF, sizeof ff);
    CHECK(sd_crc16(ff, 512) == 0x7FA1, "512 x 0xFF -> CRC16 0x7FA1 (spec example)");

    /* random data: table CRC7/CRC16 vs bit-serial */
    srand(7);
    for (int t = 0; t < 200; t++) {
        uint8_t b[64];
        size_t n = 1 + (size_t)(rand() % 64);
        for (size_t i = 0; i < n; i++) { b[i] = (uint8_t)rand(); }
        uint8_t bits[64 * 8];
        for (size_t i = 0; i < n * 8; i++) { bits[i] = (b[i / 8] >> (7 - i % 8)) & 1; }
        if (sd_crc16(b, n) != crc16_bits(bits, n * 8) || sd_crc7(b, n) != crc7_bits(b, n)) {
            CHECK(0, "table CRC differs from bit-serial");
            break;
        }
    }
    checks++;

    /* the 4-line claim: per line, collect that line's bits and CRC16 them */
    for (int t = 0; t < 50; t++) {
        uint8_t blk[512];
        for (int i = 0; i < 512; i++) { blk[i] = (uint8_t)rand(); }
        if (t == 0) { memset(blk, 0xFF, 512); }
        uint64_t all = sd_crc16x4(blk, 512);
        for (int line = 0; line < 4; line++) {
            uint8_t bits[1024];
            for (int k = 0; k < 1024; k++) {           /* nibble k: high nibble of byte k/2 first */
                uint8_t nib = (k & 1) ? (blk[k / 2] & 0x0F) : (blk[k / 2] >> 4);
                bits[k] = (nib >> line) & 1;
            }
            uint16_t want = crc16_bits(bits, 1024);
            if (sd_crc16x4_line(all, line) != want) {
                printf("line %d: got %04x want %04x\n", line, sd_crc16x4_line(all, line), want);
                CHECK(0, "CRC64 does not split into the per-line CRC16s");
                t = 99;
                break;
            }
        }
    }
    checks++;

    /* and the wire order: the 16 CRC nibbles, MSB first, carry bit i of every
     * line's CRC in nibble 15-i, D3..D0 */
    uint8_t blk[512];
    for (int i = 0; i < 512; i++) { blk[i] = (uint8_t)(i * 7 + 3); }
    uint64_t all = sd_crc16x4(blk, 512);
    int ok = 1;
    for (int k = 0; k < 16; k++) {
        uint8_t nib = (uint8_t)((all >> (60 - 4 * k)) & 0xF);
        for (int line = 0; line < 4; line++) {
            if (((nib >> line) & 1) != ((sd_crc16x4_line(all, line) >> (15 - k)) & 1)) { ok = 0; }
        }
    }
    CHECK(ok, "CRC nibbles go out MSB first, one bit of each line's CRC per nibble");

    printf("%d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
