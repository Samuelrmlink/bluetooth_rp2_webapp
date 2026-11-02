#ifndef C5_WIFI_STA_H
#define C5_WIFI_STA_H

#include <stdbool.h>
#include <stddef.h>

void wifi_start_from_cfg(void);
void wifi_apply_cfg(void);
/* Drop a 169.254 station address and keep the DHCP client running.
 * Called from the poll task. */
void wifi_dhcp_poll(void);
bool wifi_has_addr(void);
int wifi_ip(char *dst, size_t cap);
const char *wifi_current_ssid(void);
bool wifi_scanning(void);
/* Join one stored network now. psk NULL uses the stored passphrase.
 * A new pair is written into cfg/wifi.json first. */
bool wifi_join(const char *ssid, const char *psk);
bool wifi_scan_start(void);
int wifi_status_json(char *buf, size_t cap);
int wifi_scan_json(char *buf, size_t cap);

#endif
