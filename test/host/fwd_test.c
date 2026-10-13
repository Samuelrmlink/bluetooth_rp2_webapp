/*
 * src/net/fwd_core.c: the UDP session table (lookup, cap, 60 s idle expiry),
 * target health / fail-fast with its 5 s re-test, and the TCP backpressure
 * test -- on an injected clock.
 *
 *   gcc -Isrc test/host/fwd_test.c src/net/fwd_core.c
 */
#include <stdio.h>
#include <string.h>

#include "net/fwd_core.h"

static int checks, failed;
#define CHECK(c, msg) do { checks++; if (!(c)) { failed++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, msg); } } while (0)

static int dropped;
static void drop(fwd_sess_t *s) { (void)s; dropped++; }

int main(void) {
    /* --- sessions ------------------------------------------------------------ */
    fwd_sess_t tab[FWD_UDP_SESSIONS];
    memset(tab, 0, sizeof tab);
    CHECK(fwd_sess_find(tab, FWD_UDP_SESSIONS, 0, 0x0a000001, 5000) == NULL, "empty table");
    fwd_sess_t *a = fwd_sess_alloc(tab, FWD_UDP_SESSIONS);
    a->rule = 0; a->client_ip = 0x0a000001; a->client_port = 5000; a->last_ms = 1000;
    CHECK(fwd_sess_find(tab, FWD_UDP_SESSIONS, 0, 0x0a000001, 5000) == a, "found by rule + ip + port");
    CHECK(fwd_sess_find(tab, FWD_UDP_SESSIONS, 1, 0x0a000001, 5000) == NULL, "another rule is another session");
    CHECK(fwd_sess_find(tab, FWD_UDP_SESSIONS, 0, 0x0a000001, 5001) == NULL, "another port is another session");
    for (int i = 1; i < FWD_UDP_SESSIONS; i++) {
        fwd_sess_t *s = fwd_sess_alloc(tab, FWD_UDP_SESSIONS);
        CHECK(s != NULL, "slots up to the cap");
        s->client_port = (uint16_t)(6000 + i); s->last_ms = 1000 + (uint32_t)i * 1000;
    }
    CHECK(fwd_sess_alloc(tab, FWD_UDP_SESSIONS) == NULL, "the 9th is refused, nothing is stolen");
    CHECK(fwd_sess_expire(tab, FWD_UDP_SESSIONS, 60999, drop) == 0, "nothing idle for 60 s yet");
    CHECK(fwd_sess_expire(tab, FWD_UDP_SESSIONS, 61000, drop) == 1 && dropped == 1, "the oldest expires at 60 s, its socket dropped");
    CHECK(fwd_sess_find(tab, FWD_UDP_SESSIONS, 0, 0x0a000001, 5000) == NULL, "and is gone");
    a = fwd_sess_alloc(tab, FWD_UDP_SESSIONS);
    CHECK(a != NULL, "its slot is reusable");
    a->last_ms = 61000;
    CHECK(fwd_sess_expire(tab, FWD_UDP_SESSIONS, 120000, drop) == 7, "activity keeps a session alive, idle ones go");

    /* --- target health ------------------------------------------------------- */
    fwd_target_t t;
    memset(&t, 0, sizeof t);
    CHECK(fwd_target_admit(&t, true, 0), "unknown target: let it through");
    fwd_target_fail(&t, 1000, "refused");
    CHECK(t.state == FWD_DOWN && strcmp(t.why, "refused") == 0, "refused -> down");
    CHECK(!fwd_target_admit(&t, true, 1001), "fail fast: the next client is turned away at once");
    CHECK(!fwd_target_admit(&t, true, 5999), "...for 5 s");
    CHECK(fwd_target_admit(&t, false, 2000), "fail_fast off: always tried");
    CHECK(fwd_target_admit(&t, true, 6000), "after 5 s one client passes as the probe");
    CHECK(!fwd_target_admit(&t, true, 6001), "and the one after it waits for the probe's result");
    fwd_target_fail(&t, 6500, "timeout");
    CHECK(t.down_since == 1000 && t.failures == 2, "down_since kept from the first failure");
    CHECK(!fwd_target_admit(&t, true, 11000) && fwd_target_admit(&t, true, 11500), "re-test 5 s after the last failure");
    fwd_target_ok(&t);
    CHECK(t.state == FWD_UP && !t.why[0] && fwd_target_admit(&t, true, 11501), "a success reopens it");

    /* --- backpressure -------------------------------------------------------- */
    CHECK(fwd_can_pass(1460, 5840, 0, 16), "fits");
    CHECK(!fwd_can_pass(1460, 1000, 0, 16), "send buffer too small: refuse, lwIP keeps it");
    CHECK(!fwd_can_pass(10, 5840, 16, 16), "send queue full: refuse");
    CHECK(fwd_can_pass(0, 0, 0, 16), "nothing to pass");

    printf("%d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
