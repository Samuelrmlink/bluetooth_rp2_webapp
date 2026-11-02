#include "net/peer_fwd_msg.h"

#include <stdio.h>
#include <string.h>

static const char *skip_ws(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

static int name_ok(const char *s, size_t n) {
    size_t i;
    if (!n || n > 63 || s[0] == '-') return 0;
    for (i = 0; i < n; i++) {
        char c = s[i];
        int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                 (c >= '0' && c <= '9') || c == '-';
        if (!ok) return 0;
    }
    return 1;
}

/* Point at the first key, or NULL when the text is not an object. */
static const char *first_key(const char *text) {
    const char *p;
    if (!text) return NULL;
    p = skip_ws(text);
    if (*p != '{') return NULL;
    return skip_ws(p + 1);
}

/* After a leading "node":"..." field, including its comma. NULL if it does
 * not close. *empty is set when the string is empty. */
static const char *after_node(const char *text, int *empty, int *bad_name) {
    const char *p = first_key(text);
    const char *start;
    size_t n;
    *empty = 0;
    *bad_name = 0;
    if (!p || strncmp(p, "\"node\"", 6) != 0) return NULL;
    p = skip_ws(p + 6);
    if (*p != ':') return NULL;
    p = skip_ws(p + 1);
    if (*p != '"') return NULL;
    start = ++p;
    while (*p && *p != '"') {
        if (*p == '\\') return NULL;
        p++;
    }
    if (*p != '"') return NULL;
    n = (size_t)(p - start);
    if (!n) *empty = 1;
    else if (!name_ok(start, n)) *bad_name = 1;
    p = skip_ws(p + 1);
    if (*p == ',') p = skip_ws(p + 1);
    return p;
}

static const char *after_id_number(const char *p) {
    p = skip_ws(p);
    if (strncmp(p, "\"id\"", 4) != 0) return NULL;
    p = skip_ws(p + 4);
    if (*p != ':') return NULL;
    p = skip_ws(p + 1);
    if (*p == '-') p++;
    if (*p < '0' || *p > '9') return NULL;
    while (*p >= '0' && *p <= '9') p++;
    return p;
}

static int parse_id(const char *p, long *id) {
    const char *n;
    long v = 0;
    int neg = 0;
    p = skip_ws(p);
    if (strncmp(p, "\"id\"", 4) != 0) return 0;
    p = skip_ws(p + 4);
    if (*p != ':') return -1;
    p = skip_ws(p + 1);
    if (*p == '-') { neg = 1; p++; }
    if (*p < '0' || *p > '9') return -1;
    n = p;
    while (*p >= '0' && *p <= '9') {
        v = v * 10 + (*p - '0');
        p++;
    }
    if (p == n) return -1;
    if (id) *id = neg ? -v : v;
    return 1;
}

int peer_msg_node(const char *text, char *name, size_t cap) {
    int empty = 0, bad = 0;
    const char *p = first_key(text);
    const char *rest;
    size_t n;
    if (!name || cap < 2) return -1;
    name[0] = 0;
    if (!p || strncmp(p, "\"node\"", 6) != 0) return 0;
    rest = after_node(text, &empty, &bad);
    if (!rest) return -1;
    if (empty) return 0;
    if (bad) return -1;
    p = skip_ws(p + 6);
    p = skip_ws(p + 1); /* colon */
    p = skip_ws(p + 1); /* quote */
    n = 0;
    while (p[n] != '"') n++;
    if (n + 1 > cap) return -1;
    memcpy(name, p, n);
    name[n] = 0;
    return 1;
}

int peer_msg_request_id(const char *text, long *id) {
    int empty = 0, bad = 0;
    const char *p = after_node(text, &empty, &bad);
    int rc;
    if (!p || empty || bad) return -1;
    rc = parse_id(p, id);
    return rc == 1 ? 1 : -1;
}

int peer_msg_kind(const char *text, long *id) {
    const char *p = first_key(text);
    int rc;
    if (!p) return 0;
    if (strncmp(p, "\"evt\"", 5) == 0) return 2;
    if (strncmp(p, "\"id\"", 4) != 0) return 0;
    rc = parse_id(p, id);
    return rc == 0 ? 0 : rc;
}

int peer_msg_forward(const char *text, unsigned peer_id, char *dst, size_t cap) {
    int empty = 0, bad = 0;
    const char *p = after_node(text, &empty, &bad);
    const char *rest;
    int n;
    if (!p || empty || bad) return -1;
    rest = after_id_number(p);
    if (!rest) return -1;
    n = snprintf(dst, cap, "{\"id\":%u%s", peer_id, rest);
    if (n < 0 || (size_t)n >= cap) return -1;
    return n;
}

int peer_msg_tag(const char *text, const char *name, long browser_id, int replace_id,
                 char *dst, size_t cap) {
    const char *p;
    int n;
    if (!name || !name[0] || !dst) return -1;
    p = first_key(text);
    if (!p) return -1;
    if (replace_id) {
        const char *rest = after_id_number(p);
        if (!rest) return -1;
        n = snprintf(dst, cap, "{\"node\":\"%s\",\"id\":%ld%s", name, browser_id, rest);
    } else {
        n = snprintf(dst, cap, "{\"node\":\"%s\",%s", name, p);
    }
    if (n < 0 || (size_t)n >= cap) return -1;
    return n;
}
