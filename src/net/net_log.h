#ifndef BTGW_NET_LOG_H
#define BTGW_NET_LOG_H

/* Diagnostic lines for network stalls.
 *
 * net_log() only printf's and copies into a RAM ring. It is safe from a TCP
 * callback. net_log_poll() runs on the main loop and, when the SD card is in
 * the build, appends the ring to log/net.log. That file is the record that
 * survives a reboot: pull the card. It is never written to LittleFS. A log
 * write must not stall QSPI XIP or spend internal flash.
 */
void net_log(const char *fmt, ...);
void net_log_poll(void);

#endif
