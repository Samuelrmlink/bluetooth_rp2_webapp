#ifndef BTGW_HTTP_H
#define BTGW_HTTP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "autoconf.h"

#if CONFIG_NET_HTTP_SERVER

void http_server_start(void);

/* Send one text frame to a single WebSocket client. ctx is the opaque
 * per-connection token the API dispatcher was handed -- a conns[] slot, never a
 * raw tcp_pcb*, so it stays safely checkable even if that connection has since
 * closed and the slot was reused. */
void http_send_line(void *ctx, const char *line);

/* Fan-out to every connected WebSocket client. Events go out this way. */
void http_broadcast_line(const char *line);

/* Call once after a batch of send/broadcast calls. The per-line tcp_output()
 * was deliberately removed so lines coalesce into MSS-sized segments; without
 * this they would sit in the send buffer until something else flushed them. */
void http_flush_ws(void);
/* Moves /api/sd transfers along (main loop; a no-op without CONFIG_SD). */
void http_sd_pump(void);
/* Writes the JSON reply for a batch delete the same turn the last path goes. */
void http_jobs_pump(void);

/* Frames this client lost to a full send buffer. Surfaced rather than silently
 * dropped, because a stalled link otherwise looks like missing data. */
uint32_t http_conn_dropped(void *ctx);

int http_client_count(void);

/* The peer proxy finished with a connection it had taken over. Drops the
 * HTTP slot. The caller closes the pcb. */
struct tcp_pcb;
void http_conn_release(struct tcp_pcb *pcb);

#else

static inline void http_server_start(void) {}
static inline void http_send_line(void *ctx, const char *line) {
    (void)ctx;
    (void)line;
}
static inline void http_broadcast_line(const char *line) { (void)line; }
static inline void http_flush_ws(void) {}
static inline void http_sd_pump(void) {}
static inline void http_jobs_pump(void) {}
static inline uint32_t http_conn_dropped(void *ctx) {
    (void)ctx;
    return 0;
}
static inline int http_client_count(void) { return 0; }

#endif

#endif
