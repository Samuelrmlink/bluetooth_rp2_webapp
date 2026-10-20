/* The HTTP client's pure parts, testable on a host (test/host/httpc_parse_test.c):
 *
 *   httpp_*         an incremental HTTP/1.1 response parser. Bytes go in as
 *                   they arrive, split anywhere; headers come out one at a
 *                   time with lower-cased names, the body as it decodes --
 *                   Content-Length, chunked (extensions and trailers
 *                   skipped), or read-until-close.
 *   httpc_url_parse split http(s)://host[:port]/path?query
 *   httpc_url_resolve  a Location header against the URL that sent it
 *                   (absolute, scheme-relative, absolute-path, relative)
 *
 * Nothing here allocates. */
#ifndef BTGW_HTTPC_PARSE_H
#define BTGW_HTTPC_PARSE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HTTPP_LINE 768        /* a longer header line is cut, not refused */

typedef void (*httpp_header_fn)(void *ctx, const char *name, const char *value);
typedef void (*httpp_body_fn)(void *ctx, const uint8_t *data, size_t n);

typedef struct {
    int      state;
    char     line[HTTPP_LINE];
    uint16_t line_len;
    bool     line_cut;         /* the current line overflowed HTTPP_LINE */
    bool     head;             /* the request was HEAD: no body, whatever the headers say */
    int      status;           /* 0 until the status line */
    int64_t  content_length;   /* -1 = not given */
    bool     chunked;
    bool     keep_alive;
    uint64_t left;             /* bytes left in the body / the current chunk */
    uint64_t body_bytes;       /* decoded body bytes delivered */
    bool     done, error;
    const char *err;
    httpp_header_fn on_header;
    httpp_body_fn   on_body;
    void    *ctx;
} httpp_t;

void httpp_init(httpp_t *p, bool head_request, httpp_header_fn h, httpp_body_fn b, void *ctx);
/* Consumes up to n bytes and returns how many: all of them, unless the
 * response ended (done) or failed (error) part way -- the rest is not ours. */
size_t httpp_feed(httpp_t *p, const uint8_t *data, size_t n);
/* The connection closed. Ends a read-until-close body; anything else not yet
 * done becomes an error ("closed early"). */
void httpp_eof(httpp_t *p);
/* After the headers: the body is known to be empty, or exactly this long. */
bool httpp_headers_done(const httpp_t *p);

typedef struct {
    bool     tls;
    char     host[128];
    uint16_t port;
    const char *path;          /* into the URL: "/..." (a bare host gives "/") */
    size_t   path_len;
} httpc_url_t;

/* False for anything that is not http:// or https:// with a host. A literal
 * IPv6 host ("[::1]") is refused: the stack is IPv4-only. */
bool httpc_url_parse(const char *url, httpc_url_t *u);

/* `loc` (a Location header) against `base` into out[cap]; false if it does
 * not fit or is not something a client may follow. */
bool httpc_url_resolve(const char *base, const char *loc, char *out, size_t cap);

#endif
