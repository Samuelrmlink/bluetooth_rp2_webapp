#ifndef BTGW_FWD_CORE_H
#define BTGW_FWD_CORE_H

/*
 * The decisions behind forwarding rules, without lwIP: the UDP session table,
 * the target-health state machine, and the TCP backpressure test. src/net/
 * fwd.c owns the pcbs; test/host/fwd_test.c drives these on an injected
 * clock.
 *
 * Target health ("fail fast") uses real signals only: a TCP connect refused
 * or timed out, an ICMP unreachable for a UDP session, an ARP failure for a
 * target on a local subnet. Never "no reply within N s" -- a one-way UDP
 * stream would be wrongly blocked. While DOWN, new TCP clients get an
 * immediate RST and UDP senders an ICMP port unreachable, so they learn at
 * once instead of timing out. Every FWD_RETEST_MS the next attempt passes
 * through as a probe (and TCP rules are also re-tested actively).
 */
#include <stdbool.h>
#include <stdint.h>

#define FWD_RETEST_MS        5000
#define FWD_UDP_IDLE_MS      60000
#define FWD_UDP_SESSIONS     8
#define FWD_TCP_CONNS        8

typedef enum { FWD_UNKNOWN = 0, FWD_UP, FWD_DOWN } fwd_health_t;

typedef struct {
    fwd_health_t state;
    uint32_t     down_since, next_try;
    uint32_t     failures;
    char         why[24];        /* the last failure: "refused", "timeout", ... */
} fwd_target_t;

void fwd_target_fail(fwd_target_t *t, uint32_t now, const char *why);
void fwd_target_ok(fwd_target_t *t);
/* May a new client go to the target now? Always while UP/UNKNOWN or when
 * fail_fast is off; while DOWN only once next_try has passed -- and that
 * attempt is the probe, so the next one waits again until it reports. */
bool fwd_target_admit(fwd_target_t *t, bool fail_fast, uint32_t now);

/* --- UDP sessions: (rule, client ip:port) -> an outbound socket ---------- */
typedef struct {
    bool     used;
    uint8_t  rule;
    uint32_t client_ip;      /* network order, as lwIP keeps it */
    uint16_t client_port;
    uint32_t last_ms;
    void    *pcb;            /* fwd.c's udp_pcb */
} fwd_sess_t;

/* Find; NULL if absent. */
fwd_sess_t *fwd_sess_find(fwd_sess_t *tab, int n, uint8_t rule, uint32_t ip, uint16_t port);
/* A free slot, or NULL when all FWD_UDP_SESSIONS are busy (the caller
 * refuses -- an idle one is reclaimed by expiry, not stolen). */
fwd_sess_t *fwd_sess_alloc(fwd_sess_t *tab, int n);
/* Expire idle sessions: calls drop(s) for each and frees it. Returns how many. */
int fwd_sess_expire(fwd_sess_t *tab, int n, uint32_t now, void (*drop)(fwd_sess_t *));

/* --- TCP backpressure ----------------------------------------------------- */
/* Pass `len` bytes on now? Only when the destination's send buffer and
 * queue take all of it; otherwise refuse, and lwIP holds the data (and the
 * sender's window stays closed) until it is offered again. */
bool fwd_can_pass(uint16_t len, uint16_t sndbuf, uint16_t sndqueuelen, uint16_t max_queuelen);

#endif
