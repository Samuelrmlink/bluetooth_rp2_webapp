/* Forwarding decisions without lwIP. See fwd_core.h. */
#include "net/fwd_core.h"

#include <stdio.h>
#include <string.h>

void fwd_target_fail(fwd_target_t *t, uint32_t now, const char *why) {
    if (t->state != FWD_DOWN) { t->down_since = now; }
    t->state = FWD_DOWN;
    t->next_try = now + FWD_RETEST_MS;
    t->failures++;
    snprintf(t->why, sizeof t->why, "%s", why ? why : "");
}

void fwd_target_ok(fwd_target_t *t) {
    t->state = FWD_UP;
    t->why[0] = 0;
}

bool fwd_target_admit(fwd_target_t *t, bool fail_fast, uint32_t now) {
    if (!fail_fast || t->state != FWD_DOWN) { return true; }
    if ((int32_t)(now - t->next_try) < 0) { return false; }
    /* This one is the probe. Until it reports back (ok or fail), the
     * window closes again so a burst of clients is not all let through. */
    t->next_try = now + FWD_RETEST_MS;
    return true;
}

fwd_sess_t *fwd_sess_find(fwd_sess_t *tab, int n, uint8_t rule, uint32_t ip, uint16_t port) {
    for (int i = 0; i < n; i++) {
        if (tab[i].used && tab[i].rule == rule && tab[i].client_ip == ip && tab[i].client_port == port) {
            return &tab[i];
        }
    }
    return NULL;
}

fwd_sess_t *fwd_sess_alloc(fwd_sess_t *tab, int n) {
    for (int i = 0; i < n; i++) {
        if (!tab[i].used) {
            memset(&tab[i], 0, sizeof tab[i]);
            tab[i].used = true;
            return &tab[i];
        }
    }
    return NULL;
}

int fwd_sess_expire(fwd_sess_t *tab, int n, uint32_t now, void (*drop)(fwd_sess_t *)) {
    int k = 0;
    for (int i = 0; i < n; i++) {
        if (tab[i].used && (uint32_t)(now - tab[i].last_ms) >= FWD_UDP_IDLE_MS) {
            if (drop) { drop(&tab[i]); }
            memset(&tab[i], 0, sizeof tab[i]);
            k++;
        }
    }
    return k;
}

bool fwd_can_pass(uint16_t len, uint16_t sndbuf, uint16_t sndqueuelen, uint16_t max_queuelen) {
    return len <= sndbuf && sndqueuelen < max_queuelen;
}
