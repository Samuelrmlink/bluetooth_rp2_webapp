#ifndef BTGW_API_H
#define BTGW_API_H

#include <stdbool.h>
#include <stddef.h>

/* True when THIS boot followed a watchdog reset, i.e. the previous boot's main
 * loop stopped running. Set once in main() and reported by sys.info, because a
 * device reached over WireGuard has no RTT for anyone to read. */
extern bool boot_after_watchdog;

/* Reply sink: exactly http.c's ws_reply_line, passed in so the API layer never
 * includes the transport. */
typedef void (*api_reply_fn)(const char *line, void *ctx);

/* One inbound WebSocket text frame. Parses the envelope, dispatches to the
 * module registry, and writes a reply through `reply`. */
void api_handle_text(const char *text, api_reply_fn reply, void *ctx);

/* Emit an unsolicited event to every subscribed client:
 *   {"evt":"<topic>","data":<json>}
 * `json` must already be a valid JSON value. */
void api_emit(const char *topic, const char *json);

/* Escape a string INTO a JSON string body (no surrounding quotes), returning
 * the number of bytes written. Device names and script errors are
 * attacker-controlled in a way an SSID is not, so nothing user-supplied may be
 * concatenated into JSON without passing through here. */
size_t api_json_escape(char *dst, size_t cap, const char *src);

/* One shared scratch buffer for building large JSON results (a GATT tree, a
 * scan listing, a script listing). Safe to share because every caller runs
 * from the single cooperative loop, one at a time, and each fills and hands it
 * off within the call -- the same reasoning that makes these buffers static in
 * the first place. Four private copies of this size would cost ~36 KB for no
 * benefit. */
char  *api_scratch(void);
size_t api_scratch_size(void);

#endif
