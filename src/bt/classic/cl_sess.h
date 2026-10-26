#ifndef BTGW_CL_SESS_H
#define BTGW_CL_SESS_H
/*
 * Classic RFCOMM sessions, with no BTstack types.
 *
 * One session is one channel, the caller's security policy, and that
 * session's buffers. Several modules can hold several sessions. The radio
 * side (spp.c) pages, runs SDP, and feeds pairing events in. This file is
 * the rules those events have to follow, so the host test can compile it.
 *
 * A PIN is never invented here. A list passed at open is answered in order.
 * Anything past that list stays outstanding until submit on that handle.
 */
#include <stddef.h>
#include <stdint.h>

#ifdef CONFIG_BT_MAX_LINKS
#define CL_SESS_MAX CONFIG_BT_MAX_LINKS
#else
#define CL_SESS_MAX 4
#endif

#define CL_DEV_MAX 16
#define CL_PIN_MAX 4
#define CL_PIN_LEN 16
#define CL_NAME_LEN 32
#define CL_RX_CAP 1024
#define CL_TX_CAP 512
#define CL_LISTEN_MAX 252

/* Same numbers as the PEX codes the callers return. */
#define CL_OK 0
#define CL_EINVAL (-1)
#define CL_ENOMEM (-2)
#define CL_EBUSY (-10)
#define CL_ENOENT (-11)
#define CL_ESTATE (-18)
#define CL_RETRY 1

#define CL_LEVEL_DEFAULT 2
#define CL_LEVEL_OMIT 0xFF

enum {
    CL_IO_NONE = 0,
    CL_IO_DISPLAY,
    CL_IO_YESNO,
    CL_IO_KEYBOARD,
};

enum {
    CL_BOND_NONE = 0,
    CL_BOND_GENERAL,
    CL_BOND_DEDICATED,
};

enum {
    CL_FREE = 0,
    CL_OPENING,
    CL_OPEN,
    CL_CLOSING,
    CL_FAILED,
};

enum {
    CL_ASK_NONE = 0,
    CL_ASK_PIN,
    CL_ASK_PASSKEY,
    CL_ASK_CONFIRM,
};

enum {
    CL_PHASE_NONE = 0,
    CL_PHASE_PAGE,
    CL_PHASE_SDP,
    CL_PHASE_RFCOMM,
    CL_PHASE_UP,
    CL_PHASE_DEDICATED,
};

enum {
    CL_WHY_NONE = 0,
    CL_WHY_TIMEOUT,
    CL_WHY_CANCEL,
    CL_WHY_AUTH,
    CL_WHY_SECURITY,
    CL_WHY_SDP,
    CL_WHY_RFCOMM,
    CL_WHY_PAGE,
    CL_WHY_CLOSED,
    CL_WHY_REMOTE,
    CL_WHY_DEDICATED,
};

enum {
    CL_EV_STATE = 1,
    CL_EV_PIN = 2,
    CL_EV_PASSKEY = 4,
    CL_EV_CONFIRM = 8,
    CL_EV_RX = 16,
    CL_EV_TX_ROOM = 32,
};

typedef struct {
    uint8_t addr[6];
    uint16_t uuid;
    uint8_t channel;          /* 0: look uuid up */
    uint8_t fallback;         /* 0: no fallback channel */
    uint8_t level;            /* 0..4, or CL_LEVEL_OMIT */
    uint8_t io;               /* CL_IO_* */
    uint8_t bond;             /* CL_BOND_*, or 0xFF for general */
    uint8_t drop_key;
    uint8_t watch;
    uint8_t npin;
    uint32_t deadline_ms;     /* 0: no connect deadline */
    uintptr_t owner;          /* 0: a websocket op, not a module */
    uintptr_t cb;             /* module callback, or 0 */
    uintptr_t cb_arg;
    char pin[CL_PIN_MAX][CL_PIN_LEN];
} cl_open_t;

typedef struct {
    uint8_t addr[6];
    char name[CL_NAME_LEN];
    int8_t rssi;
    uint32_t cod;
} cl_dev_t;

typedef struct {
    int handle;
    uintptr_t owner;
    uintptr_t cb;
    uintptr_t cb_arg;
    uint8_t addr[6];
    uint16_t uuid;
    uint8_t channel;
    uint8_t fallback;
    uint8_t level;
    uint8_t io;
    uint8_t bond;
    uint8_t state;
    uint8_t ask;
    uint8_t phase;
    uint8_t secure;
    uint8_t watch;
    uint8_t drop_key;
    uint8_t retried;
    uint8_t key_known;
    uint8_t key_auth;
    uint8_t key_sc;
    int why;
    uint16_t mtu;
    uint16_t cid;
    uint16_t acl;
    uint32_t cod;
    uint32_t rx_drop;
    uint32_t confirm_num;
    char error[48];
} cl_info_t;

void cl_sess_reset(void);

/* Handle (> 0) or a CL_E*. No-input/no-output with level 3 or 4 is EINVAL
 * and takes no slot. A different IO mode while another opening session
 * still holds the capability exchange is EBUSY. */
int cl_open(const cl_open_t *opt, uint32_t now_ms);
int cl_close(int handle);
void cl_reap(int handle);
int cl_release_owner(uintptr_t owner);
int cl_io_done(int handle);

/* 1: *out is the next preloaded PIN. 0: this handle is waiting. */
int cl_pin_pull(int handle, char *out, size_t cap);
/* After a rejected answer: 1 if another preloaded PIN remains, 0 if the
 * next request is now outstanding. */
int cl_note_pin_rejected(int handle);
int cl_passkey_pull(int handle, uint32_t *out);
int cl_confirm_ask(int handle, uint32_t number);

int cl_submit_pin(int handle, const char *pin);
int cl_submit_passkey(int handle, int negative, uint32_t passkey);
int cl_submit_confirm(int handle, int accept);
/* 1 if a reply is waiting for the controller. kind is CL_ASK_*. */
int cl_take_answer(int handle, int *kind, int *negative, char *pin, size_t pin_cap,
                   uint32_t *num);

/* 1: drop the key and run the same policy once more. 0: the session failed. */
int cl_auth_fail(int handle);
/* CL_OK, CL_RETRY, or CL_ESTATE. OPEN only when the level is met and RFCOMM is up. */
int cl_security(int handle, int got_level, int authenticated, int sc, int final);
int cl_rfcomm_up(int handle, uint16_t mtu);
int cl_set_channel(int handle, uint8_t channel);
int cl_set_acl(int handle, uint16_t acl);
int cl_set_cid(int handle, uint16_t cid);
int cl_set_phase(int handle, int phase);
int cl_set_key(int handle, int known, int authenticated, int sc);
int cl_fail(int handle, int why, const char *err);
int cl_mark_closed(int handle, int why, const char *err);
void cl_tick(uint32_t now_ms);

int cl_tx_put(int handle, const void *data, size_t n);
int cl_tx_room(int handle);
int cl_tx_eligible(int handle);
int cl_tx_next(void);
int cl_tx_peek(int handle, const uint8_t **ptr, size_t max);
void cl_tx_advance(int handle, size_t n);

int cl_rx_put(int handle, const void *data, size_t n);
int cl_rx_peek(int handle, uint8_t *buf, size_t n);
int cl_listen(int handle, void *buf, size_t n);

int cl_inq_start(void);
void cl_inq_stop(void);
int cl_inq_active(void);
int cl_inq_note(const uint8_t *addr, int8_t rssi, uint32_t cod, const char *name);
int cl_inq_set_name(const uint8_t *addr, const char *name);
int cl_inq_count(void);
int cl_inq_get(int index, cl_dev_t *out);
int cl_inq_filter(uint32_t cod_mask, uint32_t cod_value, const char *prefix,
                  cl_dev_t *out, int max);
int cl_inq_next_dirty(cl_dev_t *out);

int cl_info(int handle, cl_info_t *out);
int cl_handles(int *out, int max);
int cl_find_addr(const uint8_t *addr, int *out, int max);
int cl_radio_setup(void);
/* IO mode held by an unfinished capability exchange, or -1. */
int cl_io_held(void);
int cl_take_ws(int handle);
int cl_take_mod(int handle);
/* Module events still queued. The radio waits to free the slot until this is 0. */
int cl_mod_pending(int handle);
int cl_set_watch(int handle, int on);
int cl_addr_held(const uint8_t *addr);

const char *cl_state_name(int state);
const char *cl_why_name(int why);

#endif
