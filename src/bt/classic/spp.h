#ifndef BTGW_CLASSIC_SPP_H
#define BTGW_CLASSIC_SPP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "api/api.h"
#include "bt/classic/cl_sess.h"

/* Inquiry, SDP, and one RFCOMM channel per session. Called from bt_init
 * when BT_CLASSIC is on. Does not power the controller and does not start
 * an inquiry. Incoming RFCOMM is not started here. */
void classic_init(void);
/* Main loop, after the radio poll. Emits events and pumps each session's TX. */
void classic_poll(void);
/* Inquiry, paging, or SDP. An open session that is only transferring is not busy. */
bool classic_radio_busy(void);

int classic_scan_start(uint32_t duration_ms);
void classic_scan_stop(void);
int classic_open(const cl_open_t *opt);
int classic_send(int handle, const void *data, size_t n);
int classic_submit_pin(int handle, const char *pin);
int classic_submit_passkey(int handle, int negative, uint32_t passkey);
int classic_submit_confirm(int handle, int accept);
int classic_close(int handle);
int classic_forget(const uint8_t *addr);
/* One SDP lookup for a 16-bit UUID. The channel comes back from classic_sdp_take. */
int classic_sdp_start(const uint8_t *addr, uint16_t uuid);
int classic_sdp_take(uint8_t addr[6], uint16_t *uuid, uint8_t *channel, int *status);

void op_classic_scan(const char *params, api_reply_fn reply, void *ctx, long id);
void op_classic_connect(const char *params, api_reply_fn reply, void *ctx, long id);
void op_classic_state(const char *params, api_reply_fn reply, void *ctx, long id);
void op_classic_disconnect(const char *params, api_reply_fn reply, void *ctx, long id);
void op_classic_send(const char *params, api_reply_fn reply, void *ctx, long id);
void op_classic_pin(const char *params, api_reply_fn reply, void *ctx, long id);
void op_classic_passkey(const char *params, api_reply_fn reply, void *ctx, long id);
void op_classic_confirm(const char *params, api_reply_fn reply, void *ctx, long id);

#endif
