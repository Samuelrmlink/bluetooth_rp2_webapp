/*
 * The LOCAL GATT database -- what a peer sees when it queries US.
 *
 * Easy to forget when writing a central, but a GATT connection is symmetric:
 * the peripheral can turn around and act as a client. Without an ATT database
 * those reads fail. BTstack's own client examples ship a .gatt with GAP
 * Service for exactly this reason (see example/gatt_device_information_query,
 * which calls att_server_init despite being a pure client).
 *
 * Built with att_db_util at RUNTIME rather than compile_gatt.py at build time.
 * That costs nothing here and buys the thing this project is for: a Lua script
 * will be able to declare services and characteristics without a reflash.
 */
#include <stdio.h>
#include <string.h>

#include "btstack.h"
#include "bt/le/gattserver.h"
#include "autoconf.h"
#if CONFIG_BT_PERIPHERAL && CONFIG_OTA
#include "ota/smp.h"
#endif

static char s_device_name[32];

#if CONFIG_BT_PERIPHERAL
/* MCUmgr SMP characteristic. Recorded when Lua or a PEX module adds it, and
 * when bt.smp does. Writes land in att_write_cb only if the property bit
 * ATT_PROPERTY_DYNAMIC is set, so that bit is forced on for this UUID. */
static uint16_t s_smp_value;
#if CONFIG_OTA
static uint8_t s_smp_rsp[512];
#endif

static int smp_chr_uuid(const char *uuid) {
    static const char k[] = "da2e7828-fbce-4e01-ae9e-261174997c48";
    if (!uuid) { return 0; }
    for (int i = 0; k[i]; i++) {
        char a = uuid[i];
        if (a >= 'A' && a <= 'Z') { a = (char)(a - 'A' + 'a'); }
        if (a != k[i]) { return 0; }
    }
    return uuid[sizeof k - 1] == 0;
}
#endif

static uint16_t att_read_cb(hci_con_handle_t c, uint16_t handle, uint16_t off,
                            uint8_t *buf, uint16_t sz) {
    (void)c; (void)handle; (void)off; (void)buf; (void)sz;
    return 0;   /* everything served so far is a static value in the db */
}

static int att_write_cb(hci_con_handle_t c, uint16_t handle, uint16_t mode,
                        uint16_t off, uint8_t *buf, uint16_t sz) {
    (void)mode;
#if CONFIG_BT_PERIPHERAL && CONFIG_OTA
    if (s_smp_value && handle == s_smp_value) {
        if (off != 0) { return ATT_ERROR_INVALID_OFFSET; }
        if (sz > sizeof s_smp_rsp) { return ATT_ERROR_INVALID_ATTRIBUTE_VALUE_LENGTH; }
        size_t n = smp_process(buf, sz, s_smp_rsp, sizeof s_smp_rsp);
        if (n > 0 && n <= 0xFFFF) {
            uint8_t st = att_server_notify(c, handle, s_smp_rsp, (uint16_t)n);
            if (st) {
                printf("bt: SMP notify failed 0x%02x (%u bytes)\n", st, (unsigned)n);
            }
        }
        return 0;
    }
#else
    (void)c; (void)handle; (void)off; (void)buf; (void)sz;
#endif
    return 0;
}

/* What the current database contains, for gatt_server.describe. att_db_util
 * cannot be read back, so we keep our own record as we build. */
#define SRV_MAX_ENTRIES 24
typedef struct { bool is_service; char uuid[40]; uint16_t props; uint16_t handle; } srv_entry_t;
static srv_entry_t entries[SRV_MAX_ENTRIES];
static uint8_t n_entries;
static bool building;

/* "180f" or the full dashed form; returns false if neither. */
static bool parse_uuid(const char *s, uint16_t *u16, uint8_t *u128) {
    size_t n = strlen(s);
    if (n == 4) {
        unsigned v = 0;
        if (sscanf(s, "%4x", &v) != 1) { return false; }
        *u16 = (uint16_t)v;
        return true;
    }
    if (n == 36) {
        unsigned b[16];
        int got = sscanf(s, "%2x%2x%2x%2x-%2x%2x-%2x%2x-%2x%2x-%2x%2x%2x%2x%2x%2x",
                         &b[0],&b[1],&b[2],&b[3],&b[4],&b[5],&b[6],&b[7],
                         &b[8],&b[9],&b[10],&b[11],&b[12],&b[13],&b[14],&b[15]);
        if (got != 16) { return false; }
        /* att_db_util wants it reversed -- little-endian on the wire. */
        for (int i = 0; i < 16; i++) { u128[i] = (uint8_t)b[15 - i]; }
        *u16 = 0;
        return true;
    }
    return false;
}

void gatt_server_begin(void) {
    att_db_util_init();
    n_entries = 0;
    building = true;
#if CONFIG_BT_PERIPHERAL
    s_smp_value = 0;
#endif
    /* GAP always comes first: a peer expects to be able to read our name, and
     * a script forgetting to declare it should not break that. */
    att_db_util_add_service_uuid16(ORG_BLUETOOTH_SERVICE_GENERIC_ACCESS);
    att_db_util_add_characteristic_uuid16(
        ORG_BLUETOOTH_CHARACTERISTIC_GAP_DEVICE_NAME,
        ATT_PROPERTY_READ, ATT_SECURITY_NONE, ATT_SECURITY_NONE,
        (uint8_t *)s_device_name, (uint16_t)strlen(s_device_name));
}

static bool note(bool is_service, const char *uuid, uint16_t props, uint16_t handle,
                 const char **err) {
    if (n_entries >= SRV_MAX_ENTRIES) { *err = "too many entries"; return false; }
    entries[n_entries].is_service = is_service;
    entries[n_entries].props = props;
    entries[n_entries].handle = handle;
    snprintf(entries[n_entries].uuid, sizeof(entries[n_entries].uuid), "%s", uuid);
    n_entries++;
    return true;
}

bool gatt_server_add_service(const char *uuid, const char **err) {
    if (!building) { *err = "call gatt_server.begin first"; return false; }
    uint16_t u16 = 0; uint8_t u128[16];
    if (!parse_uuid(uuid, &u16, u128)) { *err = "uuid must be 4 hex chars or dashed 128-bit"; return false; }
    if (u16) { att_db_util_add_service_uuid16(u16); }
    else      { att_db_util_add_service_uuid128(u128); }
    return note(true, uuid, 0, 0, err);
}

bool gatt_server_add_characteristic(const char *uuid, uint16_t properties,
                                    const uint8_t *value, uint16_t len,
                                    uint16_t *out_handle, const char **err) {
    if (!building) { *err = "call gatt_server.begin first"; return false; }
    uint16_t u16 = 0; uint8_t u128[16];
    if (!parse_uuid(uuid, &u16, u128)) { *err = "uuid must be 4 hex chars or dashed 128-bit"; return false; }
#if CONFIG_BT_PERIPHERAL
    if (smp_chr_uuid(uuid)) { properties |= ATT_PROPERTY_DYNAMIC; }
#endif
    uint16_t h;
    if (u16) {
        h = att_db_util_add_characteristic_uuid16(u16, properties, ATT_SECURITY_NONE,
                                                  ATT_SECURITY_NONE, (uint8_t *)value, len);
    } else {
        h = att_db_util_add_characteristic_uuid128(u128, properties, ATT_SECURITY_NONE,
                                                   ATT_SECURITY_NONE, (uint8_t *)value, len);
    }
    if (out_handle) { *out_handle = h; }
#if CONFIG_BT_PERIPHERAL
    if (smp_chr_uuid(uuid)) { s_smp_value = h; }
#endif
    return note(false, uuid, properties, h, err);
}

bool gatt_server_commit(const char **err) {
    if (!building) { *err = "nothing being built"; return false; }
    if (att_db_util_get_size() > MAX_ATT_DB_SIZE) {
        *err = "database exceeds MAX_ATT_DB_SIZE";
        building = false;
        return false;
    }
    building = false;
    /* BTstack holds the pointer att_server_init was given and reads it live.
     * Passing the callbacks again is required: NULL here drops the ones
     * gatt_server_init registered, and a Lua or PEX rebuild would then
     * ignore writes. Bonded peers cache the database and must be told it
     * moved. */
    att_server_init(att_db_util_get_address(), att_read_cb, att_write_cb);
    printf("bt: local GATT database rebuilt, %u bytes, %u entries\n",
           att_db_util_get_size(), n_entries);
    return true;
}

size_t gatt_server_json(char *dst, size_t cap) {
    size_t n = (size_t)snprintf(dst, cap, "{\"bytes\":%u,\"max_bytes\":%u,\"entries\":[",
                                att_db_util_get_size(), MAX_ATT_DB_SIZE);
    for (uint8_t i = 0; i < n_entries && n + 80 < cap; i++) {
        n += (size_t)snprintf(dst + n, cap - n,
                              "%s{\"kind\":\"%s\",\"uuid\":\"%s\",\"properties\":%u}",
                              i ? "," : "", entries[i].is_service ? "service" : "characteristic",
                              entries[i].uuid, entries[i].props);
    }
    return n + (size_t)snprintf(dst + n, cap - n, "]}");
}

void gatt_server_init(const char *name) {
    snprintf(s_device_name, sizeof(s_device_name), "%s", name && *name ? name : "btgw");

    /* The same begin/commit path a script uses, so the default database is
     * not a special case that could drift from the scripted one. */
    gatt_server_begin();

    /* 0x0000 = Unknown. Honest: this is a gateway, not a category the spec
     * has a number for. */
    static uint8_t appearance[2] = { 0x00, 0x00 };
    att_db_util_add_characteristic_uuid16(
        ORG_BLUETOOTH_CHARACTERISTIC_GAP_APPEARANCE,
        ATT_PROPERTY_READ, ATT_SECURITY_NONE, ATT_SECURITY_NONE,
        appearance, sizeof(appearance));

    building = false;
    att_server_init(att_db_util_get_address(), att_read_cb, att_write_cb);
    printf("bt: local GATT database %u bytes (name \"%s\")\n",
           att_db_util_get_size(), s_device_name);
}

uint16_t gatt_server_db_size(void) { return att_db_util_get_size(); }
