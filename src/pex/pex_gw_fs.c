/*
 * The fs group on the gateway: lfs:/ (LittleFS) and sd:/ (the card) with the
 * Lua file module's semantics (src/lua/lfile.c).
 *
 * Completion rule (spec 7.1, fs): a call returns its result when it is done
 * at once -- LittleFS always is. The card is asynchronous (one storage-worker
 * job slot, src/sd/sd_jobs.h): with a callback the call returns PEX_EPENDING
 * and the callback gets the result from the main loop; without one it
 * returns PEX_EAGAIN. Requests are queued and run one at a time.
 */
#include <stdio.h>
#include <string.h>
#include "autoconf.h"
#include "pex/pex_gw.h"
#include "fs/fs.h"
#include "lua/lfile_path.h"
#if CONFIG_SD
#include "sd/sd_api.h"
#include "sd/sd_core.h"
#include "sd/sd_jobs.h"
#include "sd/sd_store.h"
#endif

#define FILES 4
typedef struct {
    pex_mod_t *mod;                 /* NULL = free */
    int vol;                        /* LFILE_LFS / LFILE_SD */
    int h;                          /* fs_f* handle, or the worker's file index */
    void *mem;                      /* fs_file_mem() bytes (LittleFS) */
    uint32_t size;
} pfile_t;
static pfile_t s_files[FILES];

static int lfs_err(int rc) {
    if (rc >= 0) { return rc; }
    if (rc == -2) { return PEX_ENOENT; }                  /* LFS_ERR_NOENT */
    if (rc == -17) { return PEX_EEXIST; }                 /* LFS_ERR_EXIST */
    if (rc == FS_ERR_BUSY) { return PEX_EBUSY; }
    if (rc == FS_ERR_NOHANDLE) { return PEX_ENOMEM; }
    if (rc == -28) { return PEX_ENOMEM; }                 /* LFS_ERR_NOSPC */
    if (rc == FS_ERR_NOTEMPTY) { return PEX_EBUSY; }
    return PEX_EIO;
}

static int parse(const char *in, char *out) {
    if (!in) { return 0; }
    return lfile_parse_path(in, strlen(in), out, LFILE_PATH_MAX + 1);
}

static pfile_t *file_of(int fd) {
    if (fd < 0 || fd >= FILES || !s_files[fd].mod) { return NULL; }
    if (s_files[fd].mod != pex_self()) { return NULL; }
    return &s_files[fd];
}

/* ---- the card: a queue of requests run through the job slot ---------------- */
#if CONFIG_SD
enum { Q_OPEN = 1, Q_READ, Q_WRITE, Q_SEEK, Q_STAT, Q_LIST, Q_MKDIR, Q_REMOVE, Q_RENAME };
typedef struct req {
    struct req *next;
    pex_mod_t *mod;
    int op, fd;
    pex_fs_cb cb;
    pex_fs_list_cb each;
    void *arg;
    void *buf;
    uint32_t n;
    uint64_t pos;
    pex_fs_entry_t *out;
    char path[LFILE_PATH_MAX + 1], path2[LFILE_PATH_MAX + 1];
    uint32_t token;
    bool posted;
    int mode;
} req_t;
static req_t *s_q;

static int sd_gate(void) {
    const char *st = sd_card_state();
    if (strcmp(st, "ok") == 0) { return PEX_OK; }
    if (strcmp(st, "none") == 0) { pex_seterr(PEX_EIO, "no SD card"); return PEX_EIO; }
    return PEX_EBUSY;
}

static int sdrc_err(int rc) {
    switch (rc) {
    case LS_ENOENT: return PEX_ENOENT;
    case LS_EEXIST: return PEX_EEXIST;
    case LS_ENOSPC: case LS_ENOMEM: return PEX_ENOMEM;
    case LS_EBUSY: return PEX_EBUSY;
    case LS_EINVAL: return PEX_EINVAL;
    case SDJ_ENOTEMPTY: return PEX_EBUSY;
    default: return rc >= 0 ? rc : PEX_EIO;
    }
}

static int enqueue(const req_t *proto) {
    if (!proto->cb && !proto->each) { return PEX_EAGAIN; }
    int g = sd_gate();
    if (g) { return g; }
    req_t *r = pex_alloc_long(sizeof *r);           /* the module's: freed with it */
    if (!r) { return PEX_ENOMEM; }
    *r = *proto;
    r->mod = pex_self();
    r->next = NULL;
    req_t **pp = &s_q;
    while (*pp) { pp = &(*pp)->next; }
    *pp = r;
    return PEX_EPENDING;
}

static void finish(req_t *r, int rc) {
    s_q = r->next;
    pex_mod_t *m = r->mod;
    pex_fs_cb cb = r->cb;
    void *arg = r->arg;
    pex_free(r);
    if (cb) { pex_call(m, (uint32_t)(uintptr_t)cb, (uint32_t)(uintptr_t)arg, (uint32_t)rc, 0, 0); }
}

static void sd_step(void) {
    req_t *r = s_q;
    if (!r) { return; }
    sdj_t j;
    if (!r->posted) {
        memset(&j, 0, sizeof j);
        switch (r->op) {
        case Q_OPEN: j.op = SDJ_OPEN; j.mode = r->mode; break;
        case Q_READ: j.op = SDJ_READ; j.file = s_files[r->fd].h; j.len = r->n > SDJ_BUF ? SDJ_BUF : r->n; break;
        case Q_WRITE: j.op = SDJ_WRITE; j.file = s_files[r->fd].h; j.len = r->n > SDJ_BUF ? SDJ_BUF : r->n; break;
        case Q_SEEK: j.op = SDJ_SEEK; j.file = s_files[r->fd].h; j.pos = r->pos; break;
        case Q_STAT: j.op = SDJ_STAT; break;
        case Q_LIST: j.op = SDJ_LIST; j.pos = r->pos; break;
        case Q_MKDIR: j.op = SDJ_MKDIR; break;
        case Q_REMOVE: j.op = SDJ_REMOVE; break;
        case Q_RENAME: j.op = SDJ_RENAME; snprintf(j.path2, sizeof j.path2, "%s", r->path2); break;
        }
        snprintf(j.path, sizeof j.path, "%s", r->path);
        if (!sdj_post(&j, &r->token)) { return; }      /* slot busy: next pass */
        if (r->op == Q_WRITE) { memcpy(sdj_buf(), r->buf, j.len); }
        r->posted = true;
        return;
    }
    enum sdj_state st = sdj_poll(r->token, &j);
    if (st == SDJ_POSTED || st == SDJ_FILLING) { return; }
    if (st != SDJ_DONE) { finish(r, PEX_EIO); return; }
    int rc = j.rc;
    if (rc < 0) {
        sdj_finish(r->token);
        if (r->op == Q_OPEN) { memset(&s_files[r->fd], 0, sizeof s_files[r->fd]); }
        if (r->op == Q_LIST && r->each) {
            pex_call(r->mod, (uint32_t)(uintptr_t)r->each, (uint32_t)(uintptr_t)r->arg, 0, 0, 0);
        }
        finish(r, sdrc_err(rc));
        return;
    }
    switch (r->op) {
    case Q_OPEN:
        s_files[r->fd].h = rc;
        s_files[r->fd].size = (uint32_t)j.size;
        rc = r->fd;
        break;
    case Q_READ:
        memcpy(r->buf, sdj_buf(), (size_t)rc);
        break;
    case Q_WRITE:
        if (j.size > s_files[r->fd].size) { s_files[r->fd].size = (uint32_t)j.size; }
        break;
    case Q_STAT:
        if (r->out) {
            const char *b = strrchr(r->path, '/');
            snprintf(r->out->name, sizeof r->out->name, "%s", b ? b + 1 : r->path);
            r->out->is_dir = j.is_dir;
            r->out->size = (uint32_t)j.size;
        }
        rc = 0;
        break;
    case Q_LIST: {
        const uint8_t *p = sdj_buf();
        pex_fs_entry_t e;
        for (int i = 0; i < rc; i++) {
            uint8_t nl = p[1];
            memset(&e, 0, sizeof e);
            e.is_dir = p[0];
            e.size = (uint32_t)p[2] | (uint32_t)p[3] << 8 | (uint32_t)p[4] << 16 | (uint32_t)p[5] << 24;
            memcpy(e.name, p + SDJ_LIST_HDR, nl < sizeof e.name - 1 ? nl : sizeof e.name - 1);
            p += SDJ_LIST_HDR + nl;
            pex_call(r->mod, (uint32_t)(uintptr_t)r->each, (uint32_t)(uintptr_t)r->arg, (uint32_t)(uintptr_t)&e, 0, 0);
        }
        r->pos += (uint64_t)rc;
        bool more = j.more;
        sdj_finish(r->token);
        if (more) { r->posted = false; return; }       /* next page */
        pex_call(r->mod, (uint32_t)(uintptr_t)r->each, (uint32_t)(uintptr_t)r->arg, 0, 0, 0);
        finish(r, (int)r->pos);
        return;
    }
    default:
        rc = r->op == Q_SEEK ? (int)j.tell : 0;
        break;
    }
    sdj_finish(r->token);
    finish(r, rc);
}
#endif

void pex_gw_fs_poll(void) {
#if CONFIG_SD
    sd_step();
#endif
}

void pex_gw_fs_unload(pex_mod_t *m) {
#if CONFIG_SD
    for (req_t **pp = &s_q; *pp;) {
        req_t *r = *pp;
        if (r->mod == m) {
            if (r->posted) { sdj_abandon(r->token); }
            *pp = r->next;          /* the block itself goes with the module */
        } else {
            pp = &r->next;
        }
    }
#endif
    for (int i = 0; i < FILES; i++) {
        pfile_t *f = &s_files[i];
        if (f->mod != m) { continue; }
        if (f->vol == LFILE_LFS) { fs_fclose(f->h); }
#if CONFIG_SD
        else if (f->h >= 0) { sdj_close_async(f->h); }
#endif
        memset(f, 0, sizeof *f);
    }
}

/* ---- the group -------------------------------------------------------------- */
int pex_fs_open(const char *path, int mode, pex_fs_cb cb, void *arg) {
    char rel[LFILE_PATH_MAX + 1];
    int vol = parse(path, rel);
    if (!vol || mode < PEX_FS_READ || mode > PEX_FS_APPEND) { return PEX_EINVAL; }
    int fd = -1;
    for (int i = 0; i < FILES; i++) {
        if (!s_files[i].mod) { fd = i; break; }
    }
    if (fd < 0) { return PEX_ENOMEM; }
    pfile_t *f = &s_files[fd];
    if (vol == LFILE_LFS) {
        size_t sz;
        if (mode == PEX_FS_READ && !fs_stat(rel, &sz)) { return PEX_ENOENT; }
        void *mem = pex_alloc_long(fs_file_mem());
        if (!mem) { return PEX_ENOMEM; }
        int h = fs_fopen(rel, mode == PEX_FS_READ ? FS_O_READ : mode == PEX_FS_WRITE ? FS_O_WRITE : FS_O_APPEND, mem);
        if (h < 0) { pex_free(mem); return lfs_err(h); }
        f->mod = pex_self();
        f->vol = vol;
        f->h = h;
        f->mem = mem;
        f->size = (uint32_t)fs_fsize(h);
        return fd;
    }
#if CONFIG_SD
    req_t q = { .op = Q_OPEN, .fd = fd, .cb = cb, .arg = arg,
                .mode = mode == PEX_FS_READ ? LS_RD : mode == PEX_FS_WRITE ? LS_WR : LS_APPEND };
    snprintf(q.path, sizeof q.path, "%s", rel);
    int rc = enqueue(&q);
    if (rc == PEX_EPENDING) {          /* reserve the slot until the worker answers */
        f->mod = pex_self();
        f->vol = vol;
        f->h = -1;
    }
    return rc;
#else
    (void)cb; (void)arg;
    return PEX_ENOTSUP;
#endif
}

int pex_fs_read(int fd, void *buf, size_t n, pex_fs_cb cb, void *arg) {
    pfile_t *f = file_of(fd);
    if (!f || !buf) { return PEX_EINVAL; }
    if (f->vol == LFILE_LFS) {
        if (n > 4096) { n = 4096; }
        return lfs_err(fs_fread(f->h, buf, n));
    }
#if CONFIG_SD
    req_t q = { .op = Q_READ, .fd = fd, .cb = cb, .arg = arg, .buf = buf, .n = (uint32_t)n };
    return enqueue(&q);
#else
    (void)cb; (void)arg;
    return PEX_ENOTSUP;
#endif
}

int pex_fs_write(int fd, const void *buf, size_t n, pex_fs_cb cb, void *arg) {
    pfile_t *f = file_of(fd);
    if (!f || !buf) { return PEX_EINVAL; }
    if (f->vol == LFILE_LFS) {
        if (n > 4096) { n = 4096; }        /* an erase can take 400 ms: keep the loop moving */
        int rc = fs_fwrite(f->h, buf, n);
        if (rc == FS_ERR_TOOLARGE) { return PEX_ENOMEM; }
        if (rc >= 0) { f->size = (uint32_t)fs_fsize(f->h); }
        return lfs_err(rc);
    }
#if CONFIG_SD
    if (n > SDJ_BUF) { n = SDJ_BUF; }
    /* the data is copied now: the caller's buffer is free when this returns */
    req_t q = { .op = Q_WRITE, .fd = fd, .cb = cb, .arg = arg, .n = (uint32_t)n };
    if (!cb) { return PEX_EAGAIN; }
    void *copy = pex_alloc_long(n);
    if (!copy) { return PEX_ENOMEM; }
    memcpy(copy, buf, n);
    q.buf = copy;
    int rc = enqueue(&q);
    if (rc != PEX_EPENDING) { pex_free(copy); }
    return rc;
#else
    (void)cb; (void)arg;
    return PEX_ENOTSUP;
#endif
}

int pex_fs_seek(int fd, uint32_t pos, pex_fs_cb cb, void *arg) {
    pfile_t *f = file_of(fd);
    if (!f) { return PEX_EINVAL; }
    if (f->vol == LFILE_LFS) { return lfs_err(fs_fseek(f->h, pos)) < 0 ? PEX_EIO : (int)pos; }
#if CONFIG_SD
    req_t q = { .op = Q_SEEK, .fd = fd, .cb = cb, .arg = arg, .pos = pos };
    return enqueue(&q);
#else
    (void)cb; (void)arg;
    return PEX_ENOTSUP;
#endif
}

int32_t pex_fs_size(int fd) {
    pfile_t *f = file_of(fd);
    return f ? (int32_t)f->size : PEX_EINVAL;
}

int pex_fs_close(int fd) {
    pfile_t *f = file_of(fd);
    if (!f) { return PEX_EINVAL; }
    int rc = 0;
    if (f->vol == LFILE_LFS) {
        rc = lfs_err(fs_fclose(f->h));
        pex_free(f->mem);
    }
#if CONFIG_SD
    else if (f->h >= 0) { sdj_close_async(f->h); }
#endif
    memset(f, 0, sizeof *f);
    return rc < 0 ? rc : PEX_OK;
}

int pex_fs_stat(const char *path, pex_fs_entry_t *out, pex_fs_cb cb, void *arg) {
    char rel[LFILE_PATH_MAX + 1];
    int vol = parse(path, rel);
    if (!vol) { return PEX_EINVAL; }
    if (vol == LFILE_LFS) {
        size_t sz = 0;
        bool dir = rel[0] == 0 || fs_is_dir(rel);
        if (!dir && !fs_stat(rel, &sz)) { return PEX_ENOENT; }
        if (out) {
            const char *b = strrchr(rel, '/');
            snprintf(out->name, sizeof out->name, "%s", b ? b + 1 : rel);
            out->is_dir = dir;
            out->size = (uint32_t)sz;
        }
        return PEX_OK;
    }
#if CONFIG_SD
    req_t q = { .op = Q_STAT, .cb = cb, .arg = arg, .out = out };
    snprintf(q.path, sizeof q.path, "%s", rel);
    return enqueue(&q);
#else
    (void)cb; (void)arg;
    return PEX_ENOTSUP;
#endif
}

typedef struct { pex_mod_t *m; pex_fs_list_cb each; void *arg; int n; } walk_t;
static void walk_cb(const char *name, bool is_dir, size_t size, void *ctx) {
    walk_t *w = ctx;
    pex_fs_entry_t e;
    memset(&e, 0, sizeof e);
    snprintf(e.name, sizeof e.name, "%s", name);
    e.is_dir = is_dir;
    e.size = (uint32_t)size;
    w->n++;
    pex_call(w->m, (uint32_t)(uintptr_t)w->each, (uint32_t)(uintptr_t)w->arg, (uint32_t)(uintptr_t)&e, 0, 0);
}

int pex_fs_list(const char *dir, pex_fs_list_cb each, void *arg) {
    char rel[LFILE_PATH_MAX + 1];
    int vol = parse(dir, rel);
    if (!vol || !each) { return PEX_EINVAL; }
    pex_mod_t *m = pex_self();
    if (vol == LFILE_LFS) {
        walk_t w = { m, each, arg, 0 };
        if (fs_walk(rel, walk_cb, &w) < 0) { return PEX_ENOENT; }
        pex_call(m, (uint32_t)(uintptr_t)each, (uint32_t)(uintptr_t)arg, 0, 0, 0);
        return w.n;
    }
#if CONFIG_SD
    req_t q = { .op = Q_LIST, .each = each, .arg = arg };
    snprintf(q.path, sizeof q.path, "%s", rel);
    return enqueue(&q);
#else
    return PEX_ENOTSUP;
#endif
}

int pex_fs_mkdir(const char *path, pex_fs_cb cb, void *arg) {
    char rel[LFILE_PATH_MAX + 1];
    int vol = parse(path, rel);
    if (!vol || !rel[0]) { return PEX_EINVAL; }
    if (vol == LFILE_LFS) {
        size_t sz;
        if (fs_stat(rel, &sz)) { return PEX_EEXIST; }
        return lfs_err(fs_mkdir(rel)) < 0 ? PEX_EIO : PEX_OK;
    }
#if CONFIG_SD
    req_t q = { .op = Q_MKDIR, .cb = cb, .arg = arg };
    snprintf(q.path, sizeof q.path, "%s", rel);
    return enqueue(&q);
#else
    (void)cb; (void)arg;
    return PEX_ENOTSUP;
#endif
}

int pex_fs_remove(const char *path, pex_fs_cb cb, void *arg) {
    char rel[LFILE_PATH_MAX + 1];
    int vol = parse(path, rel);
    if (!vol || !rel[0]) { return PEX_EINVAL; }
    if (vol == LFILE_LFS) {
        int rc = lfs_err(fs_delete(rel));
        return rc < 0 ? rc : PEX_OK;
    }
#if CONFIG_SD
    req_t q = { .op = Q_REMOVE, .cb = cb, .arg = arg };
    snprintf(q.path, sizeof q.path, "%s", rel);
    return enqueue(&q);
#else
    (void)cb; (void)arg;
    return PEX_ENOTSUP;
#endif
}

int pex_fs_rename(const char *from, const char *to, pex_fs_cb cb, void *arg) {
    char a[LFILE_PATH_MAX + 1], b[LFILE_PATH_MAX + 1];
    int va = parse(from, a), vb = parse(to, b);
    if (!va || va != vb || !a[0] || !b[0]) { return PEX_EINVAL; }   /* not across volumes */
    if (va == LFILE_LFS) {
        int rc = fs_rename(a, b);
        if (rc == 1) { return PEX_EEXIST; }
        return lfs_err(rc) < 0 ? lfs_err(rc) : PEX_OK;
    }
#if CONFIG_SD
    req_t q = { .op = Q_RENAME, .cb = cb, .arg = arg };
    snprintf(q.path, sizeof q.path, "%s", a);
    snprintf(q.path2, sizeof q.path2, "%s", b);
    return enqueue(&q);
#else
    (void)cb; (void)arg;
    return PEX_ENOTSUP;
#endif
}

int pex_fs_media(pex_fs_media_t *o) {
    if (!o) { return PEX_EINVAL; }
    memset(o, 0, sizeof *o);
    uint32_t used = 0, total = 0;
    o->lfs_ok = fs_usage(&used, &total);
    o->lfs_total = total;
    o->lfs_free = total - used;
#if CONFIG_SD
    snprintf(o->sd_state, sizeof o->sd_state, "%s", sd_card_state());
    const struct sd_pub *p = sd_pub();
    if (p) { o->sd_free = p->free_bytes; o->sd_total = p->total_bytes; }
#else
    snprintf(o->sd_state, sizeof o->sd_state, "none");
#endif
    return PEX_OK;
}
