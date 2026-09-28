/*
 * SMP over UDP port 1337 -- the port MCUmgr's UDP transport uses, so
 * `smpmgr --ip <addr>`, `mcumgr --conntype udp` and mcumgr-client all work
 * unchanged. The same socket serves USB-NCM, the LAN and the WireGuard
 * tunnel: which one a request came in on decides where the reply goes.
 *
 * The receive callback does NOTHING but copy. It runs inside lwIP's input
 * path and the handlers write flash; the HTTP PUT path writes flash from
 * a callback and http.c documents what that costs. One outstanding request
 * at a time -- SMP is lock-step, every client waits for the reply before
 * sending the next frame -- and a second one arriving while the first is
 * still being processed is dropped, which the client retries.
 */
#include "ota/smp.h"
#include "ota/ota_slot.h"

#include <stdio.h>
#include <string.h>
#include "lwip/udp.h"
#include "pico/time.h"

#define SMP_PORT 1337

static struct udp_pcb *pcb;
static uint8_t req_buf[SMP_BUF];
static uint8_t rsp_buf[SMP_BUF];
static size_t req_len;
static bool req_pending;
static ip_addr_t req_addr;
static uint16_t req_port;
static uint32_t dropped;

static void on_rx(void *arg, struct udp_pcb *p, struct pbuf *pb, const ip_addr_t *addr, u16_t port) {
    (void)arg; (void)p;
    if (req_pending || pb->tot_len > sizeof req_buf) {
        dropped++;
    } else {
        req_len = pbuf_copy_partial(pb, req_buf, pb->tot_len, 0);
        req_addr = *addr;
        req_port = port;
        req_pending = true;
    }
    pbuf_free(pb);
}

static void send_rsp(size_t n) {
    struct pbuf *pb = pbuf_alloc(PBUF_TRANSPORT, (u16_t)n, PBUF_RAM);
    if (!pb) return;
    memcpy(pb->payload, rsp_buf, n);
    udp_sendto(pcb, pb, &req_addr, req_port);
    pbuf_free(pb);
}

void ota_init(void) {
    ota_slot_init();
    pcb = udp_new_ip_type(IPADDR_TYPE_ANY);
    if (!pcb) { printf("smp: no pcb\n"); return; }
    err_t err = udp_bind(pcb, IP_ANY_TYPE, SMP_PORT);
    if (err != ERR_OK) { printf("smp: bind :%d err %d\n", SMP_PORT, err); return; }
    udp_recv(pcb, on_rx, NULL);
    printf("smp: listening on udp :%d (os, image, stat, fs)\n", SMP_PORT);
}

/* The boot-health gate for a try-before-you-buy image. The ROM's own
 * ~16.7 s watchdog is running from reset; our init takes ~2 s, so buying on
 * the 50th loop pass (a few hundred ms after the HTTP server is up) leaves
 * a wide margin and proves the loop runs. Waiting for a network condition
 * instead was rejected: a slow WireGuard re-handshake must not cost a slot. */
static uint32_t passes;

void ota_tick(void) {
    uint32_t now = to_ms_since_boot(get_absolute_time());
    if (ota_buy_pending() && ++passes == 50) {
        printf("ota: try-before-you-buy image is healthy, buying\n");
        ota_buy();
    }
    if (req_pending) {
        size_t n = smp_process(req_buf, req_len, rsp_buf, sizeof rsp_buf);
        if (n) send_rsp(n);
        req_pending = false;
    }
    smp_os_tick(now);
    smp_fs_tick(now);
}
