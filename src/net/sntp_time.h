/* Wall-clock time from SNTP (lwIP's client), for sys.time() and for TLS
 * certificate dates. Until a server has answered there is no time: callers
 * get "not valid" rather than 1970.
 *
 * The server is cfg/ntp.json {"server":"..."} if present, else pool.ntp.org;
 * lwIP polls it hourly (SNTP_UPDATE_DELAY). The request goes out of whatever
 * interface routes there (the active uplink). */
#ifndef BTGW_SNTP_TIME_H
#define BTGW_SNTP_TIME_H

#include <stdbool.h>
#include <stdint.h>

void sntp_time_init(void);
bool sntp_time_valid(void);
/* Unix seconds; 0 while not valid. */
int64_t sntp_time_unix(void);
/* The server in use, for status. */
const char *sntp_time_server(void);
/* lwIP's SNTP_SET_SYSTEM_TIME_US hook (lwipopts.h). */
void sntp_time_set_us(uint32_t sec, uint32_t us);

#endif
