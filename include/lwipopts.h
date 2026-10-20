/* lwIP config — adapted from the confirmed-working reference at
 * /home/samuel/Projects/pm220-pico2w/include/lwipopts.h. Dropped the
 * cyw43/Wi-Fi-AP-specific bits (IGMP was only there for the AP's own
 * multicast needs); kept the mDNS responder (single service, `.local`
 * name) since it's essentially free and a nice dev convenience, matching
 * this project's existing web/ dev tooling ethos. */
#ifndef _LWIPOPTS_H
#define _LWIPOPTS_H

/* Build options come from Kconfig; this header is compiled inside a
 * third-party translation unit, so it needs them explicitly. */
#include "app_config.h"

#define NO_SYS                      1
#define LWIP_SOCKET                 0
#define LWIP_NETCONN                0
#define MEM_LIBC_MALLOC             0
#define MEM_ALIGNMENT               4
/* See the MEASURED note further down for why PBUF_POOL_SIZE is halved and
   MEM_SIZE is not. */
#define MEM_SIZE                    ((CONFIG_LWIP_MEM_SIZE_KB) * 1024)
#define MEMP_NUM_TCP_SEG            48
#define MEMP_NUM_TCP_PCB            40
#define MEMP_NUM_TCP_PCB_LISTEN     8   /* + forwarding rules (src/net/fwd.c) */
#define MEMP_NUM_ARP_QUEUE          10
#define PBUF_POOL_SIZE              24
#define LWIP_ARP                    1
#define LWIP_ETHERNET               1
#define LWIP_ICMP                   1
#define LWIP_RAW                    1
#define TCP_MSS                     1460
#define TCP_WND                     (4 * TCP_MSS)
#define TCP_SND_BUF                 (4 * TCP_MSS)
#define TCP_MSL                     2000
#define TCP_SND_QUEUELEN            ((4 * (TCP_SND_BUF) + (TCP_MSS - 1)) / (TCP_MSS))
#define LWIP_NETIF_STATUS_CALLBACK  1
#define LWIP_NETIF_LINK_CALLBACK    1
#define LWIP_NETIF_HOSTNAME         1
/* On: a few dozen bytes for lwip_stats.mem/memp, reported by
   /api/debug/crash, so MEM_SIZE's headroom is measured rather than guessed
   before anything is taken from it. */
#define MEM_STATS                   1
#define SYS_STATS                   0
#define MEMP_STATS                  1
#define LINK_STATS                  0
#define LWIP_CHKSUM_ALGORITHM       3
#define LWIP_DHCP                   1
#define LWIP_IPV4                   1
#define LWIP_IPV6                   0
#define LWIP_TCP                    1
#define LWIP_UDP                    1
#define MEMP_NUM_UDP_PCB            16  /* + forwarding listeners and UDP sessions */
#define CHECKSUM_CHECK_UDP          0
#define CHECKSUM_CHECK_IP           0
#define IP_SOF_BROADCAST            1
#define IP_SOF_BROADCAST_RECV       0
/* No SO_REUSE here, on purpose. A LWIP_SO_REUSE line sat in this file for a
 * long time and was never an lwIP symbol (lwIP's is SO_REUSE, default 0), so it
 * did nothing while looking like it allowed two DHCP servers to share :67. They
 * share it by binding to their own addresses instead -- see dhcpserver.c. */
#define ETHARP_SUPPORT_STATIC_ENTRIES 1
#define DHCP_OPTIONS_LEN            312
/* DNS for the Lua HTTP client (src/net/httpc.c) and SNTP. Servers come
   from DHCP (Wi-Fi, Ethernet); uplink.c falls back to the active uplink's
   gateway when DHCP named none (USB-NCM). A 4-entry table and a UDP pcb. */
#if CONFIG_NET_HTTP_CLIENT || CONFIG_NET_SNTP
#define LWIP_DNS                    1
#define DNS_TABLE_SIZE              4
#define DNS_MAX_NAME_LENGTH         96   /* firebasestorage.googleapis.com is 30 */
#define DNS_MAX_SERVERS             2
#else
#define LWIP_DNS                    0
#endif
/* SNTP (src/net/sntp_time.c): sys.time() and TLS certificate dates. */
#if CONFIG_NET_SNTP
#define SNTP_SERVER_DNS             1
#define SNTP_MAX_SERVERS            1
#define SNTP_UPDATE_DELAY           3600000
#include <stdint.h>
void sntp_time_set_us(uint32_t sec, uint32_t us);
#define SNTP_SET_SYSTEM_TIME_US(sec, us) sntp_time_set_us((uint32_t)(sec), (uint32_t)(us))
#endif
#define LWIP_TCP_KEEPALIVE          1
#define LWIP_NETIF_TX_SINGLE_PBUF   1
#define DHCP_DOES_ARP_CHECK         0
#define LWIP_DHCP_DOES_ACD_CHECK    0
/* Link-local (AutoIP, RFC 3927) for the Ethernet port when no DHCP server
   answers. It pulls in ACD (address conflict detection, which AutoIP needs);
   the DHCP client's own ACD check above stays off, so WiFi and USB-NCM DHCP
   behave exactly as before. LWIP_DHCP_AUTOIP_COOP is deliberately NOT used:
   it is global, and would start AutoIP on the WiFi station too whenever its
   DHCP was slow. src/net/eth_w5500.c cooperates for e0 alone instead. */
#if CONFIG_NET_ETH_W5500
#define LWIP_AUTOIP                 1
#endif

/* mDNS multicasts to 224.0.0.251 — needs IGMP group membership on the
 * netif to actually receive queries, not just an AP-multicast leftover. */
#define LWIP_IGMP                   1
#define LWIP_MDNS_RESPONDER         1
#define LWIP_MDNS_SEARCH            0
#define MDNS_MAX_SERVICES           4
/* mDNS claims one slot per consumer; with WiFi on, the cyw43 STA and AP
   netifs want their own. Set here rather than as a -D, because every value
   in this file is an unconditional #define and would silently win over a
   command-line definition — which is how the first attempt at retuning the
   pools below did nothing at all while appearing to work. */
#if CONFIG_NET_WIFI
#define LWIP_NUM_NETIF_CLIENT_DATA  2
#else
#define LWIP_NUM_NETIF_CLIENT_DATA  1
#endif

/* MEASURED, so it doesn't have to be guessed again: enabling WiFi costs only
   ~2.6KB of static RAM (200KB -> 203KB of data+bss) — the CYW43 firmware
   blob is ~234KB but lives in flash, of which there is 1.6MB spare.
   PBUF_POOL_SIZE 48 and MEM_SIZE 48K are together ~123KB, inherited from an
   RP2350 reference with twice this part's SRAM.

   That note used to end by saying halving them was the lever to pull if
   Phase 2 or Phase 3 ran out of room, and that it was deliberately not being
   pulled yet. Phase 2 ran out of room. The failure is worth recording because
   it does not mention lwIP anywhere: the board panics in km_ipc_init(), on
   the FOURTH queue_init(), because these pools are static and grow .bss
   downwards into the heap that km_ipc's ~32KB of queues is allocated from.
   .bss ended at 0x20037b04 leaving 34,044 bytes, and km_ipc needs about
   2.5KB more than that. Nothing in the backtrace points at networking.

   So: PBUF_POOL_SIZE is halved, freeing ~36KB — far more than the ~3KB
   that was actually needed.

   It was halved only under CONFIG_NET_WIFI at first, on the reasoning that the
   non-WiFi build had the room and could stay bit-for-bit unaffected. It
   did not keep the room: .bss went on growing with the multi-meter work
   and the CONFIG_NET_WIFI=OFF build crossed the same cliff, down to 4KB of heap
   against the ~26KB km_ipc_init() needs. It had been unbootable for some
   time before anyone flashed it, because the failure is invisible — no
   console, no network, nothing in the fault record. There is now a
   link-time check (tools/check_ram_budget.py, run from CMakeLists) that
   prints the margin on every build and fails below what boots.

   MEM_SIZE was left alone at first. Only one of the two had to move, and
   PBUF_POOL is the RX path while MEM_SIZE backs TX — this device almost
   entirely sends (a WebSocket sample stream), so the receive pool is where
   the slack was. Measured since, at 1000 SPS with two meters streaming and
   the whole web asset set being uploaded over the same link: pbuf_pool
   peaks at 2 of 24, and MEM_SIZE at 20.6KB of 48KB.

   Then the multi-meter work needed the room, and MEM_SIZE became the
   next place with slack in it: the CONFIG_NET_WIFI=ON build now takes 32KB
   (1.5x the measured peak; CONFIG_LWIP_MEM_SIZE_KB, chosen in CMakeLists.txt)
   so that it can drive more than three meters, and the CONFIG_NET_WIFI=OFF build
   keeps 48KB because it has the room. Both figures appear on the budget
   line every build prints. */
#define LWIP_NETIF_EXT_STATUS_CALLBACK 1

/* Source routing for uplink selection (src/net/uplink.c): a connection keeps
   leaving through the interface that owns its source address while that
   interface is up, so switching the default route moves only NEW
   connections. Declared with struct tags because this header is read before
   lwIP's own types exist. */
/* Forwarding to 127.0.0.1 or to one of our own addresses (net.forward to a
   local port) goes through lwIP's loopback; fwd_tick() polls it. */
#if CONFIG_NET_FORWARD
#define LWIP_NETIF_LOOPBACK         1
#define LWIP_HAVE_LOOPIF            1
#endif

#if CONFIG_NET_UPLINK
struct netif;
struct ip4_addr;
struct netif *uplink_route_src(const struct ip4_addr *src, const struct ip4_addr *dest);
#define LWIP_HOOK_IP4_ROUTE_SRC(src, dest) uplink_route_src((src), (dest))
#endif
/* lwIP's own count covers the core protocols only. Beyond it:
     - the mDNS responder keeps up to MDNS_TIMEOUTS_PER_NETIF timeouts on
       EVERY netif it is bound to (probe/announce, the three multicast rate
       timers, delayed multicast and unicast replies), all armed at once
       while a service is re-probed -- and it is bound to up to MDNS_NETIFS
       (NCM, WiFi STA, AP, Ethernet);
     - WireGuard, the mDNS group re-announce, and a few for luck.
   MEASURED, the hard way: at +8 the pool ran dry when a script re-added an
   mDNS service while another was still announcing (two netifs), and
   sys_timeout() panics on an empty pool -- a HardFault + watchdog reboot on
   a board without a debugger. It reproduced on fw 1.97. ~16 B each. */
#define MDNS_TIMEOUTS_PER_NETIF     7
#define MDNS_TIMEOUT_NETIFS         4
/* +2: SNTP's poll/retry timer and DNS's (DNS is in the internal count, the
   SNTP app is not). */
#define MEMP_NUM_SYS_TIMEOUT        (LWIP_NUM_SYS_TIMEOUT_INTERNAL + 8 + 2 + \
                                     MDNS_TIMEOUT_NETIFS * MDNS_TIMEOUTS_PER_NETIF)

#endif
