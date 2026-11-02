#ifndef C5_PEER_FWD_H
#define C5_PEER_FWD_H

#include <stdbool.h>

#include "esp_http_server.h"

/* Browser text whose first key is "node" is forwarded one hop. Empty node,
 * no node, or this board's own hostname returns false so the caller runs
 * the frame locally. True means the frame was forwarded or answered. */
bool peer_fwd_handle(const char *text, int browser_fd);

/* 300 ms dial abort, and calls with no reply freed after about 10 s. */
void peer_fwd_poll(void);

void peer_fwd_bind(httpd_handle_t server);

/* One local op to a named peer on the same socket and id map. The bytes on
 * the wire have no node. If this returns true, fn is called exactly once,
 * on the httpd task, with the reply JSON (also with no node). The pointer
 * is only valid for that call. On false, fn is not called and *why is a
 * short literal when why is non-NULL. Do not call this from the lwIP tcpip
 * thread. */
typedef void (*peer_fwd_on_reply)(const char *json, void *user);

bool peer_fwd_call(const char *name, const char *local_json,
                   peer_fwd_on_reply fn, void *user, const char **why);

#endif
