/* Browse _btgw._tcp on each up interface, one lwIP search slot at a time.
 * The callback only updates the table. mdns_browse_poll, from the main loop,
 * rotates the interface every two seconds. A peer that is not refreshed
 * within 15 seconds drops out of mdns.peers. */
#include "net/mdns.h"

#if CONFIG_NET_MDNS

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "lwip/apps/mdns.h"
#include "lwip/ip_addr.h"
#include "lwip/netif.h"
#include "lwip/prot/dns.h"
#include "pico/time.h"

#define PEERS 8
#define DROP_MS 15000u

typedef struct {
    bool used;
    char name[64];
    char ip[16];
    uint16_t port;
    char board[16];
    int assets;
    uint32_t seen;
} peer_t;

static peer_t peers[PEERS];
static u8_t req_id;
static bool req_on;
static int rot;
static uint32_t next_ms;

static uint32_t now_ms(void) { return to_ms_since_boot(get_absolute_time()); }

static bool ours(const char *ip) {
    struct netif *nif;
    NETIF_FOREACH(nif) {
        if (!ip4_addr_isany(netif_ip4_addr(nif)) && strcmp(ip, ip4addr_ntoa(netif_ip4_addr(nif))) == 0)
            return true;
    }
    return false;
}

static void label0(const u8_t *name, int len, char *dst, size_t cap) {
    dst[0] = 0;
    if (!name || len < 2) return;
    unsigned n = name[0];
    if (n == 0 || n >= cap || (int)n >= len) return;
    memcpy(dst, name + 1, n);
    dst[n] = 0;
}

static peer_t *slot_for(const char *name) {
    if (!name[0]) return NULL;
    peer_t *free_s = NULL, *old = NULL;
    for (int i = 0; i < PEERS; i++) {
        if (peers[i].used && strcasecmp(peers[i].name, name) == 0) return &peers[i];
        if (!peers[i].used && !free_s) free_s = &peers[i];
        if (peers[i].used && (!old || (int32_t)(peers[i].seen - old->seen) < 0)) old = &peers[i];
    }
    peer_t *s = free_s ? free_s : old;
    if (!s) return NULL;
    memset(s, 0, sizeof *s);
    snprintf(s->name, sizeof s->name, "%s", name);
    s->port = 80;
    s->assets = -1;
    s->used = true;
    return s;
}

static void touch(peer_t *s) { if (s) s->seen = now_ms(); }

static void on_answer(struct mdns_answer *ans, const char *varpart, int varlen, int flags, void *arg) {
    (void)flags;
    (void)arg;
    if (!ans || !varpart || varlen < 0) return;
    char owner[64];
    label0(ans->info.domain.name, ans->info.domain.length, owner, sizeof owner);
    if (ans->ttl == 0) {
        /* A goodbye. slot_for would otherwise recreate the name with an
         * empty address and keep it until DROP_MS. */
        if (ans->info.type == DNS_RRTYPE_PTR) {
            char inst[64];
            label0((const u8_t *)varpart, varlen, inst, sizeof inst);
            mdns_peers_drop_name(inst);
        } else {
            mdns_peers_drop_name(owner);
        }
        return;
    }
    if (ans->info.type == DNS_RRTYPE_PTR) {
        char inst[64];
        label0((const u8_t *)varpart, varlen, inst, sizeof inst);
        touch(slot_for(inst));
        return;
    }
    peer_t *s = slot_for(owner);
    if (!s) return;
    touch(s);
    if (ans->info.type == DNS_RRTYPE_A && varlen >= 4) {
        snprintf(s->ip, sizeof s->ip, "%u.%u.%u.%u",
                 (unsigned)(uint8_t)varpart[0], (unsigned)(uint8_t)varpart[1],
                 (unsigned)(uint8_t)varpart[2], (unsigned)(uint8_t)varpart[3]);
    } else if (ans->info.type == DNS_RRTYPE_SRV && varlen >= 6) {
        s->port = (uint16_t)(((uint8_t)varpart[4] << 8) | (uint8_t)varpart[5]);
    } else if (ans->info.type == DNS_RRTYPE_TXT) {
        int i = 0;
        while (i < varlen) {
            int n = (uint8_t)varpart[i++];
            if (n <= 0 || i + n > varlen) break;
            if (n > 6 && memcmp(varpart + i, "board=", 6) == 0) {
                int c = n - 6;
                if (c > (int)sizeof s->board - 1) c = (int)sizeof s->board - 1;
                memcpy(s->board, varpart + i + 6, (size_t)c);
                s->board[c] = 0;
            } else if (n > 7 && memcmp(varpart + i, "assets=", 7) == 0) {
                s->assets = varpart[i + 7] == '1' ? 1 : 0;
            }
            i += n;
        }
    }
}

static void search_next(void) {
    struct netif *up[8];
    int n = 0;
    struct netif *nif;
    NETIF_FOREACH(nif) {
        if (n < 8 && !ip4_addr_isany(netif_ip4_addr(nif))) up[n++] = nif;
    }
    if (!n) return;
    if (req_on) {
        mdns_search_stop(req_id);
        req_on = false;
    }
    rot = (rot + 1) % n;
    if (mdns_search_service(NULL, "_btgw", DNSSD_PROTO_TCP, up[rot], on_answer, NULL, &req_id) == ERR_OK)
        req_on = true;
}

void mdns_browse_poll(void) {
    uint32_t now = now_ms();
    if (next_ms && (int32_t)(now - next_ms) < 0) return;
    next_ms = now + 2000;
    search_next();
}

void mdns_browse_kick(void) { next_ms = 0; mdns_browse_poll(); }

bool mdns_peer_endpoint(const char *name, char *ip, size_t ip_cap, uint16_t *port) {
    if (!name || !name[0] || !ip || ip_cap < 8) return false;
    uint32_t now = now_ms();
    for (int i = 0; i < PEERS; i++) {
        peer_t *s = &peers[i];
        if (!s->used || !s->ip[0] || strcasecmp(s->name, name) != 0) continue;
        if ((uint32_t)(now - s->seen) > DROP_MS) return false;
        snprintf(ip, ip_cap, "%s", s->ip);
        if (port) *port = s->port ? s->port : 80;
        return true;
    }
    return false;
}

void mdns_peers_drop_name(const char *name) {
    if (!name || !name[0]) { return; }
    for (int i = 0; i < PEERS; i++) {
        if (peers[i].used && strcasecmp(peers[i].name, name) == 0) {
            peers[i].used = false;
        }
    }
}

bool mdns_peer_at(int index, mdns_peer_view_t *out) {
    if (!out || index < 0) { return false; }
    uint32_t now = now_ms();
    int n = 0;
    for (int i = 0; i < PEERS; i++) {
        peer_t *s = &peers[i];
        if (!s->used) { continue; }
        if ((uint32_t)(now - s->seen) > DROP_MS) { s->used = false; continue; }
        if (n++ != index) { continue; }
        out->name = s->name;
        out->ip = s->ip;
        out->port = s->port ? s->port : 80;
        out->board = s->board;
        out->assets = s->assets;
        out->age_ms = now - s->seen;
        out->self = ours(s->ip);
        return true;
    }
    return false;
}

size_t mdns_peers_json(char *dst, size_t cap) {
    uint32_t now = now_ms();
    size_t n = (size_t)snprintf(dst, cap, "{\"peers\":[");
    bool first = true;
    for (int i = 0; i < PEERS && n + 160 < cap; i++) {
        peer_t *s = &peers[i];
        if (!s->used) continue;
        uint32_t age = now - s->seen;
        if (age > DROP_MS) { s->used = false; continue; }
        n += (size_t)snprintf(dst + n, cap - n,
                              "%s{\"name\":\"%s\",\"ip\":\"%s\",\"port\":%u,\"board\":\"%s\","
                              "\"assets\":%d,\"age_ms\":%lu,\"self\":%s}",
                              first ? "" : ",", s->name, s->ip, (unsigned)(s->port ? s->port : 80),
                              s->board, s->assets, (unsigned long)age, ours(s->ip) ? "true" : "false");
        first = false;
    }
    n += (size_t)snprintf(dst + n, cap - n, "]}");
    return n;
}

#endif
