/*
 * PEX bt group on ESP32-C5. NimBLE runs only when CONFIG_C5_BLE=y.
 * With the symbol off every call still links and the int calls return
 * PEX_ENOTSUP, so either image satisfies the import table.
 *
 * Stack callbacks copy into a queue. pex_host_poll delivers it with
 * pex_call. They do not call a PEX module.
 *
 * Addresses at this boundary are display order (addr[0] is the leftmost
 * octet). NimBLE ble_addr_t.val is little-endian and is reversed here.
 *
 * Policy "never" must not complete Just Works. NimBLE does not ask the
 * app before Just Works, so while the policy is never the peripheral
 * security level is 1 (a Pairing Request is rejected before it runs)
 * and a wrapped store read makes the bond count look full. store_status
 * then returns an error from BLE_STORE_EVENT_FULL, which aborts
 * pair_initiate and pair_req_rx before the procedure starts.
 */
#include "ble_pex.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "sdkconfig.h"

int pex_bt_addr(const char *s, uint8_t *addr) {
    unsigned v[6];
    if (!s || !addr || sscanf(s, "%2x:%2x:%2x:%2x:%2x:%2x",
                              &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) {
        return PEX_EINVAL;
    }
    for (int i = 0; i < 6; i++) addr[i] = (uint8_t)v[i];
    return PEX_OK;
}

void pex_bt_addr_str(const uint8_t *a, char *out) {
    if (!out) return;
    if (!a) {
        snprintf(out, 18, "00:00:00:00:00:00");
        return;
    }
    snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
             a[0], a[1], a[2], a[3], a[4], a[5]);
}

#if !CONFIG_C5_BLE

static int off(void) { return PEX_ENOTSUP; }

int pex_bt_scan(uint32_t duration_ms, int active) { (void)duration_ms; (void)active; return off(); }
void pex_bt_scan_stop(void) {}
int pex_bt_scanning(void) { return off(); }
int pex_bt_scan_periodic(uint32_t burst_ms, uint32_t period_ms, int active) {
    (void)burst_ms; (void)period_ms; (void)active; return off();
}
int pex_bt_scan_state(void) { return PEX_BT_SCAN_NONE; }
int pex_bt_quiet(uint32_t ms) { (void)ms; return off(); }
int pex_bt_quiet_extend(int id, uint32_t ms) { (void)id; (void)ms; return PEX_ENOENT; }
int pex_bt_quiet_release(int id) { (void)id; return PEX_ENOENT; }
int pex_bt_quiet_left(int id) { (void)id; return PEX_ENOENT; }
int pex_bt_devices(pex_bt_dev_t *out, uint32_t max, const char *name_prefix, const char *uuid128) {
    (void)out; (void)max; (void)name_prefix; (void)uuid128; return off();
}
int pex_bt_uuids128(const uint8_t *addr, char (*out)[37], int max) {
    (void)addr; (void)out; (void)max; return off();
}
int pex_bt_connect(const uint8_t *addr) { (void)addr; return off(); }
int pex_bt_disconnect(const uint8_t *addr) { (void)addr; return off(); }
int pex_bt_state(const uint8_t *addr, int *ready) {
    (void)addr; if (ready) *ready = 0; return off();
}
int pex_bt_mtu(const uint8_t *addr) { (void)addr; return off(); }
int pex_bt_find(const uint8_t *addr, const char *svc_uuid, const char *chr_uuid, int instance) {
    (void)addr; (void)svc_uuid; (void)chr_uuid; (void)instance; return off();
}
int pex_bt_props(const uint8_t *addr, int chr) { (void)addr; (void)chr; return off(); }
int pex_bt_read(const uint8_t *addr, int chr) { (void)addr; (void)chr; return off(); }
int pex_bt_write(const uint8_t *addr, int chr, const void *data, size_t len, int with_response, int want_done) {
    (void)addr; (void)chr; (void)data; (void)len; (void)with_response; (void)want_done; return off();
}
int pex_bt_subscribe(const uint8_t *addr, int chr, int mode) {
    (void)addr; (void)chr; (void)mode; return off();
}
int pex_bt_listen(const uint8_t *addr, pex_bt_fn fn, void *arg) {
    (void)addr; (void)fn; (void)arg; return off();
}
int pex_bt_unlisten(int id) { (void)id; return off(); }
int pex_gatt_begin(void) { return off(); }
int pex_gatt_service(const char *uuid) { (void)uuid; return off(); }
int pex_gatt_characteristic(const char *uuid, int properties) {
    (void)uuid; (void)properties; return off();
}
int pex_gatt_set(const uint8_t *data, uint16_t len) {
    (void)data; (void)len; return off();
}
int pex_gatt_commit(void) { return off(); }
int pex_adv_data(const uint8_t *adv, uint16_t adv_len, const uint8_t *scan_rsp, uint16_t scan_len) {
    (void)adv; (void)adv_len; (void)scan_rsp; (void)scan_len; return off();
}
int pex_adv_start(uint16_t interval_min, uint16_t interval_max) {
    (void)interval_min; (void)interval_max; return off();
}
int pex_adv_stop(void) { return off(); }
int pex_bt_set_io(const char *io_capability, int mitm) {
    (void)io_capability; (void)mitm; return off();
}
int pex_bt_set_policy(const char *policy) { (void)policy; return off(); }
int pex_bt_passkey(const uint8_t *addr, uint32_t passkey) {
    (void)addr; (void)passkey; return off();
}
int pex_bt_confirm(const uint8_t *addr, int accept) { (void)addr; (void)accept; return off(); }
int pex_bt_forget(const uint8_t *addr) { (void)addr; return off(); }
int pex_bt_pair(const uint8_t *addr) { (void)addr; return off(); }
int pex_bt_set_addr(const uint8_t addr[6], int random) { (void)addr; (void)random; return off(); }

void ble_pex_start(void) {}
void ble_pex_poll(void) {}
void ble_pex_unload(pex_mod_t *m) { (void)m; }

#else

#include <stdint.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_att.h"
#include "host/ble_store.h"
#include "host/ble_sm.h"
#include "host/ble_hs_adv.h"
#include "host/ble_hs_mbuf.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "os/os_mbuf.h"
#include "ota/smp.h"
#include "port/pex_port.h"

void ble_store_config_init(void);

static const char *TAG = "ble";

#define LISTENERS 8
#define QDEPTH    8
#define QDATA     252
#define NSCAN     16
#define NLINK     3
#define DB_SVC    8
#define DB_CHR    24
#define NU16      4
#define NU128     4
#define SVC_MAX   4
#define CHR_MAX   6
#define GATT_VAL  244
#define ADV_LO    0x0020
#define ADV_HI    0x4000
#define ADV_DEF_MIN 0x00A0
#define ADV_DEF_MAX 0x00F0

enum { POL_NEVER = 0, POL_ON_DEMAND = 1, POL_ALWAYS = 2 };
enum { DISC_IDLE = 0, DISC_SEC, DISC_MTU, DISC_SVC, DISC_CHR, DISC_DSC };
enum { OP_NONE = 0, OP_READ, OP_WRITE, OP_SUB };

typedef struct {
    int id;
    pex_mod_t *mod;
    pex_bt_fn fn;
    void *arg;
    uint8_t addr[6];
    int any;
} listener_t;

typedef struct {
    pex_bt_event_t ev;
    uint8_t data[QDATA];
} qent_t;

typedef struct {
    int used;
    uint8_t addr[6];
    uint8_t addr_type;
    int8_t rssi;
    char name[31];
    uint8_t connectable;
    uint8_t n_uuid16;
    uint16_t uuid16[NU16];
    uint8_t n_uuid128;
    uint8_t uuid128[NU128][16];
    uint8_t mfg_len;
    uint8_t mfg[31];
    uint32_t last_ms;
} scan_t;

typedef struct {
    ble_uuid_any_t uuid;
    uint16_t start_h, end_h;
    uint8_t first, nchr;
} db_svc_t;

typedef struct {
    ble_uuid_any_t uuid;
    uint8_t props;
    uint16_t def_h, val_h, cccd;
} db_chr_t;

typedef struct {
    int used;
    int connecting;
    int ready;
    int refuse;
    int drop;
    int asked;
    int disc;
    int disc_i;
    int wait_sec;
    uint8_t addr[6];
    uint8_t addr_type;
    uint16_t handle;
    uint16_t mtu;
    int nsvc, nchr;
    db_svc_t svc[DB_SVC];
    db_chr_t chr[DB_CHR];
    struct {
        int kind;
        int chr;
        int want_done;
        int with_rsp;
        int retried;
        int reported;
        uint8_t mode;
        uint16_t len;
        uint8_t data[QDATA];
    } op;
    uint8_t io_action;
    uint32_t io_numcmp;
    /* Set by pex_bt_pair when a bond is already stored. ENC_CHANGE then
     * reports a restore, or asks the poll to delete the bond and pair. */
    int had_bond;
    int rekey;
} link_t;

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static listener_t s_l[LISTENERS];
static int s_next = 1;
/* Static so a host listener (the role console) does not depend on the PEX
 * pool, and unload cannot free a buffer the poll still reads. */
static qent_t s_qbuf[QDEPTH];
static qent_t *s_q = s_qbuf;
static uint32_t s_qhead, s_qtail, s_dropped;

static scan_t s_scan[NSCAN];
static int s_nscan;
static link_t s_link[NLINK];
static volatile int s_synced;
static volatile int s_policy = POL_ON_DEMAND;
static volatile int s_store_pass;
static uint8_t s_io = BLE_HS_IO_NO_INPUT_OUTPUT;
static int s_mitm;
static uint8_t s_own;
static int s_rnd_valid;
static uint8_t s_rnd_le[6];

static uint8_t s_adv[31], s_rsp[31];
static uint8_t s_adv_len, s_rsp_len;
static uint16_t s_imin = ADV_DEF_MIN, s_imax = ADV_DEF_MAX;
static int s_adv_want;

static ble_store_read_fn *s_orig_read;
static ble_store_status_fn *s_orig_status;

static int s_building;
static int s_bnsvc;
typedef struct {
    ble_uuid_any_t uuid;
    uint32_t flags;
    int smp;
    uint16_t vlen;
    uint8_t val[GATT_VAL];
} bchr_t;
static struct {
    ble_uuid_any_t uuid;
    int nchr;
    bchr_t chr[CHR_MAX];
} s_b[SVC_MAX];
static uint8_t s_gatt_bytes[GATT_VAL];
static uint16_t s_gatt_len;
static int s_gatt_have;

static int s_nsvc;
static ble_uuid_any_t s_svc_uuid[SVC_MAX];
static struct ble_gatt_svc_def s_svc[SVC_MAX + 1];
static ble_uuid_any_t s_chr_uuid[SVC_MAX][CHR_MAX];
static struct ble_gatt_chr_def s_chr[SVC_MAX][CHR_MAX + 1];
static uint16_t s_chr_handle[SVC_MAX][CHR_MAX];
static uint8_t s_chr_smp[SVC_MAX][CHR_MAX];
static uint16_t s_chr_len[SVC_MAX][CHR_MAX];
static uint8_t s_chr_val[SVC_MAX][CHR_MAX][GATT_VAL];
static int16_t s_chr_ix[SVC_MAX][CHR_MAX];

static struct {
    int pending;
    uint16_t conn;
    uint16_t handle;
    uint16_t len;
    uint8_t req[512];
} s_smp;

static const char SMP_CHR[] = "da2e7828-fbce-4e01-ae9e-261174997c48";

static void addr_from_le(uint8_t disp[6], const uint8_t le[6]) {
    for (int i = 0; i < 6; i++) disp[i] = le[5 - i];
}
static void addr_to_le(uint8_t le[6], const uint8_t disp[6]) {
    for (int i = 0; i < 6; i++) le[i] = disp[5 - i];
}
static int addr_is(const uint8_t *a, const uint8_t *b) {
    return a && b && memcmp(a, b, 6) == 0;
}
static int popcount8(uint8_t v) {
    int n = 0;
    while (v) { n += v & 1; v >>= 1; }
    return n;
}

static void uuid_pex(const ble_uuid_t *u, char *dst, size_t cap) {
    dst[0] = 0;
    if (!u || cap == 0) return;
    if (u->type == BLE_UUID_TYPE_16) {
        snprintf(dst, cap, "%04x", BLE_UUID16(u)->value);
        return;
    }
    if (u->type == BLE_UUID_TYPE_32) {
        snprintf(dst, cap, "%08lx", (unsigned long)BLE_UUID32(u)->value);
        return;
    }
    if (u->type != BLE_UUID_TYPE_128) return;
    const uint8_t *v = BLE_UUID128(u)->value;
    static const uint8_t base[12] = {
        0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80, 0x00, 0x10, 0x00, 0x00
    };
    if (memcmp(v, base, 12) == 0 && v[14] == 0 && v[15] == 0) {
        snprintf(dst, cap, "%04x", (unsigned)(v[12] | (v[13] << 8)));
        return;
    }
    snprintf(dst, cap,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             v[15], v[14], v[13], v[12], v[11], v[10], v[9], v[8],
             v[7], v[6], v[5], v[4], v[3], v[2], v[1], v[0]);
}

static int parse_uuid(const char *s, ble_uuid_any_t *out) {
    if (!s || !out) return -1;
    return ble_uuid_from_str(out, s) == 0 ? 0 : -1;
}

static int smp_uuid(const char *s) {
    if (!s) return 0;
    for (int i = 0; SMP_CHR[i]; i++) {
        char a = s[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (a != SMP_CHR[i]) return 0;
    }
    return s[sizeof SMP_CHR - 1] == 0;
}

static int att_of(uint16_t st) {
    if (st >= BLE_HS_ERR_ATT_BASE && st < BLE_HS_ERR_ATT_BASE + 0x100)
        return (int)(st - BLE_HS_ERR_ATT_BASE);
    return 0;
}
static int wants_sec(int att) {
    return att == 0x05 || att == 0x08 || att == 0x0c || att == 0x0f;
}
static int nim_rc(int rc) {
    if (rc == 0) return PEX_OK;
    if (rc == BLE_HS_EBUSY || rc == BLE_HS_EALREADY) return PEX_EBUSY;
    if (rc == BLE_HS_EINVAL) return PEX_EINVAL;
    if (rc == BLE_HS_ENOTCONN || rc == BLE_HS_EDISABLED) return PEX_ESTATE;
    if (rc == BLE_HS_ENOENT) return PEX_ENOENT;
    return PEX_EIO;
}

static link_t *link_by_addr(const uint8_t *addr) {
    if (!addr) return NULL;
    for (int i = 0; i < NLINK; i++) {
        if (s_link[i].used && addr_is(s_link[i].addr, addr)) return &s_link[i];
    }
    return NULL;
}
static link_t *link_by_handle(uint16_t h) {
    for (int i = 0; i < NLINK; i++) {
        if (s_link[i].used && !s_link[i].connecting && s_link[i].handle == h) return &s_link[i];
    }
    return NULL;
}
static link_t *link_connecting(void) {
    for (int i = 0; i < NLINK; i++) {
        if (s_link[i].used && s_link[i].connecting) return &s_link[i];
    }
    return NULL;
}
static int link_index(const link_t *l) { return (int)(l - s_link); }
static int any_busy(void) {
    for (int i = 0; i < NLINK; i++) {
        if (!s_link[i].used) continue;
        if (s_link[i].connecting || s_link[i].disc != DISC_IDLE) return 1;
    }
    return 0;
}

static void emit(const uint8_t *addr, int kind, int chr, const void *data, uint16_t len, int status) {
    taskENTER_CRITICAL(&s_mux);
    if (!s_q) { taskEXIT_CRITICAL(&s_mux); return; }
    int n = 0;
    for (int i = 0; i < LISTENERS; i++) n += s_l[i].fn != NULL;
    if (!n) { taskEXIT_CRITICAL(&s_mux); return; }
    if (s_qhead - s_qtail >= QDEPTH) { s_qtail++; s_dropped++; }
    qent_t *e = &s_q[s_qhead++ % QDEPTH];
    memset(e, 0, sizeof *e);
    if (addr) memcpy(e->ev.addr, addr, 6);
    e->ev.kind = (uint8_t)kind;
    e->ev.chr = (int16_t)chr;
    e->ev.status = (int16_t)status;
    e->ev.len = len > QDATA ? QDATA : len;
    if (data && e->ev.len) memcpy(e->data, data, e->ev.len);
    taskEXIT_CRITICAL(&s_mux);
}

static void link_clear(link_t *l) { memset(l, 0, sizeof *l); }

static void fail_link(link_t *l) {
    uint8_t addr[6];
    memcpy(addr, l->addr, 6);
    l->disc = DISC_IDLE;
    l->ready = 0;
    l->op.kind = OP_NONE;
    emit(addr, PEX_BT_FAILED, -1, NULL, 0, 0);
}

static int issue_op(link_t *l);
static int gap_event(struct ble_gap_event *ev, void *arg);

static void start_svc(link_t *l);
static void start_chr(link_t *l);
static void start_dsc(link_t *l);

static int on_mtu(uint16_t conn, const struct ble_gatt_error *err, uint16_t mtu, void *arg) {
    (void)conn; (void)err;
    link_t *l = &s_link[(int)(intptr_t)arg];
    if (!l->used || l->disc != DISC_MTU) return 0;
    if (mtu >= 23) l->mtu = mtu;
    start_svc(l);
    return 0;
}
static int on_svc(uint16_t conn, const struct ble_gatt_error *err, const struct ble_gatt_svc *svc, void *arg) {
    (void)conn;
    link_t *l = &s_link[(int)(intptr_t)arg];
    if (!l->used || l->disc != DISC_SVC) return 0;
    if (err->status == 0 && svc) {
        if (l->nsvc < DB_SVC) {
            db_svc_t *s = &l->svc[l->nsvc++];
            memset(s, 0, sizeof *s);
            ble_uuid_copy(&s->uuid, &svc->uuid.u);
            s->start_h = svc->start_handle;
            s->end_h = svc->end_handle;
        }
        return 0;
    }
    if (err->status == BLE_HS_EDONE) {
        l->disc_i = 0;
        start_chr(l);
        return 0;
    }
    fail_link(l);
    return 0;
}
static int on_chr(uint16_t conn, const struct ble_gatt_error *err, const struct ble_gatt_chr *chr, void *arg) {
    (void)conn;
    link_t *l = &s_link[(int)(intptr_t)arg];
    if (!l->used || l->disc != DISC_CHR) return 0;
    if (err->status == 0 && chr) {
        if (l->disc_i < l->nsvc && l->nchr < DB_CHR) {
            db_chr_t *c = &l->chr[l->nchr];
            memset(c, 0, sizeof *c);
            ble_uuid_copy(&c->uuid, &chr->uuid.u);
            c->props = chr->properties;
            c->def_h = chr->def_handle;
            c->val_h = chr->val_handle;
            if (l->svc[l->disc_i].nchr == 0) l->svc[l->disc_i].first = (uint8_t)l->nchr;
            l->svc[l->disc_i].nchr++;
            l->nchr++;
        }
        return 0;
    }
    if (err->status == BLE_HS_EDONE) {
        l->disc_i++;
        start_chr(l);
        return 0;
    }
    fail_link(l);
    return 0;
}
static int on_dsc(uint16_t conn, const struct ble_gatt_error *err, uint16_t chr_val, const struct ble_gatt_dsc *dsc, void *arg) {
    (void)conn; (void)chr_val;
    link_t *l = &s_link[(int)(intptr_t)arg];
    if (!l->used || l->disc != DISC_DSC) return 0;
    if (err->status == 0 && dsc) {
        if (l->disc_i < l->nchr && ble_uuid_u16(&dsc->uuid.u) == 0x2902)
            l->chr[l->disc_i].cccd = dsc->handle;
        return 0;
    }
    l->disc_i++;
    start_dsc(l);
    return 0;
}

static void become_ready(link_t *l) {
    uint8_t addr[6];
    memcpy(addr, l->addr, 6);
    l->disc = DISC_IDLE;
    l->ready = 1;
    emit(addr, PEX_BT_READY, -1, NULL, 0, 0);
}
static void start_dsc(link_t *l) {
    while (l->disc_i < l->nchr) {
        db_chr_t *c = &l->chr[l->disc_i];
        if (!(c->props & (BLE_GATT_CHR_PROP_NOTIFY | BLE_GATT_CHR_PROP_INDICATE))) {
            l->disc_i++;
            continue;
        }
        uint16_t end = 0;
        for (int s = 0; s < l->nsvc; s++) {
            db_svc_t *sv = &l->svc[s];
            if (l->disc_i >= sv->first && l->disc_i < sv->first + sv->nchr) {
                end = sv->end_h;
                if (l->disc_i + 1 < sv->first + sv->nchr) {
                    uint16_t nxt = l->chr[l->disc_i + 1].def_h;
                    if (nxt > 0) end = (uint16_t)(nxt - 1);
                }
                break;
            }
        }
        if (c->val_h == 0 || c->val_h >= end) { l->disc_i++; continue; }
        l->disc = DISC_DSC;
        int rc = ble_gattc_disc_all_dscs(l->handle, (uint16_t)(c->val_h + 1), end, on_dsc, (void *)(intptr_t)link_index(l));
        if (rc) { l->disc_i++; continue; }
        return;
    }
    become_ready(l);
}
static void start_chr(link_t *l) {
    if (l->disc_i >= l->nsvc) {
        l->disc_i = 0;
        start_dsc(l);
        return;
    }
    db_svc_t *s = &l->svc[l->disc_i];
    l->disc = DISC_CHR;
    int rc = ble_gattc_disc_all_chrs(l->handle, s->start_h, s->end_h, on_chr, (void *)(intptr_t)link_index(l));
    if (rc) fail_link(l);
}
static void start_svc(link_t *l) {
    l->nsvc = 0;
    l->nchr = 0;
    l->disc = DISC_SVC;
    l->ready = 0;
    int rc = ble_gattc_disc_all_svcs(l->handle, on_svc, (void *)(intptr_t)link_index(l));
    if (rc) fail_link(l);
}
static void start_discover(link_t *l) {
    l->disc = DISC_MTU;
    l->mtu = ble_att_mtu(l->handle);
    int rc = ble_gattc_exchange_mtu(l->handle, on_mtu, (void *)(intptr_t)link_index(l));
    if (rc) start_svc(l);
}

static int on_gatt(uint16_t conn, const struct ble_gatt_error *err,
                   struct ble_gatt_attr *attr, void *arg);

static void clear_op(link_t *l) { memset(&l->op, 0, sizeof l->op); }

static void complete_op(link_t *l, int status, const void *data, uint16_t len) {
    uint8_t addr[6];
    int kind = l->op.kind;
    int chr = l->op.chr;
    int want = l->op.want_done;
    memcpy(addr, l->addr, 6);
    clear_op(l);
    if (kind == OP_READ) emit(addr, PEX_BT_READ, chr, data, len, status);
    else if (kind == OP_WRITE && want) emit(addr, PEX_BT_WRITE, chr, NULL, 0, status);
}

static int issue_op(link_t *l) {
    void *arg = (void *)(intptr_t)link_index(l);
    int rc = BLE_HS_EINVAL;
    if (l->op.chr < 0 || l->op.chr >= l->nchr) return BLE_HS_ENOENT;
    db_chr_t *c = &l->chr[l->op.chr];
    if (l->op.kind == OP_READ) {
        rc = ble_gattc_read(l->handle, c->val_h, on_gatt, arg);
    } else if (l->op.kind == OP_WRITE) {
        if (l->op.with_rsp)
            rc = ble_gattc_write_flat(l->handle, c->val_h, l->op.data, l->op.len, on_gatt, arg);
        else
            rc = ble_gattc_write_no_rsp_flat(l->handle, c->val_h, l->op.data, l->op.len);
    } else if (l->op.kind == OP_SUB) {
        if (!c->cccd) return BLE_HS_ENOENT;
        uint8_t v[2] = { l->op.mode == 1 ? 0x01 : l->op.mode == 2 ? 0x02 : 0x00, 0 };
        rc = ble_gattc_write_flat(l->handle, c->cccd, v, 2, on_gatt, arg);
    }
    return rc;
}

static int on_gatt(uint16_t conn, const struct ble_gatt_error *err, struct ble_gatt_attr *attr, void *arg) {
    (void)conn;
    link_t *l = &s_link[(int)(intptr_t)arg];
    if (!l->used || l->op.kind == OP_NONE) return 0;
    if (err->status == BLE_HS_EDONE) {
        if (!l->op.reported) complete_op(l, 0, NULL, 0);
        else clear_op(l);
        return 0;
    }
    if (err->status == 0) {
        uint8_t buf[QDATA];
        uint16_t n = 0;
        if (l->op.kind == OP_READ && attr && attr->om)
            ble_hs_mbuf_to_flat(attr->om, buf, sizeof buf, &n);
        l->op.reported = 1;
        complete_op(l, 0, buf, n);
        return 0;
    }
    int att = att_of((uint16_t)err->status);
    if (wants_sec(att) && s_policy == POL_ON_DEMAND && !l->op.retried) {
        l->op.retried = 1;
        l->wait_sec = 1;
        if (ble_gap_security_initiate(l->handle) == 0) return 0;
        l->wait_sec = 0;
    }
    int st = att ? att : (err->status & 0xff) << 8;
    complete_op(l, st, NULL, 0);
    return 0;
}

static void note_adv(const struct ble_gap_disc_desc *d) {
    uint8_t disp[6];
    addr_from_le(disp, d->addr.val);
    struct ble_hs_adv_fields f;
    memset(&f, 0, sizeof f);
    if (d->length_data && d->data) ble_hs_adv_parse_fields(&f, d->data, d->length_data);
    taskENTER_CRITICAL(&s_mux);
    scan_t *s = NULL;
    for (int i = 0; i < s_nscan; i++) {
        if (addr_is(s_scan[i].addr, disp)) { s = &s_scan[i]; break; }
    }
    int rsp = d->event_type == BLE_HCI_ADV_RPT_EVTYPE_SCAN_RSP;
    if (!s) {
        if (s_nscan >= NSCAN || rsp) { taskEXIT_CRITICAL(&s_mux); return; }
        s = &s_scan[s_nscan++];
        memset(s, 0, sizeof *s);
        memcpy(s->addr, disp, 6);
        s->used = 1;
        s->addr_type = d->addr.type;
    }
    s->rssi = d->rssi;
    s->last_ms = pex_port_ms();
    if (!rsp) {
        s->connectable = d->event_type == BLE_HCI_ADV_RPT_EVTYPE_ADV_IND ||
                         d->event_type == BLE_HCI_ADV_RPT_EVTYPE_DIR_IND;
        s->addr_type = d->addr.type;
    }
    if (f.name && f.name_len) {
        size_t n = f.name_len > 30 ? 30 : f.name_len;
        memcpy(s->name, f.name, n);
        s->name[n] = 0;
    }
    if (f.num_uuids16) {
        s->n_uuid16 = f.num_uuids16 > NU16 ? NU16 : f.num_uuids16;
        for (int i = 0; i < s->n_uuid16; i++) s->uuid16[i] = f.uuids16[i].value;
    }
    if (f.num_uuids128) {
        s->n_uuid128 = f.num_uuids128 > NU128 ? NU128 : f.num_uuids128;
        for (int i = 0; i < s->n_uuid128; i++) {
            const uint8_t *v = f.uuids128[i].value;
            for (int b = 0; b < 16; b++) s->uuid128[i][b] = v[15 - b];
        }
    }
    if (f.mfg_data && f.mfg_data_len) {
        s->mfg_len = f.mfg_data_len > 31 ? 31 : f.mfg_data_len;
        memcpy(s->mfg, f.mfg_data, s->mfg_len);
    }
    taskEXIT_CRITICAL(&s_mux);
}

static void after_connect(link_t *l) {
    uint8_t addr[6];
    memcpy(addr, l->addr, 6);
    l->mtu = 23;
    emit(addr, PEX_BT_LINK_UP, -1, NULL, 0, 0);
    if (s_policy == POL_NEVER) {
        start_discover(l);
        return;
    }
    if (s_policy == POL_ALWAYS) {
        int rc = ble_gap_security_initiate(l->handle);
        if (rc == 0 || rc == BLE_HS_EALREADY) {
            if (rc == 0) { l->disc = DISC_SEC; return; }
        }
    }
    start_discover(l);
}

static int gap_event(struct ble_gap_event *ev, void *arg) {
    (void)arg;
    switch (ev->type) {
    case BLE_GAP_EVENT_DISC:
        note_adv(&ev->disc);
        return 0;
    case BLE_GAP_EVENT_DISC_COMPLETE:
        return 0;
    case BLE_GAP_EVENT_CONNECT: {
        link_t *l = link_connecting();
        if (ev->connect.status != 0) {
            if (l) {
                uint8_t addr[6];
                memcpy(addr, l->addr, 6);
                link_clear(l);
                emit(addr, PEX_BT_FAILED, -1, NULL, 0, ev->connect.status & 0xff);
            }
            return 0;
        }
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(ev->connect.conn_handle, &desc) != 0) {
            ble_gap_terminate(ev->connect.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            if (l) link_clear(l);
            return 0;
        }
        if (!l) {
            for (int i = 0; i < NLINK; i++) if (!s_link[i].used) { l = &s_link[i]; break; }
            if (!l) {
                ble_gap_terminate(ev->connect.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
                return 0;
            }
            memset(l, 0, sizeof *l);
            l->used = 1;
            addr_from_le(l->addr, desc.peer_id_addr.val);
            l->addr_type = desc.peer_id_addr.type;
        }
        l->connecting = 0;
        l->handle = ev->connect.conn_handle;
        l->mtu = ble_att_mtu(l->handle);
        after_connect(l);
        return 0;
    }
    case BLE_GAP_EVENT_DISCONNECT: {
        link_t *l = link_by_handle(ev->disconnect.conn.conn_handle);
        if (!l) return 0;
        uint8_t addr[6];
        memcpy(addr, l->addr, 6);
        int up = !l->connecting;
        link_clear(l);
        if (up) emit(addr, PEX_BT_LINK_DOWN, -1, NULL, 0, ev->disconnect.reason & 0xff);
        else emit(addr, PEX_BT_FAILED, -1, NULL, 0, ev->disconnect.reason & 0xff);
        return 0;
    }
    case BLE_GAP_EVENT_MTU: {
        link_t *l = link_by_handle(ev->mtu.conn_handle);
        if (l && ev->mtu.value >= 23) l->mtu = ev->mtu.value;
        return 0;
    }
    case BLE_GAP_EVENT_NOTIFY_RX: {
        link_t *l = link_by_handle(ev->notify_rx.conn_handle);
        if (!l) return 0;
        int chr = -1;
        for (int i = 0; i < l->nchr; i++) {
            if (l->chr[i].val_h == ev->notify_rx.attr_handle) { chr = i; break; }
        }
        uint8_t buf[QDATA];
        uint16_t n = 0;
        if (ev->notify_rx.om) ble_hs_mbuf_to_flat(ev->notify_rx.om, buf, sizeof buf, &n);
        emit(l->addr, ev->notify_rx.indication ? PEX_BT_INDICATE : PEX_BT_NOTIFY, chr, buf, n, 0);
        return 0;
    }
    case BLE_GAP_EVENT_ENC_CHANGE: {
        link_t *l = link_by_handle(ev->enc_change.conn_handle);
        if (!l) return 0;
        if (l->disc == DISC_SEC) {
            if (ev->enc_change.status == 0) start_discover(l);
            else fail_link(l);
        }
        if (l->wait_sec) {
            l->wait_sec = 0;
            if (ev->enc_change.status == 0) {
                int rc = issue_op(l);
                if (rc) complete_op(l, (rc & 0xff) << 8, NULL, 0);
            } else {
                complete_op(l, (ev->enc_change.status & 0xff) << 8, NULL, 0);
            }
        }
        /* A stored LTK encrypts without a pairing complete event. A missing
         * peer key fails here; the poll deletes the bond and pairs again. */
        if (l->had_bond && !l->asked) {
            if (ev->enc_change.status == 0) {
                l->had_bond = 0;
                emit(l->addr, PEX_BT_BONDED, -1, NULL, 0, 0);
            } else {
                l->rekey = 1;
            }
        }
        return 0;
    }
    case BLE_GAP_EVENT_PASSKEY_ACTION: {
        link_t *l = link_by_handle(ev->passkey.conn_handle);
        if (!l) return 0;
        l->asked = 1;
        l->io_action = ev->passkey.params.action;
        l->io_numcmp = ev->passkey.params.numcmp;
        if (s_policy == POL_NEVER) {
            l->refuse = 1;
            emit(l->addr, PEX_BT_PAIR_FAIL, -1, NULL, 0, 0);
            return 0;
        }
        {
            int kind = 0;
            if (l->io_action == BLE_SM_IOACT_INPUT) kind = PEX_BT_PASSKEY;
            else if (l->io_action == BLE_SM_IOACT_DISP) kind = PEX_BT_PASSKEY_DISP;
            else if (l->io_action == BLE_SM_IOACT_NUMCMP) kind = PEX_BT_CONFIRM;
            if (kind) emit(l->addr, kind, -1, NULL, 0, 0);
        }
        return 0;
    }
    case BLE_GAP_EVENT_PARING_COMPLETE: {
        link_t *l = link_by_handle(ev->pairing_complete.conn_handle);
        if (!l) return 0;
        if (l->wait_sec && ev->pairing_complete.status != 0) {
            l->wait_sec = 0;
            complete_op(l, (ev->pairing_complete.status & 0xff) << 8, NULL, 0);
        }
        if (ev->pairing_complete.status == 0 && s_policy == POL_NEVER) {
            l->drop = 1;
            emit(l->addr, PEX_BT_PAIR_FAIL, -1, NULL, 0, 0);
        } else if (ev->pairing_complete.status == 0) {
            /* NimBLE completes Just Works without a passkey action. */
            if (!l->asked) emit(l->addr, PEX_BT_JUST_WORKS, -1, NULL, 0, 0);
            emit(l->addr, PEX_BT_BONDED, -1, NULL, 0, 0);
        } else {
            emit(l->addr, PEX_BT_PAIR_FAIL, -1, NULL, 0, ev->pairing_complete.status & 0xff);
        }
        l->asked = 0;
        l->io_action = 0;
        return 0;
    }
    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        if (s_policy == POL_NEVER) return BLE_GAP_REPEAT_PAIRING_IGNORE;
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(ev->repeat_pairing.conn_handle, &desc) == 0) {
            s_store_pass = 1;
            ble_store_util_delete_peer(&desc.peer_id_addr);
            s_store_pass = 0;
        }
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }
    default:
        return 0;
    }
}

static int adv_apply(void) {
    if (!s_synced) return PEX_OK;
    if (ble_gap_adv_active()) ble_gap_adv_stop();
    if (s_adv_len) {
        int rc = ble_gap_adv_set_data(s_adv, s_adv_len);
        if (rc) return nim_rc(rc);
    }
    if (s_rsp_len) {
        int rc = ble_gap_adv_rsp_set_data(s_rsp, s_rsp_len);
        if (rc) return nim_rc(rc);
    }
    struct ble_gap_adv_params p;
    memset(&p, 0, sizeof p);
    p.conn_mode = BLE_GAP_CONN_MODE_UND;
    p.disc_mode = BLE_GAP_DISC_MODE_GEN;
    p.itvl_min = s_imin;
    p.itvl_max = s_imax;
    int rc = ble_gap_adv_start(s_own ? s_own : BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER, &p, gap_event, NULL);
    return rc == 0 || rc == BLE_HS_EALREADY ? PEX_OK : nim_rc(rc);
}

static void on_sync(void) {
    if (s_rnd_valid) {
        if (ble_hs_id_set_rnd(s_rnd_le) == 0) s_own = BLE_OWN_ADDR_RANDOM;
    } else {
        ble_hs_util_ensure_addr(0);
        ble_hs_id_infer_auto(0, &s_own);
    }
    s_synced = 1;
    if (s_adv_want) adv_apply();
}
static void on_reset(int reason) {
    s_synced = 0;
    ESP_LOGW(TAG, "host reset reason=%d", reason);
}

static int store_read(int obj_type, const union ble_store_key *key, union ble_store_value *val) {
    int pad = s_policy == POL_NEVER && !s_store_pass &&
              (obj_type == BLE_STORE_OBJ_TYPE_OUR_SEC || obj_type == BLE_STORE_OBJ_TYPE_PEER_SEC) &&
              ble_addr_cmp(&key->sec.peer_addr, BLE_ADDR_ANY) == 0;
    int rc = s_orig_read ? s_orig_read(obj_type, key, val) : BLE_HS_ENOENT;
    if (pad && rc == BLE_HS_ENOENT && key->sec.idx < CONFIG_BT_NIMBLE_MAX_BONDS) {
        memset(val, 0, sizeof *val);
        return 0;
    }
    return rc;
}
static int store_status(struct ble_store_status_event *event, void *arg) {
    if (s_policy == POL_NEVER &&
        (event->event_code == BLE_STORE_EVENT_FULL || event->event_code == BLE_STORE_EVENT_OVERFLOW)) {
        return BLE_HS_ENOMEM;
    }
    if (s_orig_status) return s_orig_status(event, arg);
    return ble_store_util_status_rr(event, arg);
}

static void apply_sm(void) {
    ble_hs_cfg.sm_io_cap = s_io;
    ble_hs_cfg.sm_mitm = s_mitm ? 1 : 0;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_sc_only = 0;
    /* 1 rejects a peripheral Pairing Request before Just Works can run.
     * 2 and above drop notifications from an unencrypted peer. */
    ble_hs_cfg.sm_sec_lvl = s_policy == POL_NEVER ? 1 : 0;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
}

static int access_cb(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)attr;
    unsigned ix = (unsigned)(uintptr_t)arg;
    unsigned s = ix >> 8, c = ix & 0xff;
    if (s >= SVC_MAX || c >= CHR_MAX) return BLE_ATT_ERR_UNLIKELY;
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        if (ctxt->offset > s_chr_len[s][c]) return BLE_ATT_ERR_INVALID_OFFSET;
        uint16_t n = (uint16_t)(s_chr_len[s][c] - ctxt->offset);
        if (n && os_mbuf_append(ctxt->om, s_chr_val[s][c] + ctxt->offset, n) != 0)
            return BLE_ATT_ERR_INSUFFICIENT_RES;
        return 0;
    }
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return 0;
    if (ctxt->offset) return BLE_ATT_ERR_INVALID_OFFSET;
    uint8_t buf[512];
    uint16_t n = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof buf, &n) != 0)
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    if (!s_chr_smp[s][c]) {
        if (n > GATT_VAL) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        memcpy(s_chr_val[s][c], buf, n);
        s_chr_len[s][c] = n;
    } else {
        taskENTER_CRITICAL(&s_mux);
        if (s_smp.pending) { taskEXIT_CRITICAL(&s_mux); return BLE_ATT_ERR_UNLIKELY; }
        memcpy(s_smp.req, buf, n);
        s_smp.len = n;
        s_smp.conn = conn;
        s_smp.handle = s_chr_handle[s][c];
        s_smp.pending = 1;
        taskEXIT_CRITICAL(&s_mux);
    }
    {
        link_t *l = link_by_handle(conn);
        uint8_t addr[6];
        memset(addr, 0, sizeof addr);
        if (l) memcpy(addr, l->addr, 6);
        uint16_t elen = n > QDATA ? QDATA : n;
        emit(addr, PEX_BT_LOCAL_WRITE, s_chr_ix[s][c], buf, elen, 0);
    }
    return 0;
}

static void reserve_gatt(void) {
    static ble_uuid16_t su[SVC_MAX];
    static ble_uuid16_t cu[SVC_MAX][CHR_MAX];
    static struct ble_gatt_chr_def cd[SVC_MAX][CHR_MAX + 1];
    static struct ble_gatt_svc_def sd[SVC_MAX + 1];
    memset(sd, 0, sizeof sd);
    memset(cd, 0, sizeof cd);
    for (int i = 0; i < SVC_MAX; i++) {
        su[i] = (ble_uuid16_t)BLE_UUID16_INIT((uint16_t)(0xFFF0 + i));
        for (int j = 0; j < CHR_MAX; j++) {
            cu[i][j] = (ble_uuid16_t)BLE_UUID16_INIT((uint16_t)(0xFF00 + i * 16 + j));
            cd[i][j].uuid = &cu[i][j].u;
            cd[i][j].flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE |
                             BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_NOTIFY;
        }
        sd[i].type = BLE_GATT_SVC_TYPE_PRIMARY;
        sd[i].uuid = &su[i].u;
        sd[i].characteristics = cd[i];
    }
    int rc = ble_gatts_count_cfg(sd);
    if (rc) ESP_LOGW(TAG, "gatt reserve %d", rc);
}

static void gatt_delete_live(void) {
    for (int i = 0; i < s_nsvc; i++) ble_gatts_delete_svc(&s_svc_uuid[i].u);
    s_nsvc = 0;
    memset(s_svc, 0, sizeof s_svc);
}

static int gatt_install(void) {
    gatt_delete_live();
    if (!s_synced) return PEX_ESTATE;
    memset(s_svc, 0, sizeof s_svc);
    memset(s_chr, 0, sizeof s_chr);
    memset(s_chr_smp, 0, sizeof s_chr_smp);
    memset(s_chr_handle, 0, sizeof s_chr_handle);
    memset(s_chr_len, 0, sizeof s_chr_len);
    memset(s_chr_val, 0, sizeof s_chr_val);
    int flat = 0;
    for (int i = 0; i < s_bnsvc; i++) {
        s_svc_uuid[i] = s_b[i].uuid;
        s_svc[i].type = BLE_GATT_SVC_TYPE_PRIMARY;
        s_svc[i].uuid = &s_svc_uuid[i].u;
        s_svc[i].characteristics = s_chr[i];
        for (int j = 0; j < s_b[i].nchr; j++) {
            s_chr_uuid[i][j] = s_b[i].chr[j].uuid;
            s_chr_smp[i][j] = (uint8_t)s_b[i].chr[j].smp;
            s_chr_len[i][j] = s_b[i].chr[j].vlen;
            if (s_chr_len[i][j]) memcpy(s_chr_val[i][j], s_b[i].chr[j].val, s_chr_len[i][j]);
            s_chr_ix[i][j] = (int16_t)flat++;
            s_chr[i][j].uuid = &s_chr_uuid[i][j].u;
            s_chr[i][j].access_cb = access_cb;
            s_chr[i][j].arg = (void *)(uintptr_t)((i << 8) | j);
            s_chr[i][j].flags = s_b[i].chr[j].flags & 0xff;
            s_chr[i][j].val_handle = &s_chr_handle[i][j];
        }
    }
    s_nsvc = s_bnsvc;
    if (s_nsvc == 0) return PEX_OK;
    int rc = ble_gatts_add_dynamic_svcs(s_svc);
    if (rc) {
        ESP_LOGW(TAG, "gatt add %d", rc);
        gatt_delete_live();
        return rc == BLE_HS_ENOMEM ? PEX_ENOMEM : PEX_EINVAL;
    }
    return PEX_OK;
}

static void host_task(void *arg) {
    (void)arg;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static const struct { const char *name; uint8_t cap; } IO_NAMES[] = {
    { "no_input_no_output", BLE_HS_IO_NO_INPUT_OUTPUT },
    { "display_only",       BLE_HS_IO_DISPLAY_ONLY },
    { "display_yes_no",     BLE_HS_IO_DISPLAY_YESNO },
    { "keyboard_only",      BLE_HS_IO_KEYBOARD_ONLY },
    { "keyboard_display",   BLE_HS_IO_KEYBOARD_DISPLAY },
};

void ble_pex_start(void) {
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init %d", (int)err);
        return;
    }
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    apply_sm();
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set("btgw");
    reserve_gatt();
    ble_store_config_init();
    s_orig_read = ble_hs_cfg.store_read_cb;
    s_orig_status = ble_hs_cfg.store_status_cb;
    ble_hs_cfg.store_read_cb = store_read;
    ble_hs_cfg.store_status_cb = store_status;
    apply_sm();
    nimble_port_freertos_init(host_task);
    ESP_LOGI(TAG, "NimBLE central and peripheral");
}

void ble_pex_poll(void) {
    /* The poll task has a few kilobytes of stack. These buffers used to sit
     * on that stack. launch.now can re-enter from the HTTP task; skip then. */
    static int busy;
    static uint8_t req[512], rsp[512], data[QDATA];
    static qent_t copy;
    static listener_t snap[LISTENERS];
    if (busy) return;
    busy = 1;
    for (int i = 0; i < NLINK; i++) {
        link_t *l = &s_link[i];
        if (!l->used) continue;
        if (l->rekey) {
            l->rekey = 0;
            l->had_bond = 0;
            ble_addr_t a;
            a.type = l->addr_type;
            addr_to_le(a.val, l->addr);
            s_store_pass = 1;
            ble_store_util_delete_peer(&a);
            s_store_pass = 0;
            if (s_policy != POL_NEVER && !l->connecting) {
                int rc = ble_gap_security_initiate(l->handle);
                if (rc && rc != BLE_HS_EALREADY)
                    emit(l->addr, PEX_BT_PAIR_FAIL, -1, NULL, 0, rc & 0xff);
            }
        }
        if (l->refuse) {
            l->refuse = 0;
            if (!l->connecting) ble_gap_terminate(l->handle, BLE_ERR_REM_USER_CONN_TERM);
        }
        if (l->drop) {
            l->drop = 0;
            ble_addr_t a;
            a.type = l->addr_type;
            addr_to_le(a.val, l->addr);
            s_store_pass = 1;
            ble_gap_unpair(&a);
            s_store_pass = 0;
            if (!l->connecting) ble_gap_terminate(l->handle, BLE_ERR_REM_USER_CONN_TERM);
        }
    }
    if (s_adv_want && s_synced && !ble_gap_adv_active()) adv_apply();

    int pend = 0;
    uint16_t conn = 0, handle = 0, len = 0;
    taskENTER_CRITICAL(&s_mux);
    if (s_smp.pending) {
        pend = 1;
        conn = s_smp.conn;
        handle = s_smp.handle;
        len = s_smp.len;
        if (len > sizeof req) len = sizeof req;
        memcpy(req, s_smp.req, len);
        s_smp.pending = 0;
    }
    taskEXIT_CRITICAL(&s_mux);
    if (pend && handle) {
        size_t n = smp_process(req, len, rsp, sizeof rsp);
        if (n > 0 && n <= 0xffff) {
            struct os_mbuf *om = ble_hs_mbuf_from_flat(rsp, (uint16_t)n);
            if (om && ble_gatts_notify_custom(conn, handle, om) != 0) {
                ESP_LOGW(TAG, "SMP notify failed");
            }
        }
    }

    for (;;) {
        taskENTER_CRITICAL(&s_mux);
        if (!s_q || s_qtail == s_qhead) { taskEXIT_CRITICAL(&s_mux); break; }
        copy = s_q[s_qtail++ % QDEPTH];
        memcpy(snap, s_l, sizeof snap);
        taskEXIT_CRITICAL(&s_mux);
        unsigned ncopy = copy.ev.len > QDATA ? QDATA : copy.ev.len;
        memcpy(data, copy.data, ncopy);
        pex_bt_event_t ev = copy.ev;
        ev.data = data;
        static const uint8_t zero[6];
        for (int i = 0; i < LISTENERS; i++) {
            if (!snap[i].fn) continue;
            if (!snap[i].any && memcmp(snap[i].addr, ev.addr, 6) != 0 && memcmp(ev.addr, zero, 6) != 0) continue;
            /* pex_self() is NULL on the poll task, so a host listener has no
             * module. pex_call rejects that. Call the function directly. */
            if (!snap[i].mod)
                snap[i].fn(snap[i].arg, &ev);
            else
                pex_call(snap[i].mod, (uint32_t)(uintptr_t)snap[i].fn,
                         (uint32_t)(uintptr_t)snap[i].arg, (uint32_t)(uintptr_t)&ev, 0, 0);
        }
    }
    busy = 0;
}

void ble_pex_unload(pex_mod_t *m) {
    taskENTER_CRITICAL(&s_mux);
    for (int i = 0; i < LISTENERS; i++) {
        if (s_l[i].fn && s_l[i].mod == m) memset(&s_l[i], 0, sizeof s_l[i]);
    }
    taskEXIT_CRITICAL(&s_mux);
}

int pex_bt_listen(const uint8_t *addr, pex_bt_fn fn, void *arg) {
    if (!fn) return PEX_EINVAL;
    taskENTER_CRITICAL(&s_mux);
    int id = PEX_ENOMEM;
    for (int i = 0; i < LISTENERS; i++) {
        if (!s_l[i].fn) {
            s_l[i].id = s_next++;
            s_l[i].mod = pex_self();
            s_l[i].fn = fn;
            s_l[i].arg = arg;
            s_l[i].any = addr == NULL;
            if (addr) memcpy(s_l[i].addr, addr, 6);
            id = s_l[i].id;
            break;
        }
    }
    taskEXIT_CRITICAL(&s_mux);
    return id;
}

int pex_bt_unlisten(int id) {
    int rc = PEX_ENOENT;
    taskENTER_CRITICAL(&s_mux);
    for (int i = 0; i < LISTENERS; i++) {
        if (s_l[i].fn && s_l[i].id == id && s_l[i].mod == pex_self()) {
            memset(&s_l[i], 0, sizeof s_l[i]);
            rc = PEX_OK;
            break;
        }
    }
    taskEXIT_CRITICAL(&s_mux);
    return rc;
}

int pex_bt_scan(uint32_t duration_ms, int active) {
    if (!s_synced) return PEX_ESTATE;
    if (ble_gap_disc_active()) return PEX_OK;
    struct ble_gap_disc_params p;
    memset(&p, 0, sizeof p);
    p.itvl = BLE_GAP_SCAN_FAST_INTERVAL_MIN;
    p.window = BLE_GAP_SCAN_FAST_WINDOW;
    p.passive = active ? 0 : 1;
    p.filter_duplicates = 0;
    int32_t dur = duration_ms == 0 ? BLE_HS_FOREVER : (int32_t)duration_ms;
    uint8_t own = s_own ? s_own : BLE_OWN_ADDR_PUBLIC;
    return nim_rc(ble_gap_disc(own, dur, &p, gap_event, NULL));
}
void pex_bt_scan_stop(void) {
    if (s_synced && ble_gap_disc_active()) ble_gap_disc_cancel();
}
int pex_bt_scanning(void) { return s_synced && ble_gap_disc_active() ? 1 : 0; }

/* This port has no scan scheduler yet. A low-priority request scans the way
 * pex_bt_scan(0) does, and quiet slots are not offered. */
int pex_bt_scan_periodic(uint32_t burst_ms, uint32_t period_ms, int active) {
    (void)burst_ms; (void)period_ms;
    return pex_bt_scan(0, active);
}
int pex_bt_scan_state(void) { return pex_bt_scanning() ? PEX_BT_SCAN_ON : PEX_BT_SCAN_NONE; }
int pex_bt_quiet(uint32_t ms) { (void)ms; return PEX_ENOTSUP; }
int pex_bt_quiet_extend(int id, uint32_t ms) { (void)id; (void)ms; return PEX_ENOENT; }
int pex_bt_quiet_release(int id) { (void)id; return PEX_ENOENT; }
int pex_bt_quiet_left(int id) { (void)id; return PEX_ENOENT; }

int pex_bt_devices(pex_bt_dev_t *out, uint32_t max, const char *name_prefix, const char *uuid128) {
    if (max && !out) return PEX_EINVAL;
    uint32_t now = pex_port_ms();
    size_t pl = name_prefix ? strlen(name_prefix) : 0;
    uint32_t n = 0;
    taskENTER_CRITICAL(&s_mux);
    for (int i = 0; i < s_nscan && n < max; i++) {
        scan_t *d = &s_scan[i];
        int rules = pl || (uuid128 && *uuid128);
        int match = !rules;
        if (pl && d->name[0] && strncmp(d->name, name_prefix, pl) == 0) match = 1;
        if (!match && uuid128 && *uuid128) {
            char u[40];
            for (int j = 0; j < d->n_uuid128 && !match; j++) {
                const uint8_t *b = d->uuid128[j];
                snprintf(u, sizeof u,
                         "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                         b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
                         b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
                if (strcasecmp(u, uuid128) == 0) match = 1;
            }
        }
        if (!match) continue;
        pex_bt_dev_t *o = &out[n++];
        memset(o, 0, sizeof *o);
        memcpy(o->addr, d->addr, 6);
        o->addr_type = d->addr_type;
        o->rssi = d->rssi;
        snprintf(o->name, sizeof o->name, "%s", d->name);
        o->connectable = d->connectable;
        o->n_uuid16 = d->n_uuid16;
        memcpy(o->uuid16, d->uuid16, sizeof(uint16_t) * d->n_uuid16);
        o->mfg_len = d->mfg_len;
        memcpy(o->mfg, d->mfg, d->mfg_len);
        o->age_ms = now - d->last_ms;
        link_t *l = link_by_addr(d->addr);
        o->connected = l && !l->connecting;
    }
    taskEXIT_CRITICAL(&s_mux);
    return (int)n;
}

int pex_bt_uuids128(const uint8_t *addr, char (*out)[37], int max) {
    if (!addr) return PEX_EINVAL;
    taskENTER_CRITICAL(&s_mux);
    scan_t *d = NULL;
    for (int i = 0; i < s_nscan; i++) if (addr_is(s_scan[i].addr, addr)) { d = &s_scan[i]; break; }
    if (!d) { taskEXIT_CRITICAL(&s_mux); return PEX_ENOENT; }
    int n = d->n_uuid128;
    if (max < 0) max = 0;
    if (n > max) n = max;
    for (int i = 0; i < n && out; i++) {
        const uint8_t *b = d->uuid128[i];
        snprintf(out[i], 37,
                 "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                 b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
                 b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
    }
    int total = d->n_uuid128;
    taskEXIT_CRITICAL(&s_mux);
    (void)total;
    return n;
}

int pex_bt_connect(const uint8_t *addr) {
    if (!addr) return PEX_EINVAL;
    if (!s_synced) return PEX_ESTATE;
    link_t *have = link_by_addr(addr);
    if (have && !have->connecting) return PEX_OK;
    if (any_busy()) return PEX_EBUSY;
    uint8_t at = 0xff;
    taskENTER_CRITICAL(&s_mux);
    for (int i = 0; i < s_nscan; i++) {
        if (addr_is(s_scan[i].addr, addr)) { at = s_scan[i].addr_type; break; }
    }
    taskEXIT_CRITICAL(&s_mux);
    if (at == 0xff) return PEX_ENOENT;
    if (ble_gap_disc_active()) ble_gap_disc_cancel();
    link_t *l = NULL;
    for (int i = 0; i < NLINK; i++) if (!s_link[i].used) { l = &s_link[i]; break; }
    if (!l) return PEX_EBUSY;
    memset(l, 0, sizeof *l);
    l->used = 1;
    l->connecting = 1;
    memcpy(l->addr, addr, 6);
    l->addr_type = at;
    ble_addr_t peer;
    peer.type = at;
    addr_to_le(peer.val, addr);
    struct ble_gap_conn_params cp;
    memset(&cp, 0, sizeof cp);
    cp.scan_itvl = BLE_GAP_SCAN_FAST_INTERVAL_MIN;
    cp.scan_window = BLE_GAP_SCAN_FAST_WINDOW;
    cp.itvl_min = BLE_GAP_INITIAL_CONN_ITVL_MIN;
    cp.itvl_max = BLE_GAP_INITIAL_CONN_ITVL_MAX;
    cp.latency = BLE_GAP_INITIAL_CONN_LATENCY;
    cp.supervision_timeout = BLE_GAP_INITIAL_SUPERVISION_TIMEOUT;
    cp.min_ce_len = BLE_GAP_INITIAL_CONN_MIN_CE_LEN;
    cp.max_ce_len = BLE_GAP_INITIAL_CONN_MAX_CE_LEN;
    uint8_t own = s_own ? s_own : BLE_OWN_ADDR_PUBLIC;
    int rc = ble_gap_connect(own, &peer, BLE_HS_FOREVER, &cp, gap_event, NULL);
    if (rc) { link_clear(l); return nim_rc(rc); }
    return PEX_EPENDING;
}

int pex_bt_disconnect(const uint8_t *addr) {
    link_t *l = link_by_addr(addr);
    if (!l) return PEX_ENOENT;
    if (l->connecting) {
        int rc = ble_gap_conn_cancel();
        return rc == 0 || rc == BLE_HS_EALREADY ? PEX_OK : nim_rc(rc);
    }
    int rc = ble_gap_terminate(l->handle, BLE_ERR_REM_USER_CONN_TERM);
    return rc == 0 || rc == BLE_HS_EALREADY ? PEX_OK : nim_rc(rc);
}

int pex_bt_state(const uint8_t *addr, int *ready) {
    link_t *l = link_by_addr(addr);
    if (ready) *ready = l && l->ready;
    return l && !l->connecting ? PEX_BT_CONNECTED : PEX_BT_DISCONNECTED;
}
int pex_bt_mtu(const uint8_t *addr) {
    link_t *l = link_by_addr(addr);
    if (!l || l->connecting) return 0;
    return l->mtu ? l->mtu : (int)ble_att_mtu(l->handle);
}

int pex_bt_find(const uint8_t *addr, const char *svc, const char *chr, int instance) {
    link_t *l = link_by_addr(addr);
    if (!l || !l->ready) return PEX_ESTATE;
    if (!chr) return PEX_EINVAL;
    if (instance < 0) return PEX_EINVAL;
    char u[40];
    int seen = 0;
    for (int si = 0; si < l->nsvc; si++) {
        uuid_pex(&l->svc[si].uuid.u, u, sizeof u);
        if (svc && *svc && strcasecmp(u, svc) != 0) continue;
        for (int k = 0; k < l->svc[si].nchr; k++) {
            int ci = l->svc[si].first + k;
            uuid_pex(&l->chr[ci].uuid.u, u, sizeof u);
            if (strcasecmp(u, chr) != 0) continue;
            if (seen++ < instance) continue;
            return ci;
        }
    }
    return PEX_ENOENT;
}
int pex_bt_props(const uint8_t *addr, int chr) {
    link_t *l = link_by_addr(addr);
    if (!l || !l->ready || chr < 0 || chr >= l->nchr) return PEX_ENOENT;
    return l->chr[chr].props;
}

static int queue_op(const uint8_t *addr, int chr, int kind, const void *data, uint16_t len, int with_rsp, int want, int mode) {
    link_t *l = link_by_addr(addr);
    if (!l || l->connecting) return PEX_ESTATE;
    if (!l->ready) return PEX_ESTATE;
    if (chr < 0 || chr >= l->nchr) return PEX_ENOENT;
    if (l->op.kind != OP_NONE || l->wait_sec) return PEX_EBUSY;
    memset(&l->op, 0, sizeof l->op);
    l->op.kind = kind;
    l->op.chr = chr;
    l->op.with_rsp = with_rsp;
    l->op.want_done = want;
    l->op.mode = (uint8_t)mode;
    l->op.len = len;
    if (data && len) memcpy(l->op.data, data, len);
    int rc = issue_op(l);
    if (rc) { clear_op(l); return nim_rc(rc); }
    if (kind == OP_WRITE && !with_rsp) {
        int w = want;
        int c = chr;
        uint8_t a[6];
        memcpy(a, l->addr, 6);
        clear_op(l);
        if (w) emit(a, PEX_BT_WRITE, c, NULL, 0, 0);
    }
    return PEX_OK;
}

int pex_bt_read(const uint8_t *addr, int chr) {
    if (chr < 0 || chr > 255) return PEX_EINVAL;
    return queue_op(addr, chr, OP_READ, NULL, 0, 0, 0, 0);
}
int pex_bt_write(const uint8_t *addr, int chr, const void *data, size_t len, int with_response, int want_done) {
    if (chr < 0 || chr > 255 || len > 0xffff) return PEX_EINVAL;
    if (len && !data) return PEX_EINVAL;
    if (len > QDATA) return PEX_EINVAL;
    int mtu = pex_bt_mtu(addr);
    if (mtu < 23) mtu = 23;
    if (len > (size_t)(mtu - 3)) return PEX_EINVAL;
    return queue_op(addr, chr, OP_WRITE, data, (uint16_t)len, with_response != 0, want_done != 0, 0);
}
int pex_bt_subscribe(const uint8_t *addr, int chr, int mode) {
    if (chr < 0 || chr > 255 || mode < 0 || mode > 2) return PEX_EINVAL;
    return queue_op(addr, chr, OP_SUB, NULL, 0, 1, 0, mode);
}

int pex_gatt_begin(void) {
    s_building = 1;
    s_bnsvc = 0;
    s_gatt_have = 0;
    s_gatt_len = 0;
    memset(s_b, 0, sizeof s_b);
    return PEX_OK;
}
int pex_gatt_set(const uint8_t *data, uint16_t len) {
    if (len && !data) return PEX_EINVAL;
    if (len > GATT_VAL) return PEX_EINVAL;
    if (len) memcpy(s_gatt_bytes, data, len);
    s_gatt_len = len;
    s_gatt_have = 1;
    return PEX_OK;
}
int pex_gatt_service(const char *uuid) {
    if (!s_building) return PEX_EINVAL;
    if (s_bnsvc >= SVC_MAX) return PEX_ENOMEM;
    if (parse_uuid(uuid, &s_b[s_bnsvc].uuid)) return PEX_EINVAL;
    s_b[s_bnsvc].nchr = 0;
    s_bnsvc++;
    return PEX_OK;
}
int pex_gatt_characteristic(const char *uuid, int properties) {
    if (!s_building || s_bnsvc == 0) return PEX_EINVAL;
    if (properties < 0 || properties > 0xffff) return PEX_EINVAL;
    int n = s_b[s_bnsvc - 1].nchr;
    if (n >= CHR_MAX) return PEX_ENOMEM;
    bchr_t *cslot = &s_b[s_bnsvc - 1].chr[n];
    if (parse_uuid(uuid, &cslot->uuid)) return PEX_EINVAL;
    cslot->flags = (uint32_t)properties;
    cslot->smp = smp_uuid(uuid);
    cslot->vlen = 0;
    if (s_gatt_have) {
        cslot->vlen = s_gatt_len;
        if (s_gatt_len) memcpy(cslot->val, s_gatt_bytes, s_gatt_len);
        s_gatt_have = 0;
    }
    s_b[s_bnsvc - 1].nchr = n + 1;
    return PEX_OK;
}
int pex_gatt_commit(void) {
    if (!s_building) return PEX_EINVAL;
    for (int i = 0; i < s_bnsvc; i++) if (s_b[i].nchr == 0) return PEX_EINVAL;
    if (!s_synced) return PEX_ESTATE;
    s_building = 0;
    return gatt_install();
}

int pex_adv_data(const uint8_t *adv, uint16_t adv_len, const uint8_t *scan_rsp, uint16_t scan_len) {
    if (adv_len > 31 || scan_len > 31) return PEX_EINVAL;
    if ((adv_len && !adv) || (scan_len && !scan_rsp)) return PEX_EINVAL;
    if (adv_len) memcpy(s_adv, adv, adv_len);
    if (scan_len) memcpy(s_rsp, scan_rsp, scan_len);
    s_adv_len = (uint8_t)adv_len;
    s_rsp_len = (uint8_t)scan_len;
    return PEX_OK;
}
int pex_adv_start(uint16_t interval_min, uint16_t interval_max) {
    uint16_t imin = interval_min ? interval_min : ADV_DEF_MIN;
    uint16_t imax = interval_max ? interval_max : ADV_DEF_MAX;
    if (imin < ADV_LO || imax > ADV_HI || imin > imax) return PEX_EINVAL;
    s_imin = imin;
    s_imax = imax;
    s_adv_want = 1;
    if (!s_synced) return PEX_OK;
    return adv_apply();
}
int pex_adv_stop(void) {
    s_adv_want = 0;
    if (s_synced && ble_gap_adv_active()) {
        int rc = ble_gap_adv_stop();
        if (rc && rc != BLE_HS_EALREADY) return nim_rc(rc);
    }
    return PEX_OK;
}

int pex_bt_set_io(const char *io_capability, int mitm) {
    if (io_capability) {
        size_t i = 0;
        for (; i < sizeof IO_NAMES / sizeof IO_NAMES[0]; i++) {
            if (strcmp(IO_NAMES[i].name, io_capability) == 0) break;
        }
        if (i == sizeof IO_NAMES / sizeof IO_NAMES[0]) return PEX_EINVAL;
        s_io = IO_NAMES[i].cap;
    }
    if (mitm >= 0) s_mitm = mitm != 0;
    apply_sm();
    return PEX_OK;
}
int pex_bt_set_policy(const char *policy) {
    if (!policy) return PEX_EINVAL;
    if (strcmp(policy, "never") == 0) s_policy = POL_NEVER;
    else if (strcmp(policy, "on_demand") == 0) s_policy = POL_ON_DEMAND;
    else if (strcmp(policy, "always") == 0) s_policy = POL_ALWAYS;
    else return PEX_EINVAL;
    apply_sm();
    return PEX_OK;
}

static link_t *connected(const uint8_t *addr) {
    link_t *l = link_by_addr(addr);
    return l && !l->connecting ? l : NULL;
}

int pex_bt_passkey(const uint8_t *addr, uint32_t passkey) {
    if (!addr) return PEX_EINVAL;
    if (passkey > 999999) return PEX_EINVAL;
    if (s_policy == POL_NEVER) return PEX_ESTATE;
    link_t *l = connected(addr);
    if (!l) return PEX_ENOENT;
    if (l->io_action != BLE_SM_IOACT_INPUT && l->io_action != BLE_SM_IOACT_DISP) return PEX_EINVAL;
    struct ble_sm_io io;
    memset(&io, 0, sizeof io);
    io.action = l->io_action;
    io.passkey = passkey;
    int rc = ble_sm_inject_io(l->handle, &io);
    if (rc == 0) l->io_action = 0;
    return rc == 0 ? PEX_OK : PEX_EINVAL;
}
int pex_bt_confirm(const uint8_t *addr, int accept) {
    if (!addr) return PEX_EINVAL;
    if (s_policy == POL_NEVER) return PEX_ESTATE;
    link_t *l = connected(addr);
    if (!l) return PEX_ENOENT;
    struct ble_sm_io io;
    memset(&io, 0, sizeof io);
    io.action = BLE_SM_IOACT_NUMCMP;
    io.numcmp_accept = accept ? 1 : 0;
    int rc = ble_sm_inject_io(l->handle, &io);
    if (rc == 0) l->io_action = 0;
    return rc == 0 ? PEX_OK : PEX_EIO;
}
int pex_bt_forget(const uint8_t *addr) {
    if (!s_synced) return PEX_ESTATE;
    ble_addr_t peers[CONFIG_BT_NIMBLE_MAX_BONDS];
    int n = 0;
    s_store_pass = 1;
    int rc = ble_store_util_bonded_peers(peers, &n, CONFIG_BT_NIMBLE_MAX_BONDS);
    s_store_pass = 0;
    if (rc) return PEX_EIO;
    int found = 0;
    for (int i = 0; i < n; i++) {
        uint8_t disp[6];
        addr_from_le(disp, peers[i].val);
        if (addr && !addr_is(disp, addr)) continue;
        found = 1;
        s_store_pass = 1;
        int ur = ble_gap_unpair(&peers[i]);
        if (ur == BLE_HS_EBUSY) ble_store_util_delete_peer(&peers[i]);
        s_store_pass = 0;
    }
    if (addr && !found) return PEX_ENOENT;
    return PEX_OK;
}
static int peer_bonded(const uint8_t *disp) {
    ble_addr_t peers[CONFIG_BT_NIMBLE_MAX_BONDS];
    int n = 0;
    s_store_pass = 1;
    int rc = ble_store_util_bonded_peers(peers, &n, CONFIG_BT_NIMBLE_MAX_BONDS);
    s_store_pass = 0;
    if (rc) return 0;
    for (int i = 0; i < n; i++) {
        uint8_t d[6];
        addr_from_le(d, peers[i].val);
        if (addr_is(d, disp)) return 1;
    }
    return 0;
}

int pex_bt_pair(const uint8_t *addr) {
    if (!addr) return PEX_EINVAL;
    if (s_policy == POL_NEVER) return PEX_ESTATE;
    link_t *l = connected(addr);
    if (!l) return PEX_ENOENT;
    l->had_bond = peer_bonded(addr);
    l->rekey = 0;
    int rc = ble_gap_security_initiate(l->handle);
    if (rc == 0 || rc == BLE_HS_EALREADY) return PEX_OK;
    return nim_rc(rc);
}

int pex_bt_set_addr(const uint8_t addr[6], int random) {
    if (!addr || (random != 0 && random != 1)) return PEX_EINVAL;
    if (!random) {
        /* Public identity is the eFuse MAC copied into the controller once
         * at init. esp_iface_mac_addr_set only updates the software table. */
        return PEX_ENOTSUP;
    }
    uint8_t disp[6], le[6];
    memcpy(disp, addr, 6);
    disp[0] = (uint8_t)((disp[0] & 0x3f) | 0xc0);
    addr_to_le(le, disp);
    int ones = popcount8(le[0]) + popcount8(le[1]) + popcount8(le[2]) +
               popcount8(le[3]) + popcount8(le[4]) + popcount8((uint8_t)(le[5] & 0x3f));
    if (ones == 0 || ones == 46) return PEX_EINVAL;
    memcpy(s_rnd_le, le, 6);
    s_rnd_valid = 1;
    if (!s_synced) return PEX_OK;
    int rc = ble_hs_id_set_rnd(le);
    if (rc) return PEX_EINVAL;
    s_own = BLE_OWN_ADDR_RANDOM;
    if (s_adv_want) adv_apply();
    return PEX_OK;
}

#endif
