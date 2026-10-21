/*
 * The json group (host_api.json id 8): path lookups on a JSON text and a
 * streaming writer. Portable C11, no allocation; any host can register it:
 *     pex_register_group(&pex_group_json);
 *
 * Paths: "a.b.2.c" -- object keys, and decimal indexes into arrays. A key
 * containing '.' cannot be addressed (none of ours do).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pex_rt.h"

static const char *ws(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') { p++; }
    return p;
}

/* End of the string starting at p (p at the opening quote), past the closing quote. */
static const char *skip_str(const char *p) {
    for (p++; *p; p++) {
        if (*p == '\\') { if (!*++p) { return NULL; } }
        else if (*p == '"') { return p + 1; }
    }
    return NULL;
}

/* End of the value at p, or NULL if malformed. */
static const char *skip_val(const char *p) {
    p = ws(p);
    if (*p == '"') { return skip_str(p); }
    if (*p == '{' || *p == '[') {
        int depth = 0;
        while (*p) {
            if (*p == '"') { p = skip_str(p); if (!p) { return NULL; } continue; }
            if (*p == '{' || *p == '[') { depth++; }
            else if (*p == '}' || *p == ']') { if (--depth == 0) { return p + 1; } }
            p++;
        }
        return NULL;
    }
    const char *s = p;
    while (*p && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && *p != '\r' && *p != '\n' && *p != '\t') { p++; }
    return p > s ? p : NULL;
}

/* The value for one path segment inside the container at p. */
static const char *step(const char *p, const char *seg, size_t sl) {
    p = ws(p);
    if (*p == '{') {
        p = ws(p + 1);
        while (*p == '"') {
            const char *ke = skip_str(p);
            if (!ke) { return NULL; }
            bool match = (size_t)(ke - p - 2) == sl && memcmp(p + 1, seg, sl) == 0;
            p = ws(ke);
            if (*p != ':') { return NULL; }
            p = ws(p + 1);
            if (match) { return p; }
            p = skip_val(p);
            if (!p) { return NULL; }
            p = ws(p);
            if (*p != ',') { return NULL; }
            p = ws(p + 1);
        }
        return NULL;
    }
    if (*p == '[') {
        char *end;
        char tmp[12];
        if (sl == 0 || sl >= sizeof tmp) { return NULL; }
        memcpy(tmp, seg, sl);
        tmp[sl] = 0;
        long idx = strtol(tmp, &end, 10);
        if (*end || idx < 0) { return NULL; }
        p = ws(p + 1);
        if (*p == ']') { return NULL; }
        for (long i = 0; i < idx; i++) {
            p = skip_val(p);
            if (!p) { return NULL; }
            p = ws(p);
            if (*p != ',') { return NULL; }
            p = ws(p + 1);
        }
        return p;
    }
    return NULL;
}

const char *pex_json_find(const char *json, const char *path, size_t *len) {
    if (!json) { return NULL; }
    const char *p = ws(json);
    while (path && *path) {
        const char *dot = strchr(path, '.');
        size_t sl = dot ? (size_t)(dot - path) : strlen(path);
        p = step(p, path, sl);
        if (!p) { return NULL; }
        path = dot ? dot + 1 : NULL;
    }
    const char *e = skip_val(p);
    if (!e) { return NULL; }
    if (len) { *len = (size_t)(e - p); }
    return p;
}

static void put_utf8(char **o, char *end, unsigned cp) {
    char b[4];
    int n;
    if (cp < 0x80) { b[0] = (char)cp; n = 1; }
    else if (cp < 0x800) { b[0] = (char)(0xC0 | cp >> 6); b[1] = (char)(0x80 | (cp & 63)); n = 2; }
    else { b[0] = (char)(0xE0 | cp >> 12); b[1] = (char)(0x80 | ((cp >> 6) & 63)); b[2] = (char)(0x80 | (cp & 63)); n = 3; }
    for (int i = 0; i < n && *o < end; i++) { *(*o)++ = b[i]; }
}

int pex_json_str(const char *json, const char *path, char *out, size_t cap) {
    size_t n;
    const char *v = pex_json_find(json, path, &n);
    if (!v || *v != '"' || !cap) { return 0; }
    char *o = out, *end = out + cap - 1;
    for (const char *p = v + 1; p < v + n - 1 && o < end; p++) {
        if (*p != '\\') { *o++ = *p; continue; }
        p++;
        switch (*p) {
        case 'n': *o++ = '\n'; break;
        case 't': *o++ = '\t'; break;
        case 'r': *o++ = '\r'; break;
        case 'b': *o++ = '\b'; break;
        case 'f': *o++ = '\f'; break;
        case 'u': {
            unsigned cp = 0;
            for (int k = 1; k <= 4 && p[k]; k++) {
                char c = p[k];
                cp = cp * 16 + (unsigned)(c <= '9' ? c - '0' : (c | 32) - 'a' + 10);
            }
            p += 4;
            put_utf8(&o, end, cp);
            break;
        }
        default: *o++ = *p; break;
        }
    }
    *o = 0;
    return 1;
}

int pex_json_int(const char *json, const char *path, int64_t *out) {
    size_t n;
    const char *v = pex_json_find(json, path, &n);
    if (!v || !(*v == '-' || (*v >= '0' && *v <= '9'))) { return 0; }
    char *e;
    long long x = strtoll(v, &e, 10);
    if (out) { *out = (int64_t)x; }
    return e > v;
}

int pex_json_num(const char *json, const char *path, double *out) {
    size_t n;
    const char *v = pex_json_find(json, path, &n);
    if (!v || !(*v == '-' || (*v >= '0' && *v <= '9'))) { return 0; }
    char *e;
    double x = strtod(v, &e);
    if (out) { *out = x; }
    return e > v;
}

int pex_json_bool(const char *json, const char *path, int *out) {
    size_t n;
    const char *v = pex_json_find(json, path, &n);
    if (!v) { return 0; }
    if (n == 4 && memcmp(v, "true", 4) == 0) { if (out) { *out = 1; } return 1; }
    if (n == 5 && memcmp(v, "false", 5) == 0) { if (out) { *out = 0; } return 1; }
    return 0;
}

int pex_json_len(const char *json, const char *path) {
    size_t n;
    const char *v = pex_json_find(json, path, &n);
    if (!v || (*v != '[' && *v != '{')) { return -1; }
    const char *p = ws(v + 1);
    if (*p == ']' || *p == '}') { return 0; }
    int k = 0;
    for (;;) {
        if (*v == '{') {                 /* key: */
            p = skip_str(p);
            if (!p) { return -1; }
            p = ws(p);
            if (*p != ':') { return -1; }
            p = ws(p + 1);
        }
        p = skip_val(p);
        if (!p) { return -1; }
        k++;
        p = ws(p);
        if (*p != ',') { return k; }
        p = ws(p + 1);
    }
}

/* ---- writer --------------------------------------------------------------- */
static void wr(pex_jw_t *w, const char *s, size_t n) {
    if (w->err) { return; }
    if (w->len + n + 1 > w->cap) { w->err = PEX_ENOMEM; return; }
    memcpy(w->buf + w->len, s, n);
    w->len += n;
    w->buf[w->len] = 0;
}
static void wrs(pex_jw_t *w, const char *s) { wr(w, s, strlen(s)); }

static void esc(pex_jw_t *w, const char *s, size_t n) {
    wr(w, "\"", 1);
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        char b[8];
        if (c == '"' || c == '\\') { b[0] = '\\'; b[1] = (char)c; wr(w, b, 2); }
        else if (c == '\n') { wr(w, "\\n", 2); }
        else if (c == '\r') { wr(w, "\\r", 2); }
        else if (c == '\t') { wr(w, "\\t", 2); }
        else if (c < 0x20) { snprintf(b, sizeof b, "\\u%04x", c); wr(w, b, 6); }
        else { wr(w, (const char *)&c, 1); }
    }
    wr(w, "\"", 1);
}

static void sep(pex_jw_t *w, const char *key) {
    uint32_t bit = 1u << (w->depth & 31);
    if (w->depth > 0) {
        if (w->first & bit) { w->first &= ~bit; } else { wr(w, ",", 1); }
    }
    if (key) {
        esc(w, key, strlen(key));
        wr(w, ":", 1);
    }
}

void pex_jw_init(pex_jw_t *w, char *buf, size_t cap) {
    w->buf = buf;
    w->cap = cap;
    w->len = 0;
    w->depth = 0;
    w->first = 0;
    w->arr = 0;
    w->err = cap ? 0 : PEX_ENOMEM;
    if (cap) { buf[0] = 0; }
}
static void open_(pex_jw_t *w, const char *key, const char *c) {
    sep(w, key);
    wrs(w, c);
    w->depth++;
    w->first |= 1u << (w->depth & 31);
    if (*c == '[') { w->arr |= 1u << (w->depth & 31); } else { w->arr &= ~(1u << (w->depth & 31)); }
}
void pex_jw_obj(pex_jw_t *w, const char *key) { open_(w, key, "{"); }
void pex_jw_arr(pex_jw_t *w, const char *key) { open_(w, key, "["); }
void pex_jw_end(pex_jw_t *w) {
    if (!w->depth) { w->err = PEX_EINVAL; return; }
    uint32_t bit = 1u << (w->depth & 31);
    char close = (w->arr & bit) ? ']' : '}';
    w->first &= ~bit;
    w->arr &= ~bit;
    w->depth--;
    wr(w, &close, 1);
}
void pex_jw_str(pex_jw_t *w, const char *key, const char *v) {
    sep(w, key);
    if (v) { esc(w, v, strlen(v)); } else { wrs(w, "null"); }
}
void pex_jw_strn(pex_jw_t *w, const char *key, const char *v, size_t n) { sep(w, key); esc(w, v, n); }
void pex_jw_int(pex_jw_t *w, const char *key, int64_t v) {
    char b[24];
    sep(w, key);
    snprintf(b, sizeof b, "%lld", (long long)v);
    wrs(w, b);
}
void pex_jw_num(pex_jw_t *w, const char *key, double v) {
    char b[32];
    sep(w, key);
    if (v != v || v > 1e308 || v < -1e308) { wrs(w, "null"); return; }
    snprintf(b, sizeof b, "%.17g", v);
    wrs(w, b);
}
void pex_jw_bool(pex_jw_t *w, const char *key, int v) { sep(w, key); wrs(w, v ? "true" : "false"); }
void pex_jw_null(pex_jw_t *w, const char *key) { sep(w, key); wrs(w, "null"); }
void pex_jw_raw(pex_jw_t *w, const char *key, const char *json) { sep(w, key); wrs(w, json ? json : "null"); }
void pex_jw_hex(pex_jw_t *w, const char *key, const void *data, size_t n) {
    static const char hx[] = "0123456789abcdef";
    const uint8_t *p = data;
    sep(w, key);
    wr(w, "\"", 1);
    for (size_t i = 0; i < n; i++) {
        char b[2] = { hx[p[i] >> 4], hx[p[i] & 15] };
        wr(w, b, 2);
    }
    wr(w, "\"", 1);
}
int pex_jw_finish(pex_jw_t *w) {
    if (w->err) { return w->err; }
    if (w->depth) { return PEX_EINVAL; }
    return (int)w->len;
}
