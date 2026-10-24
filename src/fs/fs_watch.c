/* Editor notices for files that changed.
 *
 * LittleFS writes run on the main loop. The SD worker is the only context
 * that may touch the card, and it yields back into that loop, so a note is
 * queued and emitted from fs_watch_poll. The Lua listener stays the first
 * fs_on_change hook and is not given this work.
 *
 * The tables below are the RAM budget for this feature. The product image
 * had about 6 KB free before them, and core0's stack is 2 KB, so the stat
 * JSON is built in api_scratch (the main loop is the only caller) and each
 * row stores one path. A card path is "sd/" plus the card-relative name;
 * the worker opens path+3. 131 covers a LittleFS name (128) and "sd/" plus
 * a job path (128).
 */
#include "fs/fs_watch.h"

#include <stdio.h>
#include <string.h>

#include "api/api.h"
#include "autoconf.h"

#if CONFIG_SD
#include "sd/sd_store.h"
#include "sd/sd_worker.h"
#endif

#define Q 8
#define WATCH_PATH 131

typedef struct {
    char path[WATCH_PATH + 1];
    char op[8];
    uint32_t size;
    uint32_t hash;
    uint8_t has_hash;
} note_t;

typedef struct {
    char path[WATCH_PATH + 1];
    uint32_t size;
    uint32_t hash;
    uint8_t has_hash;
    uint8_t missing;
    uint8_t sd;
    uint8_t done;
} row_t;

_Static_assert(sizeof(note_t) == 152, "note queue size is part of the RAM budget");
_Static_assert(sizeof(row_t) == 144, "stat table size is part of the RAM budget");

static note_t s_q[Q];
static uint8_t s_h, s_t, s_n;

uint32_t fs_fnv_feed(uint32_t h, const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

static bool lfs_hash(const char *name, uint32_t *hash) {
    size_t sz = 0;
    int h = fs_begin_read(name, &sz);
    if (h < 0) { return false; }
    uint32_t hv = FS_FNV_INIT;
    uint8_t buf[128];
    while (sz) {
        size_t n = sz > sizeof buf ? sizeof buf : sz;
        int r = fs_read(h, buf, n);
        if (r <= 0) { fs_end_read(h); return false; }
        hv = fs_fnv_feed(hv, buf, (size_t)r);
        sz -= (size_t)r;
    }
    fs_end_read(h);
    *hash = hv;
    return true;
}

void fs_watch_push(const char *path, const char *op, uint32_t size, uint32_t hash, bool has_hash) {
    if (!path || !path[0] || !op) { return; }
    if (s_n == Q) {
        s_h = (uint8_t)((s_h + 1) % Q);
        s_n--;
    }
    note_t *n = &s_q[s_t];
    memset(n, 0, sizeof *n);
    snprintf(n->path, sizeof n->path, "%s", path);
    snprintf(n->op, sizeof n->op, "%s", op);
    n->size = size;
    n->hash = hash;
    n->has_hash = has_hash ? 1 : 0;
    s_t = (uint8_t)((s_t + 1) % Q);
    s_n++;
}

void fs_watch_on_lfs(const char *name, const char *op) {
    size_t sz = 0;
    uint32_t hash = 0;
    bool has = false;
    if (name && strcmp(op, "delete") != 0 && fs_stat(name, &sz)) {
        if (sz <= FS_HASH_MAX) { has = lfs_hash(name, &hash); }
    }
    fs_watch_push(name, op, (uint32_t)sz, hash, has);
}

static bool append(char *dst, size_t cap, size_t *n, const char *s) {
    size_t k = strlen(s);
    if (*n + k >= cap) { return false; }
    memcpy(dst + *n, s, k);
    *n += k;
    dst[*n] = 0;
    return true;
}

/* fs.changed is emitted from the main loop, which owns api_scratch. */
static void emit_one(const note_t *n) {
    char *dst = api_scratch();
    size_t cap = api_scratch_size();
    size_t u = 0;
    if (!append(dst, cap, &u, "{\"path\":\"")) { return; }
    u += api_json_escape(dst + u, cap - u, n->path);
    char tail[80];
    if (n->has_hash) {
        snprintf(tail, sizeof tail, "\",\"op\":\"%s\",\"size\":%lu,\"hash\":\"%08lx\"}",
                 n->op, (unsigned long)n->size, (unsigned long)n->hash);
    } else if (strcmp(n->op, "delete") == 0) {
        snprintf(tail, sizeof tail, "\",\"op\":\"delete\",\"missing\":true}");
    } else {
        snprintf(tail, sizeof tail, "\",\"op\":\"%s\",\"size\":%lu}",
                 n->op, (unsigned long)n->size);
    }
    if (!append(dst, cap, &u, tail)) { return; }
    api_emit("fs.changed", dst);
}

/* ---- fs.stat ------------------------------------------------------------ */

static row_t s_rows[FS_STAT_MAX];
static int s_rows_n;
static int s_stat; /* 0 idle, 1 card, 2 ready */

static void fill_lfs(row_t *r) {
    size_t sz = 0;
    if (!fs_stat(r->path, &sz)) {
        r->missing = 1;
        r->done = 1;
        return;
    }
    r->size = (uint32_t)sz;
    if (sz <= FS_HASH_MAX) {
        uint32_t h = 0;
        if (lfs_hash(r->path, &h)) {
            r->hash = h;
            r->has_hash = 1;
        }
    }
    r->done = 1;
}

int fs_watch_stat_begin(const char *params) {
    if (s_stat != 0) { return -2; }
    const char *p = strstr(params ? params : "", "\"paths\"");
    if (!p) { return -1; }
    p = strchr(p, '[');
    if (!p) { return -1; }
    p++;
    memset(s_rows, 0, sizeof s_rows);
    int n = 0;
    bool over = false;
    bool need_sd = false;
    while (*p && *p != ']') {
        while (*p == ' ' || *p == ',' || *p == '\n' || *p == '\r' || *p == '\t') { p++; }
        if (*p == ']') { break; }
        if (*p != '"') { return -1; }
        p++;
        char tmp[WATCH_PATH + 1];
        size_t i = 0;
        while (*p && *p != '"') {
            if (*p == '\\' && p[1]) { p++; }
            if (i < WATCH_PATH) { tmp[i++] = *p; }
            p++;
        }
        tmp[i] = 0;
        if (*p == '"') { p++; }
        if (!i) { continue; }
        if (n >= FS_STAT_MAX) { over = true; continue; }
        memcpy(s_rows[n].path, tmp, i + 1);
        if (strncmp(tmp, "sd/", 3) == 0) {
            s_rows[n].sd = 1;
            need_sd = true;
        } else {
            fill_lfs(&s_rows[n]);
        }
        n++;
    }
    if (over) { return -3; }
    s_rows_n = n;
    s_stat = need_sd ? 1 : 2;
    return 0;
}

int fs_watch_stat_state(void) { return s_stat; }

void fs_watch_stat_drop(void) {
    if (s_stat == 2) { s_stat = 0; }
}

bool fs_watch_stat_json(char *dst, size_t cap) {
    size_t n = 0;
    if (!append(dst, cap, &n, "{\"files\":[")) { return false; }
    for (int i = 0; i < s_rows_n; i++) {
        const row_t *r = &s_rows[i];
        if (i && !append(dst, cap, &n, ",")) { return false; }
        if (!append(dst, cap, &n, "{\"path\":\"")) { return false; }
        n += api_json_escape(dst + n, cap - n, r->path);
        char tail[64];
        if (r->missing) {
            snprintf(tail, sizeof tail, "\",\"missing\":true}");
        } else if (r->has_hash) {
            snprintf(tail, sizeof tail, "\",\"size\":%lu,\"hash\":\"%08lx\"}",
                     (unsigned long)r->size, (unsigned long)r->hash);
        } else {
            snprintf(tail, sizeof tail, "\",\"size\":%lu}", (unsigned long)r->size);
        }
        if (!append(dst, cap, &n, tail)) { return false; }
    }
    return append(dst, cap, &n, "]}");
}

#if CONFIG_SD
static bool hash_open(ls_file *f, uint32_t size, uint32_t *hash) {
    if (ls_seek(f, 0) < 0) { return false; }
    uint32_t h = FS_FNV_INIT;
    uint32_t left = size;
    uint8_t buf[256];
    while (left) {
        uint32_t n = left > sizeof buf ? sizeof buf : left;
        int r = ls_read(f, buf, n);
        if (r <= 0) { return false; }
        h = fs_fnv_feed(h, buf, (size_t)r);
        left -= (uint32_t)r;
        sdw_maybe_yield(2000);
    }
    *hash = h;
    return true;
}

/* One open: size, and the hash when the file is small enough to send. */
static bool note_open(const char *rel, uint32_t *size, uint32_t *hash, bool *has_hash) {
    *size = 0;
    *has_hash = false;
    ls_file *f = ls_open(rel, LS_RD);
    if (!f) { return false; }
    uint64_t sz = ls_size(f);
    *size = sz > 0xffffffffu ? 0xffffffffu : (uint32_t)sz;
    if (sz <= FS_HASH_MAX && hash_open(f, (uint32_t)sz, hash)) { *has_hash = true; }
    ls_close(f);
    return true;
}

void fs_watch_note_sd(const char *rel, const char *op) {
    if (!rel || !rel[0] || !op) { return; }
    char path[WATCH_PATH + 1];
    snprintf(path, sizeof path, "sd/%s", rel);
    if (strcmp(op, "delete") == 0) {
        fs_watch_push(path, "delete", 0, 0, false);
        return;
    }
    uint32_t size = 0, hash = 0;
    bool has = false;
    note_open(rel, &size, &hash, &has);
    fs_watch_push(path, op, size, hash, has);
}

bool fs_watch_sd_slice(void) {
    if (s_stat != 1) { return false; }
    for (int i = 0; i < s_rows_n; i++) {
        if (!s_rows[i].sd || s_rows[i].done) { continue; }
        uint32_t size = 0, hash = 0;
        bool has = false;
        if (!note_open(s_rows[i].path + 3, &size, &hash, &has)) {
            s_rows[i].missing = 1;
        } else {
            s_rows[i].size = size;
            s_rows[i].hash = hash;
            s_rows[i].has_hash = has ? 1 : 0;
        }
        s_rows[i].done = 1;
        for (int k = i + 1; k < s_rows_n; k++) {
            if (s_rows[k].sd && !s_rows[k].done) { return true; }
        }
        s_stat = 2;
        return true;
    }
    s_stat = 2;
    return false;
}
#else
bool fs_watch_sd_slice(void) { return false; }
void fs_watch_note_sd(const char *rel, const char *op) {
    (void)rel; (void)op;
}
#endif

void fs_watch_poll(void) {
    while (s_n) {
        emit_one(&s_q[s_h]);
        s_h = (uint8_t)((s_h + 1) % Q);
        s_n--;
    }
    if (s_stat == 2) {
        char *json = api_scratch();
        if (fs_watch_stat_json(json, api_scratch_size())) { api_emit("fs.stated", json); }
        s_stat = 0;
    }
}
