/* lfile_parse_path: the volume prefix, slash normalisation, and every way a
 * path can be bad (both volumes' character sets, the 64/128 limits, "."/"..",
 * empty segments, an embedded NUL). */
#include <stdio.h>
#include <string.h>

#include "lua/lfile_path.h"

static int failures, checks;

static void want(const char *in, int vol, const char *rel) {
    char out[LFILE_PATH_MAX + 1];
    int v = lfile_parse_path(in, strlen(in), out, sizeof out);
    checks++;
    if (v != vol || (vol && strcmp(out, rel) != 0)) {
        printf("FAIL: '%s' -> vol %d '%s', want vol %d '%s'\n", in, v, v ? out : "", vol, rel ? rel : "");
        failures++;
    }
}

static void bad(const char *in) { want(in, 0, NULL); }

int main(void) {
    want("lfs:/a/b.ext", LFILE_LFS, "a/b.ext");
    want("lfs:a/b.ext", LFILE_LFS, "a/b.ext");
    want("lfs:///a/b/", LFILE_LFS, "a/b");
    want("lfs:/", LFILE_LFS, "");
    want("lfs:", LFILE_LFS, "");
    want("sd:/", LFILE_SD, "");
    want("sd:/x/y.tmp", LFILE_SD, "x/y.tmp");
    want("sd:/Firmware Images/v1.2 (beta)/fw.bleota", LFILE_SD, "Firmware Images/v1.2 (beta)/fw.bleota");
    want("sd:/caf\xc3\xa9.txt", LFILE_SD, "caf\xc3\xa9.txt");
    want("lfs:/cfg/autostart.json", LFILE_LFS, "cfg/autostart.json");
    want("lfs:/a.b.c/d-e_f", LFILE_LFS, "a.b.c/d-e_f");
    want("sd:/.hidden", LFILE_SD, ".hidden");
    want("sd:/a/...b", LFILE_SD, "a/...b");

    bad("");
    bad("/a/b");
    bad("a/b");
    bad("LFS:/a");
    bad("sd/a");
    bad("http://x");
    bad("lfs:/a//b");
    bad("lfs:/a/./b");
    bad("lfs:/a/../b");
    bad("lfs:/..");
    bad("sd:/..");
    bad("sd:/a/..");
    bad("sd:/.");
    bad("lfs:/.hidden");           /* fs_valid_path refuses a leading dot */
    bad("lfs:/a b");               /* LittleFS charset */
    bad("lfs:/caf\xc3\xa9");
    bad("lfs:/a\\b");
    bad("sd:/a\\b");
    bad("sd:/a:b");
    bad("sd:/a*b");
    bad("sd:/a?b");
    bad("sd:/a\"b");
    bad("sd:/a<b");
    bad("sd:/a>b");
    bad("sd:/a|b");
    bad("sd:/a\tb");
    bad("sd:/a\x7f");

    /* Segment limit: 64 is fine, 65 is not. */
    char p[300];
    memset(p, 0, sizeof p);
    strcpy(p, "lfs:/");
    memset(p + 5, 'x', 64);
    char rel[100];
    memset(rel, 0, sizeof rel);
    memset(rel, 'x', 64);
    want(p, LFILE_LFS, rel);
    p[5 + 64] = 'x';
    bad(p);

    /* Path limit: 128 is fine, 129 is not (segments of 32 + '/'). */
    char rel128[200] = "";
    while (strlen(rel128) + 33 <= 128) { strcat(rel128, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa/"); }
    while (strlen(rel128) < 128) { strcat(rel128, "b"); }
    snprintf(p, sizeof p, "sd:/%s", rel128);
    want(p, LFILE_SD, rel128);
    snprintf(p, sizeof p, "sd:/%sb", rel128);
    bad(p);

    /* An embedded NUL is not the end of the path. */
    {
        char out[LFILE_PATH_MAX + 1];
        checks++;
        if (lfile_parse_path("lfs:/a\0b", 8, out, sizeof out) != 0) { printf("FAIL: NUL accepted\n"); failures++; }
        checks++;
        if (lfile_parse_path("lfs:/abc", 8, out, 3) != 0) { printf("FAIL: small out accepted\n"); failures++; }
    }

    printf("lfile paths: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
