#ifndef KM003C_USB_NCM_H
#define KM003C_USB_NCM_H

#include <stddef.h>

struct netif;

void usb_ncm_init(void);
void usb_ncm_poll(void);
void usb_ncm_ensure_mac(void);
void usb_ncm_log(void);
/* One line of NCM health for /api/debug/crash. Readable over WiFi when
 * NCM itself is the thing that has stopped working. */
int usb_ncm_debug_report(char *buf, size_t cap);
/* Call from the main loop: re-presents the USB disconnect if the host
 * never enumerated us, which a warm reset can cause. No-op once
 * configured. */
void usb_ncm_recover_tick(void);

/* Counters for the MCUmgr stat group (src/ota/stat_mgmt.c). */
#include <stdint.h>
typedef struct { uint32_t rx_ok, rx_drop, tx_ok, tx_drop, recoveries; } usb_ncm_stats_t;
void usb_ncm_stats(usb_ncm_stats_t *out);
struct netif *usb_ncm_netif(void);

#endif
