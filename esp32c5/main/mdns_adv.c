/* Advertises _btgw._tcp on UDP/5353. There is no ESP-IDF mDNS component in
 * this image, so nothing else binds the port. The name is ESP32C5-<STA MAC>
 * with no file behind it: a stored name is a later feature. */
#include "mdns_adv.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"

static const char *TAG = "mdns";
static int fd = -1;
static char host[32];
static int64_t next_us;

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

static bool sta_ip(uint32_t *be) {
    esp_netif_t *n = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t info;
    if (!n || esp_netif_get_ip_info(n, &info) != ESP_OK || info.ip.addr == 0) return false;
    *be = info.ip.addr;
    return true;
}

static int build_answers(uint8_t *pkt, int cap, uint16_t id, uint16_t flags) {
    uint32_t ip = 0;
    if (!sta_ip(&ip) || !host[0]) return 0;
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

static void send_mcast(const uint8_t *pkt, int n) {
    if (fd < 0 || n <= 0) return;
    struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons(5353) };
    to.sin_addr.s_addr = inet_addr("224.0.0.251");
    sendto(fd, pkt, (size_t)n, 0, (struct sockaddr *)&to, sizeof to);
}

static void announce(void) {
    uint8_t pkt[512];
    int n = build_answers(pkt, sizeof pkt, 0, 0x8400);
    send_mcast(pkt, n);
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
    uint8_t pkt[512];
    uint16_t id = (uint16_t)((msg[0] << 8) | msg[1]);
    int n = build_answers(pkt, sizeof pkt, id, 0x8400);
    if (n > 0 && fd >= 0) sendto(fd, pkt, (size_t)n, 0, (struct sockaddr *)from, sizeof *from);
}

void mdns_adv_start(void) {
    uint8_t mac[6];
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
    struct ip_mreq m;
    m.imr_multiaddr.s_addr = inet_addr("224.0.0.251");
    m.imr_interface.s_addr = htonl(INADDR_ANY);
    setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &m, sizeof m);
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    ESP_LOGI(TAG, "advertising %s._btgw._tcp.local", host);
    next_us = 0;
}

void mdns_adv_poll(void) {
    if (fd < 0) return;
    uint8_t buf[512];
    struct sockaddr_in from;
    socklen_t fl = sizeof from;
    int n;
    while ((n = (int)recvfrom(fd, buf, sizeof buf, 0, (struct sockaddr *)&from, &fl)) > 0) {
        on_query(buf, n, &from);
        fl = sizeof from;
    }
    int64_t now = esp_timer_get_time();
    if (now >= next_us) {
        announce();
        next_us = now + 2000 * 1000;
    }
}
