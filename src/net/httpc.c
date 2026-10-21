/*
 * httpc.h on raw lwIP TCP and mbedTLS. See the header for the shape.
 *
 * Receive path: tcp_recv queues the pbufs (unacknowledged); the pump takes
 * bytes from the queue -- straight into the response parser for http://,
 * through mbedtls_ssl_read for https:// -- and tcp_recved()s them as they are
 * consumed. When the sink has no room the pump stops, the queue stays put,
 * and TCP's window closes on the server: that is the backpressure that lets
 * a slow SD card take a fast download.
 *
 * TLS: mbedtls talks to the queue through bio_recv/bio_send. The CA set is
 * src/net/ca_roots.h, searched by the trusted-CA callback (one root parsed at
 * a time, only when a chain asks for it, so the roots cost flash but no RAM
 * between handshakes). Certificate dates are checked only once the clock has
 * been set (sys.time(): SNTP); before that the date flags are cleared rather
 * than failing every handshake on a board that has not had time yet.
 */
#include "net/httpc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "autoconf.h"
#include "lwip/tcp.h"
#include "lwip/dns.h"
#include "lwip/ip_addr.h"
#include "lwip/netif.h"
#include "fs/fs.h"
#include "net/httpc_parse.h"
#include "lua/lheap.h"
#include "pico/time.h"

#if CONFIG_NET_HTTP_TLS
#include "mbedtls/ssl.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/ecp.h"
#include "mbedtls/platform.h"
#include "pico/rand.h"
#include "net/ca_roots.h"
#endif

#include "net/sntp_time.h"

#define RX_PIECE 512
/* Basic ECC operations per handshake step (mbedtls_ecp_set_max_ops). */
#ifndef HTTPC_ECP_MAX_OPS
#define HTTPC_ECP_MAX_OPS 600
#endif          /* bytes moved into the parser per step */
#define LOC_MAX  1024          /* the longest Location followed */

typedef struct req {
    int      id;
    httpc_state_t state;
    httpc_err_t   err;
    const char   *errmsg;
    /* what was asked */
    char    *url;              /* current URL (changes on a redirect) */
    char    *method;
    char    *headers;
    uint8_t *body;
    size_t   body_len;
    uint32_t deadline_ms;
    uint8_t  follow_left;
    bool     insecure;
    httpc_url_t u;
    /* the request text, built per connection */
    char    *reqtext;
    size_t   reqlen, reqsent;
    size_t   bodysent;
    /* connection */
    struct tcp_pcb *pcb;
    struct pbuf *rx;           /* queued, not yet consumed */
    size_t   rx_off;           /* consumed bytes of the first pbuf */
    bool     rx_closed;
    ip_addr_t ip;
    /* response */
    httpp_t  parser;
    bool     redirecting;      /* this response is a redirect we will follow */
    char    *location;
    int      status;
    /* sink */
    httpc_sink_t sink;
    void    *user;
    bool     done_called;
    bool     started;          /* the first connection has been begun (httpc_tick) */
    void    *scope;            /* the request's memory (httpc_opts_t.scope) */
#if CONFIG_NET_HTTP_TLS
    mbedtls_ssl_context *ssl;
    bool roots_held;           /* this handshake holds a reference on s_roots */
    bool clock_ok;             /* the clock was set when this handshake began */
    bool tls_waited;           /* counted once in tls_waits */
#endif
} req_t;

static req_t *s_req[HTTPC_MAX];
static int s_gen;
static httpc_alloc_fns s_mem;
static httpc_stats_t s_stats;

static uint32_t now_ms(void) { return to_ms_since_boot(get_absolute_time()); }

/* Two places for memory. The request's bulk data (its body, and the sink's
 * buffers, which lhttp.c owns) go through httpc_alloc_fns -- the Lua side,
 * since they become or come from Lua values. Everything else -- the request
 * itself, its URL and header copies, the request text, a redirect's
 * Location, and the TLS session (mbedTLS's every allocation) -- is the pool
 * (lheap_pool_*, CONFIG_NET_TLS_POOL_KB): memory of its own, apart from the
 * Lua arena, so HTTPS neither takes space from the scripts nor fails because
 * the scripts' objects have cut theirs into pieces. With no pool configured
 * the pool's share goes through httpc_alloc_fns too. */
static void *xalloc(size_t n) {
    if (!s_mem.alloc) { void *p = calloc(1, n); return p; }
    return s_mem.alloc(n);
}
static void *palloc(size_t n) {
#if LHEAP_POOL_KB > 0
    void *p = lheap_pool_alloc(n ? n : 1);
    if (p) { memset(p, 0, n); }
    return p;
#else
    return xalloc(n);
#endif
}
static void xfree(void *p) {
    if (!p) { return; }
    if (lheap_pool_owns(p)) { lheap_pool_free(p); return; }
    if (!s_mem.free) { free(p); return; }
    s_mem.free(p);
}
/* The scope allocations go to (see httpc_alloc_fns.use): the request being
 * worked on, or NULL (the long-term side) for what outlives requests. */
static void *use(void *scope) { return s_mem.use ? s_mem.use(scope) : NULL; }

static char *xstrdup(const char *s) {
    size_t n = strlen(s);
    char *d = palloc(n + 1);
    if (d) { memcpy(d, s, n + 1); }
    return d;
}

void httpc_set_alloc(const httpc_alloc_fns *fns) { s_mem = *fns; }

const char *httpc_err_name(httpc_err_t e) {
    switch (e) {
        case HTTPC_OK:         return "ok";
        case HTTPC_EDNS:       return "dns";
        case HTTPC_ECONNECT:   return "connect";
        case HTTPC_ETLS:       return "tls";
        case HTTPC_ETIMEOUT:   return "timeout";
        case HTTPC_ETOOLARGE:  return "toolarge";
        case HTTPC_ECANCELLED: return "cancelled";
        case HTTPC_ENOMEM:     return "nomem";
        case HTTPC_EPROTO:     return "io";      /* a broken response is an I/O failure to a script */
        default:               return "io";
    }
}

const char *httpc_state_name(httpc_state_t s) {
    static const char *n[] = { "dns", "connect", "tls", "send", "recv", "done", "error" };
    return (unsigned)s < sizeof n / sizeof n[0] ? n[s] : "?";
}

static req_t *by_id(int id) {
    int slot = id & 0xF;
    if (id <= 0 || slot >= HTTPC_MAX) { return NULL; }
    req_t *r = s_req[slot];
    return (r && r->id == id) ? r : NULL;
}

/* ---- TLS -------------------------------------------------------------------- */
#if CONFIG_NET_HTTP_TLS
static mbedtls_ssl_config s_conf;
/* insecure = true: the same, with verification off (mbedTLS lets only a
 * server change the auth mode per handshake). No roots are loaded for it. */
static mbedtls_ssl_config s_conf_insecure;
static mbedtls_ctr_drbg_context s_drbg;
static bool s_tls_ready;
static uint32_t s_tls_alloc_fails;        /* allocations mbedTLS asked for and did not get */
static size_t s_tls_fail_size;
/* The one TLS session at a time (see tick_one): the request that may have
 * one. Others for https:// wait, before they even connect, until it is
 * done -- the pool is sized for one session, and two at once would only
 * fail each other half way through a handshake. */
static struct req *s_tls_owner;
static uint32_t s_tls_waits;

/* mbedTLS's allocator: the pool, and nothing else. */
static void *tls_calloc(size_t n, size_t sz) {
    if (sz && n > SIZE_MAX / sz) { return NULL; }
    size_t want = n * sz;
    void *p = palloc(want);
    if (!p) {
        s_tls_alloc_fails++;
        s_tls_fail_size = want;
        lheap_pool_stats_t ps;
        lheap_pool_stats(&ps);
        printf("httpc: TLS wanted %u B: pool %u of %u B used, largest free %u B\n",
               (unsigned)want, (unsigned)ps.used, (unsigned)ps.capacity, (unsigned)ps.largest_free);
    }
    return p;
}
static void tls_free(void *p) { xfree(p); }

int mbedtls_hardware_poll(void *data, unsigned char *output, size_t len, size_t *olen);

static int entropy(void *ctx, unsigned char *out, size_t len) {
    (void)ctx;
    size_t got = 0;
    return mbedtls_hardware_poll(NULL, out, len, &got) == 0 && got >= len ? 0 : -1;
}

mbedtls_ms_time_t mbedtls_ms_time(void) {
    return (mbedtls_ms_time_t)to_ms_since_boot(get_absolute_time());
}

/* The trusted roots, as a parsed chain -- but only while some handshake
 * needs them. Parsed in place (_nocopy: the DER stays in flash), ~7 KB of
 * arena for all seven, taken by the first handshake and freed when the last
 * one is done.
 *
 * Not the trusted-CA callback this used to be: mbedTLS verifies a chain
 * found through a callback in ONE call, never restarting it, and Google's
 * all-ECDSA chain (P-256 leaf <- WE2 <- GTS Root R4, P-384) then held the
 * loop ~1.35 s per handshake (mcusim, against the real server). With the
 * roots as a chain the verification runs in the same bounded steps as the
 * key exchange. The callback also turned an allocation failure while parsing
 * a root into BADCERT_NOT_TRUSTED (0x8) -- a full arena looked like a
 * certificate problem; a failure here is reported as the memory it is. */
static mbedtls_x509_crt s_roots;
static int s_roots_users;

static int roots_ref(void) {
    if (s_roots_users == 0) {
        int rc = 0;
        mbedtls_x509_crt_init(&s_roots);
        for (size_t i = 0; i < CA_ROOTS_N && rc == 0; i++) {
            rc = mbedtls_x509_crt_parse_der_nocopy(&s_roots, ca_roots[i].der, ca_roots[i].len);
        }
        if (rc != 0) {
            mbedtls_x509_crt_free(&s_roots);
            return rc;
        }
        mbedtls_ssl_conf_ca_chain(&s_conf, &s_roots, NULL);
    }
    s_roots_users++;
    return 0;
}

static void roots_unref(void) {
    if (s_roots_users <= 0) { return; }
    if (--s_roots_users == 0) {
        mbedtls_ssl_conf_ca_chain(&s_conf, NULL, NULL);
        mbedtls_x509_crt_free(&s_roots);
    }
}

/* Per-connection verify hook: `insecure` passes anything, and before the
 * clock is set the validity dates cannot be judged, so they are not. */
static int verify_cb(void *ctx, mbedtls_x509_crt *crt, int depth, uint32_t *flags) {
    (void)crt; (void)depth;
    req_t *r = ctx;
    if (r && r->insecure) { *flags = 0; return 0; }
    /* Judged by the clock as it was when the handshake BEGAN: verification
     * runs over several loop passes, and SNTP setting the clock half way
     * through left dates checked against 1970 and then not forgiven
     * (BADCERT_FUTURE, 0x200, seen in mcusim). */
    if (!r || !r->clock_ok) { *flags &= ~(uint32_t)(MBEDTLS_X509_BADCERT_EXPIRED | MBEDTLS_X509_BADCERT_FUTURE); }
    return 0;
}

static bool conf_init(mbedtls_ssl_config *c, int authmode) {
    mbedtls_ssl_config_init(c);
    if (mbedtls_ssl_config_defaults(c, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT) != 0) {
        return false;
    }
    mbedtls_ssl_conf_rng(c, mbedtls_ctr_drbg_random, &s_drbg);
    mbedtls_ssl_conf_authmode(c, authmode);
    return true;
}

static bool tls_init(void) {
    if (s_tls_ready) { return true; }
    mbedtls_platform_set_calloc_free(tls_calloc, tls_free);
    mbedtls_ctr_drbg_init(&s_drbg);
    if (mbedtls_ctr_drbg_seed(&s_drbg, entropy, NULL, (const unsigned char *)"btgw-httpc", 10) != 0) { return false; }
    if (!conf_init(&s_conf, MBEDTLS_SSL_VERIFY_REQUIRED) ||
        !conf_init(&s_conf_insecure, MBEDTLS_SSL_VERIFY_NONE)) {
        return false;
    }
    /* Bounded ECC steps: see MBEDTLS_ECP_RESTARTABLE in mbedtls_config.h. */
    mbedtls_ecp_set_max_ops(HTTPC_ECP_MAX_OPS);
    s_tls_ready = true;
    return true;
}

static int bio_send(void *ctx, const unsigned char *buf, size_t len) {
    req_t *r = ctx;
    if (!r->pcb) { return -0x0050;   /* MBEDTLS_ERR_NET_CONN_RESET; net_sockets.h is not built */ }
    size_t room = tcp_sndbuf(r->pcb);
    if (room == 0 || tcp_sndqueuelen(r->pcb) >= TCP_SND_QUEUELEN - 1) { return MBEDTLS_ERR_SSL_WANT_WRITE; }
    if (len > room) { len = room; }
    if (len > 0xFFFF) { len = 0xFFFF; }
    if (tcp_write(r->pcb, buf, (u16_t)len, TCP_WRITE_FLAG_COPY) != ERR_OK) { return MBEDTLS_ERR_SSL_WANT_WRITE; }
    tcp_output(r->pcb);
    return (int)len;
}
#endif

/* ---- the receive queue ------------------------------------------------------ */

static size_t rx_take(req_t *r, uint8_t *dst, size_t cap) {
    size_t got = 0;
    while (r->rx && got < cap) {
        struct pbuf *p = r->rx;
        size_t avail = p->len - r->rx_off;
        size_t n = avail < cap - got ? avail : cap - got;
        memcpy(dst + got, (const uint8_t *)p->payload + r->rx_off, n);
        got += n;
        r->rx_off += n;
        if (r->rx_off >= p->len) {
            /* Unlink the first pbuf. The chain (pbuf_cat) owns the rest's
             * reference, which now passes to r->rx as it stands -- taking
             * another here leaked every pbuf consumed from the middle of a
             * chain. Chains form when segments queue faster than the sink
             * drains (an SD download under backpressure), so that drained
             * PBUF_POOL and with it ALL receive, Wi-Fi and USB alike, while
             * the loop ran on (mcusim, a 237 KB download to sd:/ stopped at
             * 36 KB). */
            r->rx = p->next;
            p->next = NULL;
            p->tot_len = p->len;
            pbuf_free(p);
            r->rx_off = 0;
        }
    }
    if (got && r->pcb) { tcp_recved(r->pcb, (u16_t)got); }
    return got;
}

#if CONFIG_NET_HTTP_TLS
static int bio_recv(void *ctx, unsigned char *buf, size_t len) {
    req_t *r = ctx;
    size_t n = rx_take(r, buf, len);
    if (n) { return (int)n; }
    if (r->rx_closed) { return 0; }
    return MBEDTLS_ERR_SSL_WANT_READ;
}
#endif

/* ---- life cycle --------------------------------------------------------------- */

static void conn_close(req_t *r, bool abort_it) {
    if (r->pcb) {
        tcp_arg(r->pcb, NULL);
        tcp_recv(r->pcb, NULL);
        tcp_sent(r->pcb, NULL);
        tcp_err(r->pcb, NULL);
        tcp_poll(r->pcb, NULL, 0);
        if (abort_it || tcp_close(r->pcb) != ERR_OK) { tcp_abort(r->pcb); }
        r->pcb = NULL;
    }
    if (r->rx) { pbuf_free(r->rx); r->rx = NULL; r->rx_off = 0; }
    r->rx_closed = false;
#if CONFIG_NET_HTTP_TLS
    if (r->roots_held) { roots_unref(); r->roots_held = false; }
    if (r->ssl) {
        mbedtls_ssl_free(r->ssl);
        mbedtls_free(r->ssl);
        r->ssl = NULL;
        s_stats.tls_now--;
    }
#endif
    xfree(r->reqtext);
    r->reqtext = NULL;
}

static void tls_let_go(req_t *r) {
#if CONFIG_NET_HTTP_TLS
    if (s_tls_owner == r) { s_tls_owner = NULL; }
#else
    (void)r;
#endif
}

static void req_free(req_t *r) {
    conn_close(r, true);
    tls_let_go(r);
    xfree(r->url); xfree(r->method); xfree(r->headers); xfree(r->body); xfree(r->location);
    int slot = r->id & 0xF;
    if (slot < HTTPC_MAX && s_req[slot] == r) { s_req[slot] = NULL; }
    xfree(r);
}

static void finish(req_t *r, httpc_err_t err, const char *msg) {
    if (r->state == HTTPC_DONE || r->state == HTTPC_ERROR) { return; }
    conn_close(r, err != HTTPC_OK);
    tls_let_go(r);
    r->err = err;
    r->errmsg = msg;
    r->state = err == HTTPC_OK ? HTTPC_DONE : HTTPC_ERROR;
    if (err != HTTPC_OK) { s_stats.failures++; }
    if (!r->done_called && r->sink.on_done) {
        r->done_called = true;
        r->sink.on_done(r->user, err, msg);
    }
}

/* ---- the request text --------------------------------------------------------- */

static bool header_is(const char *line, const char *name) {
    size_t n = strlen(name);
    return strncasecmp(line, name, n) == 0 && line[n] == ':';
}

/* Whether the caller's header block (CRLF lines) has a header of this name. */
static bool headers_have(const char *h, const char *name) {
    while (h && *h) {
        if (header_is(h, name)) { return true; }
        const char *e = strstr(h, "\r\n");
        if (!e) { break; }
        h = e + 2;
    }
    return false;
}

static bool build_request(req_t *r, bool same_host) {
    const char *m = r->method;
    bool with_body = r->body_len > 0 || !strcmp(m, "POST") || !strcmp(m, "PUT") || !strcmp(m, "PATCH");
    char hostport[150];
    bool dflt = r->u.port == (r->u.tls ? 443 : 80);
    if (dflt) { snprintf(hostport, sizeof hostport, "%s", r->u.host); }
    else { snprintf(hostport, sizeof hostport, "%s:%u", r->u.host, r->u.port); }
    size_t hl = r->headers ? strlen(r->headers) : 0;
    size_t cap = strlen(m) + r->u.path_len + strlen(hostport) + hl + 160;
    char *t = palloc(cap);
    if (!t) { return false; }
    /* "User-Agent: btgw" unless the caller names its own (sent below, with its headers) */
    int n = snprintf(t, cap, "%s %.*s HTTP/1.1\r\nHost: %s\r\n%s"
                     "Accept-Encoding: identity\r\nConnection: close\r\n",
                     m, (int)r->u.path_len, r->u.path, hostport,
                     headers_have(r->headers, "user-agent") ? "" : "User-Agent: btgw\r\n");
    /* The caller's headers, minus what we own -- and minus credentials when
     * a redirect has taken us to another host. */
    for (const char *h = r->headers; h && *h; ) {
        const char *e = strstr(h, "\r\n");
        size_t ll = e ? (size_t)(e - h) + 2 : strlen(h);
        bool drop = header_is(h, "host") || header_is(h, "content-length") || header_is(h, "connection") ||
                    header_is(h, "transfer-encoding") || (!same_host && (header_is(h, "authorization") ||
                    header_is(h, "cookie")));
        if (!drop && (size_t)n + ll + 1 < cap) { memcpy(t + n, h, ll); n += (int)ll; }
        h += ll;
    }
    if (with_body) { n += snprintf(t + n, cap - (size_t)n, "Content-Length: %u\r\n", (unsigned)r->body_len); }
    n += snprintf(t + n, cap - (size_t)n, "\r\n");
    r->reqtext = t;
    r->reqlen = (size_t)n;
    r->reqsent = 0;
    r->bodysent = 0;
    return true;
}

/* ---- the response ------------------------------------------------------------- */

static bool is_redirect(int st) { return st == 301 || st == 302 || st == 303 || st == 307 || st == 308; }

/* The status line is in: is this the response, or a redirect to follow? */
static void note_status(req_t *r) {
    if (r->status != 0 || r->parser.status == 0) { return; }
    r->status = r->parser.status;
    r->redirecting = is_redirect(r->status) && r->follow_left > 0;
    if (!r->redirecting && r->sink.on_status) { r->sink.on_status(r->user, r->status); }
}

static void on_hdr(void *ctx, const char *name, const char *value) {
    req_t *r = ctx;
    note_status(r);
    if (r->redirecting) {
        if (!strcmp(name, "location") && !r->location) { r->location = xstrdup(value); }
        return;
    }
    if (r->sink.on_header) { r->sink.on_header(r->user, name, value); }
}

static void on_body(void *ctx, const uint8_t *d, size_t n) {
    req_t *r = ctx;
    note_status(r);
    if (r->redirecting) { return; }       /* a redirect's body is discarded */
    if (r->sink.on_body) { r->sink.on_body(r->user, d, n); }
}

static void start_connection(req_t *r);

static void follow_redirect(req_t *r) {
    char *next = palloc(LOC_MAX + 1);
    if (!next || !r->location || !httpc_url_resolve(r->url, r->location, next, LOC_MAX + 1)) {
        xfree(next);
        finish(r, HTTPC_EPROTO, "bad redirect location");
        return;
    }
    char oldhost[sizeof r->u.host];
    snprintf(oldhost, sizeof oldhost, "%s", r->u.host);
    bool was_tls = r->u.tls;
    conn_close(r, false);
    xfree(r->url);
    r->url = next;
    xfree(r->location);
    r->location = NULL;
    if (!httpc_url_parse(r->url, &r->u)) { finish(r, HTTPC_EPROTO, "bad redirect URL"); return; }
    if (was_tls && !r->u.tls) { finish(r, HTTPC_EPROTO, "redirect from https to http refused"); return; }
    int st = r->status;
    if (st == 303 || ((st == 301 || st == 302) && strcmp(r->method, "GET") && strcmp(r->method, "HEAD"))) {
        /* What browsers do: the follow-up is a GET without the body. */
        xfree(r->method);
        r->method = xstrdup("GET");
        xfree(r->body);
        r->body = NULL;
        r->body_len = 0;
        if (!r->method) { finish(r, HTTPC_ENOMEM, "out of memory"); return; }
    }
    r->follow_left--;
    r->status = 0;
    r->redirecting = false;
    bool same = strcasecmp(oldhost, r->u.host) == 0;
    if (!build_request(r, same)) { finish(r, HTTPC_ENOMEM, "out of memory"); return; }
#if CONFIG_NET_HTTP_TLS
    if (r->u.tls && s_tls_owner != r) {
        /* An http:// redirect to https://: it waits its turn like any other. */
        r->state = HTTPC_DNS;
        r->started = false;
        return;
    }
#endif
    start_connection(r);
}

static void parsed(req_t *r) {
    note_status(r);
    if (r->parser.error) { finish(r, HTTPC_EPROTO, r->parser.err); return; }
    if (!r->parser.done) { return; }
    if (r->redirecting) { follow_redirect(r); return; }
    finish(r, HTTPC_OK, NULL);
}

/* ---- the pump ----------------------------------------------------------------- */

static void pump(req_t *r);

static void send_some(req_t *r) {
    while (r->state == HTTPC_SEND) {
        const uint8_t *src;
        size_t left;
        if (r->reqsent < r->reqlen) { src = (const uint8_t *)r->reqtext + r->reqsent; left = r->reqlen - r->reqsent; }
        else if (r->bodysent < r->body_len) { src = r->body + r->bodysent; left = r->body_len - r->bodysent; }
        else {
            r->state = HTTPC_RECV;
            httpp_init(&r->parser, !strcmp(r->method, "HEAD"), on_hdr, on_body, r);
            return;
        }
        int w;
#if CONFIG_NET_HTTP_TLS
        if (r->ssl) {
            w = mbedtls_ssl_write(r->ssl, src, left);
            if (w == MBEDTLS_ERR_SSL_WANT_WRITE || w == MBEDTLS_ERR_SSL_WANT_READ) { return; }
            if (w < 0) { finish(r, HTTPC_ETLS, "tls write failed"); return; }
        } else
#endif
        {
            size_t room = tcp_sndbuf(r->pcb);
            if (!room || tcp_sndqueuelen(r->pcb) >= TCP_SND_QUEUELEN - 1) { return; }
            w = (int)(left < room ? left : room);
            if (w > 0xFFFF) { w = 0xFFFF; }
            if (tcp_write(r->pcb, src, (u16_t)w, TCP_WRITE_FLAG_COPY) != ERR_OK) { return; }
            tcp_output(r->pcb);
        }
        if (r->reqsent < r->reqlen) { r->reqsent += (size_t)w; } else { r->bodysent += (size_t)w; }
    }
}

static void recv_some(req_t *r) {
    static uint8_t piece[RX_PIECE];
    for (int guard = 0; guard < 64 && r->state == HTTPC_RECV; guard++) {
        size_t want = RX_PIECE;
        if (httpp_headers_done(&r->parser) && !r->redirecting && r->sink.room) {
            size_t room = r->sink.room(r->user);
            if (room == 0) { return; }             /* backpressure: try again next tick */
            if (room < want) { want = room; }
        }
        int n;
#if CONFIG_NET_HTTP_TLS
        if (r->ssl) {
            n = mbedtls_ssl_read(r->ssl, piece, want);
            if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) { return; }
            if (n == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || n == 0) {
                httpp_eof(&r->parser);
                parsed(r);
                return;
            }
            if (n < 0) {
                /* A server that just closes TCP after the body is common:
                 * let the parser decide whether the response was complete. */
                if (r->rx_closed) { httpp_eof(&r->parser); parsed(r); return; }
                finish(r, HTTPC_ETLS, "tls read failed");
                return;
            }
        } else
#endif
        {
            n = (int)rx_take(r, piece, want);
            if (n == 0) {
                if (r->rx_closed) { httpp_eof(&r->parser); parsed(r); }
                return;
            }
        }
        httpp_feed(&r->parser, piece, (size_t)n);
        parsed(r);
    }
}

#if CONFIG_NET_HTTP_TLS
/* The handshake, one state at a time, so the trusted roots are parsed only
 * for the state that verifies the server's chain and let go the moment it is
 * done -- they never add to the key exchange's peak. Returns what
 * mbedtls_ssl_handshake would: 0 when the handshake is over. */
static int tls_step(req_t *r) {
    for (int guard = 0; guard < 32; guard++) {
        if (mbedtls_ssl_is_handshake_over(r->ssl)) { break; }
        int st = r->ssl->MBEDTLS_PRIVATE(state);
        if (st == MBEDTLS_SSL_SERVER_CERTIFICATE && !r->roots_held && !r->insecure) {
            int rr = roots_ref();
            if (rr != 0) { return MBEDTLS_ERR_SSL_ALLOC_FAILED; }
            r->roots_held = true;
        } else if (st > MBEDTLS_SSL_SERVER_CERTIFICATE && r->roots_held) {
            roots_unref();
            r->roots_held = false;
        }
        int rc = mbedtls_ssl_handshake_step(r->ssl);
        if (rc != 0) { return rc; }
    }
    if (r->roots_held) { roots_unref(); r->roots_held = false; }
    return mbedtls_ssl_is_handshake_over(r->ssl) ? 0 : MBEDTLS_ERR_SSL_WANT_READ;
}
#endif

static void pump(req_t *r) {
#if CONFIG_NET_HTTP_TLS
    if (r->state == HTTPC_TLS) {
        uint32_t fails_before = s_tls_alloc_fails;
        int rc = tls_step(r);
        if (rc == MBEDTLS_ERR_SSL_CRYPTO_IN_PROGRESS) { return; }     /* the next pass carries on */
        if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
            if (r->rx_closed) { finish(r, HTTPC_ETLS, "closed during the handshake"); }
            return;
        }
        if (rc != 0) {
            uint32_t v = mbedtls_ssl_get_verify_result(r->ssl);
            static char msg[80];
            if (s_tls_alloc_fails != fails_before) {
                /* Whatever mbedTLS made of it (a fatal X.509 error, a failed
                 * key parse...), the cause is memory: say so. */
                snprintf(msg, sizeof msg, "TLS pool full in the handshake (wanted %u B)",
                         (unsigned)s_tls_fail_size);
                finish(r, HTTPC_ENOMEM, msg);
                return;
            }
            if (v && v != (uint32_t)-1) {
                snprintf(msg, sizeof msg, "certificate not accepted (flags 0x%lx)", (unsigned long)v);
            } else {
                snprintf(msg, sizeof msg, "handshake failed (-0x%04x)", (unsigned)-rc);
            }
            finish(r, HTTPC_ETLS, msg);
            return;
        }
        if (r->roots_held) { roots_unref(); r->roots_held = false; }   /* verified: let them go */
        r->state = HTTPC_SEND;
    }
#endif
    if (r->state == HTTPC_SEND) { send_some(r); }
    if (r->state == HTTPC_RECV) { recv_some(r); }
}

/* ---- lwIP callbacks ------------------------------------------------------------ */

static req_t *from_arg(void *arg) { return by_id((int)(intptr_t)arg); }

static err_t cb_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    (void)err;
    req_t *r = from_arg(arg);
    if (!r || r->pcb != pcb) {
        if (p) { tcp_recved(pcb, p->tot_len); pbuf_free(p); }
        return ERR_OK;
    }
    void *prev = use(r->scope);
    if (!p) {
        r->rx_closed = true;
    } else if (r->rx) {
        pbuf_cat(r->rx, p);
    } else {
        r->rx = p;
        r->rx_off = 0;
    }
    pump(r);
    use(prev);
    return ERR_OK;
}

static err_t cb_sent(void *arg, struct tcp_pcb *pcb, u16_t len) {
    (void)pcb; (void)len;
    req_t *r = from_arg(arg);
    if (r) { void *prev = use(r->scope); pump(r); use(prev); }
    return ERR_OK;
}

static void cb_err_on(req_t *r, err_t err);
static void cb_err(void *arg, err_t err) {
    req_t *r = from_arg(arg);
    if (!r) { return; }
    void *prev = use(r->scope);
    cb_err_on(r, err);
    use(prev);
}

static void cb_err_on(req_t *r, err_t err) {
    r->pcb = NULL;                 /* lwIP has freed it */
    if (r->state == HTTPC_CONNECT) { finish(r, HTTPC_ECONNECT, err == ERR_RST ? "refused" : "connect failed"); }
    else if (r->state == HTTPC_RECV && !r->parser.error) {
        /* A reset after a complete read-until-close body is still the end. */
        r->rx_closed = true;
        httpp_eof(&r->parser);
        if (r->parser.done && !r->redirecting) { finish(r, HTTPC_OK, NULL); }
        else { finish(r, HTTPC_EIO, "connection reset"); }
    } else {
        finish(r, HTTPC_EIO, "connection reset");
    }
}

static err_t cb_connected_on(req_t *r, err_t err);
static err_t cb_connected(void *arg, struct tcp_pcb *pcb, err_t err) {
    (void)pcb;
    req_t *r = from_arg(arg);
    if (!r) { return ERR_OK; }
    void *prev = use(r->scope);
    err_t e = cb_connected_on(r, err);
    use(prev);
    return e;
}

static err_t cb_connected_on(req_t *r, err_t err) {
    if (err != ERR_OK) { finish(r, HTTPC_ECONNECT, "connect failed"); return ERR_OK; }
#if CONFIG_NET_HTTP_TLS
    if (r->u.tls) {
        bool ok = tls_init();
        if (!ok) { finish(r, HTTPC_ETLS, "tls init failed"); return ERR_OK; }
        r->ssl = mbedtls_calloc(1, sizeof(mbedtls_ssl_context));
        if (!r->ssl) { finish(r, HTTPC_ENOMEM, "out of memory for tls"); return ERR_OK; }
        mbedtls_ssl_init(r->ssl);
        s_stats.tls_now++;
        if (mbedtls_ssl_setup(r->ssl, r->insecure ? &s_conf_insecure : &s_conf) != 0 || mbedtls_ssl_set_hostname(r->ssl, r->u.host) != 0) {
            finish(r, HTTPC_ENOMEM, "out of memory for tls");
            return ERR_OK;
        }
        r->clock_ok = sntp_time_valid();
        mbedtls_ssl_set_verify(r->ssl, verify_cb, r);
        mbedtls_ssl_set_bio(r->ssl, r, bio_send, bio_recv, NULL);
        r->state = HTTPC_TLS;
    } else
#endif
    {
        r->state = HTTPC_SEND;
    }
    pump(r);
    return ERR_OK;
}

static void do_connect(req_t *r) {
    r->state = HTTPC_CONNECT;
    r->pcb = tcp_new_ip_type(IPADDR_TYPE_V4);
    if (!r->pcb) { finish(r, HTTPC_ENOMEM, "no TCP pcb"); return; }
    tcp_arg(r->pcb, (void *)(intptr_t)r->id);
    tcp_recv(r->pcb, cb_recv);
    tcp_sent(r->pcb, cb_sent);
    tcp_err(r->pcb, cb_err);
    tcp_nagle_disable(r->pcb);
    err_t e = tcp_connect(r->pcb, &r->ip, r->u.port, cb_connected);
    if (e != ERR_OK) { finish(r, HTTPC_ECONNECT, e == ERR_RTE ? "no route" : "connect failed"); }
}

#if LWIP_DNS
static void cb_dns(const char *name, const ip_addr_t *ip, void *arg) {
    (void)name;
    req_t *r = from_arg(arg);
    if (!r || r->state != HTTPC_DNS) { return; }
    void *prev = use(r->scope);
    if (!ip) { finish(r, HTTPC_EDNS, "name not resolved"); }
    else { r->ip = *ip; do_connect(r); }
    use(prev);
}
#endif

#if LWIP_DNS
/* DHCP fills the DNS servers on Wi-Fi and Ethernet. Where it did not (USB-NCM:
 * the gateway is the DHCP server there, not the client), use cfg/dns.json
 * {"server":"a.b.c.d"} if present, else the default route's gateway -- a
 * home router or a sharing host usually answers DNS. Checked per request,
 * since the uplink can change under us. */
static void dns_fallback(void) {
    if (!ip_addr_isany_val(*dns_getserver(0))) { return; }
    static bool cfg_read;
    static ip_addr_t cfg_ip;
    if (!cfg_read) {
        cfg_read = true;
        char buf[128];
        size_t sz = 0;
        if (fs_stat("cfg/dns.json", &sz) && sz < sizeof buf) {
            int h = fs_begin_read("cfg/dns.json", NULL);
            int n = h >= 0 ? fs_read(h, (uint8_t *)buf, sz) : -1;
            if (h >= 0) { fs_end_read(h); }
            if (n > 0) {
                buf[n] = 0;
                const char *k = strstr(buf, "\"server\"");
                const char *v = k ? strchr(k + 8, '"') : NULL;
                char ip[20];
                if (v && sscanf(v + 1, "%19[0-9.]", ip) == 1) { ipaddr_aton(ip, &cfg_ip); }
            }
        }
    }
    if (!ip_addr_isany_val(cfg_ip)) { dns_setserver(0, &cfg_ip); return; }
    if (netif_default && !ip4_addr_isany_val(*netif_ip4_gw(netif_default))) {
        ip_addr_t gw;
        ip_addr_copy_from_ip4(gw, *netif_ip4_gw(netif_default));
        dns_setserver(1, &gw);
    }
}
#endif

static void start_connection(req_t *r) {
    r->state = HTTPC_DNS;
    if (ipaddr_aton(r->u.host, &r->ip)) { do_connect(r); return; }
#if LWIP_DNS
    dns_fallback();
    err_t e = dns_gethostbyname(r->u.host, &r->ip, cb_dns, (void *)(intptr_t)r->id);
    if (e == ERR_OK) { do_connect(r); return; }
    if (e != ERR_INPROGRESS) { finish(r, HTTPC_EDNS, "no DNS server"); }
#else
    finish(r, HTTPC_EDNS, "this build has no DNS");
#endif
}

/* ---- API ----------------------------------------------------------------------- */

int httpc_start(const httpc_opts_t *o, const httpc_sink_t *sink, void *user, httpc_err_t *err) {
    *err = HTTPC_OK;
    int slot = -1;
    for (int i = 0; i < HTTPC_MAX; i++) { if (!s_req[i]) { slot = i; break; } }
    if (slot < 0) { *err = HTTPC_ENOMEM; return -1; }
    req_t *r = palloc(sizeof *r);
    if (!r) { *err = HTTPC_ENOMEM; return -1; }
    r->scope = o->scope;
    if (++s_gen > 0x7FFFFF) { s_gen = 1; }
    r->id = (s_gen << 4) | slot;
    r->url = xstrdup(o->url);
    r->method = xstrdup(o->method ? o->method : "GET");
    r->headers = o->headers ? xstrdup(o->headers) : NULL;
    if (o->body_len) {
        r->body = xalloc(o->body_len);
        if (r->body) { memcpy(r->body, o->body, o->body_len); }
    }
    r->body_len = o->body_len;
    r->sink = *sink;
    r->user = user;
    r->insecure = o->insecure;
    r->follow_left = o->follow;
    r->deadline_ms = now_ms() + (o->timeout_ms ? o->timeout_ms : 30000);
    if (!r->url || !r->method || (o->headers && !r->headers) || (o->body_len && !r->body)) {
        s_req[slot] = r;
        r->done_called = true;
        req_free(r);
        *err = HTTPC_ENOMEM;
        return -1;
    }
    if (!httpc_url_parse(r->url, &r->u)) {
        s_req[slot] = r;
        r->done_called = true;
        req_free(r);
        *err = HTTPC_EPROTO;
        return -1;
    }
#if !CONFIG_NET_HTTP_TLS
    if (r->u.tls) {
        s_req[slot] = r; r->done_called = true; req_free(r);
        *err = HTTPC_ETLS;
        return -1;
    }
#endif
    s_req[slot] = r;
    s_stats.requests++;
    if (!build_request(r, true)) { r->done_called = true; req_free(r); *err = HTTPC_ENOMEM; return -1; }
    r->state = HTTPC_DNS;      /* the connection starts on the next tick */
    return r->id;
}

void httpc_cancel(int id) {
    req_t *r = by_id(id);
    if (!r) { return; }
    r->done_called = true;     /* no callback for a cancel */
    req_free(r);
}

bool httpc_info(int id, httpc_state_t *state, uint64_t *bytes, int64_t *total, int *status) {
    req_t *r = by_id(id);
    if (!r) { return false; }
    if (state) { *state = r->state; }
    bool final = r->state == HTTPC_RECV && !r->redirecting;
    if (bytes) { *bytes = final ? r->parser.body_bytes : 0; }
    if (total) { *total = final && httpp_headers_done(&r->parser) ? r->parser.content_length : -1; }
    if (status) { *status = r->redirecting ? 0 : r->status; }
    return true;
}

static void tick_one(req_t *r, uint32_t now);

void httpc_tick(void) {
    uint32_t now = now_ms();
    for (int i = 0; i < HTTPC_MAX; i++) {
        req_t *r = s_req[i];
        if (!r) { continue; }
        void *prev = use(r->scope);
        tick_one(r, now);
        use(prev);
    }
}

static void tick_one(req_t *r, uint32_t now) {
    {
        if (r->state == HTTPC_DONE || r->state == HTTPC_ERROR) {
            /* Finished: the owner frees it through httpc_cancel when it has
             * collected the result. */
            return;
        }
        if ((int32_t)(now - r->deadline_ms) >= 0) { finish(r, HTTPC_ETIMEOUT, "request timed out"); return; }
        if (!r->started) {
#if CONFIG_NET_HTTP_TLS
            if (r->u.tls && s_tls_owner && s_tls_owner != r) {
                if (!r->tls_waited) { r->tls_waited = true; s_tls_waits++; }
                return;            /* another request has the TLS session: wait (its deadline runs) */
            }
            if (r->u.tls) { s_tls_owner = r; }
#endif
            r->started = true;
            start_connection(r);
            return;
        }
        /* A frame the interface refused is gone, and lwIP leaves the
         * segment on `unsent` with nothing to retry it -- for a SYN that is
         * a connect that never completes. USB-NCM refuses (ERR_USE) whenever
         * its one transmit buffer is busy, which a reply to the host going
         * out at the same moment is enough for (found in mcusim). Asking
         * again each pass is cheap: tcp_output() is a no-op with nothing
         * queued. */
        if (r->pcb && r->pcb->unsent) { tcp_output(r->pcb); }
        pump(r);                   /* sink room may have opened */
    }
}

void httpc_reset(void) {
    /* Requests are freed for real: their memory is the pool's (which a VM
     * restart does not reset) and the sink's (whose arena is reset only
     * after this). The shared configs and RNG are static and survive. */
    for (int i = 0; i < HTTPC_MAX; i++) {
        if (s_req[i]) { s_req[i]->done_called = true; req_free(s_req[i]); }
    }
}

void httpc_stats(httpc_stats_t *out) {
    *out = s_stats;
    lheap_pool_stats_t ps;
    lheap_pool_stats(&ps);
    out->pool_size = (uint32_t)ps.capacity;
    out->pool_used = (uint32_t)ps.used;
    out->pool_peak = (uint32_t)ps.peak;
    out->pool_largest_free = (uint32_t)ps.largest_free;
    out->pool_fails = ps.fails;
#if CONFIG_NET_HTTP_TLS
    out->tls_waits = s_tls_waits;
    out->tls_alloc_fails = s_tls_alloc_fails;
#endif
}
