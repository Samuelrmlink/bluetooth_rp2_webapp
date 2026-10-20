#include "net/httpc_parse.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { S_STATUS, S_HEADER, S_BODY_LEN, S_BODY_CLOSE, S_CHUNK_SIZE, S_CHUNK_DATA, S_CHUNK_CRLF,
       S_TRAILER, S_DONE, S_ERROR };

static bool fail(httpp_t *p, const char *why) {
    p->error = true;
    p->err = why;
    p->state = S_ERROR;
    return false;
}

void httpp_init(httpp_t *p, bool head_request, httpp_header_fn h, httpp_body_fn b, void *ctx) {
    memset(p, 0, sizeof *p);
    p->head = head_request;
    p->content_length = -1;
    p->on_header = h;
    p->on_body = b;
    p->ctx = ctx;
    p->state = S_STATUS;
}

static int lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

static bool ieq(const char *a, const char *b) {
    while (*a && *b) { if (lower((unsigned char)*a++) != lower((unsigned char)*b++)) { return false; } }
    return *a == *b;
}

/* Does a comma-separated header value contain `tok` (case-insensitive)? */
static bool has_token(const char *v, const char *tok);
static bool has_token(const char *v, const char *tok) {
    size_t tl = strlen(tok);
    while (*v) {
        while (*v == ' ' || *v == '\t' || *v == ',') { v++; }
        const char *s = v;
        while (*v && *v != ',') { v++; }
        const char *e = v;
        while (e > s && (e[-1] == ' ' || e[-1] == '\t')) { e--; }
        if ((size_t)(e - s) == tl) {
            size_t k = 0;
            while (k < tl && lower((unsigned char)s[k]) == lower((unsigned char)tok[k])) { k++; }
            if (k == tl) { return true; }
        }
    }
    return false;
}

static void finish(httpp_t *p) { p->done = true; p->state = S_DONE; }

/* The headers are in: decide how the body is framed. */
static void body_start(httpp_t *p) {
    if (p->head || p->status == 204 || p->status == 304 || (p->status >= 100 && p->status < 200)) {
        finish(p);
    } else if (p->chunked) {
        p->state = S_CHUNK_SIZE;
    } else if (p->content_length >= 0) {
        p->left = (uint64_t)p->content_length;
        if (p->left == 0) { finish(p); } else { p->state = S_BODY_LEN; }
    } else {
        p->state = S_BODY_CLOSE;
    }
}

static bool status_line(httpp_t *p) {
    /* HTTP/1.x SP 3DIGIT [SP reason] */
    const char *l = p->line;
    if (strncmp(l, "HTTP/1.", 7) != 0 || !l[7] || l[8] != ' ') { return fail(p, "bad status line"); }
    const char *d = l + 9;
    if (d[0] < '1' || d[0] > '5' || d[1] < '0' || d[1] > '9' || d[2] < '0' || d[2] > '9' ||
        (d[3] && d[3] != ' ')) {
        return fail(p, "bad status code");
    }
    p->status = (d[0] - '0') * 100 + (d[1] - '0') * 10 + (d[2] - '0');
    p->keep_alive = l[7] == '1';
    p->state = S_HEADER;
    return true;
}

static bool header_line(httpp_t *p) {
    if (p->line_len == 0) {
        /* End of headers. A 1xx is an interim response: its headers end it,
         * and the real status line follows. */
        if (p->status >= 100 && p->status < 200) {
            p->status = 0;
            p->content_length = -1;
            p->chunked = false;
            p->state = S_STATUS;
            return true;
        }
        body_start(p);
        return true;
    }
    if (p->line[0] == ' ' || p->line[0] == '\t') { return true; }   /* obsolete folding: dropped */
    char *colon = strchr(p->line, ':');
    if (!colon || colon == p->line) { return fail(p, "bad header line"); }
    *colon = 0;
    for (char *c = p->line; *c; c++) {
        if (*c == ' ' || *c == '\t') { return fail(p, "space in a header name"); }
        *c = (char)lower((unsigned char)*c);
    }
    char *v = colon + 1;
    while (*v == ' ' || *v == '\t') { v++; }
    char *e = v + strlen(v);
    while (e > v && (e[-1] == ' ' || e[-1] == '\t')) { *--e = 0; }
    const char *name = p->line;
    if (strcmp(name, "content-length") == 0) {
        char *end = NULL;
        long long n = strtoll(v, &end, 10);
        if (end == v || *end || n < 0) { return fail(p, "bad content-length"); }
        if (p->content_length >= 0 && p->content_length != n) { return fail(p, "conflicting content-length"); }
        p->content_length = n;
    } else if (strcmp(name, "transfer-encoding") == 0) {
        /* Only chunked (and the no-op identity): a compressed transfer
         * coding would hand the script bytes it cannot use. We never send
         * TE, so a server that does this is broken. */
        char tmp[64];
        snprintf(tmp, sizeof tmp, "%s", v);
        for (char *t = strtok(tmp, ", \t"); t; t = strtok(NULL, ", \t")) {
            if (ieq(t, "chunked")) { p->chunked = true; }
            else if (!ieq(t, "identity")) { return fail(p, "unsupported transfer-encoding"); }
        }
    } else if (strcmp(name, "connection") == 0) {
        if (has_token(v, "close")) { p->keep_alive = false; }
    }
    if (p->on_header) { p->on_header(p->ctx, name, v); }
    return true;
}

static bool chunk_size_line(httpp_t *p) {
    const char *l = p->line;
    uint64_t n = 0;
    int digits = 0;
    for (; *l; l++) {
        int c = lower((unsigned char)*l), h;
        if (c >= '0' && c <= '9') { h = c - '0'; }
        else if (c >= 'a' && c <= 'f') { h = c - 'a' + 10; }
        else { break; }
        if (++digits > 15) { return fail(p, "chunk too large"); }
        n = (n << 4) | (uint64_t)h;
    }
    if (!digits) { return fail(p, "bad chunk size"); }
    while (*l == ' ' || *l == '\t') { l++; }
    if (*l && *l != ';') { return fail(p, "bad chunk size"); }       /* ;ext=... is ignored */
    if (n == 0) { p->state = S_TRAILER; return true; }
    p->left = n;
    p->state = S_CHUNK_DATA;
    return true;
}

/* A line-oriented state has a complete line in p->line. */
static bool on_line(httpp_t *p) {
    switch (p->state) {
        case S_STATUS:
            if (p->line_len == 0) { return true; }      /* tolerate a stray CRLF before it */
            return status_line(p);
        case S_HEADER:     return header_line(p);
        case S_CHUNK_SIZE: return chunk_size_line(p);
        case S_CHUNK_CRLF:
            if (p->line_len != 0) { return fail(p, "no CRLF after a chunk"); }
            p->state = S_CHUNK_SIZE;
            return true;
        case S_TRAILER:
            if (p->line_len == 0) { finish(p); }        /* trailers are skipped */
            return true;
        default:
            return fail(p, "internal");
    }
}

static bool line_state(int s) {
    return s == S_STATUS || s == S_HEADER || s == S_CHUNK_SIZE || s == S_CHUNK_CRLF || s == S_TRAILER;
}

size_t httpp_feed(httpp_t *p, const uint8_t *data, size_t n) {
    size_t i = 0;
    while (i < n && !p->done && !p->error) {
        if (line_state(p->state)) {
            uint8_t c = data[i++];
            if (c == '\n') {
                if (p->line_len && p->line[p->line_len - 1] == '\r') { p->line_len--; }
                p->line[p->line_len] = 0;
                on_line(p);
                p->line_len = 0;
                p->line_cut = false;
                continue;
            }
            if (p->line_len + 1 < HTTPP_LINE) {
                p->line[p->line_len++] = (char)c;
            } else {
                /* A status or chunk-size line this long is garbage; a long
                 * header value is cut (the name, which is what matters, is
                 * at the front). */
                if (p->state != S_HEADER && p->state != S_TRAILER) { fail(p, "line too long"); break; }
                p->line_cut = true;
            }
            continue;
        }
        size_t avail = n - i;
        if (p->state == S_BODY_CLOSE) {
            if (p->on_body) { p->on_body(p->ctx, data + i, avail); }
            p->body_bytes += avail;
            i += avail;
            continue;
        }
        /* S_BODY_LEN, S_CHUNK_DATA */
        size_t take = avail < p->left ? avail : (size_t)p->left;
        if (p->on_body && take) { p->on_body(p->ctx, data + i, take); }
        p->body_bytes += take;
        p->left -= take;
        i += take;
        if (p->left == 0) {
            if (p->state == S_BODY_LEN) { finish(p); }
            else { p->state = S_CHUNK_CRLF; }
        }
    }
    return i;
}

void httpp_eof(httpp_t *p) {
    if (p->done || p->error) { return; }
    if (p->state == S_BODY_CLOSE) { finish(p); return; }
    fail(p, p->status ? "closed early" : "closed before a response");
}

bool httpp_headers_done(const httpp_t *p) {
    return p->state != S_STATUS && p->state != S_HEADER;
}

/* --- URLs ------------------------------------------------------------------ */

bool httpc_url_parse(const char *url, httpc_url_t *u) {
    memset(u, 0, sizeof *u);
    const char *p;
    if (strncmp(url, "http://", 7) == 0)       { u->tls = false; p = url + 7; u->port = 80; }
    else if (strncmp(url, "https://", 8) == 0) { u->tls = true;  p = url + 8; u->port = 443; }
    else { return false; }
    const char *h = p;
    while (*p && *p != '/' && *p != ':' && *p != '?' && *p != '#') {
        if (*p == '@' || *p == '[' || *p == ' ' || (unsigned char)*p < 0x21) { return false; }  /* userinfo, IPv6, junk */
        p++;
    }
    size_t hl = (size_t)(p - h);
    if (hl == 0 || hl >= sizeof u->host) { return false; }
    memcpy(u->host, h, hl);
    u->host[hl] = 0;
    if (*p == ':') {
        p++;
        long port = 0;
        int digits = 0;
        while (*p >= '0' && *p <= '9') { port = port * 10 + (*p++ - '0'); if (++digits > 5) { return false; } }
        if (!digits || port < 1 || port > 65535) { return false; }
        if (*p && *p != '/' && *p != '?' && *p != '#') { return false; }
        u->port = (uint16_t)port;
    }
    if (*p == '/' || *p == '?') {
        u->path = p;
        const char *e = p;
        while (*e && *e != '#') { e++; }   /* a fragment is never sent */
        u->path_len = (size_t)(e - p);
    } else {
        u->path = "/";
        u->path_len = 1;
    }
    for (size_t k = 0; k < u->path_len; k++) {
        unsigned char c = (unsigned char)u->path[k];
        if (c <= 0x20 || c == 0x7F) { return false; }   /* would break the request line */
    }
    return true;
}

bool httpc_url_resolve(const char *base, const char *loc, char *out, size_t cap) {
    while (*loc == ' ' || *loc == '\t') { loc++; }
    if (!*loc) { return false; }
    httpc_url_t b;
    if (!httpc_url_parse(base, &b)) { return false; }
    int n;
    if (strncmp(loc, "http://", 7) == 0 || strncmp(loc, "https://", 8) == 0) {
        n = snprintf(out, cap, "%s", loc);
    } else if (loc[0] == '/' && loc[1] == '/') {
        n = snprintf(out, cap, "%s:%s", b.tls ? "https" : "http", loc);
    } else {
        /* scheme://host[:port] of the base */
        char origin[160];
        bool dflt = b.port == (b.tls ? 443 : 80);
        if (dflt) { snprintf(origin, sizeof origin, "%s://%s", b.tls ? "https" : "http", b.host); }
        else { snprintf(origin, sizeof origin, "%s://%s:%u", b.tls ? "https" : "http", b.host, b.port); }
        if (loc[0] == '/') {
            n = snprintf(out, cap, "%s%s", origin, loc);
        } else if (loc[0] == '?') {
            size_t pl = 0;
            while (pl < b.path_len && b.path[pl] != '?') { pl++; }
            n = snprintf(out, cap, "%s%.*s%s", origin, (int)pl, b.path, loc);
        } else {
            /* Relative to the base path's directory. "./" and "../" are not
             * collapsed: servers that redirect relatively use plain names. */
            size_t dir = 0;
            for (size_t k = 0; k < b.path_len && b.path[k] != '?'; k++) {
                if (b.path[k] == '/') { dir = k + 1; }
            }
            n = snprintf(out, cap, "%s%.*s%s", origin, (int)dir, b.path, loc);
        }
    }
    if (n < 0 || (size_t)n >= cap) { return false; }
    httpc_url_t chk;
    return httpc_url_parse(out, &chk);
}
