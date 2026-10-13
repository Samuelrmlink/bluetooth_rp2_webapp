/* Uplink metrics and policy. See uplink_core.h. */
#include "net/uplink_core.h"

#include <limits.h>
#include <string.h>

void upl_reset(upl_cand_t *c) {
    bool gw = c->has_gw, pr = c->probing;
    memset(c, 0, sizeof *c);
    c->has_gw = gw;
    c->probing = pr;
}

void upl_sample(upl_cand_t *c, int32_t rtt) {
    c->samples++;
    c->lossbits <<= 1;
    if (c->nres < UPL_WINDOW) { c->nres++; }
    c->lossbits &= (UPL_WINDOW >= 32) ? 0xFFFFFFFFu : ((1u << UPL_WINDOW) - 1u);
    if (rtt < 0) {
        c->lossbits |= 1u;
        if (c->consec_loss < 255) { c->consec_loss++; }
        return;
    }
    c->consec_loss = 0;
    if (!c->answered) {
        /* First answer: start the averages there, not at zero. */
        c->answered = true;
        c->rtt_x8 = rtt * 8;
        c->jit_x8 = 0;
    } else {
        int32_t d = rtt - c->last_rtt;
        if (d < 0) { d = -d; }
        c->rtt_x8 += (rtt * 8 - c->rtt_x8) / 4;
        c->jit_x8 += (d * 8 - c->jit_x8) / 4;
    }
    c->last_rtt = rtt;
}

bool upl_failed(const upl_cand_t *c) {
    return c->probing && c->consec_loss >= UPL_FAIL_CONSEC;
}

bool upl_known(const upl_cand_t *c) {
    return c->probing && c->samples >= UPL_MIN_SAMPLES && c->answered;
}

bool upl_eligible(const upl_cand_t *c) {
    return c->has_gw && !upl_failed(c);
}

int upl_loss_pct(const upl_cand_t *c) {
    if (!c->nres) { return 0; }
    return __builtin_popcount(c->lossbits) * 100 / c->nres;
}

int32_t upl_rtt(const upl_cand_t *c) { return (c->rtt_x8 + 4) / 8; }
int32_t upl_jitter(const upl_cand_t *c) { return (c->jit_x8 + 4) / 8; }

int32_t upl_metric(const upl_cand_t *c) {
    if (!upl_eligible(c)) { return INT32_MAX; }
    if (!upl_known(c)) { return INT32_MAX - 1; }
    return upl_rtt(c) + 2 * upl_jitter(c) +
           UPL_LOSS_PENALTY_MS * __builtin_popcount(c->lossbits);
}

/* The best eligible candidate: lowest known metric, else the first eligible
 * in priority (index) order. */
static int best_of(const upl_cand_t *c, int n) {
    int best = -1;
    for (int i = 0; i < n; i++) {
        if (!upl_eligible(&c[i])) { continue; }
        if (best < 0 || upl_metric(&c[i]) < upl_metric(&c[best])) { best = i; }
    }
    return best;
}

/* The best eligible candidate that has proven itself (answered probes). */
static int best_known(const upl_cand_t *c, int n) {
    int best = -1;
    for (int i = 0; i < n; i++) {
        if (!upl_eligible(&c[i]) || !upl_known(&c[i])) { continue; }
        if (best < 0 || upl_metric(&c[i]) < upl_metric(&c[best])) { best = i; }
    }
    return best;
}

int upl_choose(upl_policy_t pol, int fixed, upl_cand_t *c, int n, int active,
               uint32_t now, const char **why) {
    static const char *const none = "";
    if (why) { *why = none; }

    if (pol == UPL_FIXED && fixed >= 0 && fixed < n && c[fixed].has_gw) {
        if (fixed != active && why) { *why = "fixed"; }
        for (int i = 0; i < n; i++) { c[i].better_since = 0; }
        return fixed;
    }

    bool active_ok = active >= 0 && active < n && upl_eligible(&c[active]);
    if (!active_ok) {
        for (int i = 0; i < n; i++) { c[i].better_since = 0; }
        bool have_active = active >= 0 && active < n && c[active].has_gw;
        /* Leaving a FAILED uplink for one nothing is known about is a coin
         * toss that can strand the tunnel on a link with no way out (seen:
         * w0's probes failed, e0 had just got a gateway with no internet
         * behind it, the policy moved to it, and then e0 failed too). So a
         * failover target must have answered probes; only the very first
         * pick, with nothing active, may go by priority alone. */
        int b = have_active ? best_known(c, n) : best_of(c, n);
        if (b < 0) {
            /* Every probe is failing everywhere: the target is more likely
             * down (or drops ICMP) than every uplink at once. Do not flap,
             * and do not strand the device: keep the active one while it
             * has a gateway, else take any that has one. */
            if (active >= 0 && active < n && c[active].has_gw) { return active; }
            for (int i = 0; i < n; i++) {
                if (c[i].has_gw) { b = i; break; }
            }
        }
        if (b >= 0 && b != active && why) {
            *why = (active >= 0 && active < n && c[active].has_gw) ? "active failed"
                 : (active >= 0 ? "active lost its gateway" : "initial");
        }
        return b;
    }

    if (pol != UPL_BEST || !upl_known(&c[active])) {
        for (int i = 0; i < n; i++) { c[i].better_since = 0; }
        return active;
    }

    int32_t ma = upl_metric(&c[active]);
    int pick = active;
    for (int i = 0; i < n; i++) {
        if (i == active) { continue; }
        if (!upl_eligible(&c[i]) || !upl_known(&c[i]) ||
            (int64_t)upl_metric(&c[i]) * 100 > (int64_t)ma * (100 - UPL_BETTER_PCT)) {
            c[i].better_since = 0;
            continue;
        }
        if (!c[i].better_since) { c[i].better_since = now ? now : 1; }
        if ((uint32_t)(now - c[i].better_since) >= UPL_BETTER_HOLD_MS &&
            (pick == active || upl_metric(&c[i]) < upl_metric(&c[pick]))) {
            pick = i;
        }
    }
    if (pick != active) {
        for (int i = 0; i < n; i++) { c[i].better_since = 0; }
        if (why) { *why = "better metric"; }
    }
    return pick;
}
