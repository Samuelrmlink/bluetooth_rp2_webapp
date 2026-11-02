/* Gateway-LAN policy. See gw_core.h. No lwIP, so the host test links it. */
#include "net/gw_core.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

static const char *field(const char *json, const char *key) {
    char pat[24];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = json ? strstr(json, pat) : NULL;
    if (!p) { return NULL; }
    p += strlen(pat);
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') { p++; }
    if (*p != ':') { return NULL; }
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') { p++; }
    return p;
}

static bool get_str(const char *json, const char *key, char *out, size_t cap) {
    const char *p = field(json, key);
    if (!p || *p != '"' || cap == 0) { return false; }
    p++;
    size_t n = 0;
    while (*p && *p != '"') {
        if (n + 1 >= cap) { return false; }
        out[n++] = *p++;
    }
    if (*p != '"') { return false; }
    out[n] = 0;
    return true;
}

static bool iface_known(const char *s) {
    static const char *const ok[] = { "none", "e0", "w0", "us", "b0" };
    for (size_t i = 0; i < sizeof ok / sizeof ok[0]; i++) {
        if (strcmp(s, ok[i]) == 0) { return true; }
    }
    return false;
}

bool gw_cfg_parse(const char *json, char *iface, size_t cap, const char **why) {
    char tmp[GW_IFACE_LEN];
    if (!iface || cap < GW_IFACE_LEN) { if (why) { *why = "iface buffer"; } return false; }
    if (!get_str(json, "iface", tmp, sizeof tmp) || !iface_known(tmp)) {
        if (why) { *why = "iface is none, e0, w0, us, or b0"; }
        return false;
    }
    snprintf(iface, cap, "%s", tmp);
    return true;
}

void gw_cfg_load(const char *json, bool have_w5500, char *iface, size_t cap) {
    const char *why = NULL;
    char tmp[GW_IFACE_LEN];
    if (json && json[0] && gw_cfg_parse(json, tmp, sizeof tmp, &why)) {
        snprintf(iface, cap, "%s", tmp);
        return;
    }
    snprintf(iface, cap, "%s", have_w5500 ? "e0" : "none");
}

bool gw_hostname_ok(const char *name) {
    if (!name || !name[0] || name[0] == '-' || strlen(name) > 63) { return false; }
    for (const char *p = name; *p; p++) {
        char c = *p;
        int lower = (c >= 'a' && c <= 'z');
        int upper = (c >= 'A' && c <= 'Z');
        int digit = (c >= '0' && c <= '9');
        if (!lower && !upper && !digit && c != '-') { return false; }
    }
    return true;
}

bool gw_hostname_parse(const char *json, char *name, size_t cap, const char **why) {
    char tmp[64];
    if (!get_str(json, "name", tmp, sizeof tmp) || !gw_hostname_ok(tmp)) {
        if (why) { *why = "name must be a DNS label"; }
        return false;
    }
    if (strlen(tmp) + 1 > cap) { if (why) { *why = "name buffer"; } return false; }
    snprintf(name, cap, "%s", tmp);
    return true;
}

void gw_default_hostname(const char *prefix, const uint8_t *id, size_t id_len,
                         char *out, size_t cap) {
    static const char hex[] = "0123456789abcdef";
    size_t n = 0;
    if (!out || cap == 0) { return; }
    const char *p = prefix ? prefix : "";
    while (*p && n + 1 < cap) { out[n++] = *p++; }
    if (id && id_len && n + 1 < cap) { out[n++] = '-'; }
    for (size_t i = 0; id && i < id_len && n + 2 < cap; i++) {
        out[n++] = hex[id[i] >> 4];
        out[n++] = hex[id[i] & 0x0f];
    }
    out[n] = 0;
}

bool gw_ll_kept(bool gw_on, bool have_lease) {
    if (gw_on) { return true; }
    return !have_lease;
}

bool gw_advertise(bool has_gw, bool iface_is_gw_lan) {
    return has_gw && !iface_is_gw_lan;
}

bool gw_instance_name(const char *host, const char *iface, char *out, size_t cap) {
    if (!host || !host[0] || !iface || !iface[0] || !out || cap < 4) { return false; }
    size_t n = strlen(host) + 1 + strlen(iface);
    if (n > 63 || n + 1 > cap) { return false; }
    snprintf(out, cap, "%s-%s", host, iface);
    return true;
}

bool gw_split_instance(const char *instance, char *host, size_t hcap,
                       char *iface, size_t icap) {
    if (!instance || !host || !iface || hcap == 0 || icap < 3) { return false; }
    const char *hy = strrchr(instance, '-');
    if (!hy || hy == instance) { return false; }
    size_t hlen = (size_t)(hy - instance);
    size_t ilen = strlen(hy + 1);
    if (hlen + 1 > hcap || ilen != 2) { return false; }
    memcpy(host, instance, hlen);
    host[hlen] = 0;
    iface[0] = hy[1];
    iface[1] = hy[2];
    iface[2] = 0;
    return host[0] != 0;
}

void gw_ad_txt(const char *iface, int32_t metric,
               char *iface_item, size_t icap, char *metric_item, size_t mcap) {
    if (iface_item && icap) {
        snprintf(iface_item, icap, "iface=%s", iface ? iface : "");
    }
    if (metric_item && mcap) {
        snprintf(metric_item, mcap, "metric=%ld", (long)metric);
    }
}

void gw_table_init(gw_table_t *t) {
    if (t) { memset(t, 0, sizeof *t); }
}

static gw_peer_t *peer_slot(gw_table_t *t, const char *name, bool create) {
    gw_peer_t *free_s = NULL;
    for (int i = 0; i < GW_PEERS; i++) {
        if (t->peers[i].used && strcmp(t->peers[i].name, name) == 0) {
            return &t->peers[i];
        }
        if (!t->peers[i].used && !free_s) { free_s = &t->peers[i]; }
    }
    if (!create || !free_s) { return NULL; }
    memset(free_s, 0, sizeof *free_s);
    snprintf(free_s->name, sizeof free_s->name, "%s", name);
    free_s->used = true;
    return free_s;
}

bool gw_peer_note(gw_table_t *t, const char *name, const char *iface,
                  int32_t metric, const char *ll, uint32_t now_ms) {
    if (!t || !name || !name[0] || !iface || !iface[0] || strlen(iface) > 2) {
        return false;
    }
    if (strlen(name) >= GW_NAME_LEN) { return false; }
    gw_peer_t *p = peer_slot(t, name, true);
    if (!p) { return false; }
    gw_conn_t *slot = NULL;
    for (int i = 0; i < GW_CONNS; i++) {
        if (p->conns[i].used && strcmp(p->conns[i].iface, iface) == 0) {
            slot = &p->conns[i];
            break;
        }
        if (!p->conns[i].used && !slot) { slot = &p->conns[i]; }
    }
    if (!slot) { return false; }
    if (!slot->used) {
        memset(slot, 0, sizeof *slot);
        snprintf(slot->iface, sizeof slot->iface, "%s", iface);
        slot->metric = metric >= 0 ? metric : (INT32_MAX - 1);
        slot->used = true;
    } else if (metric >= 0) {
        slot->metric = metric;
    }
    slot->seen_ms = now_ms;
    if (ll && ll[0]) { snprintf(slot->ll, sizeof slot->ll, "%s", ll); }
    return true;
}

bool gw_peer_touch(gw_table_t *t, const char *name, const char *iface,
                   uint32_t now_ms) {
    if (!t || !name || !iface) { return false; }
    for (int i = 0; i < GW_PEERS; i++) {
        gw_peer_t *p = &t->peers[i];
        if (!p->used || strcmp(p->name, name) != 0) { continue; }
        for (int c = 0; c < GW_CONNS; c++) {
            if (!p->conns[c].used || strcmp(p->conns[c].iface, iface) != 0) { continue; }
            p->conns[c].seen_ms = now_ms;
            return true;
        }
    }
    return false;
}

int gw_peer_expire(gw_table_t *t, uint32_t now_ms) {
    if (!t) { return 0; }
    int dropped = 0;
    for (int i = 0; i < GW_PEERS; i++) {
        gw_peer_t *p = &t->peers[i];
        if (!p->used) { continue; }
        int left = 0;
        for (int c = 0; c < GW_CONNS; c++) {
            if (!p->conns[c].used) { continue; }
            if ((uint32_t)(now_ms - p->conns[c].seen_ms) >= GW_DROP_MS) {
                p->conns[c].used = false;
                dropped++;
            } else {
                left++;
            }
        }
        if (!left) { p->used = false; }
    }
    return dropped;
}

static bool better_conn(const gw_conn_t *c, uint32_t now_ms, bool found, int32_t best) {
    if (!c->used || !c->ll[0]) { return false; }
    if ((uint32_t)(now_ms - c->seen_ms) >= GW_DROP_MS) { return false; }
    return !found || c->metric < best;
}

bool gw_peer_best(const gw_table_t *t, uint32_t now_ms,
                  char *name, size_t name_cap, char *iface, size_t iface_cap,
                  int32_t *metric, char *ll, size_t ll_cap) {
    if (!t) { return false; }
    bool found = false;
    int32_t best = 0;
    const gw_peer_t *bp = NULL;
    const gw_conn_t *bc = NULL;
    for (int i = 0; i < GW_PEERS; i++) {
        const gw_peer_t *p = &t->peers[i];
        if (!p->used) { continue; }
        for (int c = 0; c < GW_CONNS; c++) {
            if (!better_conn(&p->conns[c], now_ms, found, best)) { continue; }
            found = true;
            best = p->conns[c].metric;
            bp = p;
            bc = &p->conns[c];
        }
    }
    if (!found) { return false; }
    if (name && name_cap) { snprintf(name, name_cap, "%s", bp->name); }
    if (iface && iface_cap) { snprintf(iface, iface_cap, "%s", bc->iface); }
    if (ll && ll_cap) { snprintf(ll, ll_cap, "%s", bc->ll); }
    if (metric) { *metric = bc->metric; }
    return true;
}

bool gw_peer_ll(const gw_table_t *t, const char *name, uint32_t now_ms,
                char *ll, size_t ll_cap) {
    if (!t || !name || !name[0]) { return false; }
    bool found = false;
    int32_t best = 0;
    const gw_conn_t *bc = NULL;
    for (int i = 0; i < GW_PEERS; i++) {
        const gw_peer_t *p = &t->peers[i];
        if (!p->used || strcmp(p->name, name) != 0) { continue; }
        for (int c = 0; c < GW_CONNS; c++) {
            if (!better_conn(&p->conns[c], now_ms, found, best)) { continue; }
            found = true;
            best = p->conns[c].metric;
            bc = &p->conns[c];
        }
    }
    if (!found || !ll || ll_cap == 0) { return found; }
    snprintf(ll, ll_cap, "%s", bc->ll);
    return true;
}

bool gw_nat_out_ok(const char *out_iface, const char *lan_iface) {
    if (!out_iface || !out_iface[0] || !lan_iface || !lan_iface[0]) { return false; }
    if (strcmp(out_iface, lan_iface) == 0) { return false; }
    if (strcmp(out_iface, "el") == 0) { return false; }
    if (strcmp(out_iface, "wg") == 0) { return false; }
    return true;
}
