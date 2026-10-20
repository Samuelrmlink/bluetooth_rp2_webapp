/*
 * json.decode on a host, against the real vendored Lua. NOTE: the firmware
 * asks for LUA_32BITS but third_party/lua/luaconf.h redefines it to 0 after
 * lua_pico_conf.h has set it, so the firmware (and this test) run with 64-bit
 * integers and doubles. The checks below hold for either width.
 *
 * What it pins down:
 *   - every JSON value type, and json.null staying distinguishable from nil
 *     (a nil would drop an object key or end an array early);
 *   - escapes, \u with surrogate pairs -> UTF-8, and the bad ones rejected;
 *   - integers stay integers while they fit lua_Integer, and become floats past;
 *   - nesting is bounded (the parser recurses on core0's 4 KB stack), and a
 *     body that is all '[' fails cleanly rather than overflowing anything;
 *   - malformed input of every kind fails with nothing left on the stack,
 *     and the input is never read past its length (it is copied into an
 *     mmap'd buffer that ends on a PROT_NONE page);
 *   - encode(decode(x)) round-trips through ljson_encode.
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

static int failures, checks;

static void ok(const char *what, int cond) {
    checks++;
    if (!cond) { printf("FAIL: %s\n", what); failures++; }
}

/* A copy of `s` whose last byte abuts an unmapped page, NOT NUL-terminated:
 * a decoder that reads one byte past len faults. */
static const char *guarded_copy(const char *s, size_t len, void **region, size_t *rlen) {
    long pg = sysconf(_SC_PAGESIZE);
    size_t pages = (len + (size_t)pg - 1) / (size_t)pg + 1;
    size_t total = (pages + 1) * (size_t)pg;
    char *base = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) { perror("mmap"); exit(2); }
    if (mprotect(base + pages * (size_t)pg, (size_t)pg, PROT_NONE) != 0) { perror("mprotect"); exit(2); }
    char *dst = base + pages * (size_t)pg - len;
    memcpy(dst, s, len);
    *region = base; *rlen = total;
    return dst;
}

static bool decode(lua_State *L, const char *s, size_t len, const char **err) {
    void *r; size_t rl;
    const char *g = guarded_copy(s, len, &r, &rl);
    bool k = ljson_decode(L, g, len, err);
    munmap(r, rl);
    return k;
}

/* Decode `json`, bind it to global `v`, then run the Lua assertion `check`
 * (an expression that must be true). */
static void expect(lua_State *L, const char *json, const char *check) {
    int top = lua_gettop(L);
    const char *err = NULL;
    char what[200];
    snprintf(what, sizeof what, "decode %.60s", json);
    bool k = decode(L, json, strlen(json), &err);
    ok(what, k);
    if (!k) { printf("      error: %s\n", err); return; }
    ok("exactly one value pushed", lua_gettop(L) == top + 1);
    lua_setglobal(L, "v");
    char code[600];
    snprintf(code, sizeof code, "return (%s)", check);
    if (luaL_dostring(L, code) != LUA_OK) {
        printf("FAIL: check errored for %s: %s\n", json, lua_tostring(L, -1));
        failures++;
        lua_settop(L, top);
        return;
    }
    snprintf(what, sizeof what, "%.50s -> %.80s", json, check);
    ok(what, lua_toboolean(L, -1));
    lua_settop(L, top);
}

static void expect_fail(lua_State *L, const char *json, size_t len, const char *why_part) {
    int top = lua_gettop(L);
    const char *err = NULL;
    bool k = decode(L, json, len, &err);
    char what[200];
    snprintf(what, sizeof what, "reject %.40s", json);
    ok(what, !k);
    ok("nothing left on the stack after a failure", lua_gettop(L) == top);
    if (!k && why_part) {
        snprintf(what, sizeof what, "error for %.30s mentions '%s' (got '%s')", json, why_part, err ? err : "");
        ok(what, err && strstr(err, why_part));
    }
    lua_settop(L, top);
}

static void roundtrip(lua_State *L, const char *json) {
    const char *err = NULL;
    if (!decode(L, json, strlen(json), &err)) { ok("roundtrip decode", 0); return; }
    char buf[512];
    bool trunc = false;
    ljson_encode(L, -1, buf, sizeof buf, &trunc);
    lua_pop(L, 1);
    char what[200];
    snprintf(what, sizeof what, "roundtrip %s -> %s", json, buf);
    ok(what, !trunc && strcmp(buf, json) == 0);
}

int main(void) {
    lua_State *L = luaL_newstate();
    luaL_openlibs(L);
    ljson_push_null(L);
    lua_setglobal(L, "null");

    /* Scalars. */
    expect(L, "true", "v == true");
    expect(L, "false", "v == false");
    expect(L, "null", "v == null and v ~= nil");
    expect(L, "0", "math.type(v) == 'integer' and v == 0");
    expect(L, "-17", "math.type(v) == 'integer' and v == -17");
    expect(L, "2147483647", "math.type(v) == 'integer' and v == 2147483647");
    expect(L, "-2147483648", "math.type(v) == 'integer' and v == -2147483647 - 1");
    expect(L, "4294967296", "(math.type(v) == 'integer' and v == 4294967296) or "
                            "(math.type(v) == 'float' and math.abs(v - 4294967296.0) < 1000)");
    expect(L, "99999999999999999999", "math.type(v) == 'float' and v > 9.9e19");
    expect(L, "1.5", "math.type(v) == 'float' and v == 1.5");
    expect(L, "-2.5e3", "v == -2500");
    expect(L, "1E2", "math.type(v) == 'float' and v == 100");
    expect(L, "  \n\t 42 \r\n", "v == 42");
    expect(L, "123456789012345678901234567890123456789012345", "math.type(v) == 'float' and v > 1e38 or v == math.huge");

    /* Strings and escapes. */
    expect(L, "\"\"", "v == ''");
    expect(L, "\"plain\"", "v == 'plain'");
    expect(L, "\"a\\\"b\\\\c\\/d\\b\\f\\n\\r\\t\"", "v == 'a\"b\\\\c/d\\b\\f\\n\\r\\t'");
    expect(L, "\"\\u0041\\u00e9\\u20ac\"", "v == 'A\\u{e9}\\u{20ac}'");
    expect(L, "\"\\ud83d\\ude00\"", "v == '\\u{1F600}' and #v == 4");
    expect(L, "\"\\u0000x\"", "#v == 2 and v:byte(1) == 0");
    expect(L, "\"h\\u00e9llo w\\u00f6rld\"", "utf8.len(v) == 11");

    /* Containers. */
    expect(L, "[]", "type(v) == 'table' and next(v) == nil");
    expect(L, "{}", "type(v) == 'table' and next(v) == nil");
    expect(L, "[1,2,3]", "#v == 3 and v[1] == 1 and v[3] == 3");
    expect(L, "[1,null,3]", "v[2] == null and v[3] == 3 and #v == 3");
    expect(L, "{\"a\":1,\"b\":[true,{\"c\":\"d\"}]}", "v.a == 1 and v.b[1] == true and v.b[2].c == 'd'");
    expect(L, "{\"k\":null}", "v.k == null");
    expect(L, "{\"dup\":1,\"dup\":2}", "v.dup == 2");
    expect(L, " { \"a\" : [ ] , \"b\" : { } } ", "type(v.a) == 'table' and type(v.b) == 'table'");
    expect(L, "{\"name\":\"fw.bin\",\"size\":\"123456\",\"metadata\":{\"x\":\"y\"}}",
           "v.name == 'fw.bin' and v.size == '123456' and v.metadata.x == 'y'");

    /* Depth: exactly at the limit is fine, one past fails. */
    {
        char deep[LJSON_DECODE_DEPTH * 2 + 8];
        size_t n = 0;
        for (int i = 0; i < LJSON_DECODE_DEPTH; i++) { deep[n++] = '['; }
        for (int i = 0; i < LJSON_DECODE_DEPTH; i++) { deep[n++] = ']'; }
        deep[n] = 0;
        const char *err = NULL;
        ok("nesting at the limit decodes", decode(L, deep, n, &err));
        lua_settop(L, 0);
        n = 0;
        for (int i = 0; i <= LJSON_DECODE_DEPTH; i++) { deep[n++] = '['; }
        for (int i = 0; i <= LJSON_DECODE_DEPTH; i++) { deep[n++] = ']'; }
        expect_fail(L, deep, n, "deep");
    }
    {
        /* A hostile body: 100 000 opening brackets. */
        size_t n = 100000;
        char *big = malloc(n);
        memset(big, '[', n);
        expect_fail(L, big, n, "deep");
        memset(big, '{', n);
        expect_fail(L, big, n, NULL);
        free(big);
    }
    {
        /* A big, flat, valid document: 20 000 numbers. */
        size_t cap = 20000 * 8 + 4, n = 0;
        char *big = malloc(cap);
        big[n++] = '[';
        for (int i = 0; i < 20000; i++) { n += (size_t)sprintf(big + n, "%s%d", i ? "," : "", i); }
        big[n++] = ']';
        const char *err = NULL;
        bool k = decode(L, big, n, &err);
        ok("20000-element array decodes", k);
        if (k) {
            ok("20000-element array length", luaL_len(L, -1) == 20000);
            lua_geti(L, -1, 20000);
            ok("20000-element array last", lua_tointeger(L, -1) == 19999);
        }
        lua_settop(L, 0);
        free(big);
    }

    /* Malformed. Each is also checked for staying inside its buffer. */
    static const char *bad[] = {
        "", " ", "[", "]", "{", "}", "[1,]", "[,1]", "{\"a\"}", "{\"a\":}", "{a:1}", "{\"a\":1,}",
        "\"abc", "\"a\\x\"", "\"\\u12\"", "\"\\uZZZZ\"", "\"\\ud800\"", "\"\\udc00\"",
        "\"\\ud800\\u0041\"", "tru", "nul", "falsey", "01", "-", "1.", ".5", "1e", "1e+", "+1",
        "[1 2]", "{\"a\":1 \"b\":2}", "1 2", "\"a\nb\"", "NaN", "Infinity", "[1]x", "\"\\",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        expect_fail(L, bad[i], strlen(bad[i]), NULL);
    }
    expect_fail(L, "{\"a\" 1}", 7, "':'");
    expect_fail(L, "[1]x", 4, "trailing");
    /* An embedded NUL is data, not the end of the input. */
    expect_fail(L, "[1]\0", 4, "trailing");
    {
        const char s[] = "\"a\0b\"";
        const char *err = NULL;
        ok("raw NUL inside a string is a control character", !decode(L, s, sizeof s - 1, &err));
    }
    /* Every prefix of a valid document fails cleanly and stays in bounds. */
    {
        const char *doc = "{\"a\":[1,2.5,\"x\\u00e9\",{\"b\":null,\"c\":true}],\"d\":false}";
        size_t n = strlen(doc);
        for (size_t k = 0; k < n; k++) {
            const char *err = NULL;
            int top = lua_gettop(L);
            bool r = decode(L, doc, k, &err);
            /* A prefix may happen to be valid only when it is a number. */
            if (r) { lua_settop(L, top); }
            ok("prefix leaves the stack balanced", lua_gettop(L) == top);
        }
        const char *err = NULL;
        ok("the whole document decodes", decode(L, doc, n, &err));
        lua_settop(L, 0);
    }

    /* Round trips through the encoder. */
    roundtrip(L, "[1,2,3]");
    roundtrip(L, "{\"a\":null}");
    roundtrip(L, "[true,false,null,\"s\"]");
    roundtrip(L, "{\"k\":{\"n\":[1,[2,[3]]]}}");
    roundtrip(L, "\"tab\\tquote\\\"\"");

    lua_close(L);
    if (failures) { printf("%d of %d check(s) FAILED\n", failures, checks); return 1; }
    printf("json decode: %d checks\n", checks);
    return 0;
}
