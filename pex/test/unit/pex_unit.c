/* Portable parts of the runtime, on the build host: the json group, CRC-32, SHA-256.
 *   gcc -I pex/runtime -I pex/sdk/include -I <gen> pex_unit.c ../../runtime/groups/pex_json.c ../../runtime/pex_hash.c */
#include <stdio.h>
#include <string.h>
#include "pex_internal.h"
int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)
int main(void) {
    const char *j = "{\"a\": {\"b\": [10, {\"c\": \"x\\\"y\\u00e9\"}, true]}, \"n\": -3.5, \"s\":\"q\", \"e\":[]}";
    int64_t i; double d; int b; char s[32];
    CHECK(pex_json_int(j, "a.b.0", &i) && i == 10);
    CHECK(pex_json_str(j, "a.b.1.c", s, sizeof s) && strcmp(s, "x\"y\xc3\xa9") == 0);
    CHECK(pex_json_bool(j, "a.b.2", &b) && b == 1);
    CHECK(pex_json_num(j, "n", &d) && d == -3.5);
    CHECK(!pex_json_str(j, "a.b.3", s, sizeof s));
    CHECK(pex_json_len(j, "a.b") == 3 && pex_json_len(j, "a") == 1 && pex_json_len(j, "e") == 0 && pex_json_len(j, "s") == -1);
    CHECK(pex_json_str(j, "s", s, sizeof s) && strcmp(s, "q") == 0);
    char buf[200]; pex_jw_t w; pex_jw_init(&w, buf, sizeof buf);
    pex_jw_obj(&w, NULL); pex_jw_str(&w, "k", "v\"1"); pex_jw_arr(&w, "a"); pex_jw_int(&w, NULL, 1); pex_jw_obj(&w, NULL);
    pex_jw_bool(&w, "t", 1); pex_jw_end(&w); pex_jw_hex(&w, NULL, "\x01\xab", 2); pex_jw_end(&w); pex_jw_null(&w, "z"); pex_jw_end(&w);
    CHECK(pex_jw_finish(&w) > 0 && strcmp(buf, "{\"k\":\"v\\\"1\",\"a\":[1,{\"t\":true},\"01ab\"],\"z\":null}") == 0);
    printf("%s\n", buf);
    char small[8]; pex_jw_init(&w, small, sizeof small); pex_jw_str(&w, NULL, "too long string"); CHECK(pex_jw_finish(&w) == PEX_ENOMEM);
    CHECK(pex_crc32_update(0, "123456789", 9) == 0xCBF43926u);
    pex_sha256_t sh; uint8_t h[32]; pex_sha256_init(&sh); pex_sha256_update(&sh, "abc", 3); pex_sha256_final(&sh, h);
    CHECK(h[0] == 0xba && h[1] == 0x78 && h[31] == 0xad);
    printf("%d failed\n", fails); return fails != 0;
}
