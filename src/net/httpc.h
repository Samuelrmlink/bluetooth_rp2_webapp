/* An HTTP/1.1 client on raw lwIP TCP, with TLS (mbedTLS) for https://.
 *
 * Asynchronous and callback-driven, run from the main loop (lwIP callbacks
 * and httpc_tick): nothing here blocks. The Lua `http` module (lhttp.c) is
 * its one user and supplies the sink -- where the response headers and body
 * go -- and the memory: every request, and every TLS session, is allocated
 * through httpc_alloc_fns, which the Lua glue points at the top of the Lua
 * arena. Nothing is static beyond the slot table and one shared TLS config.
 *
 * One request is one connection ("Connection: close"): DNS, connect, TLS
 * handshake, send, receive, follow up to `follow` redirects (each a new
 * connection), done. The route is whatever lwIP's routing says for the
 * destination -- the active uplink (Wi-Fi, Ethernet, USB) via uplink.c.
 */
#ifndef BTGW_HTTPC_H
#define BTGW_HTTPC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HTTPC_MAX 3            /* requests at once (each TLS one ~35 KB of arena at peak) */

typedef enum {
    HTTPC_DNS = 0, HTTPC_CONNECT, HTTPC_TLS, HTTPC_SEND, HTTPC_RECV, HTTPC_DONE, HTTPC_ERROR,
} httpc_state_t;

/* Error codes, as the Lua API names them. */
typedef enum {
    HTTPC_OK = 0, HTTPC_EDNS, HTTPC_ECONNECT, HTTPC_ETLS, HTTPC_ETIMEOUT, HTTPC_ETOOLARGE,
    HTTPC_EIO, HTTPC_ECANCELLED, HTTPC_ENOMEM, HTTPC_EPROTO,
} httpc_err_t;
const char *httpc_err_name(httpc_err_t e);
const char *httpc_state_name(httpc_state_t s);

typedef struct {
    /* The FINAL response's headers (a redirect's are not passed on). */
    void   (*on_status)(void *user, int status);
    void   (*on_header)(void *user, const char *name, const char *value);
    /* How many body bytes the sink can take right now. 0 stops reading (TCP
     * flow control pushes back on the server) until the next httpc_tick.
     * NOT consulted for the piece that ends the headers: up to 512 bytes of
     * body may arrive with them, before room() has been asked -- a sink must
     * be able to take that much as soon as on_status has run. */
    size_t (*room)(void *user);
    void   (*on_body)(void *user, const uint8_t *data, size_t n);
    /* Exactly once: done (err HTTPC_OK) or failed. */
    void   (*on_done)(void *user, httpc_err_t err, const char *msg);
} httpc_sink_t;

typedef struct {
    const char *method;        /* "GET" if NULL */
    const char *url;
    const char *headers;       /* "Name: value\r\n" lines, or NULL */
    const uint8_t *body;
    size_t body_len;
    uint32_t timeout_ms;       /* the whole request, redirects included */
    uint8_t follow;            /* redirects to follow */
    bool insecure;             /* https without certificate verification */
} httpc_opts_t;

typedef struct {
    void *(*alloc)(size_t n);  /* zeroed */
    void  (*free)(void *p);
    void  (*stat)(size_t *used, size_t *largest_free);   /* for diagnostics; may be NULL */
} httpc_alloc_fns;
void httpc_set_alloc(const httpc_alloc_fns *fns);

/* -1 when no slot is free or memory ran out (*err says which). The strings in
 * opts are copied. The sink's callbacks start on a later pass, never inside
 * this call. */
int  httpc_start(const httpc_opts_t *opts, const httpc_sink_t *sink, void *user, httpc_err_t *err);
/* Stops it; on_done is NOT called. */
void httpc_cancel(int id);
/* Slot reuse is safe: a cancelled or finished id stops matching. */
bool httpc_info(int id, httpc_state_t *state, uint64_t *bytes, int64_t *total, int *status);
void httpc_tick(void);
/* Drop everything without callbacks (the Lua VM is restarting). */
void httpc_reset(void);

/* For sys/http stats: TLS sessions now, and the most arena any one used. */
typedef struct { uint32_t tls_now, tls_peak_bytes, tls_bytes_now, requests, failures, tls_reserved; } httpc_stats_t;
void httpc_stats(httpc_stats_t *out);
/* Give back the reserved TLS record buffers if no session is using them
 * (the Lua runtime's out-of-memory hook). True if anything was freed. */
bool httpc_release_reserve(void);
/* http.reserve(): reserve the record buffers now / give them back (false
 * while a session uses them). */
bool httpc_reserve(void);
bool httpc_unreserve(void);

#endif
