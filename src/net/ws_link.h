#ifndef BTGW_WS_LINK_H
#define BTGW_WS_LINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A file chunk on the websocket. WS_RX_MAX is 1024 per connection and is
 * eight of those in SRAM, so this stays small: 480 raw bytes is about 640
 * of base64, and the JSON envelope still fits in one inbound frame.
 * WS_LINK_SND_BUF is lwIP TCP_SND_BUF (4 * MSS). The ESP32 httpd does not
 * hand that number back; it uses this constant and treats queued telemetry
 * as an empty send buffer. */
#define WS_FILE_RAW        480u
#define WS_LINK_RESERVE_MAX 4096u
#define WS_LINK_SND_BUF    5840u

/* A bulk frame (a file chunk, or its ack) may go out only when what remains
 * of the send buffer after that frame is still enough for telemetry.
 * reserve is two thirds of the buffer, never more than WS_LINK_RESERVE_MAX,
 * and never the whole buffer. The caller sends at most one bulk frame per
 * connection per turn and answers `busy` when this is false. */
static inline bool ws_link_allow_bulk(unsigned snd_buf, unsigned snd_avail, unsigned frame_len) {
    unsigned reserve;
    if (frame_len == 0 || snd_buf == 0) {
        return false;
    }
    reserve = snd_buf > 1024u ? (snd_buf * 2u) / 3u : snd_buf / 2u;
    if (reserve > WS_LINK_RESERVE_MAX) {
        reserve = WS_LINK_RESERVE_MAX;
    }
    if (reserve >= snd_buf) {
        reserve = snd_buf / 2u;
    }
    if (snd_avail < frame_len) {
        return false;
    }
    return (snd_avail - frame_len) >= reserve;
}

/* Standard base64, no newlines. encode returns the character count (not
 * including the NUL) or 0 if `out` cannot hold it. decode returns the byte
 * count, or -1 if the input is not valid base64 or does not fit. */
size_t ws_b64_encode(const uint8_t *in, size_t n, char *out, size_t cap);
int ws_b64_decode(const char *in, size_t n, uint8_t *out, size_t cap);

#endif
