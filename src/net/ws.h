#ifndef BTGW_WS_H
#define BTGW_WS_H

/* Largest frame we will send. The JSON API's replies (a GATT tree, a scan
 * listing) are far larger than the ASCII lines this code was written for,
 * where 512 bytes was ample -- and an oversized frame is silently DROPPED by
 * ws_encode_text_frame rather than truncated, which presents as an op that
 * simply never answers. */
#define WS_TX_MAX 4096
#define WS_RX_MAX 4096

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Minimal RFC6455 WebSocket support — just enough for our own
 * js/ws-client.js: text frames only, no fragmentation, no ping/pong, no
 * permessage-deflate. New code, not part of the pm220-pico2w reference
 * (it has no WebSocket) — the ASCII line protocol our browser app already
 * speaks needs a *real* WS server here, since in this architecture the
 * device itself is the backend (no PC-side bridge in production). */

/* Computes the Sec-WebSocket-Accept header value (base64(SHA1(key + the
 * RFC6455 magic GUID))) for the handshake response. `key` is the
 * Sec-WebSocket-Key header value as sent by the client (not
 * NUL-terminated requirement — pass its exact length via key_len).
 * `out` must be at least 29 bytes (28 base64 chars + NUL for a 20-byte
 * SHA-1 digest). */
void ws_accept_key(const char *key, size_t key_len, char *out, size_t out_cap);

/* Encodes one text frame (unmasked, server->client — RFC6455 never
 * requires the server to mask) into `out`. Returns the frame length, or 0
 * if `out_cap` is too small. Payloads over 65535 bytes are not supported
 * (not needed for our line-at-a-time protocol). */
size_t ws_encode_text_frame(const uint8_t *payload, size_t len, uint8_t *out, size_t out_cap);

/* Incremental client->server frame parser — client frames are always
 * masked (RFC6455 requirement); feed it raw bytes as they arrive and it
 * calls `on_text` once per complete text-frame payload (unmasked
 * in-place). Returns the number of bytes consumed from `data` (always
 * `len` unless a malformed/oversized frame forces the connection closed,
 * signaled via *closed = true). */
typedef void (*ws_on_text_fn)(const uint8_t *payload, size_t len, void *user);

typedef struct {
    uint8_t hdr[14];
    size_t hdr_got;
    size_t hdr_need;
    uint8_t mask[4];
    size_t payload_len;
    size_t payload_got;
    /* Sized for a script.draft, which is the largest thing a client sends --
     * an API request is a couple of hundred bytes. This is per-connection
     * state, so it is HTTP_CONNS times this much RAM; 4 KB x 8 = 32 KB of the
     * RP2350's 520 KB, which is worth it to avoid silently truncating source
     * code on the way in. */
    uint8_t payload[WS_RX_MAX];
    bool in_payload;
} ws_parser_t;

void ws_parser_reset(ws_parser_t *p);
size_t ws_parser_feed(ws_parser_t *p, const uint8_t *data, size_t len, ws_on_text_fn on_text,
                      void *user, bool *closed);

#endif
