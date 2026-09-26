#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "btstack.h"
#include "bt/le/gattdb.h"

/* Defined below; used by both the whole-tree and per-service writers. */
static size_t props_json(uint16_t p, char *dst, size_t cap);

/* The Bluetooth Base UUID: 0000xxxx-0000-1000-8000-00805F9B34FB. A 128-bit
 * UUID matching it outside bytes 2..3 is really a 16-bit UUID, and storing it
 * as 16 bytes would waste a pool slot per service. */
static const uint8_t base_uuid[16] = {
    0x00,0x00,0x00,0x00, 0x00,0x00, 0x10,0x00, 0x80,0x00, 0x00,0x80,0x5F,0x9B,0x34,0xFB
};

static bool is_base_uuid(const uint8_t *u) {
    if (!u) {
        return false;
    }
    for (int i = 0; i < 16; i++) {
        if (i == 2 || i == 3) {
            continue;
        }
        if (u[i] != base_uuid[i]) {
            return false;
        }
    }
    return true;
}

void gattdb_reset(gattdb_t *db, uint16_t con_handle) {
    memset(db, 0, sizeof(*db));
    db->in_use = true;
    db->con_handle = con_handle;
    db->ready = false;
    db->mtu = ATT_DEFAULT_MTU;
}

uuid_ref_t gattdb_intern(gattdb_t *db, uint16_t uuid16, const uint8_t *uuid128) {
    uuid_ref_t r = { .uuid16 = uuid16, .u128 = UUID128_NONE };
    if (uuid16 != 0 || is_base_uuid(uuid128)) {
        if (uuid16 == 0 && uuid128) {
            r.uuid16 = (uint16_t)((uuid128[2] << 8) | uuid128[3]);
        }
        return r;
    }
    if (!uuid128) {
        return r;
    }
    for (uint8_t i = 0; i < db->n_uuid128; i++) {
        if (memcmp(db->uuid128[i], uuid128, 16) == 0) {
            r.u128 = i;
            return r;
        }
    }
    if (db->n_uuid128 < CONFIG_GATT_MAX_UUID128) {
        memcpy(db->uuid128[db->n_uuid128], uuid128, 16);
        r.u128 = db->n_uuid128++;
    } else {
        /* Out of pool. Mark it lost rather than leaving uuid16 at 0, which
         * would render as the perfectly valid-looking "0000". */
        r.u128 = UUID128_LOST;
        db->truncated = true;
    }
    return r;
}

size_t gattdb_uuid_str(const gattdb_t *db, uuid_ref_t u, char *dst, size_t cap) {
    if (u.u128 == UUID128_LOST) {
        return (size_t)snprintf(dst, cap, "unavailable");
    }
    if (u.u128 == UUID128_NONE) {
        return (size_t)snprintf(dst, cap, "%04x", u.uuid16);
    }
    const uint8_t *b = db->uuid128[u.u128];
    return (size_t)snprintf(dst, cap,
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        b[0],b[1],b[2],b[3], b[4],b[5], b[6],b[7], b[8],b[9], b[10],b[11],b[12],b[13],b[14],b[15]);
}

/* Human-readable property flags. Clients get both these and the raw bitfield:
 * the names are what a person reads, the number is what a script tests. */
size_t gattdb_json(const gattdb_t *db, char *dst, size_t cap) {
    char u[40], pj[160];
    size_t n = (size_t)snprintf(dst, cap, "{\"mtu\":%u,\"ready\":%s,\"truncated\":%s,\"services\":[",
                                db->mtu, db->ready ? "true" : "false",
                                db->truncated ? "true" : "false");
    for (uint8_t si = 0; si < db->n_svc && n + 96 < cap; si++) {
        const gdb_service_t *s = &db->svc[si];
        gattdb_uuid_str(db, s->uuid, u, sizeof(u));
        n += (size_t)snprintf(dst + n, cap - n,
                              "%s{\"id\":\"%u\",\"uuid\":\"%s\",\"characteristics\":[",
                              si ? "," : "", si, u);
        for (uint8_t k = 0; k < s->n_chr && n + 128 < cap; k++) {
            uint8_t ci = (uint8_t)(s->first_chr + k);
            const gdb_char_t *c = &db->chr[ci];
            gattdb_uuid_str(db, c->uuid, u, sizeof(u));
            props_json(c->properties, pj, sizeof(pj));
            n += (size_t)snprintf(dst + n, cap - n,
                "%s{\"id\":\"%u.%u\",\"uuid\":\"%s\",\"properties\":%u,\"flags\":%s,\"descriptors\":[",
                k ? "," : "", si, k, u, c->properties, pj);
            for (uint8_t j = 0; j < c->n_dsc && n + 64 < cap; j++) {
                const gdb_desc_t *d = &db->dsc[c->first_dsc + j];
                gattdb_uuid_str(db, d->uuid, u, sizeof(u));
                n += (size_t)snprintf(dst + n, cap - n, "%s{\"id\":\"%u.%u.%u\",\"uuid\":\"%s\"}",
                                      j ? "," : "", si, k, j, u);
            }
            n += (size_t)snprintf(dst + n, cap - n, "]}");
        }
        n += (size_t)snprintf(dst + n, cap - n, "]}");
    }
    return n + (size_t)snprintf(dst + n, cap - n, "]}");
}

static size_t props_json(uint16_t p, char *dst, size_t cap) {
    static const struct { uint16_t bit; const char *name; } tbl[] = {
        { 0x01, "broadcast" }, { 0x02, "read" }, { 0x04, "write_no_response" },
        { 0x08, "write" }, { 0x10, "notify" }, { 0x20, "indicate" },
        { 0x40, "authenticated_signed_writes" }, { 0x80, "extended" },
    };
    size_t n = (size_t)snprintf(dst, cap, "[");
    bool first = true;
    for (size_t i = 0; i < sizeof(tbl)/sizeof(tbl[0]); i++) {
        if (p & tbl[i].bit) {
            n += (size_t)snprintf(dst + n, cap - n, "%s\"%s\"", first ? "" : ",", tbl[i].name);
            first = false;
        }
    }
    return n + (size_t)snprintf(dst + n, cap - n, "]");
}

/* A device with a hundred-odd characteristics serialises to more than lwIP's
 * TCP send buffer can take in one write, and an oversized frame is dropped
 * rather than split -- so the API pages the tree instead of sending it whole.
 * These two forms are what it uses; gattdb_json stays for small devices and
 * for tests. */
size_t gattdb_summary_json(const gattdb_t *db, char *dst, size_t cap) {
    char u[40];
    size_t n = (size_t)snprintf(dst, cap,
        "{\"mtu\":%u,\"ready\":%s,\"truncated\":%s,\"counts\":"
        "{\"services\":%u,\"characteristics\":%u,\"descriptors\":%u},\"services\":[",
        db->mtu, db->ready ? "true" : "false", db->truncated ? "true" : "false",
        db->n_svc, db->n_chr, db->n_dsc);
    for (uint8_t si = 0; si < db->n_svc && n + 96 < cap; si++) {
        gattdb_uuid_str(db, db->svc[si].uuid, u, sizeof(u));
        n += (size_t)snprintf(dst + n, cap - n,
                              "%s{\"id\":\"%u\",\"uuid\":\"%s\",\"characteristics\":%u}",
                              si ? "," : "", si, u, db->svc[si].n_chr);
    }
    return n + (size_t)snprintf(dst + n, cap - n, "]}");
}

size_t gattdb_service_json(const gattdb_t *db, uint8_t svc_i, char *dst, size_t cap) {
    if (svc_i >= db->n_svc) { return 0; }
    char u[40], pj[160];
    const gdb_service_t *s = &db->svc[svc_i];
    gattdb_uuid_str(db, s->uuid, u, sizeof(u));
    size_t n = (size_t)snprintf(dst, cap, "{\"id\":\"%u\",\"uuid\":\"%s\",\"characteristics\":[",
                                svc_i, u);
    for (uint8_t k = 0; k < s->n_chr && n + 160 < cap; k++) {
        uint8_t ci = (uint8_t)(s->first_chr + k);
        const gdb_char_t *c = &db->chr[ci];
        gattdb_uuid_str(db, c->uuid, u, sizeof(u));
        props_json(c->properties, pj, sizeof(pj));
        n += (size_t)snprintf(dst + n, cap - n,
            "%s{\"id\":\"%u.%u\",\"uuid\":\"%s\",\"properties\":%u,\"flags\":%s,\"descriptors\":[",
            k ? "," : "", svc_i, k, u, c->properties, pj);
        for (uint8_t j = 0; j < c->n_dsc && n + 64 < cap; j++) {
            const gdb_desc_t *d = &db->dsc[c->first_dsc + j];
            gattdb_uuid_str(db, d->uuid, u, sizeof(u));
            n += (size_t)snprintf(dst + n, cap - n, "%s{\"id\":\"%u.%u.%u\",\"uuid\":\"%s\"}",
                                  j ? "," : "", svc_i, k, j, u);
        }
        n += (size_t)snprintf(dst + n, cap - n, "]}");
    }
    return n + (size_t)snprintf(dst + n, cap - n, "]}");
}

bool gattdb_resolve_chr(const gattdb_t *db, const char *path, uint8_t *svc_i, uint8_t *chr_i) {
    char *end;
    long s = strtol(path, &end, 10);
    if (end == path || s < 0 || s >= db->n_svc) {
        return false;
    }
    if (*end != '.') {
        return false;
    }
    long c = strtol(end + 1, &end, 10);
    if (c < 0 || c >= db->svc[s].n_chr) {
        return false;
    }
    *svc_i = (uint8_t)s;
    *chr_i = (uint8_t)(db->svc[s].first_chr + c);
    return true;
}

bool gattdb_find_chr(const gattdb_t *db, const char *svc_uuid, const char *chr_uuid,
                     int instance, uint8_t *svc_i, uint8_t *chr_i) {
    char u[40];
    int seen = 0;
    for (uint8_t si = 0; si < db->n_svc; si++) {
        gattdb_uuid_str(db, db->svc[si].uuid, u, sizeof(u));
        if (svc_uuid && *svc_uuid && strcasecmp(u, svc_uuid) != 0) {
            continue;
        }
        for (uint8_t k = 0; k < db->svc[si].n_chr; k++) {
            uint8_t ci = (uint8_t)(db->svc[si].first_chr + k);
            gattdb_uuid_str(db, db->chr[ci].uuid, u, sizeof(u));
            if (strcasecmp(u, chr_uuid) != 0) {
                continue;
            }
            if (seen++ < instance) {
                continue;
            }
            *svc_i = si;
            *chr_i = ci;
            return true;
        }
    }
    return false;
}
