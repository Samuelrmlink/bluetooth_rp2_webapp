/* imports: host calls across the groups, and a WEAK import of the optional
 * board group (present on the RP2350 test host, absent on the RP2040 one). */
#include "pex.h"

PEX_WEAK_IMPORT(pex_board_cpu_hz);
PEX_WEAK_IMPORT(pex_board_uid);

/* Bit mask of what worked; the host knows what to expect of itself. */
#define IMP_TIME     (1u << 0)
#define IMP_FMT      (1u << 1)
#define IMP_LIBC     (1u << 2)
#define IMP_BOARD    (1u << 3)    /* board group present and answering */
#define IMP_NOBOARD  (1u << 4)    /* board group absent and the slot is 0 */
#define IMP_VERSION  (1u << 5)
#define IMP_MEM      (1u << 6)
#define IMP_JSON     (1u << 7)

uint32_t imports_run(void) {
    uint32_t r = 0;
    uint32_t t0 = pex_ms();
    uint64_t u0 = pex_us();
    volatile int spin = 0;
    for (int i = 0; i < 2000; i++) { spin += i; }
    if (pex_us() > u0 && pex_ms() >= t0) { r |= IMP_TIME; }
    char buf[48];
    int n = pex_snprintf(buf, sizeof buf, "%d-%s-%x", 42, "pex", 0xBEEFu);
    if (n == 11 && strcmp(buf, "42-pex-beef") == 0) { r |= IMP_FMT; }
    char a[16], b[16];
    memset(a, 'x', sizeof a);
    memcpy(b, a, sizeof b);
    memmove(b + 1, b, 8);
    if (memcmp(a, b, sizeof a) == 0 && strlen("abcdef") == 6 && strchr("abc", 'c')) { r |= IMP_LIBC; }
    if (pex_has(PEX_GROUP_BOARD)) {
        uint8_t uid[8];
        if (pex_avail(pex_board_cpu_hz) && pex_board_cpu_hz() > 1000000 && pex_board_uid(uid, sizeof uid) > 0) {
            r |= IMP_BOARD;
        }
    } else if (!pex_avail(pex_board_cpu_hz) && !pex_avail(pex_board_uid)) {
        r |= IMP_NOBOARD;
    }
    if ((pex_api_version() >> 16) == PEX_API_MAJOR && (pex_has(PEX_GROUP_CORE) >> 16) == PEX_GROUP_CORE_MAJOR) {
        r |= IMP_VERSION;
    }
    void *p = pex_malloc(100);
    if (p) { memset(p, 0, 100); pex_free(p); r |= IMP_MEM; }
    int64_t v = 0;
    char out[64];
    pex_jw_t w;
    pex_jw_init(&w, out, sizeof out);
    pex_jw_obj(&w, NULL);
    pex_jw_int(&w, "n", 7);
    pex_jw_arr(&w, "l");
    pex_jw_str(&w, NULL, "a");
    pex_jw_end(&w);
    pex_jw_end(&w);
    if (pex_json_int("{\"x\":{\"y\":[1,42]}}", "x.y.1", &v) && v == 42 && pex_jw_finish(&w) > 0 &&
        strcmp(out, "{\"n\":7,\"l\":[\"a\"]}") == 0) {
        r |= IMP_JSON;
    }
    pex_log(PEX_LOG_INFO, "imports: 0x%x", (unsigned)r);
    return r;
}
PEX_EXPORT(imports_run);

int pex_main(const pex_host_t *host, uint32_t reason, void *arg) { return PEX_OK; }
