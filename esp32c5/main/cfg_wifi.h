/* cfg/wifi.json, the same object the RP2350 store writes. Loading does not
 * mark the store dirty. An empty store does not join a station. */
#ifndef C5_CFG_WIFI_H
#define C5_CFG_WIFI_H

#include <stdbool.h>
#include <stddef.h>

#define CFG_MAX_NET  8
#define CFG_SSID_MAX 32
#define CFG_PSK_MAX  63

void cfg_load(void);
void cfg_tick(void);

const char *cfg_mode(void);
const char *cfg_ap_ssid(void);
const char *cfg_ap_psk(void);
int cfg_net_count(void);
bool cfg_net_at(int i, char *ssid, size_t ssid_cap, char *psk, size_t psk_cap);

bool cfg_set_mode(const char *mode);
bool cfg_set_ap(const char *ssid, const char *psk);
bool cfg_net_upsert(const char *ssid, const char *psk);
bool cfg_net_forget(const char *ssid);

int cfg_networks_json(char *buf, size_t cap, const char *current_ssid);

#endif
