#include <stdio.h>
#include <string.h>

#include "btstack.h"
#include "bt/registry.h"
#include "api/api.h"
#include "autoconf.h"

/* Sized for a room, not a link budget: scanning sees far more devices than we
 * will ever connect to, and a full table must degrade by forgetting the least
 * recently seen rather than by refusing to see anything new. */
#ifndef BT_REGISTRY_MAX
#define BT_REGISTRY_MAX 48
#endif

static bt_device_t devs[BT_REGISTRY_MAX];

const char *bt_transport_name(bt_transport_t t) {
    return t == BT_TRANSPORT_CLASSIC ? "classic" : "le";
}

void bt_registry_reset(void) {
    memset(devs, 0, sizeof(devs));
}

bt_device_t *bt_registry_find(const uint8_t *addr, bt_transport_t transport) {
    for (size_t i = 0; i < BT_REGISTRY_MAX; i++) {
        if (devs[i].used && devs[i].transport == transport &&
            memcmp(devs[i].addr, addr, BT_ADDR_LEN) == 0) {
            return &devs[i];
        }
    }
    return NULL;
}

bt_device_t *bt_registry_get(const uint8_t *addr, bt_transport_t transport) {
    bt_device_t *d = bt_registry_find(addr, transport);
    if (d) {
        return d;
    }
    for (size_t i = 0; i < BT_REGISTRY_MAX; i++) {
        if (!devs[i].used) {
            memset(&devs[i], 0, sizeof(devs[i]));
            devs[i].used = true;
            devs[i].transport = transport;
            memcpy(devs[i].addr, addr, BT_ADDR_LEN);
            return &devs[i];
        }
    }
    /* Full: evict the stalest entry that is not an open connection. */
    bt_device_t *oldest = NULL;
    for (size_t i = 0; i < BT_REGISTRY_MAX; i++) {
        if (devs[i].connected) {
            continue;
        }
        if (!oldest || devs[i].last_seen_ms < oldest->last_seen_ms) {
            oldest = &devs[i];
        }
    }
    if (!oldest) {
        return NULL;
    }
    memset(oldest, 0, sizeof(*oldest));
    oldest->used = true;
    oldest->transport = transport;
    memcpy(oldest->addr, addr, BT_ADDR_LEN);
    return oldest;
}

bt_device_t *bt_registry_by_str(const char *addr_str, bt_transport_t transport) {
    bd_addr_t a;
    if (!sscanf_bd_addr(addr_str, a)) {
        return NULL;
    }
    return bt_registry_find(a, transport);
}

bt_device_t *bt_registry_by_handle(uint16_t con_handle) {
    for (size_t i = 0; i < BT_REGISTRY_MAX; i++) {
        if (devs[i].used && devs[i].connected && devs[i].con_handle == con_handle) {
            return &devs[i];
        }
    }
    return NULL;
}

size_t bt_registry_count(void) {
    size_t n = 0;
    for (size_t i = 0; i < BT_REGISTRY_MAX; i++) {
        if (devs[i].used) {
            n++;
        }
    }
    return n;
}

bt_device_t *bt_registry_at(size_t i) {
    size_t seen = 0;
    for (size_t k = 0; k < BT_REGISTRY_MAX; k++) {
        if (devs[k].used && seen++ == i) {
            return &devs[k];
        }
    }
    return NULL;
}

static size_t hex_append(char *dst, size_t cap, const uint8_t *b, size_t n) {
    size_t w = 0;
    for (size_t i = 0; i < n && w + 3 < cap; i++) {
        w += (size_t)snprintf(dst + w, cap - w, "%02x", b[i]);
    }
    return w;
}

/* AD structures are generic (length, type, data) on every transport that has
 * them; only the stored payloads are LE's. */
static int uuids128_in(const uint8_t *p, uint8_t len, char (*out)[BT_UUID128_STR], int n, int max) {
    for (uint8_t i = 0; i + 1 < len && n < max; ) {
        uint8_t l = p[i];
        if (l == 0 || i + 1 + l > len) { break; }
        uint8_t type = p[i + 1];
        if (type == 0x06 || type == 0x07) {
            for (uint8_t off = 2; off + 16 <= l + 1 && n < max; off += 16) {
                const uint8_t *u = p + i + off;          /* little-endian on air */
                char s[BT_UUID128_STR];
                snprintf(s, sizeof s,
                         "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
                         u[15], u[14], u[13], u[12], u[11], u[10], u[9], u[8],
                         u[7], u[6], u[5], u[4], u[3], u[2], u[1], u[0]);
                bool dup = false;
                for (int k = 0; k < n; k++) { if (!strcmp(out[k], s)) { dup = true; } }
                if (!dup) { memcpy(out[n++], s, sizeof s); }
            }
        }
        i = (uint8_t)(i + 1 + l);
    }
    return n;
}

int bt_device_uuids128(const bt_device_t *d, char (*out)[BT_UUID128_STR], int max) {
    int n = uuids128_in(d->adv, d->adv_len, out, 0, max);
    return uuids128_in(d->rsp, d->rsp_len, out, n, max);
}

size_t bt_device_json(const bt_device_t *d, char *dst, size_t cap) {
    char name_esc[BT_NAME_MAX * 2 + 8] = "";
    if (d->have_name) {
        /* A device name is attacker-controlled in a way an SSID is not: it is
         * whatever the peripheral chose to broadcast. It must not reach a
         * client unescaped. */
        api_json_escape(name_esc, sizeof(name_esc), d->name);
    }

    size_t n = (size_t)snprintf(dst, cap,
        "{\"addr\":\"%02X:%02X:%02X:%02X:%02X:%02X\",\"transport\":\"%s\","
        "\"addr_type\":%u,\"rssi\":%d,\"connectable\":%s,\"connected\":%s",
        d->addr[0], d->addr[1], d->addr[2], d->addr[3], d->addr[4], d->addr[5],
        bt_transport_name(d->transport), d->addr_type, d->rssi,
        d->connectable ? "true" : "false", d->connected ? "true" : "false");

    if (d->have_name && n + 16 < cap) {
        n += (size_t)snprintf(dst + n, cap - n, ",\"name\":\"%s\"", name_esc);
    }
    if (d->have_tx_power && n + 24 < cap) {
        n += (size_t)snprintf(dst + n, cap - n, ",\"tx_power\":%d", d->tx_power);
    }
    if (d->have_mfg && n + 16 < cap) {
        n += (size_t)snprintf(dst + n, cap - n, ",\"mfg_data\":\"");
        n += hex_append(dst + n, cap - n, d->mfg, d->mfg_len);
        n += (size_t)snprintf(dst + n, cap - n, "\"");
    }
    if (d->have_uuids && n + 16 < cap) {
        n += (size_t)snprintf(dst + n, cap - n, ",\"service_uuids\":[");
        for (uint8_t i = 0; i < d->n_uuid16 && n + 12 < cap; i++) {
            n += (size_t)snprintf(dst + n, cap - n, "%s\"%04x\"", i ? "," : "", d->uuid16[i]);
        }
        n += (size_t)snprintf(dst + n, cap - n, "]");
    }
    {
        char u128[4][BT_UUID128_STR];
        int k = bt_device_uuids128(d, u128, 4);
        if (k && n + 16 + (size_t)k * 40 < cap) {
            n += (size_t)snprintf(dst + n, cap - n, ",\"uuids128\":[");
            for (int i = 0; i < k; i++) { n += (size_t)snprintf(dst + n, cap - n, "%s\"%s\"", i ? "," : "", u128[i]); }
            n += (size_t)snprintf(dst + n, cap - n, "]");
        }
    }
    if (d->adv_len && n + 2u * d->adv_len + 12 < cap) {
        n += (size_t)snprintf(dst + n, cap - n, ",\"adv\":\"");
        n += hex_append(dst + n, cap - n, d->adv, d->adv_len);
        n += (size_t)snprintf(dst + n, cap - n, "\"");
    }
    if (d->rsp_len && n + 2u * d->rsp_len + 12 < cap) {
        n += (size_t)snprintf(dst + n, cap - n, ",\"rsp\":\"");
        n += hex_append(dst + n, cap - n, d->rsp, d->rsp_len);
        n += (size_t)snprintf(dst + n, cap - n, "\"");
    }
    if (n + 2 < cap) {
        n += (size_t)snprintf(dst + n, cap - n, "}");
    }
    return n;
}
