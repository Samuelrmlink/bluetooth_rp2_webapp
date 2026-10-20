/*
 * The storage worker's side of sd_jobs.h, and the main-context half that
 * only ever touches the job record. See the header for the shape.
 */
#include "sd/sd_jobs.h"

#include <stdio.h>
#include <string.h>

#include "sd/sd_store.h"
#include "sd/sd_worker.h"

static sdj_t s_job;
static enum sdj_state s_state;
static uint32_t s_token, s_next_token = 1;
static bool s_abandoned;
static int s_ring = -1;         /* the borrowed transfer ring, -1 = none */
static uint8_t *s_buf;

/* Worker side: the open files, and closes owed. */
static struct {
    ls_file *f;
    uint32_t gen;
    bool close_owed;
} s_files[SDJ_FILES];
static uint32_t s_gen;          /* ls_mount_count() when the table was last valid */

uint8_t *sdj_buf(void) { return s_buf; }

static bool needs_buf(int op) { return op == SDJ_READ || op == SDJ_WRITE || op == SDJ_LIST; }

static void give_back(void) {
    if (s_ring >= 0) { sd_xfer_return(s_ring); }
    s_ring = -1;
    s_buf = NULL;
}

/* ---- main context ---- */

bool sdj_post(const sdj_t *req, uint32_t *token) {
    sdw_lock();
    if (s_state != SDJ_IDLE) { sdw_unlock(); return false; }
    sdw_unlock();
    if (needs_buf(req->op)) {
        s_ring = sd_xfer_borrow();
        if (s_ring < 0) { return false; }
        s_buf = sd_xfer_buf(s_ring);
    }
    sdw_lock();
    s_job = *req;
    s_job.rc = 0;
    s_job.more = false;
    s_abandoned = false;
    s_token = s_next_token++;
    if (!s_next_token) { s_next_token = 1; }
    s_state = SDJ_POSTED;
    if (token) { *token = s_token; }
    sdw_unlock();
    sdw_kick();
    return true;
}

enum sdj_state sdj_poll(uint32_t token, sdj_t *out) {
    sdw_lock();
    enum sdj_state st = (token && token == s_token) ? s_state : SDJ_IDLE;
    if (st == SDJ_DONE && out) { *out = s_job; }
    sdw_unlock();
    return st;
}

void sdj_finish(uint32_t token) {
    sdw_lock();
    if (token && token == s_token && s_state == SDJ_DONE) {
        s_state = SDJ_IDLE;
        s_token = 0;
        give_back();
    }
    sdw_unlock();
}

void sdj_close_async(int file) {
    if (file < 0 || file >= SDJ_FILES) { return; }
    s_files[file].close_owed = true;
    sdw_kick();
}

void sdj_abandon(uint32_t token) {
    sdw_lock();
    if (!token || token != s_token) { sdw_unlock(); return; }
    if (s_state == SDJ_DONE) {
        /* Finished but never collected: a file it opened has no owner now. */
        if (s_job.op == SDJ_OPEN && s_job.rc >= 0) { s_files[s_job.rc].close_owed = true; }
        s_state = SDJ_IDLE;
        s_token = 0;
        give_back();
    } else if (s_state == SDJ_POSTED) {
        s_abandoned = true;     /* the worker frees it (and closes) when done */
    }
    sdw_unlock();
}

/* ---- worker ---- */

static void check_generation(void) {
    uint32_t g = ls_mount_count();
    if (g != s_gen) {
        /* The card was unmounted (pulled, formatted, failed) since: every FIL
         * it had went with it (ls_unmount forgot them), and our indices must
         * not reach whatever reuses those slots. */
        s_gen = g;
        for (int i = 0; i < SDJ_FILES; i++) { s_files[i].f = NULL; s_files[i].close_owed = false; }
    }
}

static ls_file *file_at(int i) {
    if (i < 0 || i >= SDJ_FILES || !s_files[i].f || s_files[i].gen != s_gen) { return NULL; }
    return s_files[i].f;
}

static void close_file(int i) {
    ls_file *f = file_at(i);
    if (f) { ls_close(f); }
    s_files[i].f = NULL;
    s_files[i].close_owed = false;
}

static int mkdir_p(char *p) {
    size_t n = strlen(p);
    for (size_t i = 0; i <= n; i++) {
        if (p[i] != '/' && p[i] != 0) { continue; }
        char c = p[i];
        p[i] = 0;
        int r = p[0] ? ls_mkdir(p) : LS_OK;
        p[i] = c;
        if (r != LS_OK && r != LS_EEXIST) { return r; }
    }
    return LS_OK;
}

typedef struct { const char *want; bool found, is_dir; uint64_t size; } stat_ctx;
static bool stat_cb(const char *name, bool is_dir, uint64_t size, void *ctx) {
    stat_ctx *c = ctx;
    if (strcmp(name, c->want) != 0) { return true; }
    c->found = true; c->is_dir = is_dir; c->size = size;
    return false;
}

/* f_stat is not wrapped by the store, and the root has no entry of its own:
 * list the parent and find the name. */
static int do_stat(const char *path, bool *is_dir, uint64_t *size) {
    if (!path[0]) { *is_dir = true; *size = 0; return LS_OK; }
    static char parent[SDJ_PATH_MAX + 1];
    snprintf(parent, sizeof parent, "%s", path);
    char *slash = strrchr(parent, '/');
    const char *name = path;
    if (slash) { *slash = 0; name = path + (slash - parent) + 1; }
    else { parent[0] = 0; }
    stat_ctx c = { .want = name };
    int r = ls_list(parent, stat_cb, &c);
    if (r < 0) { return r; }
    if (!c.found) { return LS_ENOENT; }
    *is_dir = c.is_dir;
    *size = c.size;
    return LS_OK;
}

typedef struct { uint64_t skip; uint32_t off; int count; bool more; } list_ctx;
static bool list_cb(const char *name, bool is_dir, uint64_t size, void *ctx) {
    list_ctx *c = ctx;
    if (c->skip) { c->skip--; return true; }
    size_t nl = strlen(name);
    if (nl > 255) { nl = 255; }
    if (c->off + SDJ_LIST_HDR + nl > SDJ_BUF) { c->more = true; return false; }
    uint8_t *p = s_buf + c->off;
    uint32_t sz = size > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)size;
    p[0] = is_dir ? 1 : 0;
    p[1] = (uint8_t)nl;
    p[2] = (uint8_t)sz; p[3] = (uint8_t)(sz >> 8); p[4] = (uint8_t)(sz >> 16); p[5] = (uint8_t)(sz >> 24);
    memcpy(p + SDJ_LIST_HDR, name, nl);
    c->off += SDJ_LIST_HDR + (uint32_t)nl;
    c->count++;
    return true;
}

static void file_result(sdj_t *j, ls_file *f) {
    j->tell = ls_tell(f);
    j->size = ls_size(f);
}

static void run_job(sdj_t *j) {
    if (ls_media() != LS_MEDIA_OK) { j->rc = LS_ENOMEDIA; return; }
    switch (j->op) {
    case SDJ_OPEN: {
        int slot = -1;
        for (int i = 0; i < SDJ_FILES; i++) { if (!s_files[i].f) { slot = i; break; } }
        if (slot < 0) { j->rc = LS_ENOMEM; return; }
        if (j->mode != LS_RD) {
            static char parent[SDJ_PATH_MAX + 1];
            snprintf(parent, sizeof parent, "%s", j->path);
            char *slash = strrchr(parent, '/');
            if (slash) { *slash = 0; mkdir_p(parent); }
        }
        ls_file *f = ls_open(j->path, j->mode);
        if (!f) {
            /* The store does not say why; tell "not there" from the rest. */
            bool d = false; uint64_t sz = 0;
            int r = do_stat(j->path, &d, &sz);
            j->rc = (r == LS_ENOENT && j->mode == LS_RD) ? LS_ENOENT : (r == LS_OK && d) ? LS_EINVAL : LS_EIO;
            return;
        }
        s_files[slot].f = f;
        s_files[slot].gen = s_gen;
        s_files[slot].close_owed = false;
        file_result(j, f);
        j->rc = slot;
        return;
    }
    case SDJ_READ: case SDJ_WRITE: case SDJ_SEEK: case SDJ_CLOSE: {
        ls_file *f = file_at(j->file);
        if (!f) { j->rc = LS_EIO; if (j->op == SDJ_CLOSE) { j->rc = LS_OK; } return; }
        if (j->op == SDJ_CLOSE) { j->rc = ls_close(f); s_files[j->file].f = NULL; return; }
        uint32_t n = j->len > SDJ_BUF ? SDJ_BUF : j->len;
        if (j->op == SDJ_READ)  { j->rc = ls_read(f, s_buf, n); }
        if (j->op == SDJ_WRITE) { j->rc = ls_write(f, s_buf, n); }
        if (j->op == SDJ_SEEK)  { j->rc = ls_seek(f, j->pos); }
        file_result(j, f);
        return;
    }
    case SDJ_STAT:
        j->rc = do_stat(j->path, &j->is_dir, &j->size);
        return;
    case SDJ_LIST: {
        list_ctx c = { .skip = j->pos };
        int r = ls_list(j->path, list_cb, &c);
        j->rc = r < 0 ? r : c.count;
        j->more = c.more;
        return;
    }
    case SDJ_MKDIR:
        j->rc = mkdir_p(j->path);
        return;
    case SDJ_REMOVE: {
        j->rc = ls_remove(j->path);
        bool d = false; uint64_t sz = 0;
        if (j->rc == LS_ENOSPC && do_stat(j->path, &d, &sz) == LS_OK && d) { j->rc = SDJ_ENOTEMPTY; }
        return;
    }
    case SDJ_RENAME: {
        bool d = false; uint64_t sz = 0;
        if (do_stat(j->path2, &d, &sz) == LS_OK) { j->rc = LS_EEXIST; return; }
        static char parent[SDJ_PATH_MAX + 1];
        snprintf(parent, sizeof parent, "%s", j->path2);
        char *slash = strrchr(parent, '/');
        if (slash) { *slash = 0; mkdir_p(parent); }
        j->rc = ls_rename(j->path, j->path2);
        return;
    }
    case SDJ_STATFS:
        j->rc = ls_statfs(&j->free_bytes, &j->total_bytes);
        return;
    default:
        j->rc = LS_EINVAL;
    }
}

bool sdj_run(void) {
    check_generation();
    bool worked = false;
    for (int i = 0; i < SDJ_FILES; i++) {
        if (s_files[i].close_owed) { sdw_set_phase("lua close"); close_file(i); worked = true; }
    }
    if (s_state != SDJ_POSTED) { return worked; }
    const char *was = sdw_set_phase("lua file");
    run_job(&s_job);
    sdw_set_phase(was);
    sdw_lock();
    bool orphan_file = s_abandoned && s_job.op == SDJ_OPEN && s_job.rc >= 0;
    int orphan = s_job.rc;
    if (s_abandoned) {
        s_state = SDJ_IDLE;
        s_token = 0;
        give_back();
    } else {
        s_state = SDJ_DONE;
    }
    sdw_unlock();
    if (orphan_file) { close_file(orphan); }     /* card I/O: outside the lock */
    return true;
}
