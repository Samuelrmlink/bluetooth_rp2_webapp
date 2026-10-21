/*
 * The bt group: the LE central of src/bt/le/central.h for native modules,
 * the same operations as Lua's bt.*. Values, write completions and link
 * changes come from central.c's le_pex_event() hook (BTstack context, the
 * main loop), are copied into a small queue, and are handed to the
 * listeners from pex_gw_poll() -- never from inside BTstack.
 */
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "pex/pex_gw.h"
#include "bt/registry.h"
#include "bt/le/central.h"
#include "bt/le/gattdb.h"
#include "pico/stdlib.h"

#define LISTENERS 8
#define QDEPTH 8
#define QDATA 244

typedef struct { int id; pex_mod_t *mod; pex_bt_fn fn; void *arg; uint8_t addr[6]; bool any; } listener_t;
typedef struct { pex_bt_event_t ev; uint8_t data[QDATA]; } qent_t;

static listener_t s_l[LISTENERS];
static int s_next = 1;
static qent_t *s_q;                 /* QDEPTH entries from the PEX pool, while anyone listens */
static uint32_t s_qhead, s_qtail, s_dropped;

static int nlisteners(void) {
    int n = 0;
    for (int i = 0; i < LISTENERS; i++) { n += s_l[i].fn != NULL; }
    return n;
}

void le_pex_event(int kind, const uint8_t *addr, int chr, const uint8_t *data, uint16_t len, int status) {
    if (!s_q || !nlisteners()) { return; }
    if (s_qhead - s_qtail >= QDEPTH) { s_qtail++; s_dropped++; }     /* drop the oldest */
    qent_t *e = &s_q[s_qhead++ % QDEPTH];
    memset(&e->ev, 0, sizeof e->ev);
    if (addr) { memcpy(e->ev.addr, addr, 6); }
    e->ev.kind = (uint8_t)kind;
    e->ev.chr = (int16_t)chr;
    e->ev.status = (int16_t)status;
    e->ev.len = len > QDATA ? QDATA : len;
    if (data && e->ev.len) { memcpy(e->data, data, e->ev.len); }
}

void pex_gw_bt_poll(void) {
    while (s_q && s_qtail != s_qhead) {
        qent_t *e = &s_q[s_qtail++ % QDEPTH];
        pex_bt_event_t ev = e->ev;
        uint8_t data[QDATA];
        memcpy(data, e->data, ev.len);
        ev.data = data;
        static const uint8_t zero[6];
        for (int i = 0; i < LISTENERS; i++) {
            listener_t *l = &s_l[i];
            if (!l->fn) { continue; }
            if (!l->any && memcmp(l->addr, ev.addr, 6) != 0 && memcmp(ev.addr, zero, 6) != 0) { continue; }
            pex_call(l->mod, (uint32_t)(uintptr_t)l->fn, (uint32_t)(uintptr_t)l->arg, (uint32_t)(uintptr_t)&ev, 0, 0);
        }
    }
}

int pex_bt_listen(const uint8_t *addr, pex_bt_fn fn, void *arg) {
    if (!fn) { return PEX_EINVAL; }
    if (!s_q) {
        s_q = pex_alloc_host(sizeof(qent_t) * QDEPTH);
        if (!s_q) { return PEX_ENOMEM; }
        s_qhead = s_qtail = 0;
    }
    for (int i = 0; i < LISTENERS; i++) {
        if (!s_l[i].fn) {
            s_l[i].id = s_next++;
            s_l[i].mod = pex_self();
            s_l[i].fn = fn;
            s_l[i].arg = arg;
            s_l[i].any = addr == NULL;
            if (addr) { memcpy(s_l[i].addr, addr, 6); }
            return s_l[i].id;
        }
    }
    return PEX_ENOMEM;
}

static void drop_queue_if_idle(void) {
    if (s_q && !nlisteners()) {
        pex_free(s_q);
        s_q = NULL;
    }
}

int pex_bt_unlisten(int id) {
    for (int i = 0; i < LISTENERS; i++) {
        if (s_l[i].fn && s_l[i].id == id && s_l[i].mod == pex_self()) {
            memset(&s_l[i], 0, sizeof s_l[i]);
            drop_queue_if_idle();
            return PEX_OK;
        }
    }
    return PEX_ENOENT;
}

void pex_gw_bt_unload(pex_mod_t *m) {
    for (int i = 0; i < LISTENERS; i++) {
        if (s_l[i].fn && s_l[i].mod == m) { memset(&s_l[i], 0, sizeof s_l[i]); }
    }
    drop_queue_if_idle();
}

/* ---- scanning ------------------------------------------------------------- */
int pex_bt_scan(uint32_t duration_ms, int active) {
    if (le_scanning()) { return PEX_OK; }
    le_scan_filter_t f;
    memset(&f, 0, sizeof f);
    f.active = active != 0;
    f.rssi_min = -127;
    return le_scan_start(&f, duration_ms) ? PEX_OK : PEX_EBUSY;
}
void pex_bt_scan_stop(void) { le_scan_stop(); }
int pex_bt_scanning(void) { return le_scanning(); }

int pex_bt_devices(pex_bt_dev_t *out, uint32_t max, const char *name_prefix, const char *uuid128) {
    uint32_t n = 0, now = to_ms_since_boot(get_absolute_time());
    size_t pl = name_prefix ? strlen(name_prefix) : 0;
    for (size_t i = 0; i < bt_registry_count() && n < max; i++) {
        bt_device_t *d = bt_registry_at(i);
        if (!d || !d->used || d->transport != BT_TRANSPORT_LE) { continue; }
        bool rules = pl || (uuid128 && *uuid128), match = !rules;
        if (pl && d->have_name && strncmp(d->name, name_prefix, pl) == 0) { match = true; }
        if (!match && uuid128 && *uuid128) {
            char u[BT_UUIDS_MAX][BT_UUID128_STR];
            int k = bt_device_uuids128(d, u, BT_UUIDS_MAX);
            for (int j = 0; j < k && !match; j++) { match = strcasecmp(u[j], uuid128) == 0; }
        }
        if (!match) { continue; }
        pex_bt_dev_t *o = &out[n++];
        memset(o, 0, sizeof *o);
        memcpy(o->addr, d->addr, 6);
        o->addr_type = d->addr_type;
        o->rssi = d->rssi;
        if (d->have_name) { snprintf(o->name, sizeof o->name, "%s", d->name); }
        o->connectable = d->connectable;
        o->connected = d->connected;
        o->n_uuid16 = d->n_uuid16 < 4 ? d->n_uuid16 : 4;
        memcpy(o->uuid16, d->uuid16, sizeof(uint16_t) * o->n_uuid16);
        o->mfg_len = d->have_mfg ? d->mfg_len : 0;
        memcpy(o->mfg, d->mfg, o->mfg_len);
        o->age_ms = now - d->last_seen_ms;
    }
    return (int)n;
}

int pex_bt_uuids128(const uint8_t *addr, char (*out)[37], int max) {
    bt_device_t *d = bt_registry_find(addr, BT_TRANSPORT_LE);
    return d ? bt_device_uuids128(d, out, max) : PEX_ENOENT;
}

int pex_bt_addr(const char *s, uint8_t *addr) {
    unsigned v[6];
    if (!s || sscanf(s, "%2x:%2x:%2x:%2x:%2x:%2x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) { return PEX_EINVAL; }
    for (int i = 0; i < 6; i++) { addr[i] = (uint8_t)v[i]; }
    return PEX_OK;
}
void pex_bt_addr_str(const uint8_t *a, char *out) {
    snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X", a[0], a[1], a[2], a[3], a[4], a[5]);
}

/* ---- links and GATT --------------------------------------------------------- */
int pex_bt_connect(const uint8_t *addr) {
    bt_device_t *d = bt_registry_find(addr, BT_TRANSPORT_LE);
    if (!d) { return PEX_ENOENT; }        /* scan first: the address type comes from the advertisement */
    if (d->connected) { return PEX_OK; }
    return le_connect(d->addr, d->addr_type) ? PEX_EPENDING : PEX_EBUSY;
}
int pex_bt_disconnect(const uint8_t *addr) { return le_disconnect(addr) ? PEX_OK : PEX_ENOENT; }

int pex_bt_state(const uint8_t *addr, int *ready) {
    bt_device_t *d = bt_registry_find(addr, BT_TRANSPORT_LE);
    gattdb_t *db = le_db_for_addr(addr);
    if (ready) { *ready = db && db->ready; }
    return d && d->connected ? PEX_BT_CONNECTED : PEX_BT_DISCONNECTED;
}
int pex_bt_mtu(const uint8_t *addr) { return le_mtu(addr); }

int pex_bt_find(const uint8_t *addr, const char *svc, const char *chr, int instance) {
    gattdb_t *db = le_db_for_addr(addr);
    if (!db || !db->ready) { return PEX_ESTATE; }
    uint8_t si, ci;
    return gattdb_find_chr(db, svc, chr, instance, &si, &ci) ? ci : PEX_ENOENT;
}

int pex_bt_props(const uint8_t *addr, int chr) {
    gattdb_t *db = le_db_for_addr(addr);
    if (!db || !db->ready || chr < 0 || chr >= db->n_chr) { return PEX_ENOENT; }
    return db->chr[chr].properties;
}

static int le_rc(int rc) {
    switch (rc) {
    case 0: return PEX_OK;
    case -1: return PEX_ESTATE;      /* not connected */
    case -2: return PEX_ENOENT;      /* not discovered / no such characteristic */
    case -3: return PEX_EINVAL;      /* too long */
    case -5: return PEX_EBUSY;       /* queue full */
    }
    return PEX_EIO;
}

int pex_bt_read(const uint8_t *addr, int chr) {
    if (chr < 0 || chr > 255) { return PEX_EINVAL; }
    return le_rc(le_read(addr, 0, (uint8_t)chr));
}

int pex_bt_write(const uint8_t *addr, int chr, const void *data, size_t len, int with_response, int want_done) {
    if (chr < 0 || chr > 255 || len > 0xFFFF) { return PEX_EINVAL; }
    int mtu = le_mtu(addr);
    if (mtu > 3 && len > (size_t)(mtu - 3)) { return PEX_EINVAL; }
    return le_rc(le_write(addr, 0, (uint8_t)chr, data, (uint16_t)len, with_response != 0, want_done != 0));
}

int pex_bt_subscribe(const uint8_t *addr, int chr, int mode) {
    if (chr < 0 || chr > 255 || mode < 0 || mode > 2) { return PEX_EINVAL; }
    return le_rc(le_subscribe(addr, 0, (uint8_t)chr, mode));
}
