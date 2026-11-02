/* Advertises _btgw._tcp on UDP/5353. There is no ESP-IDF mDNS component in
 * this image, so nothing else binds the port. The name is ESP32C5-<STA MAC>
 * with no file behind it: a stored name is a later feature.
 *
 * The announcement goes out each interface that has an address: the station,
 * otherwise Ethernet e0, and the link-local companion only while e0 has
 * none. A station with no address used to build an empty packet, so a
 * board that is only on Ethernet never sent the name.
 *
 * Each announcement is sent with udp_sendto_if on the netif that owns that
 * address. IP_MULTICAST_IF only stores an address for routing, and lwIP
 * then routes 224.0.0.251 out the first netif on that subnet, so a shared
 * /24 made the station transmit the W5500 address.
 *
 * The same socket fills a small _btgw._tcp cache (name, A, SRV port) for
 * the peer forwarder. The announce send path is unchanged. */
#include "mdns_adv.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "lwip/ip4_addr.h"
#include "lwip/ip_addr.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/tcpip.h"
#include "lwip/udp.h"

static const char *TAG = "mdns";
static int fd = -1;
static char host[32];
static int64_t next_us;

#define ADV_MAX 3
struct adv_addr {
    uint32_t ip;
    uint32_t mask;
};
static struct adv_addr joined[ADV_MAX];
static int njoined;
static uint32_t logged_ip[ADV_MAX];
static int nlogged;

const char *mdns_adv_hostname(void) { return host[0] ? host : "ESP32C5"; }

static int put_name(uint8_t *p, int cap, const char *dotted) {
    int n = 0;
    while (*dotted) {
        const char *dot = strchr(dotted, '.');
        int len = dot ? (int)(dot - dotted) : (int)strlen(dotted);
        if (len <= 0 || len > 63 || n + 1 + len >= cap) return -1;
        p[n++] = (uint8_t)len;
        memcpy(p + n, dotted, (size_t)len);
        n += len;
        if (!dot) break;
        dotted = dot + 1;
    }
    if (n + 1 > cap) return -1;
    p[n++] = 0;
    return n;
}

static int put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; return 2; }
static int put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
    return 4;
}

static int rr(uint8_t *p, int cap, const char *name, uint16_t type, uint32_t ttl, const uint8_t *rdata, int rdlen) {
    int n = put_name(p, cap, name);
    if (n < 0 || n + 10 + rdlen > cap) return -1;
    n += put16(p + n, type);
    n += put16(p + n, 0x8001);
    n += put32(p + n, ttl);
    n += put16(p + n, (uint16_t)rdlen);
    memcpy(p + n, rdata, (size_t)rdlen);
    return n + rdlen;
}

static void note_netif(struct adv_addr *o, int *n, int cap, struct netif *nif) {
    if (*n >= cap || !netif_is_up(nif) || !netif_is_link_up(nif)) return;
    const ip4_addr_t *a = netif_ip4_addr(nif);
    if (ip4_addr_isany(a)) return;
    o[*n].ip = ip4_addr_get_u32(a);
    o[*n].mask = ip4_addr_get_u32(netif_ip4_netmask(nif));
    (*n)++;
}

/* Station first, then e0. The companion is the address only while e0 has none. */
static int collect(struct adv_addr *o, int cap) {
    int n = 0;
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t info;
    if (n < cap && sta && esp_netif_get_ip_info(sta, &info) == ESP_OK && info.ip.addr) {
        o[n].ip = info.ip.addr;
        o[n].mask = info.netmask.addr ? info.netmask.addr : htonl(0xffffff00);
        n++;
    }
    bool have_e0 = false;
    LOCK_TCPIP_CORE();
    for (struct netif *nif = netif_list; nif; nif = nif->next) {
        if (!(nif->name[0] == 'e' && nif->name[1] == '0')) continue;
        int before = n;
        note_netif(o, &n, cap, nif);
        have_e0 = n > before;
    }
    if (!have_e0) {
        for (struct netif *nif = netif_list; nif; nif = nif->next) {
            if (nif->name[0] == 'e' && nif->name[1] == 'l') note_netif(o, &n, cap, nif);
        }
    }
    UNLOCK_TCPIP_CORE();
    return n;
}

static int build_answers(uint8_t *pkt, int cap, uint16_t id, uint16_t flags, uint32_t ip) {
    if (!ip || !host[0]) return 0;
    char inst[80], fqdn[48];
    snprintf(inst, sizeof inst, "%s._btgw._tcp.local", host);
    snprintf(fqdn, sizeof fqdn, "%s.local", host);
    uint8_t body[400];
    int n = 0;
    uint8_t name[80];
    int nl = put_name(name, sizeof name, inst);
    if (nl < 0) return 0;
    int k = rr(body + n, (int)sizeof body - n, "_btgw._tcp.local", 12, 10, name, nl);
    if (k < 0) return 0;
    n += k;
    uint8_t srv[96];
    put16(srv, 0);
    put16(srv + 2, 0);
    put16(srv + 4, 80);
    int tn = put_name(srv + 6, (int)sizeof srv - 6, fqdn);
    if (tn < 0) return 0;
    k = rr(body + n, (int)sizeof body - n, inst, 33, 10, srv, 6 + tn);
    if (k < 0) return 0;
    n += k;
    const char *txts[] = { "board=esp32c5", "ws=/ws", "api=/api", "assets=0" };
    uint8_t txt[96];
    int to = 0;
    for (int i = 0; i < 4; i++) {
        size_t L = strlen(txts[i]);
        txt[to++] = (uint8_t)L;
        memcpy(txt + to, txts[i], L);
        to += (int)L;
    }
    k = rr(body + n, (int)sizeof body - n, inst, 16, 10, txt, to);
    if (k < 0) return 0;
    n += k;
    uint8_t a[4];
    memcpy(a, &ip, 4);
    k = rr(body + n, (int)sizeof body - n, fqdn, 1, 10, a, 4);
    if (k < 0) return 0;
    n += k;
    if (12 + n > cap) return 0;
    memset(pkt, 0, 12);
    put16(pkt, id);
    put16(pkt + 2, flags);
    put16(pkt + 6, 4);
    memcpy(pkt + 12, body, (size_t)n);
    return 12 + n;
}

/* Source port 5353, not on the receive list. The BSD socket stays the only
 * listener. udp_send would bind an ephemeral port if local_port were 0, and
 * mDNS receivers drop that. */
static struct udp_pcb *tx;

static struct netif *netif_owning(uint32_t ip) {
    struct netif *nif;
    for (nif = netif_list; nif; nif = nif->next) {
        if (!ip4_addr_isany(netif_ip4_addr(nif)) && ip4_addr_get_u32(netif_ip4_addr(nif)) == ip)
            return nif;
    }
    return NULL;
}

static struct udp_pcb *tx_pcb(void) {
    if (tx) return tx;
    tx = udp_new();
    if (!tx) return NULL;
    tx->local_port = 5353;
    tx->ttl = 255;
    udp_set_multicast_ttl(tx, 255);
    return tx;
}

/* src_ip and dst_ip are network order. dst_port is host order. The packet
 * leaves the netif that owns src_ip, not whichever netif routes the multicast
 * group. */
static void send_owned(const uint8_t *pkt, int n, uint32_t src_ip, uint32_t dst_ip, uint16_t dst_port) {
    struct netif *nif;
    struct udp_pcb *pcb;
    struct pbuf *p;
    ip_addr_t dst;
    if (n <= 0 || !pkt) return;
    LOCK_TCPIP_CORE();
    nif = netif_owning(src_ip);
    pcb = tx_pcb();
    p = (nif && pcb) ? pbuf_alloc(PBUF_TRANSPORT, (u16_t)n, PBUF_RAM) : NULL;
    if (p) {
        memcpy(p->payload, pkt, (size_t)n);
        ip_addr_set_ip4_u32_val(dst, dst_ip);
        udp_sendto_if(pcb, p, &dst, dst_port, nif);
        pbuf_free(p);
    }
    UNLOCK_TCPIP_CORE();
}

static void announce_one(uint32_t ip) {
    uint8_t pkt[512];
    int n = build_answers(pkt, sizeof pkt, 0, 0x8400, ip);
    send_owned(pkt, n, ip, inet_addr("224.0.0.251"), 5353);
}

static uint32_t pick(const struct adv_addr *a, int n, uint32_t from) {
    for (int i = 0; i < n; i++) {
        if (a[i].mask && (from & a[i].mask) == (a[i].ip & a[i].mask)) return a[i].ip;
    }
    return n ? a[0].ip : 0;
}

static void join_set(const struct adv_addr *a, int n) {
    struct ip_mreq m;
    m.imr_multiaddr.s_addr = inet_addr("224.0.0.251");
    for (int i = 0; i < njoined; i++) {
        m.imr_interface.s_addr = joined[i].ip;
        setsockopt(fd, IPPROTO_IP, IP_DROP_MEMBERSHIP, &m, sizeof m);
    }
    njoined = 0;
    for (int i = 0; i < n && njoined < ADV_MAX; i++) {
        m.imr_interface.s_addr = a[i].ip;
        if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &m, sizeof m) == 0) {
            joined[njoined++] = a[i];
        } else {
            struct in_addr ia = { .s_addr = a[i].ip };
            ESP_LOGW(TAG, "join 224.0.0.251 on %s failed: %d", inet_ntoa(ia), errno);
        }
    }
}

/* Hostnames heard as _btgw._tcp. Eight is enough for the two dials. */
#define PEERS 8
#define DROP_US (15LL * 1000 * 1000)

typedef struct {
    int used;
    char name[64];
    char ip[16];
    uint16_t port;
    int64_t seen;
} peer_t;

static peer_t peers[PEERS];
static SemaphoreHandle_t peer_mu;

static int host_ok(const char *s) {
    size_t i, n;
    if (!s) return 0;
    n = strlen(s);
    if (n < 1 || n > 63 || s[0] == '-') return 0;
    for (i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-'))
            return 0;
    }
    return 1;
}

static int skip_name(const uint8_t *msg, int len, int off) {
    int guard = 0;
    while (off >= 0 && off < len && guard++ < 64) {
        uint8_t n = msg[off];
        if (n == 0) return off + 1;
        if ((n & 0xc0) == 0xc0) {
            if (off + 1 >= len) return -1;
            return off + 2;
        }
        if (n & 0xc0) return -1;
        if (off + 1 + n > len) return -1;
        off += 1 + n;
    }
    return -1;
}

static int name_dot(const uint8_t *msg, int len, int off, char *dst, size_t cap) {
    int jumps = 0;
    size_t n = 0;
    if (!dst || cap < 2) return 0;
    dst[0] = 0;
    while (off >= 0 && off < len && jumps < 8) {
        uint8_t lab = msg[off];
        if (lab == 0) {
            dst[n] = 0;
            return 1;
        }
        if ((lab & 0xc0) == 0xc0) {
            if (off + 1 >= len) return 0;
            off = ((lab & 0x3f) << 8) | msg[off + 1];
            jumps++;
            continue;
        }
        if (lab & 0xc0 || off + 1 + lab > len) return 0;
        if (n && n + 1 < cap) dst[n++] = '.';
        if (n + lab >= cap) return 0;
        memcpy(dst + n, msg + off + 1, lab);
        n += (size_t)lab;
        off += 1 + lab;
    }
    return 0;
}

/* First label in host, remainder equal to suf (including its leading dot). */
static int split_suffix(const uint8_t *msg, int len, int off, const char *suf,
                        char *host, size_t cap) {
    char full[96];
    size_t n, sl;
    if (!name_dot(msg, len, off, full, sizeof full)) return 0;
    sl = strlen(suf);
    n = strlen(full);
    if (n <= sl || strcasecmp(full + (n - sl), suf) != 0) return 0;
    if (n - sl >= cap || n - sl > 63) return 0;
    memcpy(host, full, n - sl);
    host[n - sl] = 0;
    if (strchr(host, '.')) return 0;
    return host_ok(host);
}

static void peer_drop(const char *name) {
    int i;
    if (!name || !name[0]) return;
    for (i = 0; i < PEERS; i++) {
        if (peers[i].used && strcasecmp(peers[i].name, name) == 0) peers[i].used = 0;
    }
}

static peer_t *peer_slot(const char *name, int64_t now, int create) {
    peer_t *found = NULL;
    peer_t *free_s = NULL;
    peer_t *old = NULL;
    peer_t *s;
    int i;
    for (i = 0; i < PEERS; i++) {
        if (peers[i].used && now - peers[i].seen > DROP_US) peers[i].used = 0;
        if (peers[i].used && strcasecmp(peers[i].name, name) == 0) found = &peers[i];
        if (!peers[i].used && !free_s) free_s = &peers[i];
        if (peers[i].used && (!old || peers[i].seen < old->seen)) old = &peers[i];
    }
    if (found || !create) return found;
    s = free_s ? free_s : old;
    if (!s) return NULL;
    memset(s, 0, sizeof *s);
    snprintf(s->name, sizeof s->name, "%s", name);
    s->port = 80;
    s->used = 1;
    s->seen = now;
    return s;
}

static void note_rr(const uint8_t *msg, int len, int name_off, uint16_t type, uint32_t ttl,
                    const uint8_t *rd, int rdlen, int pass, int64_t now) {
    char host[64];
    char owner[96];
    peer_t *s;
    if (pass == 0 && type == 12) {
        if (!name_dot(msg, len, name_off, owner, sizeof owner)) return;
        if (strcasecmp(owner, "_btgw._tcp.local") != 0) return;
        if (rdlen < 1 || !split_suffix(msg, len, (int)(rd - msg), "._btgw._tcp.local", host, sizeof host))
            return;
        if (!ttl) { peer_drop(host); return; }
        peer_slot(host, now, 1);
        return;
    }
    if (pass == 0 && type == 33) {
        if (!split_suffix(msg, len, name_off, "._btgw._tcp.local", host, sizeof host)) return;
        if (!ttl) { peer_drop(host); return; }
        s = peer_slot(host, now, 1);
        if (!s) return;
        s->seen = now;
        if (rdlen >= 6) {
            uint16_t port = (uint16_t)((rd[4] << 8) | rd[5]);
            if (port) s->port = port;
        }
        return;
    }
    if (pass == 1 && type == 1) {
        if (!split_suffix(msg, len, name_off, ".local", host, sizeof host)) return;
        if (!ttl) { peer_drop(host); return; }
        if (rdlen < 4 || !(rd[0] | rd[1] | rd[2] | rd[3])) return;
        s = peer_slot(host, now, 0);
        if (!s) return;
        if (!s->ip[0]) {
            snprintf(s->ip, sizeof s->ip, "%u.%u.%u.%u", rd[0], rd[1], rd[2], rd[3]);
            ESP_LOGI(TAG, "peer %s %s:%u", s->name, s->ip, (unsigned)(s->port ? s->port : 80));
        } else {
            snprintf(s->ip, sizeof s->ip, "%u.%u.%u.%u", rd[0], rd[1], rd[2], rd[3]);
        }
        s->seen = now;
    }
}

/* Answers then additionals. Pass 0 learns the service name and port; pass 1
 * attaches an A record to a name already heard, so order inside the packet
 * does not matter. */
static void note_response(const uint8_t *msg, int len) {
    int pass;
    if (!peer_mu || len < 12 || !(msg[2] & 0x80)) return;
    xSemaphoreTake(peer_mu, portMAX_DELAY);
    for (pass = 0; pass < 2; pass++) {
        int qd = (msg[4] << 8) | msg[5];
        int recs = ((msg[6] << 8) | msg[7]) + ((msg[8] << 8) | msg[9]) + ((msg[10] << 8) | msg[11]);
        int off = 12;
        int i;
        int64_t now = esp_timer_get_time();
        for (i = 0; i < qd; i++) {
            off = skip_name(msg, len, off);
            if (off < 0 || off + 4 > len) goto done;
            off += 4;
        }
        for (i = 0; i < recs; i++) {
            int name_off = off;
            uint16_t type, clas, rdlen;
            uint32_t ttl;
            off = skip_name(msg, len, off);
            if (off < 0 || off + 10 > len) goto done;
            type = (uint16_t)((msg[off] << 8) | msg[off + 1]);
            clas = (uint16_t)((msg[off + 2] << 8) | msg[off + 3]);
            ttl = ((uint32_t)msg[off + 4] << 24) | ((uint32_t)msg[off + 5] << 16) |
                  ((uint32_t)msg[off + 6] << 8) | msg[off + 7];
            rdlen = (uint16_t)((msg[off + 8] << 8) | msg[off + 9]);
            off += 10;
            if (off + (int)rdlen > len) goto done;
            if ((clas & 0x7fff) == 1)
                note_rr(msg, len, name_off, type, ttl, msg + off, (int)rdlen, pass, now);
            off += rdlen;
        }
    }
done:
    xSemaphoreGive(peer_mu);
}

static const uint8_t ptr_query[] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x05, '_', 'b', 't', 'g', 'w',
    0x04, '_', 't', 'c', 'p',
    0x05, 'l', 'o', 'c', 'a', 'l',
    0x00, 0x00, 0x0c, 0x00, 0x01,
};

void mdns_adv_browse_kick(void) {
    struct adv_addr addrs[ADV_MAX];
    int na, i;
    if (fd < 0) return;
    na = collect(addrs, ADV_MAX);
    for (i = 0; i < na; i++)
        send_owned(ptr_query, (int)sizeof ptr_query, addrs[i].ip, inet_addr("224.0.0.251"), 5353);
}

bool mdns_adv_peer(const char *name, char *ip, size_t ip_cap, uint16_t *port) {
    int i;
    int64_t now;
    bool ok = false;
    if (!name || !name[0] || !ip || ip_cap < 8 || !peer_mu) return false;
    now = esp_timer_get_time();
    xSemaphoreTake(peer_mu, portMAX_DELAY);
    for (i = 0; i < PEERS; i++) {
        peer_t *s = &peers[i];
        if (!s->used) continue;
        if (now - s->seen > DROP_US) { s->used = 0; continue; }
        if (!s->ip[0] || strcasecmp(s->name, name) != 0) continue;
        snprintf(ip, ip_cap, "%s", s->ip);
        if (port) *port = s->port ? s->port : 80;
        ok = true;
        break;
    }
    xSemaphoreGive(peer_mu);
    return ok;
}

static bool name_is_ours(const uint8_t *p, const uint8_t *end) {
    char tmp[96];
    int o = 0;
    while (p < end && *p) {
        int n = *p++;
        if (n > 63 || p + n > end || o + n + 1 >= (int)sizeof tmp) return false;
        if (o) tmp[o++] = '.';
        memcpy(tmp + o, p, (size_t)n);
        o += n;
        p += n;
    }
    tmp[o] = 0;
    return strstr(tmp, "_btgw._tcp.local") != NULL || (host[0] && strstr(tmp, host) != NULL);
}

static void on_query(const uint8_t *msg, int len, const struct sockaddr_in *from) {
    if (len < 12 || (msg[2] & 0x80)) return;
    int qd = (msg[4] << 8) | msg[5];
    const uint8_t *p = msg + 12;
    const uint8_t *end = msg + len;
    bool hit = false;
    for (int i = 0; i < qd && p < end; i++) {
        const uint8_t *start = p;
        while (p < end && *p) {
            if ((*p & 0xc0) == 0xc0) { p += 2; break; }
            p += 1 + *p;
        }
        if (p < end && *p == 0) p++;
        if (name_is_ours(start, end)) hit = true;
        p += 4;
    }
    if (!hit) return;
    struct adv_addr addrs[ADV_MAX];
    int na = collect(addrs, ADV_MAX);
    uint32_t ip = pick(addrs, na, from->sin_addr.s_addr);
    uint8_t pkt[512];
    uint16_t id = (uint16_t)((msg[0] << 8) | msg[1]);
    int n = build_answers(pkt, sizeof pkt, id, 0x8400, ip);
    if (n > 0) send_owned(pkt, n, ip, from->sin_addr.s_addr, ntohs(from->sin_port));
}

void mdns_adv_start(void) {
    uint8_t mac[6];
    if (!peer_mu) peer_mu = xSemaphoreCreateMutex();
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
        snprintf(host, sizeof host, "ESP32C5-%02x%02x%02x%02x%02x%02x",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    } else {
        snprintf(host, sizeof host, "ESP32C5");
    }
    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { ESP_LOGW(TAG, "socket failed"); return; }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(5353), .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (bind(fd, (struct sockaddr *)&a, sizeof a) != 0) {
        ESP_LOGW(TAG, "bind 5353 failed: %d", errno);
        close(fd);
        fd = -1;
        return;
    }
    /* mDNS receivers drop a packet whose TTL is not 255. */
    uint8_t ttl = 255;
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof ttl);
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    ESP_LOGI(TAG, "hostname %s", host);
    next_us = 0;
}

void mdns_adv_poll(void) {
    static uint8_t buf[1024];
    struct sockaddr_in from;
    socklen_t fl = sizeof from;
    int n;
    if (fd < 0) return;
    while ((n = (int)recvfrom(fd, buf, sizeof buf, 0, (struct sockaddr *)&from, &fl)) > 0) {
        if (n >= 12 && (buf[2] & 0x80)) note_response(buf, n);
        else on_query(buf, n, &from);
        fl = sizeof from;
    }
    struct adv_addr addrs[ADV_MAX];
    int na = collect(addrs, ADV_MAX);
    bool ips_changed = na != nlogged;
    if (!ips_changed) {
        for (int i = 0; i < na; i++) if (addrs[i].ip != logged_ip[i]) ips_changed = true;
    }
    if (ips_changed) {
        join_set(addrs, na);
        nlogged = na;
        for (int i = 0; i < na; i++) {
            logged_ip[i] = addrs[i].ip;
            struct in_addr ia = { .s_addr = addrs[i].ip };
            ESP_LOGI(TAG, "advertising %s.local via %s", host, inet_ntoa(ia));
        }
    }
    int64_t now = esp_timer_get_time();
    if (now >= next_us) {
        for (int i = 0; i < na; i++) announce_one(addrs[i].ip);
        mdns_adv_browse_kick();
        next_us = now + 2000 * 1000;
    }
}
