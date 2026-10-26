/* fs_batch_parse / covers / next. No littlefs. Prints "N checks, M failed". */
#include <stdio.h>
#include <string.h>

#include "fs/fs_batch.h"

static int checks, failures;

static void expect(int cond, const char *what) {
    checks++;
    if (!cond) {
        printf("FAIL: %s\n", what);
        failures++;
    }
}

static void test_parse(void) {
    char paths[FS_BATCH_MAX][FS_BATCH_PATH];
    int n = 0;
    uint32_t mtime = 0, id = 0;
    const char *ok = "{\"paths\":[\"a\",\"b\",\"c\"],\"mtime\":12,\"id\":3}";
    expect(fs_batch_parse(ok, strlen(ok), paths, &n, &mtime, &id) == 0, "parse 3");
    expect(n == 3 && strcmp(paths[0], "a") == 0 && strcmp(paths[2], "c") == 0, "paths a,c");
    expect(mtime == 12 && id == 3, "mtime and id");

    const char *id_first = "{\"id\":4,\"paths\":[\"x\"]}";
    expect(fs_batch_parse(id_first, strlen(id_first), paths, &n, &mtime, &id) == 0, "id before paths");
    expect(n == 1 && id == 4 && strcmp(paths[0], "x") == 0, "id 4 path x");

    const char *esc = "{\"paths\":[\"a\\\"b\\\\c\\/d\\n\",\"\\u0041\"]}";
    expect(fs_batch_parse(esc, strlen(esc), paths, &n, &mtime, &id) == 0, "escapes");
    expect(n == 2 && strcmp(paths[0], "a\"b\\c/d\n") == 0 && strcmp(paths[1], "A") == 0, "unescaped");

    const char *missing = "{\"mtime\":1}";
    expect(fs_batch_parse(missing, strlen(missing), paths, &n, &mtime, &id) == -1, "missing paths");
    const char *empty = "{\"paths\":[]}";
    expect(fs_batch_parse(empty, strlen(empty), paths, &n, &mtime, &id) == -1, "empty array");
    const char *blank = "{\"paths\":[\"\"]}";
    expect(fs_batch_parse(blank, strlen(blank), paths, &n, &mtime, &id) == -1, "empty string");
    const char *high = "{\"paths\":[\"\\u0080\"]}";
    expect(fs_batch_parse(high, strlen(high), paths, &n, &mtime, &id) == -1, "unicode above 7f");

    char longp[200];
    memset(longp, 'p', 127);
    longp[127] = 0;
    char body[256];
    snprintf(body, sizeof body, "{\"paths\":[\"%s\"]}", longp);
    expect(fs_batch_parse(body, strlen(body), paths, &n, &mtime, &id) == 0, "127-char path");
    expect(n == 1 && strlen(paths[0]) == 127, "stored 127");

    memset(longp, 'q', 128);
    longp[128] = 0;
    snprintf(body, sizeof body, "{\"paths\":[\"%s\"]}", longp);
    expect(fs_batch_parse(body, strlen(body), paths, &n, &mtime, &id) == -1, "128-char path refused");

    char many[4096];
    size_t u = 0;
    u += (size_t)snprintf(many + u, sizeof many - u, "{\"paths\":[");
    for (int i = 0; i < 33; i++) {
        u += (size_t)snprintf(many + u, sizeof many - u, "%s\"p%d\"", i ? "," : "", i);
    }
    snprintf(many + u, sizeof many - u, "]}");
    expect(fs_batch_parse(many, strlen(many), paths, &n, &mtime, &id) == -2, "33 paths");
}

static void test_covers(void) {
    char roots[4][FS_BATCH_PATH];
    snprintf(roots[0], FS_BATCH_PATH, "pex");
    snprintf(roots[1], FS_BATCH_PATH, "lua/anker.lua");
    expect(fs_batch_covers(roots, 2, "pex"), "exact pex");
    expect(fs_batch_covers(roots, 2, "pex/a"), "pex/a");
    expect(!fs_batch_covers(roots, 2, "pexfoo"), "not pexfoo");
    expect(fs_batch_covers(roots, 2, "lua/anker.lua"), "exact file");
    expect(!fs_batch_covers(roots, 2, "lua/anker.lua.bak"), "file is not a prefix");
    expect(!fs_batch_covers(roots, 2, "other"), "unrelated");
}

/* Step twice, then the connection closes: the index stays and nothing
 * further is deleted. */
static void test_next(void) {
    int index = 0;
    int deleted = 0;
    int n = fs_batch_next(&index, 3, false);
    expect(n == 0, "first index");
    if (n >= 0) { deleted++; }
    n = fs_batch_next(&index, 3, false);
    expect(n == 1, "second index");
    if (n >= 0) { deleted++; }
    int at = index;
    n = fs_batch_next(&index, 3, true);
    expect(n == -1, "closed returns -1");
    expect(index == at, "closed does not advance");
    expect(deleted == 2, "delete not called after close");
    n = fs_batch_next(&index, 3, false);
    expect(n == 2, "third after the close check");
    index = 3;
    expect(fs_batch_next(&index, 3, false) == -2, "done");
    expect(index == 3, "done does not advance");
}

int main(void) {
    test_parse();
    test_covers();
    test_next();
    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
