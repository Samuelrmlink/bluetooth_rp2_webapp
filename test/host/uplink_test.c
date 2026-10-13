/*
 * src/net/uplink_core.c: metrics and the selection policy, from scripted
 * ping results on an injected clock.
 *
 *   gcc -Isrc test/host/uplink_test.c src/net/uplink_core.c
 */
#include <stdio.h>
#include <string.h>

#include "net/uplink_core.h"

static int checks, failed;
#define CHECK(c, msg) do { checks++; if (!(c)) { failed++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, msg); } } while (0)

static upl_cand_t c[2];
static uint32_t now;

static void fresh(void) {
    memset(c, 0, sizeof c);
    for (int i = 0; i < 2; i++) { c[i].has_gw = true; c[i].probing = true; }
    now = 1000;
}

/* n probe rounds, 2 s apart: each candidate gets rtt (or -1 = lost). */
static int rounds(int n, int32_t r0, int32_t r1, upl_policy_t pol, int fixed, int active, const char **why) {
    for (int k = 0; k < n; k++) {
        if (c[0].has_gw) { upl_sample(&c[0], r0); }
        if (c[1].has_gw) { upl_sample(&c[1], r1); }
        now += 2000;
        active = upl_choose(pol, fixed, c, 2, active, now, why);
    }
    return active;
}

int main(void) {
    const char *why;

    /* --- metrics ---------------------------------------------------------- */
    fresh();
    upl_sample(&c[0], 40);
    CHECK(upl_rtt(&c[0]) == 40 && upl_jitter(&c[0]) == 0, "first answer seeds the average");
    for (int i = 0; i < 30; i++) { upl_sample(&c[0], i % 2 ? 50 : 30); }
    CHECK(upl_rtt(&c[0]) >= 36 && upl_rtt(&c[0]) <= 44, "rtt averages a 30/50 alternation to ~40");
    CHECK(upl_jitter(&c[0]) >= 15 && upl_jitter(&c[0]) <= 20, "jitter tracks |delta| = 20");
    CHECK(upl_loss_pct(&c[0]) == 0, "no loss");
    upl_sample(&c[0], -1); upl_sample(&c[0], -1);
    CHECK(upl_loss_pct(&c[0]) == 10 && !upl_failed(&c[0]), "2 of 20 lost = 10%, not failed");
    int32_t m = upl_metric(&c[0]);
    CHECK(m == upl_rtt(&c[0]) + 2 * upl_jitter(&c[0]) + 2 * UPL_LOSS_PENALTY_MS, "metric = rtt + 2 jitter + loss penalty");
    upl_sample(&c[0], -1);
    CHECK(upl_failed(&c[0]) && !upl_eligible(&c[0]), "3 in a row = failed");
    upl_sample(&c[0], 40);
    CHECK(!upl_failed(&c[0]), "one answer recovers it");
    for (int i = 0; i < 25; i++) { upl_sample(&c[0], 40); }
    CHECK(upl_loss_pct(&c[0]) == 0, "losses age out of the 20-sample window");

    fresh();
    c[0].probing = false;
    upl_sample(&c[0], -1); upl_sample(&c[0], -1); upl_sample(&c[0], -1);
    CHECK(!upl_failed(&c[0]), "no probe target -> never failed by pings");

    /* --- initial pick ----------------------------------------------------- */
    fresh();
    CHECK(upl_choose(UPL_BEST, -1, c, 2, -1, now, &why) == 0 && strcmp(why, "initial") == 0,
          "nothing measured yet: first by priority (the station)");
    c[0].has_gw = false;
    CHECK(upl_choose(UPL_BEST, -1, c, 2, -1, now, &why) == 1, "no gateway on 0 -> 1");
    c[0].has_gw = c[1].has_gw = false;
    CHECK(upl_choose(UPL_BEST, -1, c, 2, -1, now, &why) == -1, "no gateway anywhere -> none");

    /* --- best: switches only for a sustained 30% improvement ---------------- */
    fresh();
    int a = rounds(5, 60, 55, UPL_BEST, -1, 0, &why);
    CHECK(a == 0, "55 vs 60 ms is not 30% better: stay");
    a = rounds(5, 60, 30, UPL_BEST, -1, a, &why);
    CHECK(a == 0, "30 vs 60 but for under 20 s: stay");
    a = rounds(10, 60, 30, UPL_BEST, -1, a, &why);
    CHECK(a == 1 && strcmp(why, "better metric") == 0, "held for 20 s: switch");
    /* a brief good spell on the old one does not pull it back */
    a = rounds(3, 10, 30, UPL_BEST, -1, a, &why);
    CHECK(a == 1, "a 6 s improvement does not switch back (hysteresis)");
    /* the improvement must be continuous */
    fresh();
    a = 0;
    for (int k = 0; k < 6; k++) {
        a = rounds(4, 60, 30, UPL_BEST, -1, a, &why);      /* 8 s better */
        a = rounds(1, 60, 60, UPL_BEST, -1, a, &why);      /* then not */
    }
    CHECK(a == 0, "intermittent improvements never add up to a switch");

    /* --- best: failure switches at once --------------------------------------- */
    fresh();
    a = rounds(5, 40, 80, UPL_BEST, -1, 0, &why);
    CHECK(a == 0, "steady on 0");
    a = rounds(2, -1, 80, UPL_BEST, -1, a, &why);
    CHECK(a == 0, "two losses: not yet");
    a = rounds(1, -1, 80, UPL_BEST, -1, a, &why);
    CHECK(a == 1 && strcmp(why, "active failed") == 0, "third loss: switch at once, to a worse but working uplink");
    a = rounds(15, 40, 80, UPL_BEST, -1, a, &why);
    CHECK(a == 1, "the recovered one carries its losses in the window: no switch back yet...");
    a = rounds(20, 40, 80, UPL_BEST, -1, a, &why);
    CHECK(a == 0, "...once they age out and it has been 30% better for 20 s, it is chosen again");

    /* --- failover ------------------------------------------------------------ */
    fresh();
    a = rounds(30, 80, 10, UPL_FAILOVER, -1, 0, &why);
    CHECK(a == 0, "failover: a far better uplink is ignored while the active one works");
    a = rounds(3, -1, 10, UPL_FAILOVER, -1, a, &why);
    CHECK(a == 1, "failover: switches when it fails");
    a = rounds(30, 5, 10, UPL_FAILOVER, -1, a, &why);
    CHECK(a == 1, "failover: and does not go back on its own");

    /* --- fixed --------------------------------------------------------------- */
    fresh();
    a = rounds(5, 10, 200, UPL_FIXED, 1, 0, &why);
    CHECK(a == 1, "fixed:1 wins regardless of metric");
    a = rounds(5, 10, -1, UPL_FIXED, 1, a, &why);
    CHECK(a == 1, "fixed stays put even while its probes fail");
    c[1].has_gw = false;
    a = upl_choose(UPL_FIXED, 1, c, 2, a, now, &why);
    CHECK(a == 0, "fixed uplink lost its gateway: fall back rather than strand the device");
    c[1].has_gw = true;
    a = upl_choose(UPL_FIXED, 1, c, 2, a, now, &why);
    CHECK(a == 1 && strcmp(why, "fixed") == 0, "and return when it is back");

    /* --- all failing: the target, not the uplinks ------------------------------ */
    fresh();
    a = rounds(5, 40, 40, UPL_BEST, -1, 1, &why);
    a = rounds(5, -1, -1, UPL_BEST, -1, a, &why);
    CHECK(a == 1, "probes failing everywhere: keep the active one, no flapping");
    c[1].has_gw = false;
    a = upl_choose(UPL_BEST, -1, c, 2, a, now, &why);
    CHECK(a == 0, "...unless it loses its gateway, then any uplink that has one");

    /* --- gateway loss ---------------------------------------------------------- */
    fresh();
    a = rounds(5, 40, 60, UPL_BEST, -1, 0, &why);
    c[0].has_gw = false;
    a = upl_choose(UPL_BEST, -1, c, 2, a, now, &why);
    CHECK(a == 1 && strcmp(why, "active lost its gateway") == 0, "gateway gone: switch at once");

    printf("%d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
