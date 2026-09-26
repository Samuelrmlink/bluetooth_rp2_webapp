#ifndef BTGW_HTTP_H
#define BTGW_HTTP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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

/* Frames this client lost to a full send buffer. Surfaced rather than silently
 * dropped, because a stalled link otherwise looks like missing data. */
uint32_t http_conn_dropped(void *ctx);

int http_client_count(void);

#endif
