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

/*
 * NO auto-buy. A try-before-you-buy image is committed ONLY by an explicit
 * `image state-write --confirm` over MCUmgr (img_mgmt.c -> ota_buy()).
 *
 * Why not buy on a boot-health gate: the buy is an interrupts-off flash
 * blackout of tens-to-hundreds of ms, and firing it a few hundred ms into
 * the boot (WiFi join + BT enable + WireGuard handshake all in flight)
 * intermittently desynced the shared CYW43 host interface and wedged the
 * radio until a power-cycle. An operator-driven confirm moves the buy well
 * clear of that storm AND only commits firmware the operator has verified is
 * reachable. Holding "buy pending" open indefinitely is supported by the
 * silicon: the ROM's 16.7 s TBYB deadline IS the watchdog, and main() already
 * owns and feeds an 8 s watchdog every loop pass, so a healthy image runs
 * pending until confirmed. A reboot before the confirm -- a hang that stops
 * feeding the watchdog, a power-cycle, or any normal reboot -- reverts to the
 * previous slot, which stays intact until the buy. (RP2350 datasheet 5.1.17.)
 */
void ota_tick(void) {
    uint32_t now = to_ms_since_boot(get_absolute_time());
    static bool announced;
    if (!announced && ota_buy_pending()) {
        announced = true;
        int me = ota_slot_active();
        printf("ota: running a try-before-you-buy image (slot %c) -- awaiting "
               "'image state-write --confirm'; any reboot before that reverts\n",
               me < 0 ? '?' : (char)("AB"[me]));
    }
    if (req_pending) {
        size_t n = smp_process(req_buf, req_len, rsp_buf, sizeof rsp_buf);
        if (n) send_rsp(n);
        req_pending = false;
    }
    smp_os_tick(now);
    smp_fs_tick(now);
}
