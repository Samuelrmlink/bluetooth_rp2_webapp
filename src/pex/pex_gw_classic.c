/*
 * The classic group: RFCOMM sessions for native modules.
 *
 * Inquiry, SDP, paging, and pairing run in spp.c. This file copies a
 * session's queued events to its owner from pex_gw_poll, after classic_poll,
 * and never from inside BTstack. A PIN, passkey, or numeric comparison is
 * one submit call: the module may answer at once with a fixed code, or
 * later after a person types. Unload closes only this module's sessions.
 */
#include "autoconf.h"
#include "pex/pex_gw.h"

#include <string.h>

#include "bt/classic/spp.h"
#include "pico/stdlib.h"

static pex_mod_t *s_sdp_mod;
static uint32_t s_sdp_fn;
static void *s_sdp_arg;

static int own(int handle, cl_info_t *out) {
    pex_mod_t *m = pex_self();
    if (!m || cl_info(handle, out) != CL_OK) { return PEX_ENOENT; }
    if (out->owner != (uintptr_t)m) { return PEX_ENOENT; }
    return PEX_OK;
}

static void emit(pex_mod_t *m, uint32_t fn, void *arg, const pex_cl_event_t *ev) {
    if (!m || !fn || !ev) { return; }
    pex_call(m, fn, (uint32_t)(uintptr_t)arg, (uint32_t)(uintptr_t)ev, 0, 0);
}

static void take_sdp(void) {
    uint8_t addr[6], channel = 0;
    uint16_t uuid = 0;
    int status = 0;
    pex_cl_event_t ev;
    if (!classic_sdp_take(addr, &uuid, &channel, &status)) { return; }
    if (s_sdp_mod && s_sdp_fn) {
        memset(&ev, 0, sizeof ev);
        ev.kind = PEX_CL_SDP;
        ev.status = status;
        ev.channel = channel;
        ev.uuid = uuid;
        memcpy(ev.addr, addr, 6);
        emit(s_sdp_mod, s_sdp_fn, s_sdp_arg, &ev);
    }
    s_sdp_mod = NULL;
    s_sdp_fn = 0;
    s_sdp_arg = NULL;
}

static void one_session(int handle) {
    cl_info_t in;
    pex_cl_event_t ev;
    uint8_t rx[CL_LISTEN_MAX];
    int bits, kind;
    if (cl_info(handle, &in) != CL_OK) { return; }
    bits = cl_take_mod(handle);
    if (!bits || !in.cb || !in.owner) { return; }
    memset(&ev, 0, sizeof ev);
    ev.handle = in.handle;
    ev.why = in.why;
    ev.number = in.confirm_num;
    ev.channel = in.channel;
    ev.uuid = in.uuid;
    memcpy(ev.addr, in.addr, 6);
    if (bits & CL_EV_STATE) {
        kind = 0;
        if (in.state == CL_OPEN) { kind = PEX_CL_OPEN; }
        else if (in.state == CL_FAILED) { kind = PEX_CL_FAILED; }
        else if (in.state == CL_CLOSING) { kind = PEX_CL_CLOSED; }
        if (kind) {
            ev.kind = kind;
            ev.data = NULL;
            ev.len = 0;
            emit((pex_mod_t *)in.owner, (uint32_t)in.cb, (void *)in.cb_arg, &ev);
        }
    }
    if (bits & CL_EV_PIN) {
        ev.kind = PEX_CL_PIN;
        ev.data = NULL;
        ev.len = 0;
        emit((pex_mod_t *)in.owner, (uint32_t)in.cb, (void *)in.cb_arg, &ev);
    }
    if (bits & CL_EV_PASSKEY) {
        ev.kind = PEX_CL_PASSKEY;
        ev.data = NULL;
        ev.len = 0;
        emit((pex_mod_t *)in.owner, (uint32_t)in.cb, (void *)in.cb_arg, &ev);
    }
    if (bits & CL_EV_CONFIRM) {
        ev.kind = PEX_CL_CONFIRM;
        ev.data = NULL;
        ev.len = 0;
        emit((pex_mod_t *)in.owner, (uint32_t)in.cb, (void *)in.cb_arg, &ev);
    }
    if (bits & CL_EV_RX) {
        int n = cl_rx_peek(handle, rx, sizeof rx);
        ev.kind = PEX_CL_RX;
        ev.data = n > 0 ? rx : NULL;
        ev.len = n > 0 ? (uint16_t)n : 0;
        emit((pex_mod_t *)in.owner, (uint32_t)in.cb, (void *)in.cb_arg, &ev);
    }
    if (bits & CL_EV_TX_ROOM) {
        ev.kind = PEX_CL_TX_ROOM;
        ev.data = NULL;
        ev.len = 0;
        emit((pex_mod_t *)in.owner, (uint32_t)in.cb, (void *)in.cb_arg, &ev);
    }
}

void pex_gw_classic_poll(void) {
    int hs[CL_SESS_MAX];
    int n;
    take_sdp();
    n = cl_handles(hs, CL_SESS_MAX);
    for (int i = 0; i < n; i++) { one_session(hs[i]); }
}

void pex_gw_classic_unload(pex_mod_t *m) {
    if (!m) { return; }
    if (s_sdp_mod == m) {
        s_sdp_mod = NULL;
        s_sdp_fn = 0;
        s_sdp_arg = NULL;
    }
    cl_release_owner((uintptr_t)m);
}

int pex_bt_classic_scan(uint32_t duration_ms) { return classic_scan_start(duration_ms); }
void pex_bt_classic_scan_stop(void) { classic_scan_stop(); }
int pex_bt_classic_scanning(void) { return cl_inq_active(); }

int pex_bt_classic_devices(pex_cl_dev_t *out, uint32_t max, uint32_t cod_mask, uint32_t cod_value,
                           const char *name_prefix) {
    cl_dev_t tmp[CL_DEV_MAX];
    int n;
    if (max > CL_DEV_MAX) { max = CL_DEV_MAX; }
    if (max && !out) { return PEX_EINVAL; }
    n = cl_inq_filter(cod_mask, cod_value, name_prefix, tmp, (int)max);
    for (int i = 0; i < n; i++) {
        memset(&out[i], 0, sizeof out[i]);
        memcpy(out[i].addr, tmp[i].addr, 6);
        memcpy(out[i].name, tmp[i].name, sizeof out[i].name);
        out[i].name[sizeof out[i].name - 1] = 0;
        out[i].rssi = tmp[i].rssi;
        out[i].cod = tmp[i].cod;
    }
    return n;
}

int pex_bt_classic_sdp(const uint8_t *addr, uint16_t uuid, pex_cl_fn fn, void *arg) {
    int rc;
    pex_mod_t *m = pex_self();
    if (!m || !fn) { return PEX_EINVAL; }
    rc = classic_sdp_start(addr, uuid);
    if (rc) { return rc; }
    s_sdp_mod = m;
    s_sdp_fn = (uint32_t)(uintptr_t)fn;
    s_sdp_arg = arg;
    return PEX_OK;
}

int pex_bt_classic_open(const pex_cl_open_t *opt) {
    cl_open_t o;
    pex_mod_t *m = pex_self();
    if (!opt || !m) { return PEX_EINVAL; }
    if (opt->npin > PEX_CL_PIN_MAX) { return PEX_EINVAL; }
    memset(&o, 0, sizeof o);
    memcpy(o.addr, opt->addr, 6);
    o.uuid = opt->uuid;
    o.channel = opt->channel;
    o.fallback = opt->fallback;
    o.level = opt->level;
    o.io = opt->io;
    o.bond = opt->bond;
    o.drop_key = opt->drop_key ? 1 : 0;
    o.deadline_ms = opt->deadline_ms;
    o.owner = (uintptr_t)m;
    o.cb = (uintptr_t)opt->fn;
    o.cb_arg = (uintptr_t)opt->arg;
    o.npin = opt->npin;
    for (int i = 0; i < opt->npin; i++) {
        size_t n = 0;
        while (opt->pin[i][n] && n < CL_PIN_LEN) { n++; }
        if (n == 0 || n >= CL_PIN_LEN) { return PEX_EINVAL; }
        memcpy(o.pin[i], opt->pin[i], n);
    }
    return classic_open(&o);
}

int pex_bt_classic_pin(int handle, const char *pin) {
    cl_info_t in;
    int rc = own(handle, &in);
    if (rc) { return rc; }
    return classic_submit_pin(handle, pin);
}

int pex_bt_classic_passkey(int handle, int negative, uint32_t passkey) {
    cl_info_t in;
    int rc = own(handle, &in);
    if (rc) { return rc; }
    return classic_submit_passkey(handle, negative, passkey);
}

int pex_bt_classic_confirm(int handle, int accept) {
    cl_info_t in;
    int rc = own(handle, &in);
    if (rc) { return rc; }
    return classic_submit_confirm(handle, accept);
}

int pex_bt_classic_send(int handle, const void *data, size_t len) {
    cl_info_t in;
    int rc = own(handle, &in);
    if (rc) { return rc; }
    return classic_send(handle, data, len);
}

int pex_bt_classic_tx_room(int handle) {
    cl_info_t in;
    int rc = own(handle, &in);
    if (rc) { return rc; }
    return cl_tx_room(handle);
}

int pex_bt_classic_listen(int handle, void *buf, size_t cap) {
    cl_info_t in;
    int rc = own(handle, &in);
    if (rc) { return rc; }
    return cl_listen(handle, buf, cap);
}

int pex_bt_classic_close(int handle) {
    cl_info_t in;
    int rc = own(handle, &in);
    if (rc) { return rc; }
    return classic_close(handle);
}

int pex_bt_classic_state(int handle, pex_cl_info_t *out) {
    cl_info_t in;
    int rc = own(handle, &in);
    if (rc) { return rc; }
    if (!out) { return PEX_EINVAL; }
    memset(out, 0, sizeof *out);
    out->handle = in.handle;
    memcpy(out->addr, in.addr, 6);
    out->uuid = in.uuid;
    out->channel = in.channel;
    out->state = in.state;
    out->ask = in.ask;
    out->level = in.level;
    out->secure = in.secure;
    out->why = in.why;
    out->mtu = in.mtu;
    out->cod = in.cod;
    out->rx_drop = in.rx_drop;
    out->confirm_num = in.confirm_num;
    memcpy(out->error, in.error, sizeof out->error);
    return PEX_OK;
}

int pex_bt_classic_forget(const uint8_t *addr) { return classic_forget(addr); }
