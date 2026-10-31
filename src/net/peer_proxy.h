#ifndef BTGW_PEER_PROXY_H
#define BTGW_PEER_PROXY_H

#include <stdbool.h>
#include <stddef.h>
#include "lwip/tcp.h"
#include "lwip/pbuf.h"

/* True when path is /ws/peer/<name> or /api/peer/<name>/... */
bool peer_proxy_wants(const char *path);

/* Copies what it needs from hdr and the first pbuf. On success the proxy
 * owns the TCP callbacks and the caller keeps the connection slot. On
 * failure a response has already been written and the pcb closed. */
bool peer_proxy_begin(struct tcp_pcb *front, const char *hdr, const char *method,
                      const char *path, struct pbuf *first, size_t body_off);

#endif
