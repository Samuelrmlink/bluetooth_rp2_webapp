#ifndef BTGW_PEER_FWD_MSG_H
#define BTGW_PEER_FWD_MSG_H

/* Envelope rewrite for the one-hop peer forwarder. `node` counts only when
 * it is the first key. The forwarded frame has no `node`, so the far board
 * runs it locally and does not forward again. */

#include <stddef.h>

/* 1: a leading node name was copied. 0: no leading node, or it is empty
 * (both mean this board). -1: a leading node that is not a hostname. */
int peer_msg_node(const char *text, char *name, size_t cap);

/* 1 and *id set when the first key is "id". 2 when the first key is "evt".
 * 0 for anything else. -1 when that key is present but the value is bad. */
int peer_msg_kind(const char *text, long *id);

/* The id that follows a leading node. 1 on success, -1 when it is missing. */
int peer_msg_request_id(const char *text, long *id);

/* Drop the leading node and write the same object with this id. */
int peer_msg_forward(const char *text, unsigned peer_id, char *dst, size_t cap);

/* Put node first. replace_id rewrites a leading id to browser_id; an event
 * (replace_id 0) keeps the rest of the object. */
int peer_msg_tag(const char *text, const char *name, long browser_id, int replace_id,
                 char *dst, size_t cap);

#endif
