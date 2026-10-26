#if defined(__has_include)
#  if __has_include("autoconf.h")
#    include "autoconf.h"
#  endif
#endif
#include "bt/classic/cl_sess.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    uint8_t gen;
    uint8_t state;
    uint8_t ask;
    uint8_t phase;
    uint8_t level;
    uint8_t io;
    uint8_t bond;
    uint8_t drop_key;
    uint8_t retried;
    uint8_t secure;
    uint8_t rfcomm_up;
    uint8_t io_hold;
    uint8_t watch;
    uint8_t npin;
    uint8_t pin_i;
    uint8_t ans_kind;
    uint8_t ans_neg;
    uint8_t tx_starved;
    uint8_t key_known;
    uint8_t key_auth;
    uint8_t key_sc;
    uint8_t addr[6];
    uint16_t uuid;
    uint8_t channel;
    uint8_t fallback;
    uint16_t mtu;
    uint16_t cid;
    uint16_t acl;
    uint32_t cod;
    uint32_t deadline;
    uint32_t confirm_num;
    uint32_t ans_num;
    uint32_t rx_drop;
    uint32_t pend_ws;
    uint32_t pend_mod;
    uintptr_t owner;
    uintptr_t cb;
    uintptr_t cb_arg;
    int why;
    char error[48];
    char pin[CL_PIN_MAX][CL_PIN_LEN];
    char ans_pin[CL_PIN_LEN];
    uint8_t *rx;
    uint16_t rx_w, rx_r, rx_n;
    uint8_t *tx;
    uint16_t tx_cap, tx_off, tx_len;
} sess_t;

typedef struct {
    uint8_t addr[6];
    char name[CL_NAME_LEN];
    int8_t rssi;
    uint32_t cod;
    uint8_t used;
    uint8_t dirty;
} inq_t;

static sess_t s_tab[CL_SESS_MAX];
static inq_t s_dev[CL_DEV_MAX];
static int s_ndev;
static int s_inq;
static int s_rr;

static sess_t *slot_of(int handle) {
    int slot = (handle & 0xFF) - 1;
    int gen = (handle >> 8) & 0xFF;
    if (slot < 0 || slot >= CL_SESS_MAX) { return NULL; }
    if (s_tab[slot].state == CL_FREE || s_tab[slot].gen != (uint8_t)gen) { return NULL; }
    return &s_tab[slot];
}

static int handle_of(const sess_t *s) {
    int slot = (int)(s - s_tab);
    return ((int)s->gen << 8) | (slot + 1);
}

static void pend(sess_t *s, uint32_t bit) {
    s->pend_ws |= bit;
    s->pend_mod |= bit;
}

static void set_err(sess_t *s, const char *err) {
    size_t i = 0;
    if (!err) { err = ""; }
    for (; i + 1 < sizeof s->error && err[i]; i++) { s->error[i] = err[i]; }
    s->error[i] = 0;
}

static void become(sess_t *s, uint8_t st, int why, const char *err) {
    s->state = st;
    s->why = why;
    if (err) { set_err(s, err); }
    if (st != CL_OPENING) { s->io_hold = 0; }
    pend(s, CL_EV_STATE);
}

static int live(const sess_t *s) {
    return s->state == CL_OPENING || s->state == CL_OPEN || s->state == CL_CLOSING
        || s->state == CL_FAILED;
}

static void free_bufs(sess_t *s) {
    free(s->rx);
    free(s->tx);
    s->rx = NULL;
    s->tx = NULL;
}

static void consider_open(sess_t *s) {
    if (s->state == CL_OPENING && s->secure && s->rfcomm_up) {
        s->state = CL_OPEN;
        s->phase = CL_PHASE_UP;
        s->io_hold = 0;
        pend(s, CL_EV_STATE);
        if (s->rx_n) { pend(s, CL_EV_RX); }
    }
}

static int io_conflict(uint8_t io) {
    for (int i = 0; i < CL_SESS_MAX; i++) {
        if (s_tab[i].io_hold && s_tab[i].io != io) { return 1; }
    }
    return 0;
}

static int digits_passkey(const char *p, uint32_t *out) {
    uint32_t v = 0;
    int n = 0;
    if (!p || !p[0]) { return 0; }
    for (; p[n]; n++) {
        if (p[n] < '0' || p[n] > '9' || n >= 6) { return 0; }
        v = v * 10u + (uint32_t)(p[n] - '0');
    }
    if (n == 0 || v > 999999u) { return 0; }
    *out = v;
    return 1;
}

static void copy_cod(sess_t *s) {
    for (int i = 0; i < s_ndev; i++) {
        if (s_dev[i].used && memcmp(s_dev[i].addr, s->addr, 6) == 0) {
            s->cod = s_dev[i].cod;
            return;
        }
    }
}

void cl_sess_reset(void) {
    for (int i = 0; i < CL_SESS_MAX; i++) {
        free_bufs(&s_tab[i]);
        memset(&s_tab[i], 0, sizeof s_tab[i]);
    }
    memset(s_dev, 0, sizeof s_dev);
    s_ndev = 0;
    s_inq = 0;
    s_rr = 0;
}

int cl_open(const cl_open_t *opt, uint32_t now_ms) {
    if (!opt) { return CL_EINVAL; }
    int zero = 1;
    for (int i = 0; i < 6; i++) { if (opt->addr[i]) { zero = 0; } }
    if (zero) { return CL_EINVAL; }
    uint8_t level = opt->level == CL_LEVEL_OMIT ? CL_LEVEL_DEFAULT : opt->level;
    uint8_t bond = opt->bond == 0xFF ? CL_BOND_GENERAL : opt->bond;
    uint8_t io = opt->io;
    if (level > 4 || io > CL_IO_KEYBOARD || bond > CL_BOND_DEDICATED) { return CL_EINVAL; }
    if (opt->npin > CL_PIN_MAX) { return CL_EINVAL; }
    /* No display and no keyboard cannot prove the peer. */
    if ((level >= 3) && io == CL_IO_NONE) { return CL_EINVAL; }
    if (io_conflict(io)) { return CL_EBUSY; }
    for (int i = 0; i < opt->npin; i++) {
        size_t n = 0;
        while (opt->pin[i][n] && n < CL_PIN_LEN) { n++; }
        if (n == 0 || n >= CL_PIN_LEN) { return CL_EINVAL; }
    }

    sess_t *s = NULL;
    for (int i = 0; i < CL_SESS_MAX; i++) {
        if (s_tab[i].state == CL_FREE) { s = &s_tab[i]; break; }
    }
    if (!s) { return CL_ENOMEM; }

    uint8_t gen = s->gen ? s->gen : 1;
    memset(s, 0, sizeof *s);
    s->gen = gen;
    s->state = CL_OPENING;
    s->level = level;
    s->io = io;
    s->bond = bond;
    s->drop_key = opt->drop_key ? 1 : 0;
    s->watch = opt->watch ? 1 : 0;
    s->uuid = opt->uuid;
    s->channel = opt->channel;
    s->fallback = opt->fallback;
    s->owner = opt->owner;
    s->cb = opt->cb;
    s->cb_arg = opt->cb_arg;
    s->npin = opt->npin;
    s->io_hold = 1;
    memcpy(s->addr, opt->addr, 6);
    for (int i = 0; i < opt->npin; i++) {
        size_t n = 0;
        while (opt->pin[i][n] && n + 1 < CL_PIN_LEN) { n++; }
        memcpy(s->pin[i], opt->pin[i], n);
        s->pin[i][n] = 0;
    }
    if (opt->deadline_ms) {
        s->deadline = now_ms + opt->deadline_ms;
        if (s->deadline == 0) { s->deadline = 1; }
    }
    s->rx = malloc(CL_RX_CAP);
    s->tx = malloc(CL_TX_CAP);
    s->tx_cap = CL_TX_CAP;
    if (!s->rx || !s->tx) {
        free_bufs(s);
        memset(s, 0, sizeof *s);
        s->gen = gen;
        return CL_ENOMEM;
    }
    copy_cod(s);
    pend(s, CL_EV_STATE);
    return handle_of(s);
}

static void wipe(sess_t *s) {
    uint8_t gen = (uint8_t)(s->gen + 1);
    if (gen == 0) { gen = 1; }
    free_bufs(s);
    memset(s, 0, sizeof *s);
    s->gen = gen;
}

int cl_close(int handle) {
    sess_t *s = slot_of(handle);
    if (!s) { return CL_ENOENT; }
    if (s->state == CL_CLOSING) { return CL_OK; }
    become(s, CL_CLOSING, CL_WHY_CLOSED, "closed");
    s->ask = CL_ASK_NONE;
    return CL_OK;
}

void cl_reap(int handle) {
    sess_t *s = slot_of(handle);
    if (!s) { return; }
    if (s->state == CL_CLOSING || s->state == CL_FAILED) { wipe(s); }
}

int cl_release_owner(uintptr_t owner) {
    int n = 0;
    if (!owner) { return 0; }
    for (int i = 0; i < CL_SESS_MAX; i++) {
        if (!live(&s_tab[i]) || s_tab[i].owner != owner) { continue; }
        if (s_tab[i].state != CL_CLOSING) {
            become(&s_tab[i], CL_CLOSING, CL_WHY_CLOSED, "unloaded");
            s_tab[i].ask = CL_ASK_NONE;
        }
        /* The module is gone. Do not deliver, and do not hold the slot for it. */
        s_tab[i].cb = 0;
        s_tab[i].cb_arg = 0;
        s_tab[i].pend_mod = 0;
        n++;
    }
    return n;
}

int cl_io_done(int handle) {
    sess_t *s = slot_of(handle);
    if (!s) { return CL_ENOENT; }
    s->io_hold = 0;
    return CL_OK;
}

int cl_pin_pull(int handle, char *out, size_t cap) {
    sess_t *s = slot_of(handle);
    if (!s) { return CL_ENOENT; }
    if (s->state != CL_OPENING && s->state != CL_OPEN) { return CL_ESTATE; }
    if (s->ask == CL_ASK_PIN) { return 0; }
    if (s->pin_i < s->npin) {
        size_t n = 0;
        while (s->pin[s->pin_i][n]) { n++; }
        if (out && cap) {
            size_t c = n < cap - 1 ? n : cap - 1;
            memcpy(out, s->pin[s->pin_i], c);
            out[c] = 0;
        }
        s->pin_i++;
        s->ask = CL_ASK_NONE;
        return 1;
    }
    s->ask = CL_ASK_PIN;
    pend(s, CL_EV_PIN);
    if (out && cap) { out[0] = 0; }
    return 0;
}

int cl_note_pin_rejected(int handle) {
    sess_t *s = slot_of(handle);
    if (!s) { return CL_ENOENT; }
    if (s->pin_i < s->npin) { return 1; }
    s->ask = CL_ASK_PIN;
    pend(s, CL_EV_PIN);
    return 0;
}

int cl_passkey_pull(int handle, uint32_t *out) {
    sess_t *s = slot_of(handle);
    if (!s) { return CL_ENOENT; }
    if (s->state != CL_OPENING && s->state != CL_OPEN) { return CL_ESTATE; }
    if (s->ask == CL_ASK_PASSKEY) { return 0; }
    while (s->pin_i < s->npin) {
        uint32_t v = 0;
        if (!digits_passkey(s->pin[s->pin_i], &v)) { break; }
        s->pin_i++;
        if (out) { *out = v; }
        s->ask = CL_ASK_NONE;
        return 1;
    }
    s->ask = CL_ASK_PASSKEY;
    pend(s, CL_EV_PASSKEY);
    return 0;
}

int cl_confirm_ask(int handle, uint32_t number) {
    sess_t *s = slot_of(handle);
    if (!s) { return CL_ENOENT; }
    s->ask = CL_ASK_CONFIRM;
    s->confirm_num = number;
    pend(s, CL_EV_CONFIRM);
    return 0;
}

int cl_submit_pin(int handle, const char *pin) {
    sess_t *s = slot_of(handle);
    if (!s) { return CL_ENOENT; }
    if (s->ask != CL_ASK_PIN) { return CL_ESTATE; }
    if (pin && pin[0]) {
        size_t n = 0;
        while (pin[n]) { n++; }
        if (n >= CL_PIN_LEN) { return CL_EINVAL; }
    }
    s->ask = CL_ASK_NONE;
    s->ans_kind = CL_ASK_PIN;
    s->ans_pin[0] = 0;
    if (!pin || !pin[0]) {
        s->ans_neg = 1;
        become(s, CL_FAILED, CL_WHY_CANCEL, "pin declined");
        return CL_OK;
    }
    size_t n = 0;
    while (pin[n]) { n++; }
    memcpy(s->ans_pin, pin, n);
    s->ans_pin[n] = 0;
    s->ans_neg = 0;
    return CL_OK;
}

int cl_submit_passkey(int handle, int negative, uint32_t passkey) {
    sess_t *s = slot_of(handle);
    if (!s) { return CL_ENOENT; }
    if (s->ask != CL_ASK_PASSKEY) { return CL_ESTATE; }
    if (!negative && passkey > 999999u) { return CL_EINVAL; }
    s->ask = CL_ASK_NONE;
    s->ans_kind = CL_ASK_PASSKEY;
    s->ans_neg = negative ? 1 : 0;
    s->ans_num = passkey;
    if (negative) { become(s, CL_FAILED, CL_WHY_CANCEL, "passkey declined"); }
    return CL_OK;
}

int cl_submit_confirm(int handle, int accept) {
    sess_t *s = slot_of(handle);
    if (!s) { return CL_ENOENT; }
    if (s->ask != CL_ASK_CONFIRM) { return CL_ESTATE; }
    s->ask = CL_ASK_NONE;
    s->ans_kind = CL_ASK_CONFIRM;
    s->ans_neg = accept ? 0 : 1;
    s->ans_num = s->confirm_num;
    if (!accept) { become(s, CL_FAILED, CL_WHY_CANCEL, "confirm declined"); }
    return CL_OK;
}

int cl_take_answer(int handle, int *kind, int *negative, char *pin, size_t pin_cap, uint32_t *num) {
    sess_t *s = slot_of(handle);
    if (!s || !s->ans_kind) { return 0; }
    if (kind) { *kind = s->ans_kind; }
    if (negative) { *negative = s->ans_neg; }
    if (num) { *num = s->ans_num; }
    if (pin && pin_cap) {
        size_t n = 0;
        while (s->ans_pin[n] && n + 1 < pin_cap) { n++; }
        memcpy(pin, s->ans_pin, n);
        pin[n] = 0;
    }
    s->ans_kind = CL_ASK_NONE;
    return 1;
}

static int arm_retry(sess_t *s) {
    if (!s->drop_key || s->retried || s->state != CL_OPENING) { return 0; }
    s->retried = 1;
    s->pin_i = 0;
    s->ask = CL_ASK_NONE;
    s->secure = 0;
    s->rfcomm_up = 0;
    s->phase = CL_PHASE_PAGE;
    s->cid = 0;
    s->ans_kind = CL_ASK_NONE;
    return 1;
}

int cl_auth_fail(int handle) {
    sess_t *s = slot_of(handle);
    if (!s) { return CL_ENOENT; }
    if (arm_retry(s)) { return CL_RETRY; }
    become(s, CL_FAILED, CL_WHY_AUTH, "authentication failed");
    return 0;
}

int cl_security(int handle, int got_level, int authenticated, int sc, int final) {
    sess_t *s = slot_of(handle);
    if (!s) { return CL_ENOENT; }
    if (s->state != CL_OPENING && s->state != CL_OPEN) { return CL_ESTATE; }
    int ok = got_level >= (int)s->level
        && (s->level < 3 || authenticated)
        && (s->level < 4 || sc);
    if (ok) {
        s->secure = 1;
        consider_open(s);
        return CL_OK;
    }
    if (final) {
        if (arm_retry(s)) { return CL_RETRY; }
        become(s, CL_FAILED, CL_WHY_SECURITY, "security level not met");
    }
    return CL_ESTATE;
}

int cl_rfcomm_up(int handle, uint16_t mtu) {
    sess_t *s = slot_of(handle);
    if (!s) { return CL_ENOENT; }
    s->rfcomm_up = 1;
    s->mtu = mtu ? mtu : s->mtu;
    if (s->phase == CL_PHASE_PAGE || s->phase == CL_PHASE_SDP) { s->phase = CL_PHASE_RFCOMM; }
    if (mtu && (uint32_t)mtu * 2u > s->tx_cap && s->tx) {
        uint8_t *nbuf = malloc((size_t)mtu * 2u);
        if (nbuf) {
            if (s->tx_len) { memcpy(nbuf, s->tx + s->tx_off, s->tx_len); }
            free(s->tx);
            s->tx = nbuf;
            s->tx_cap = (uint16_t)(mtu * 2u);
            s->tx_off = 0;
        }
    }
    consider_open(s);
    return CL_OK;
}

int cl_set_channel(int handle, uint8_t channel) {
    sess_t *s = slot_of(handle);
    if (!s) { return CL_ENOENT; }
    if (!channel) { return CL_EINVAL; }
    s->channel = channel;
    return CL_OK;
}

int cl_set_acl(int handle, uint16_t acl) {
    sess_t *s = slot_of(handle);
    if (!s) { return CL_ENOENT; }
    s->acl = acl;
    if (s->phase == CL_PHASE_PAGE) { s->phase = CL_PHASE_RFCOMM; }
    return CL_OK;
}

int cl_set_cid(int handle, uint16_t cid) {
    sess_t *s = slot_of(handle);
    if (!s) { return CL_ENOENT; }
    s->cid = cid;
    return CL_OK;
}

int cl_set_phase(int handle, int phase) {
    sess_t *s = slot_of(handle);
    if (!s) { return CL_ENOENT; }
    s->phase = (uint8_t)phase;
    return CL_OK;
}

int cl_set_key(int handle, int known, int authenticated, int sc) {
    sess_t *s = slot_of(handle);
    if (!s) { return CL_ENOENT; }
    s->key_known = known ? 1 : 0;
    s->key_auth = authenticated ? 1 : 0;
    s->key_sc = sc ? 1 : 0;
    return CL_OK;
}

int cl_fail(int handle, int why, const char *err) {
    sess_t *s = slot_of(handle);
    if (!s) { return CL_ENOENT; }
    if (s->state == CL_FAILED || s->state == CL_FREE) { return CL_OK; }
    become(s, CL_FAILED, why, err ? err : "failed");
    s->ask = CL_ASK_NONE;
    return CL_OK;
}

int cl_mark_closed(int handle, int why, const char *err) {
    sess_t *s = slot_of(handle);
    if (!s) { return CL_ENOENT; }
    become(s, CL_CLOSING, why, err ? err : "closed");
    s->ask = CL_ASK_NONE;
    s->cid = 0;
    return CL_OK;
}

void cl_tick(uint32_t now_ms) {
    for (int i = 0; i < CL_SESS_MAX; i++) {
        sess_t *s = &s_tab[i];
        if (s->state != CL_OPENING || !s->deadline) { continue; }
        if ((int32_t)(now_ms - s->deadline) < 0) { continue; }
        become(s, CL_FAILED, CL_WHY_TIMEOUT, "connect deadline");
        s->ask = CL_ASK_NONE;
    }
}

static void tx_compact(sess_t *s) {
    if (s->tx_off && s->tx_len) {
        memmove(s->tx, s->tx + s->tx_off, s->tx_len);
    }
    if (s->tx_len == 0) { s->tx_off = 0; }
    else { s->tx_off = 0; }
}

int cl_tx_put(int handle, const void *data, size_t n) {
    sess_t *s = slot_of(handle);
    if (!s) { return CL_ENOENT; }
    if (s->state != CL_OPEN || !s->secure) { return CL_EBUSY; }
    if (!n) { return 0; }
    if (!data || !s->tx) { return CL_EINVAL; }
    tx_compact(s);
    if ((size_t)s->tx_len + n > s->tx_cap) {
        s->tx_starved = 1;
        return CL_EBUSY;
    }
    memcpy(s->tx + s->tx_len, data, n);
    s->tx_len = (uint16_t)(s->tx_len + n);
    return (int)n;
}

int cl_tx_room(int handle) {
    sess_t *s = slot_of(handle);
    if (!s || !s->tx) { return CL_ENOENT; }
    tx_compact(s);
    return (int)(s->tx_cap - s->tx_len);
}

int cl_tx_eligible(int handle) {
    sess_t *s = slot_of(handle);
    if (!s) { return 0; }
    return s->state == CL_OPEN && s->secure && s->tx_len > 0;
}

int cl_tx_next(void) {
    for (int n = 0; n < CL_SESS_MAX; n++) {
        int i = (s_rr + n) % CL_SESS_MAX;
        if (s_tab[i].state == CL_FREE) { continue; }
        int h = handle_of(&s_tab[i]);
        if (!cl_tx_eligible(h)) { continue; }
        s_rr = (i + 1) % CL_SESS_MAX;
        return h;
    }
    return 0;
}

int cl_tx_peek(int handle, const uint8_t **ptr, size_t max) {
    sess_t *s = slot_of(handle);
    if (!s || !s->tx_len) { return 0; }
    if (ptr) { *ptr = s->tx + s->tx_off; }
    size_t n = s->tx_len;
    if (max && n > max) { n = max; }
    return (int)n;
}

void cl_tx_advance(int handle, size_t n) {
    sess_t *s = slot_of(handle);
    if (!s || !n) { return; }
    if (n > s->tx_len) { n = s->tx_len; }
    s->tx_off = (uint16_t)(s->tx_off + n);
    s->tx_len = (uint16_t)(s->tx_len - n);
    if (!s->tx_len) { s->tx_off = 0; }
    if (s->tx_starved && s->tx_len < s->tx_cap) {
        s->tx_starved = 0;
        pend(s, CL_EV_TX_ROOM);
    }
}

int cl_rx_put(int handle, const void *data, size_t n) {
    sess_t *s = slot_of(handle);
    if (!s || !s->rx) { return CL_ENOENT; }
    const uint8_t *p = data;
    int stored = 0;
    for (size_t i = 0; i < n; i++) {
        if (s->rx_n >= CL_RX_CAP) { s->rx_drop++; continue; }
        s->rx[s->rx_w] = p ? p[i] : 0;
        s->rx_w = (uint16_t)((s->rx_w + 1) % CL_RX_CAP);
        s->rx_n++;
        stored++;
    }
    if (stored && s->secure && s->state == CL_OPEN) { pend(s, CL_EV_RX); }
    return stored;
}

int cl_rx_peek(int handle, uint8_t *buf, size_t n) {
    sess_t *s = slot_of(handle);
    if (!s) { return CL_ENOENT; }
    if (n > CL_LISTEN_MAX) { n = CL_LISTEN_MAX; }
    if (n > s->rx_n) { n = s->rx_n; }
    for (size_t i = 0; i < n; i++) {
        buf[i] = s->rx[(s->rx_r + i) % CL_RX_CAP];
    }
    return (int)n;
}

int cl_listen(int handle, void *buf, size_t n) {
    sess_t *s = slot_of(handle);
    if (!s) { return CL_ENOENT; }
    if (s->state != CL_OPEN || !s->secure) { return CL_EBUSY; }
    if (n > CL_LISTEN_MAX) { n = CL_LISTEN_MAX; }
    if (n > s->rx_n) { n = s->rx_n; }
    uint8_t *out = buf;
    for (size_t i = 0; i < n; i++) {
        out[i] = s->rx[s->rx_r];
        s->rx_r = (uint16_t)((s->rx_r + 1) % CL_RX_CAP);
    }
    s->rx_n = (uint16_t)(s->rx_n - n);
    return (int)n;
}

int cl_inq_start(void) {
    if (s_inq) { return CL_EBUSY; }
    memset(s_dev, 0, sizeof s_dev);
    s_ndev = 0;
    s_inq = 1;
    return CL_OK;
}

void cl_inq_stop(void) { s_inq = 0; }
int cl_inq_active(void) { return s_inq; }

static inq_t *dev_find(const uint8_t *addr) {
    for (int i = 0; i < s_ndev; i++) {
        if (s_dev[i].used && memcmp(s_dev[i].addr, addr, 6) == 0) { return &s_dev[i]; }
    }
    return NULL;
}

static void copy_name(char *dst, const char *name) {
    size_t n = 0;
    if (!name) { dst[0] = 0; return; }
    while (name[n] && n + 1 < CL_NAME_LEN) { n++; }
    memcpy(dst, name, n);
    dst[n] = 0;
}

int cl_inq_note(const uint8_t *addr, int8_t rssi, uint32_t cod, const char *name) {
    if (!addr) { return CL_EINVAL; }
    inq_t *d = dev_find(addr);
    if (d) {
        d->rssi = rssi;
        d->cod = cod;
        if (name && name[0]) { copy_name(d->name, name); }
        d->dirty = 1;
        return 0;
    }
    if (s_ndev >= CL_DEV_MAX) { return CL_ENOMEM; }
    d = &s_dev[s_ndev++];
    memset(d, 0, sizeof *d);
    memcpy(d->addr, addr, 6);
    d->rssi = rssi;
    d->cod = cod;
    d->used = 1;
    d->dirty = 1;
    copy_name(d->name, name);
    return 1;
}

int cl_inq_set_name(const uint8_t *addr, const char *name) {
    inq_t *d = addr ? dev_find(addr) : NULL;
    if (!d) { return CL_ENOENT; }
    copy_name(d->name, name);
    d->dirty = 1;
    return CL_OK;
}

int cl_inq_count(void) { return s_ndev; }

int cl_inq_get(int index, cl_dev_t *out) {
    if (index < 0 || index >= s_ndev || !out) { return CL_EINVAL; }
    memset(out, 0, sizeof *out);
    memcpy(out->addr, s_dev[index].addr, 6);
    memcpy(out->name, s_dev[index].name, CL_NAME_LEN);
    out->rssi = s_dev[index].rssi;
    out->cod = s_dev[index].cod;
    return CL_OK;
}

int cl_inq_filter(uint32_t cod_mask, uint32_t cod_value, const char *prefix,
                  cl_dev_t *out, int max) {
    size_t pl = 0;
    if (prefix) { while (prefix[pl]) { pl++; } }
    int n = 0;
    for (int i = 0; i < s_ndev && n < max; i++) {
        if (cod_mask && (s_dev[i].cod & cod_mask) != cod_value) { continue; }
        if (pl && strncmp(s_dev[i].name, prefix, pl) != 0) { continue; }
        if (out) { cl_inq_get(i, &out[n]); }
        n++;
    }
    return n;
}

int cl_inq_next_dirty(cl_dev_t *out) {
    for (int i = 0; i < s_ndev; i++) {
        if (!s_dev[i].dirty) { continue; }
        s_dev[i].dirty = 0;
        if (out) { cl_inq_get(i, out); }
        return 1;
    }
    return 0;
}

int cl_info(int handle, cl_info_t *out) {
    sess_t *s = slot_of(handle);
    if (!s || !out) { return s ? CL_EINVAL : CL_ENOENT; }
    memset(out, 0, sizeof *out);
    out->handle = handle;
    out->owner = s->owner;
    out->cb = s->cb;
    out->cb_arg = s->cb_arg;
    memcpy(out->addr, s->addr, 6);
    out->uuid = s->uuid;
    out->channel = s->channel;
    out->fallback = s->fallback;
    out->level = s->level;
    out->io = s->io;
    out->bond = s->bond;
    out->state = s->state;
    out->ask = s->ask;
    out->phase = s->phase;
    out->secure = s->secure;
    out->watch = s->watch;
    out->drop_key = s->drop_key;
    out->retried = s->retried;
    out->key_known = s->key_known;
    out->key_auth = s->key_auth;
    out->key_sc = s->key_sc;
    out->why = s->why;
    out->mtu = s->mtu;
    out->cid = s->cid;
    out->acl = s->acl;
    out->cod = s->cod;
    out->rx_drop = s->rx_drop;
    out->confirm_num = s->confirm_num;
    memcpy(out->error, s->error, sizeof out->error);
    return CL_OK;
}

int cl_handles(int *out, int max) {
    int n = 0;
    for (int i = 0; i < CL_SESS_MAX && n < max; i++) {
        if (!live(&s_tab[i])) { continue; }
        if (out) { out[n] = handle_of(&s_tab[i]); }
        n++;
    }
    return n;
}

int cl_find_addr(const uint8_t *addr, int *out, int max) {
    int n = 0;
    if (!addr) { return 0; }
    for (int i = 0; i < CL_SESS_MAX && n < max; i++) {
        if (!live(&s_tab[i])) { continue; }
        if (memcmp(s_tab[i].addr, addr, 6) != 0) { continue; }
        if (out) { out[n] = handle_of(&s_tab[i]); }
        n++;
    }
    return n;
}

int cl_io_held(void) {
    int io = -1;
    for (int i = 0; i < CL_SESS_MAX; i++) {
        if (s_tab[i].io_hold) { io = s_tab[i].io; }
    }
    return io;
}

int cl_radio_setup(void) {
    if (s_inq) { return 1; }
    for (int i = 0; i < CL_SESS_MAX; i++) {
        uint8_t p = s_tab[i].phase;
        if (s_tab[i].state == CL_FREE) { continue; }
        if (p == CL_PHASE_PAGE || p == CL_PHASE_SDP || p == CL_PHASE_DEDICATED) { return 1; }
    }
    return 0;
}

int cl_take_ws(int handle) {
    sess_t *s = slot_of(handle);
    if (!s) { return 0; }
    int f = (int)s->pend_ws;
    s->pend_ws = 0;
    return f;
}

int cl_take_mod(int handle) {
    sess_t *s = slot_of(handle);
    if (!s) { return 0; }
    int f = (int)s->pend_mod;
    s->pend_mod = 0;
    return f;
}

int cl_mod_pending(int handle) {
    sess_t *s = slot_of(handle);
    if (!s) { return 0; }
    return s->pend_mod != 0;
}

int cl_set_watch(int handle, int on) {
    sess_t *s = slot_of(handle);
    if (!s) { return CL_ENOENT; }
    s->watch = on ? 1 : 0;
    return CL_OK;
}

int cl_addr_held(const uint8_t *addr) {
    if (!addr) { return 0; }
    for (int i = 0; i < CL_SESS_MAX; i++) {
        if (s_tab[i].state != CL_OPENING && s_tab[i].state != CL_OPEN) { continue; }
        if (memcmp(s_tab[i].addr, addr, 6) == 0 && s_tab[i].acl) { return 1; }
    }
    return 0;
}

const char *cl_state_name(int state) {
    switch (state) {
    case CL_OPENING: return "opening";
    case CL_OPEN: return "open";
    case CL_CLOSING: return "closing";
    case CL_FAILED: return "failed";
    default: return "idle";
    }
}

const char *cl_why_name(int why) {
    switch (why) {
    case CL_WHY_TIMEOUT: return "timeout";
    case CL_WHY_CANCEL: return "cancel";
    case CL_WHY_AUTH: return "auth";
    case CL_WHY_SECURITY: return "security";
    case CL_WHY_SDP: return "sdp";
    case CL_WHY_RFCOMM: return "rfcomm";
    case CL_WHY_PAGE: return "page";
    case CL_WHY_CLOSED: return "closed";
    case CL_WHY_REMOTE: return "remote";
    case CL_WHY_DEDICATED: return "dedicated";
    default: return "";
    }
}
