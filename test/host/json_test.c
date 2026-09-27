/*
 * The Lua -> JSON encoder, on a host, with a guard page after the buffer.
 *
 * This test exists because the encoder shipped with an out-of-bounds write.
 * Every append was `n += snprintf(dst + n, cap - n, ...)`, and snprintf
 * returns the length it WOULD have written -- so once a table outgrew the
 * buffer, n ran past cap, `dst + n` pointed past the end, `cap - n` wrapped to
 * a huge size_t, and the closing brace (emitted unconditionally) was written
 * out of bounds. The trigger was an ordinary Anker telemetry report that grew
 * one field too many.
 *
 * Asserting "the output looks right" would NOT have caught it: the visible
 * symptom was merely truncated JSON. So the buffer here is placed at the end
 * of an mmap'd region with a PROT_NONE page after it, and an overrun is a
 * SIGSEGV rather than a silently corrupted neighbour. Re-introduce the old
 * accumulate-the-return-value idiom in ljson.c and this test crashes.
 *
 * The second thing asserted is the contract the caller depends on: when
 * anything is dropped, *truncated must be true. lbind.c refuses to put a
 * clamped encode on the wire, and that refusal is only as good as this flag.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lua/ljson.h"

static int failures;

static void ok(const char *what, int cond) {
    if (!cond) { printf("FAIL: %s\n", what); failures++; }
}

/* A buffer whose last byte abuts an unmapped page. */
static char *guarded(size_t cap, void **region, size_t *region_len) {
    long pg = sysconf(_SC_PAGESIZE);
    size_t pages = (cap + (size_t)pg - 1) / (size_t)pg;
    size_t len = (pages + 1) * (size_t)pg;
    char *base = mmap(NULL, len, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) { perror("mmap"); exit(2); }
    /* Guard page: any write past the buffer's end faults. */
    if (mprotect(base + pages * (size_t)pg, (size_t)pg, PROT_NONE) != 0) {
        perror("mprotect"); exit(2);
    }
    *region = base;
    *region_len = len;
    /* Right-align, so dst + cap is exactly the guard page. */
    return base + pages * (size_t)pg - cap;
}

static void run_one(lua_State *L, size_t cap, const char *what,
                    int expect_trunc) {
    void *region; size_t region_len;
    char *buf = guarded(cap, &region, &region_len);
    bool trunc = false;
    size_t n = ljson_encode(L, lua_gettop(L), buf, cap, &trunc);

    char msg[160];
    snprintf(msg, sizeof(msg), "%s: n < cap (%zu < %zu)", what, n, cap);
    ok(msg, n < cap);
    snprintf(msg, sizeof(msg), "%s: NUL terminated at n", what);
    ok(msg, buf[n] == 0);
    snprintf(msg, sizeof(msg), "%s: strlen matches n", what);
    ok(msg, strlen(buf) == n);
    if (expect_trunc >= 0) {
        snprintf(msg, sizeof(msg), "%s: truncated flag is %d", what, expect_trunc);
        ok(msg, trunc == (expect_trunc != 0));
    }
    munmap(region, region_len);
}

int main(void) {
    lua_State *L = luaL_newstate();
    luaL_openlibs(L);

    /* 1. A table that fits: no truncation, exact JSON. */
    if (luaL_dostring(L, "return {a=1,b='two',c=true}") != LUA_OK) {
        printf("FAIL: setup 1: %s\n", lua_tostring(L, -1)); return 1;
    }
    {
        char buf[256];
        bool trunc = true;
        size_t n = ljson_encode(L, lua_gettop(L), buf, sizeof(buf), &trunc);
        ok("small table is not truncated", !trunc);
        ok("small table has all three keys",
           strstr(buf, "\"a\":1") && strstr(buf, "\"b\":\"two\"") &&
           strstr(buf, "\"c\":true"));
        ok("small table length agrees", strlen(buf) == n);
    }
    lua_pop(L, 1);

    /* 2. The shape that actually broke it: a telemetry-sized flat table of
     *    numbers, encoded into a buffer it cannot fit. Every cap from tiny to
     *    just-too-small is tried, because the overrun distance depended on
     *    where the buffer ran out relative to a key boundary. */
    if (luaL_dostring(L,
        "local t = {} "
        "for i = 1, 60 do t['field_number_' .. i] = i * 1000 end "
        "t.body_hex = string.rep('ab', 350) "
        "return t") != LUA_OK) {
        printf("FAIL: setup 2: %s\n", lua_tostring(L, -1)); return 1;
    }
    for (size_t cap = 8; cap <= 2048; cap++) {
        run_one(L, cap, "flat telemetry table", 1);
    }
    lua_pop(L, 1);

    /* 3. A long string value, which is the case where the clamped write lands
     *    inside api_json_escape rather than in the encoder itself. */
    if (luaL_dostring(L, "return {s = string.rep('x', 4000)}") != LUA_OK) {
        printf("FAIL: setup 3: %s\n", lua_tostring(L, -1)); return 1;
    }
    for (size_t cap = 8; cap <= 600; cap++) {
        run_one(L, cap, "long string value", 1);
    }
    lua_pop(L, 1);

    /* 4. A string full of characters that escape to six bytes each, so the
     *    escape expands well past the room left. */
    if (luaL_dostring(L, "return {s = string.rep('\\1\\2\"\\\\\\n', 200)}") != LUA_OK) {
        printf("FAIL: setup 4: %s\n", lua_tostring(L, -1)); return 1;
    }
    for (size_t cap = 8; cap <= 600; cap++) {
        run_one(L, cap, "escape-heavy string", 1);
    }
    lua_pop(L, 1);

    /* 5. Nested tables past the depth limit, and a long array. Both take the
     *    recursive path, which is where a stale n does the most damage. */
    if (luaL_dostring(L,
        "local t = {} local c = t "
        "for i = 1, 12 do c.child = {} c = c.child end "
        "t.arr = {} for i = 1, 300 do t.arr[i] = i end "
        "return t") != LUA_OK) {
        printf("FAIL: setup 5: %s\n", lua_tostring(L, -1)); return 1;
    }
    for (size_t cap = 8; cap <= 1200; cap++) {
        run_one(L, cap, "deep nesting and long array", -1);
    }
    lua_pop(L, 1);

    /* 6. A cap of exactly 1 leaves room for nothing but the terminator. */
    if (luaL_dostring(L, "return {a=1}") != LUA_OK) {
        printf("FAIL: setup 6: %s\n", lua_tostring(L, -1)); return 1;
    }
    run_one(L, 1, "cap of one", 1);
    run_one(L, 2, "cap of two", 1);
    lua_pop(L, 1);

    lua_close(L);

    if (failures) { printf("%d check(s) FAILED\n", failures); return 1; }
    printf("json encoder: bounded under a guard page; truncation always reported\n");
    return 0;
}
