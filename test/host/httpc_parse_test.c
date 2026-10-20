/*
 * The HTTP client's response parser and URL handling, on a host.
 *
 * Every response is fed three ways -- whole, one byte at a time, and split at
 * every possible offset into two pieces -- because the parser sees TCP
 * segments (and TLS records) cut wherever the network cut them, and a state
 * machine that only works on nicely aligned input is the classic bug here.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "net/httpc_parse.h"

static int checks, failed;
#define CHECK(c, msg) do { checks++; if (!(c)) { failed++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, msg); } } while (0)

typedef struct {
    char hdrs[4096];
    size_t hn;
    char body[70000];
    size_t bn;
    int nheaders;
} sink_t;

static void on_h(void *ctx, const char *name, const char *value) {
    sink_t *s = ctx;
    s->hn += (size_t)snprintf(s->hdrs + s->hn, sizeof s->hdrs - s->hn, "%s=%s\n", name, value);
    s->nheaders++;
}

static void on_b(void *ctx, const uint8_t *d, size_t n) {
    sink_t *s = ctx;
    if (s->bn + n <= sizeof s->body) { memcpy(s->body + s->bn, d, n); }
    s->bn += n;
}

typedef struct { int status; bool done, error; size_t consumed; const char *err; } outcome_t;

/* mode 0 whole, 1 byte-wise, else split at `mode - 2`. */
static outcome_t run(const char *resp, size_t len, bool head, bool eof, int mode, sink_t *s) {
    httpp_t p;
    memset(s, 0, sizeof *s);
    httpp_init(&p, head, on_h, on_b, s);
    size_t used = 0;
    if (mode == 0) {
        used = httpp_feed(&p, (const uint8_t *)resp, len);
    } else if (mode == 1) {
        for (size_t i = 0; i < len && !p.done && !p.error; i++) { used += httpp_feed(&p, (const uint8_t *)resp + i, 1); }
    } else {
        size_t cut = (size_t)(mode - 2);
        used = httpp_feed(&p, (const uint8_t *)resp, cut);
        if (!p.done && !p.error) { used += httpp_feed(&p, (const uint8_t *)resp + cut, len - cut); }
    }
    if (eof) { httpp_eof(&p); }
    outcome_t o = { p.status, p.done, p.error, used, p.err };
    return o;
}

/* Run all three ways and check each against the expectation. */
static void expect(const char *what, const char *resp, size_t len, bool head, bool eof,
                   int status, const char *body, size_t body_len, const char *hdr_has, bool want_err,
                   size_t want_consumed) {
    sink_t s;
    char msg[200];
    for (int mode = 0; mode < (int)len + 2; mode++) {
        if (mode > 2 && len > 3000 && mode % 97) { continue; }    /* big bodies: a sample of splits */
        outcome_t o = run(resp, len, head, eof, mode, &s);
        snprintf(msg, sizeof msg, "%s (feed mode %d): %s%s", what, mode, o.error ? "error " : "",
                 o.err ? o.err : "");
        if (want_err) {
            CHECK(o.error, msg);
            continue;
        }
        CHECK(o.done && !o.error, msg);
        CHECK(o.status == status, msg);
        CHECK(s.bn == body_len && (!body || memcmp(s.body, body, body_len) == 0), msg);
        if (hdr_has) { CHECK(strstr(s.hdrs, hdr_has) != NULL, msg); }
        if (want_consumed) { CHECK(o.consumed == want_consumed, msg); }
        if (mode >= 2 && len > 400) { mode += 0; }
    }
}

#define S(x) x, sizeof(x) - 1

int main(void) {
    expect("content-length",
           S("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 11\r\n\r\n{\"a\":true}\n"),
           false, false, 200, "{\"a\":true}\n", 11, "content-type=application/json\n", false, 0);
    expect("header names are lower-cased, values trimmed",
           S("HTTP/1.1 200 OK\r\nX-Goog-Hash:   crc32c=abc==  \r\ncontent-LENGTH: 0\r\n\r\n"),
           false, false, 200, "", 0, "x-goog-hash=crc32c=abc==\n", false, 0);
    expect("chunked, with extensions and a trailer",
           S("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
             "5;name=val\r\nhello\r\n1\r\n \r\n6\r\nworld!\r\n0\r\nX-Trailer: t\r\n\r\n"),
           false, false, 200, "hello world!", 12, "transfer-encoding=chunked\n", false, 0);
    expect("chunked with upper-case hex",
           S("HTTP/1.1 200 OK\r\ntransfer-encoding: gzip, chunked\r\n\r\nA\r\n0123456789\r\n0\r\n\r\n"),
           false, false, 200, "0123456789", 10, NULL, true, 0);   /* gzip is refused */
    expect("chunked, hex A",
           S("HTTP/1.1 200 OK\r\nTransfer-Encoding: Chunked\r\n\r\nA\r\n0123456789\r\n0\r\n\r\n"),
           false, false, 200, "0123456789", 10, NULL, false, 0);
    expect("read until close",
           S("HTTP/1.0 200 OK\r\nServer: x\r\n\r\nall of the rest"),
           false, true, 200, "all of the rest", 15, NULL, false, 0);
    expect("bare LF line endings",
           S("HTTP/1.1 404 Not Found\nContent-Length: 3\n\nnop"),
           false, false, 404, "nop", 3, NULL, false, 0);
    expect("204 has no body whatever it says",
           S("HTTP/1.1 204 No Content\r\nContent-Length: 50\r\n\r\n"),
           false, false, 204, "", 0, NULL, false, 0);
    expect("HEAD has no body",
           S("HTTP/1.1 200 OK\r\nContent-Length: 12345\r\n\r\n"),
           true, false, 200, "", 0, "content-length=12345\n", false, 0);
    expect("100 Continue is skipped",
           S("HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 201 Created\r\nContent-Length: 2\r\n\r\nok"),
           false, false, 201, "ok", 2, NULL, false, 0);
    expect("redirect headers",
           S("HTTP/1.1 302 Found\r\nLocation: /v0/b/x/o?alt=media\r\nContent-Length: 0\r\n\r\n"),
           false, false, 302, "", 0, "location=/v0/b/x/o?alt=media\n", false, 0);
    {
        /* Trailing bytes after a complete response are not consumed. */
        const char r[] = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nokEXTRA";
        expect("bytes after the end are left alone", r, sizeof r - 1, false, false, 200, "ok", 2, NULL, false,
               sizeof r - 1 - 5);
    }
    /* Errors. */
    expect("garbage status line", S("HTP/1.1 200 OK\r\n\r\n"), false, false, 0, NULL, 0, NULL, true, 0);
    expect("bad status code", S("HTTP/1.1 2x0 OK\r\n\r\n"), false, false, 0, NULL, 0, NULL, true, 0);
    expect("bad content-length", S("HTTP/1.1 200 OK\r\nContent-Length: -1\r\n\r\n"), false, false, 0, NULL, 0, NULL, true, 0);
    expect("conflicting content-length",
           S("HTTP/1.1 200 OK\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\nab"), false, false, 0, NULL, 0, NULL, true, 0);
    expect("bad chunk size", S("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\n"), false, false, 0, NULL, 0, NULL, true, 0);
    expect("chunk without its CRLF",
           S("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n2\r\nabXY0\r\n\r\n"), false, false, 0, NULL, 0, NULL, true, 0);
    expect("closed early (content-length)", S("HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nabc"),
           false, true, 0, NULL, 0, NULL, true, 0);
    expect("closed early (chunked)", S("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nab"),
           false, true, 0, NULL, 0, NULL, true, 0);
    expect("closed before anything", S(""), false, true, 0, NULL, 0, NULL, true, 0);
    expect("space in a header name", S("HTTP/1.1 200 OK\r\nBad Name: x\r\n\r\n"), false, false, 0, NULL, 0, NULL, true, 0);

    /* A long header value is cut, not refused; the response still parses. */
    {
        static char r[4000];
        int n = snprintf(r, sizeof r, "HTTP/1.1 200 OK\r\nContent-Security-Policy: ");
        for (int i = 0; i < 2000; i++) { r[n++] = 'a' + i % 26; }
        n += snprintf(r + n, sizeof r - (size_t)n, "\r\nContent-Length: 4\r\n\r\nbody");
        expect("an over-long header is cut", r, (size_t)n, false, false, 200, "body", 4, "content-length=4\n", false, 0);
    }
    /* A 64 KiB chunked body in many chunks. */
    {
        static char r[80000];
        static char want[65536];
        int n = snprintf(r, sizeof r, "HTTP/1.1 200 OK\r\ntransfer-encoding: chunked\r\n\r\n");
        size_t w = 0;
        for (int c = 0; w < sizeof want; c++) {
            size_t len = (size_t)(1 + (c * 37) % 1500);
            if (w + len > sizeof want) { len = sizeof want - w; }
            n += snprintf(r + n, sizeof r - (size_t)n, "%zx\r\n", len);
            for (size_t k = 0; k < len; k++) { want[w + k] = (char)(w + k); r[n + (int)k] = (char)(w + k); }
            n += (int)len;
            w += len;
            n += snprintf(r + n, sizeof r - (size_t)n, "\r\n");
        }
        n += snprintf(r + n, sizeof r - (size_t)n, "0\r\n\r\n");
        expect("64 KiB in many chunks", r, (size_t)n, false, false, 200, want, sizeof want, NULL, false, 0);
    }

    /* URLs. */
    httpc_url_t u;
    CHECK(httpc_url_parse("https://firebasestorage.googleapis.com/v0/b/x.appspot.com/o/fw%2Fa.bin?alt=media&token=t", &u)
          && u.tls && u.port == 443 && strcmp(u.host, "firebasestorage.googleapis.com") == 0 &&
          u.path_len == strlen("/v0/b/x.appspot.com/o/fw%2Fa.bin?alt=media&token=t") &&
          strncmp(u.path, "/v0/b/", 6) == 0, "an https URL with a query");
    CHECK(httpc_url_parse("http://10.0.0.7/api/op", &u) && !u.tls && u.port == 80 && strcmp(u.host, "10.0.0.7") == 0 &&
          u.path_len == 7, "http, IP host");
    CHECK(httpc_url_parse("http://host:8080", &u) && u.port == 8080 && u.path_len == 1 && u.path[0] == '/', "a bare host with a port");
    CHECK(httpc_url_parse("http://host?x=1", &u) && u.path[0] == '?' && u.path_len == 4, "a query with no path");
    CHECK(httpc_url_parse("https://h/p#frag", &u) && u.path_len == 2, "the fragment is not sent");
    CHECK(!httpc_url_parse("ftp://h/", &u), "ftp refused");
    CHECK(!httpc_url_parse("http://", &u), "no host");
    CHECK(!httpc_url_parse("http://:80/", &u), "empty host");
    CHECK(!httpc_url_parse("http://u:p@h/", &u), "userinfo refused");
    CHECK(!httpc_url_parse("http://[::1]/", &u), "IPv6 refused");
    CHECK(!httpc_url_parse("http://h:0/", &u), "port 0");
    CHECK(!httpc_url_parse("http://h:65536/", &u), "port too big");
    CHECK(!httpc_url_parse("http://h:12x/", &u), "port junk");
    CHECK(!httpc_url_parse("http://h/a b", &u), "space in the path");
    CHECK(!httpc_url_parse("http://h/a\r\nX: y", &u), "CRLF injection in the path");

    char out[300];
    const char *base = "https://a.example.com/dir/sub/file.bin?x=1";
    CHECK(httpc_url_resolve(base, "https://b.example.org/z", out, sizeof out) && !strcmp(out, "https://b.example.org/z"), "absolute");
    CHECK(httpc_url_resolve(base, "//cdn.example.net/q", out, sizeof out) && !strcmp(out, "https://cdn.example.net/q"), "scheme-relative");
    CHECK(httpc_url_resolve(base, "/root?y=2", out, sizeof out) && !strcmp(out, "https://a.example.com/root?y=2"), "absolute path");
    CHECK(httpc_url_resolve(base, "other.bin", out, sizeof out) && !strcmp(out, "https://a.example.com/dir/sub/other.bin"), "relative");
    CHECK(httpc_url_resolve(base, "?z=3", out, sizeof out) && !strcmp(out, "https://a.example.com/dir/sub/file.bin?z=3"), "query only");
    CHECK(httpc_url_resolve("http://h:8080/a", "/b", out, sizeof out) && !strcmp(out, "http://h:8080/b"), "keeps a port");
    CHECK(!httpc_url_resolve(base, "", out, sizeof out), "empty location");
    CHECK(!httpc_url_resolve(base, "mailto:x@y", out, sizeof out) || strncmp(out, "https://", 8) == 0, "mailto is not followed off-site");
    CHECK(!httpc_url_resolve(base, "/a", out, 10), "does not fit");
    CHECK(!httpc_url_resolve(base, "/a b", out, sizeof out), "a location the request line cannot carry");

    printf("httpc parse: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
