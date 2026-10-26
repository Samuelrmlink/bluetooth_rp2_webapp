/* A path list in a POST body. No fs.h: the host test compiles this alone. */
#include "fs_batch.h"

#include <string.h>

static const char *skip_ws(const char *p, const char *end) {
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) { p++; }
    return p;
}

/* One JSON string into out. -1 on a bad escape, a truncated path, or "". */
static int parse_string(const char **pp, const char *end, char *out, size_t cap) {
    const char *p = *pp;
    if (p >= end || *p != '"') { return -1; }
    p++;
    size_t n = 0;
    while (p < end && *p != '"') {
        unsigned char c;
        if (*p == '\\') {
            if (++p >= end) { return -1; }
            switch (*p) {
                case '"': case '\\': case '/': c = (unsigned char)*p; break;
                case 'n': c = '\n'; break;
                case 'r': c = '\r'; break;
                case 't': c = '\t'; break;
                case 'u': {
                    if (p + 4 >= end) { return -1; }
                    unsigned v = 0;
                    for (int i = 1; i <= 4; i++) {
                        char h = p[i];
                        int d = -1;
                        if (h >= '0' && h <= '9') { d = h - '0'; }
                        else if (h >= 'a' && h <= 'f') { d = h - 'a' + 10; }
                        else if (h >= 'A' && h <= 'F') { d = h - 'A' + 10; }
                        if (d < 0) { return -1; }
                        v = (v << 4) | (unsigned)d;
                    }
                    if (v > 0x7fu) { return -1; }
                    c = (unsigned char)v;
                    p += 4;
                    break;
                }
                default: return -1;
            }
        } else {
            c = (unsigned char)*p;
        }
        if (n + 1 >= cap) { return -1; }
        out[n++] = (char)c;
        p++;
    }
    if (p >= end || *p != '"' || n == 0) { return -1; }
    out[n] = 0;
    *pp = p + 1;
    return 0;
}

static int parse_u32(const char **pp, const char *end, uint32_t *out) {
    const char *p = *pp;
    if (p >= end || *p < '0' || *p > '9') { return -1; }
    uint32_t v = 0;
    while (p < end && *p >= '0' && *p <= '9') {
        v = v * 10u + (uint32_t)(*p - '0');
        p++;
    }
    *out = v;
    *pp = p;
    return 0;
}

static int skip_string(const char **pp, const char *end) {
    const char *p = *pp;
    if (p >= end || *p != '"') { return -1; }
    p++;
    while (p < end && *p != '"') {
        if (*p == '\\') {
            if (++p >= end) { return -1; }
            if (*p == 'u') {
                if (p + 4 >= end) { return -1; }
                p += 4;
            }
        }
        p++;
    }
    if (p >= end || *p != '"') { return -1; }
    *pp = p + 1;
    return 0;
}

static int skip_value(const char **pp, const char *end);

static int skip_value(const char **pp, const char *end) {
    const char *p = skip_ws(*pp, end);
    if (p >= end) { return -1; }
    if (*p == '"') { *pp = p; return skip_string(pp, end); }
    if (*p == '{') {
        p++;
        p = skip_ws(p, end);
        if (p < end && *p == '}') { *pp = p + 1; return 0; }
        for (;;) {
            *pp = p;
            if (skip_string(pp, end) < 0) { return -1; }
            p = skip_ws(*pp, end);
            if (p >= end || *p != ':') { return -1; }
            *pp = p + 1;
            if (skip_value(pp, end) < 0) { return -1; }
            p = skip_ws(*pp, end);
            if (p < end && *p == ',') { p = skip_ws(p + 1, end); continue; }
            if (p < end && *p == '}') { *pp = p + 1; return 0; }
            return -1;
        }
    }
    if (*p == '[') {
        p++;
        p = skip_ws(p, end);
        if (p < end && *p == ']') { *pp = p + 1; return 0; }
        for (;;) {
            *pp = p;
            if (skip_value(pp, end) < 0) { return -1; }
            p = skip_ws(*pp, end);
            if (p < end && *p == ',') { p = skip_ws(p + 1, end); continue; }
            if (p < end && *p == ']') { *pp = p + 1; return 0; }
            return -1;
        }
    }
    if (*p == '-' || (*p >= '0' && *p <= '9')) {
        if (*p == '-') { p++; }
        if (p >= end || *p < '0' || *p > '9') { return -1; }
        while (p < end && ((*p >= '0' && *p <= '9') || *p == '.' || *p == 'e' || *p == 'E' || *p == '+' || *p == '-')) { p++; }
        *pp = p;
        return 0;
    }
    if (end - p >= 4 && memcmp(p, "true", 4) == 0) { *pp = p + 4; return 0; }
    if (end - p >= 5 && memcmp(p, "false", 5) == 0) { *pp = p + 5; return 0; }
    if (end - p >= 4 && memcmp(p, "null", 4) == 0) { *pp = p + 4; return 0; }
    return -1;
}

int fs_batch_parse(const char *json, size_t len,
                   char paths[][FS_BATCH_PATH], int *npaths,
                   uint32_t *mtime, uint32_t *id) {
    if (!json || !paths || !npaths) { return -1; }
    const char *p = json;
    const char *end = json + len;
    int n = 0;
    bool saw_paths = false;
    if (mtime) { *mtime = 0; }
    if (id) { *id = 0; }
    p = skip_ws(p, end);
    if (p >= end || *p != '{') { return -1; }
    p++;
    p = skip_ws(p, end);
    if (p < end && *p == '}') { return -1; }
    while (p < end) {
        char key[16];
        const char *ks = p;
        if (parse_string(&ks, end, key, sizeof key) < 0) { return -1; }
        p = skip_ws(ks, end);
        if (p >= end || *p != ':') { return -1; }
        p = skip_ws(p + 1, end);
        if (strcmp(key, "paths") == 0) {
            if (saw_paths || p >= end || *p != '[') { return -1; }
            saw_paths = true;
            p++;
            p = skip_ws(p, end);
            if (p < end && *p == ']') {
                return -1;                          /* an empty list is not a request */
            }
            while (p < end && *p != ']') {
                if (n >= FS_BATCH_MAX) { return -2; }
                if (parse_string(&p, end, paths[n], FS_BATCH_PATH) < 0) { return -1; }
                n++;
                p = skip_ws(p, end);
                if (p < end && *p == ',') { p = skip_ws(p + 1, end); continue; }
                break;
            }
            if (p >= end || *p != ']') { return -1; }
            p++;
        } else if (strcmp(key, "mtime") == 0 || strcmp(key, "id") == 0) {
            uint32_t v = 0;
            if (parse_u32(&p, end, &v) < 0) { return -1; }
            if (key[0] == 'm') { if (mtime) { *mtime = v; } }
            else if (id) { *id = v; }
        } else {
            const char *s = p;
            if (skip_value(&s, end) < 0) { return -1; }
            p = s;
        }
        p = skip_ws(p, end);
        if (p < end && *p == ',') { p = skip_ws(p + 1, end); continue; }
        if (p < end && *p == '}') { p++; break; }
        return -1;
    }
    if (!saw_paths || n == 0) { return -1; }
    *npaths = n;
    return 0;
}

bool fs_batch_covers(const char roots[][FS_BATCH_PATH], int n, const char *path) {
    if (!path || !roots || n <= 0) { return false; }
    size_t plen = strlen(path);
    for (int i = 0; i < n; i++) {
        size_t rlen = strlen(roots[i]);
        if (rlen == 0) { continue; }
        if (rlen == plen && memcmp(roots[i], path, plen) == 0) { return true; }
        if (rlen < plen && memcmp(roots[i], path, rlen) == 0 && path[rlen] == '/') { return true; }
    }
    return false;
}

int fs_batch_next(int *index, int total, bool closed) {
    if (closed) { return -1; }
    if (!index || *index >= total) { return -2; }
    int cur = *index;
    (*index)++;
    return cur;
}
