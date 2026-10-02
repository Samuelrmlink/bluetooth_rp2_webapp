/* Host test for src/util/inflate.c: each .gz in the directory given must
 * inflate to the bytes of the matching .raw (tools/run_tests.py writes them
 * with Python's zlib); each .bad must fail cleanly. Prints one line per file
 * that disagrees and a summary. */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util/inflate.h"

typedef struct { const uint8_t *p; size_t n, at, piece; } src_t;

static int rd(void *ctx, uint8_t *buf, size_t cap) {
    src_t *s = ctx;
    size_t k = s->n - s->at;
    if (k > cap) { k = cap; }
    if (k > s->piece) { k = s->piece; }          /* odd piece sizes: refills mid-code */
    memcpy(buf, s->p + s->at, k);
    s->at += k;
    return (int)k;
}

static uint8_t *slurp(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) { return NULL; }
    fseek(f, 0, SEEK_END);
    *n = (size_t)ftell(f);
    rewind(f);
    uint8_t *b = malloc(*n + 1);
    if (fread(b, 1, *n, f) != *n) { *n = 0; }
    fclose(f);
    return b;
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : ".";
    static uint8_t out[32768];
    int good = 0, bad = 0, failed = 0;
    DIR *d = opendir(dir);
    struct dirent *e;
    char path[512];
    while ((e = readdir(d))) {
        size_t L = strlen(e->d_name);
        int is_gz = L > 3 && !strcmp(e->d_name + L - 3, ".gz");
        int is_bad = L > 4 && !strcmp(e->d_name + L - 4, ".bad");
        if (!is_gz && !is_bad) { continue; }
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        size_t n;
        uint8_t *gz = slurp(path, &n);
        for (size_t piece = 1; piece <= 300; piece = piece < 8 ? piece + 1 : piece * 3) {
            src_t s = { gz, n, 0, piece };
            long r = gz_inflate(rd, &s, out, sizeof out);
            if (is_bad) {
                if (r >= 0) { printf("FAIL %s: corrupt input inflated (%ld B)\n", e->d_name, r); failed++; }
                else { bad++; }
                continue;
            }
            snprintf(path, sizeof path, "%s/%.*s.raw", dir, (int)(L - 3), e->d_name);
            size_t rn;
            uint8_t *raw = slurp(path, &rn);
            int want_big = rn > sizeof out;
            if (want_big ? r != INFLATE_ETOOBIG : (r != (long)rn || memcmp(out, raw, rn) != 0)) {
                printf("FAIL %s (piece %zu): got %ld, want %s%zu\n", e->d_name, piece, r, want_big ? "ETOOBIG for " : "", rn);
                failed++;
            } else {
                good++;
            }
            free(raw);
        }
        free(gz);
    }
    closedir(d);
    printf("%d inflated, %d rejected, %d failed\n", good, bad, failed);
    return failed ? 1 : 0;
}
