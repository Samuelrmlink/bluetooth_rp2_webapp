#ifndef BTGW_UPLINK_CORE_H
#define BTGW_UPLINK_CORE_H

/*
 * Uplink metrics and the selection policy, as pure functions over plain
 * structs: no lwIP, no clock of its own. src/net/uplink.c feeds it ping
 * results and applies what it decides; test/host/uplink_test.c scripts the
 * ping results and checks the decisions.
 *
 *   RTT       EWMA (alpha 1/4) of the echo round trip.
 *   jitter    EWMA of |RTT - previous RTT|.
 *   loss      lost probes among the last UPL_WINDOW.
 *   metric    rtt + 2*jitter + UPL_LOSS_PENALTY_MS per lost probe in the
 *             window. Lower is better.
 *   failed    UPL_FAIL_CONSEC lost in a row.
 */
#include <stdbool.h>
#include <stdint.h>

#define UPL_MAX              3
#define UPL_WINDOW           20
#define UPL_FAIL_CONSEC      3
#define UPL_MIN_SAMPLES      3        /* before a metric is trusted */
#define UPL_LOSS_PENALTY_MS  50
#define UPL_BETTER_PCT       30       /* "best": another must be this much better... */
#define UPL_BETTER_HOLD_MS   20000    /* ...for this long */

typedef enum { UPL_BEST = 0, UPL_FAILOVER, UPL_FIXED } upl_policy_t;

typedef struct {
    bool     has_gw;          /* up, linked, addressed, with a gateway */
    bool     probing;         /* there is a probe target to measure with */
    uint32_t samples;         /* results since the gateway appeared */
    uint32_t lossbits;        /* last UPL_WINDOW results, 1 = lost (bit 0 newest) */
    uint8_t  nres;            /* results in the window */
    uint8_t  consec_loss;
    bool     answered;        /* at least one reply since the reset */
    int32_t  rtt_x8, jit_x8;  /* ms * 8 */
    int32_t  last_rtt;
    uint32_t better_since;    /* "best": when it first beat the active one; 0 = not */
} upl_cand_t;

void upl_reset(upl_cand_t *c);
/* One probe result: rtt_ms >= 0, or < 0 for a lost probe. */
void upl_sample(upl_cand_t *c, int32_t rtt_ms);

bool    upl_failed(const upl_cand_t *c);
bool    upl_known(const upl_cand_t *c);
bool    upl_eligible(const upl_cand_t *c);
int     upl_loss_pct(const upl_cand_t *c);
int32_t upl_rtt(const upl_cand_t *c);
int32_t upl_jitter(const upl_cand_t *c);
int32_t upl_metric(const upl_cand_t *c);

/*
 * Which candidate should carry the traffic. `active` is the current one (-1
 * none), `fixed` the index named by a fixed:<iface> policy (-1 none). Returns
 * the index to use (-1: nothing is eligible) and sets *why to a short reason
 * when it differs from `active`.
 *
 *   best      stay unless the active one fails (then the best eligible, at
 *             once) or another is UPL_BETTER_PCT better for
 *             UPL_BETTER_HOLD_MS.
 *   failover  stay until the active one fails.
 *   fixed     the named one whenever it has a gateway; if it does not,
 *             behave as failover so the device is not stranded.
 */
int upl_choose(upl_policy_t pol, int fixed, upl_cand_t *c, int n, int active,
               uint32_t now_ms, const char **why);

#endif
