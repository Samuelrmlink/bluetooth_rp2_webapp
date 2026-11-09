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
#include "bt/le/gattserver.h"
#include "bt/le/peripheral.h"
#include "bt/le/bond.h"
#include "autoconf.h"
#include "pex/pex_scan_sched.h"
#include "btstack.h"
#include "pico/stdlib.h"

#define LISTENERS 8
#define QDEPTH 8
/* ATT value ceiling on this build: HCI_ACL_PAYLOAD_SIZE minus the 4-byte
 * L2CAP header minus the 3-byte ATT header, which is 252. The C1000's
 * report fragments are that long. A 244-byte slot clipped them, the
 * checksum failed, and the frame was dropped, so the station never
 * published telemetry and its window never opened. The A2687's reports
 * are 218 and were unaffected. */
#define QDATA 252

typedef struct { int id; pex_mod_t *mod; pex_bt_fn fn; void *arg; uint8_t addr[6]; bool any; } listener_t;
typedef struct { pex_bt_event_t ev; uint8_t data[QDATA]; } qent_t;

static listener_t s_l[LISTENERS];
static int s_next = 1;
/* Static: the pool is taken only when a module loads, and the role console
 * listens before that. pex_alloc_host returned NULL and every event was dropped. */
static qent_t s_qbuf[QDEPTH];
static qent_t *s_q;
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
    pex_scan_sched_apply();
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
            /* A host listener (mod == NULL) is a plain function. pex_call
             * refuses a null module, which dropped the role console's events. */
            if (!l->mod) { l->fn(l->arg, &ev); continue; }
            pex_call(l->mod, (uint32_t)(uintptr_t)l->fn, (uint32_t)(uintptr_t)l->arg, (uint32_t)(uintptr_t)&ev, 0, 0);
        }
    }
}

int pex_bt_listen(const uint8_t *addr, pex_bt_fn fn, void *arg) {
    if (!fn) { return PEX_EINVAL; }
    if (!s_q) {
        s_q = s_qbuf;
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
        s_qhead = s_qtail = 0;
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
    pex_scan_sched_unload(m);
}

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

/* Local ATT database. Present in the group either way so a module built
 * against minor 1 still links; the calls do nothing useful until
 * BT_PERIPHERAL is on. */
/* Consumed by the next pex_gatt_characteristic. The ATT builder copies it. */
static uint8_t s_gatt_bytes[244];
static uint16_t s_gatt_len;
static int s_gatt_have;

int pex_gatt_begin(void) {
#if !CONFIG_BT_PERIPHERAL
    return PEX_ENOTSUP;
#else
    s_gatt_have = 0;
    s_gatt_len = 0;
    gatt_server_begin();
    return PEX_OK;
#endif
}

int pex_gatt_service(const char *uuid) {
#if !CONFIG_BT_PERIPHERAL
    (void)uuid;
    return PEX_ENOTSUP;
#else
    const char *err = NULL;
    if (!uuid || !gatt_server_add_service(uuid, &err)) { return PEX_EINVAL; }
    return PEX_OK;
#endif
}

int pex_gatt_set(const uint8_t *data, uint16_t len) {
#if !CONFIG_BT_PERIPHERAL
    (void)data; (void)len;
    return PEX_ENOTSUP;
#else
    if (len && !data) { return PEX_EINVAL; }
    if (len > sizeof s_gatt_bytes) { return PEX_EINVAL; }
    if (len) { memcpy(s_gatt_bytes, data, len); }
    s_gatt_len = len;
    s_gatt_have = 1;
    return PEX_OK;
#endif
}

int pex_gatt_characteristic(const char *uuid, int properties) {
#if !CONFIG_BT_PERIPHERAL
    (void)uuid;
    (void)properties;
    return PEX_ENOTSUP;
#else
    if (!uuid || properties < 0 || properties > 0xFFFF) { return PEX_EINVAL; }
    const uint8_t *val = NULL;
    uint16_t n = 0;
    if (s_gatt_have) {
        val = s_gatt_bytes;
        n = s_gatt_len;
        s_gatt_have = 0;
    }
    const char *err = NULL;
    if (!gatt_server_add_characteristic(uuid, (uint16_t)properties, val, n, NULL, &err)) {
        return PEX_EINVAL;
    }
    return PEX_OK;
#endif
}

int pex_gatt_commit(void) {
#if !CONFIG_BT_PERIPHERAL
    return PEX_ENOTSUP;
#else
    const char *err = NULL;
    if (!gatt_server_commit(&err)) { return PEX_EINVAL; }
    return PEX_OK;
#endif
}

/* Advertising payload. Same rule as pex_gatt_*: the calls stay in the group
 * so a module still links, and do nothing useful until BT_PERIPHERAL is on. */
int pex_adv_data(const uint8_t *adv, uint16_t adv_len, const uint8_t *scan_rsp, uint16_t scan_len) {
#if !CONFIG_BT_PERIPHERAL
    (void)adv; (void)adv_len; (void)scan_rsp; (void)scan_len;
    return PEX_ENOTSUP;
#else
    return bt_adv_data(adv, adv_len, scan_rsp, scan_len) ? PEX_EINVAL : PEX_OK;
#endif
}

int pex_adv_start(uint16_t interval_min, uint16_t interval_max) {
#if !CONFIG_BT_PERIPHERAL
    (void)interval_min; (void)interval_max;
    return PEX_ENOTSUP;
#else
    return bt_adv_start(interval_min, interval_max) ? PEX_EINVAL : PEX_OK;
#endif
}

int pex_adv_stop(void) {
#if !CONFIG_BT_PERIPHERAL
    return PEX_ENOTSUP;
#else
    bt_adv_stop();
    return PEX_OK;
#endif
}

/* Pairing. bond.c owns the policy, including declining Just Works when it
 * is "never". This file is the bt group, which is only registered with BT_LE. */
#if CONFIG_BT_LE
static bt_device_t *connected_le(const uint8_t *addr) {
    if (!addr) { return NULL; }
    bt_device_t *d = bt_registry_find(addr, BT_TRANSPORT_LE);
    return (d && d->connected) ? d : NULL;
}
#endif

int pex_bt_set_io(const char *io_capability, int mitm) {
#if !CONFIG_BT_LE
    (void)io_capability; (void)mitm;
    return PEX_ENOTSUP;
#else
    return bond_set_io(io_capability, mitm) ? PEX_OK : PEX_EINVAL;
#endif
}

int pex_bt_set_policy(const char *policy) {
#if !CONFIG_BT_LE
    (void)policy;
    return PEX_ENOTSUP;
#else
    if (!policy || !bond_set_policy(policy)) { return PEX_EINVAL; }
    return PEX_OK;
#endif
}

int pex_bt_passkey(const uint8_t *addr, uint32_t passkey) {
#if !CONFIG_BT_LE
    (void)addr; (void)passkey;
    return PEX_ENOTSUP;
#else
    bt_device_t *d = connected_le(addr);
    if (!d) { return addr ? PEX_ENOENT : PEX_EINVAL; }
    return bond_supply_passkey(d->con_handle, passkey, "pex") ? PEX_OK : PEX_EINVAL;
#endif
}

int pex_bt_confirm(const uint8_t *addr, int accept) {
#if !CONFIG_BT_LE
    (void)addr; (void)accept;
    return PEX_ENOTSUP;
#else
    bt_device_t *d = connected_le(addr);
    if (!d) { return addr ? PEX_ENOENT : PEX_EINVAL; }
    return bond_confirm(d->con_handle, accept != 0, "pex") ? PEX_OK : PEX_EIO;
#endif
}

int pex_bt_forget(const uint8_t *addr) {
#if !CONFIG_BT_LE
    (void)addr;
    return PEX_ENOTSUP;
#else
    if (!addr) {
        bond_forget_all();
        return PEX_OK;
    }
    char s[18];
    pex_bt_addr_str(addr, s);
    return bond_forget(s) ? PEX_OK : PEX_ENOENT;
#endif
}

int pex_bt_pair(const uint8_t *addr) {
#if !CONFIG_BT_LE
    (void)addr;
    return PEX_ENOTSUP;
#else
    bt_device_t *d = connected_le(addr);
    if (!d) { return addr ? PEX_ENOENT : PEX_EINVAL; }
    return bond_request(d->con_handle) ? PEX_OK : PEX_ESTATE;
#endif
}

int pex_bt_set_addr(const uint8_t addr[6], int random) {
#if !CONFIG_BT_LE
    (void)addr; (void)random;
    return PEX_ENOTSUP;
#else
    if (!addr || (random != 0 && random != 1)) { return PEX_EINVAL; }
    if (!random) {
        /* Public address is CYW43439 OTP. BTstack cannot change it. */
        return PEX_ENOTSUP;
    }
    /* Static random. BTstack forces the two MSBs and queues HCI LE Set Random Address. */
    gap_random_address_set(addr);
    return PEX_OK;
#endif
}
