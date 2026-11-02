/* fs.stat: up to 32 editor paths, for a browser that reconnected.
 * LittleFS is answered in this call. An SD path is read by the storage
 * worker and comes back as fs.stated. fs.changed is the live notice.
 *
 * fs.read / fs.write move one chunk (WS_FILE_RAW bytes, base64) over the
 * same websocket. HTTP /api/fs and /api/sd stay. A chunk is sent only when
 * ws_link_allow_bulk says the send buffer still has room for telemetry;
 * otherwise the answer is `busy` and the client retries the same offset.
 * An sd/ path is one sd_jobs step per call. The worker is the only thing
 * that touches the card, so this returns busy until that step finishes. */
#include "fs/fs_watch.h"

#include <stdio.h>
#include <string.h>

#include <stdbool.h>

#include "api/api.h"
#include "api/ops.h"
#include "autoconf.h"
#include "fs/fs.h"
#include "net/http.h"
#include "net/ws_link.h"

#if CONFIG_SD
#include "sd/sd_jobs.h"
#include "sd/sd_store.h"
#endif

static int is_sd_path(const char *path) {
    return path && (strcmp(path, "sd") == 0 || strncmp(path, "sd/", 3) == 0);
}

static const char *sd_rel(const char *path) {
    if (strcmp(path, "sd") == 0) {
        return "";
    }
    return path + 3;
}

/* 1: a final answer was sent (ok or a hard error). 0: `busy`, and an SD
 * result that is already READY must stay READY for the same retry. */
static int reply_chunk(api_reply_fn reply, void *ctx, long id, const char *path,
                       uint32_t offset, uint32_t size, unsigned n, int eof,
                       const uint8_t *raw) {
    char *b = api_scratch();
    /* Static: core0's stack is 2 KB, and this runs from the lwIP recv path. */
    static char esc[FS_PATH_MAX * 2 + 4];
    static char b64[((WS_FILE_RAW + 2) / 3) * 4 + 1];
    unsigned frame;
    if (!ws_b64_encode(raw, n, b64, sizeof b64)) {
        api_reply_err(reply, ctx, id, "io", "encode");
        return 1;
    }
    api_json_escape(esc, sizeof esc, path);
    snprintf(b, api_scratch_size(),
             "{\"path\":\"%s\",\"offset\":%lu,\"size\":%lu,\"n\":%u,\"eof\":%s,\"data\":\"%s\"}",
             esc, (unsigned long)offset, (unsigned long)size, n, eof ? "true" : "false", b64);
    frame = (unsigned)strlen(b) + 48u;
    if (frame > 65535u) {
        frame = 65535u;
    }
    if (!http_ws_bulk_ok(ctx, (uint16_t)frame)) {
        api_reply_err(reply, ctx, id, "busy", "telemetry has the link");
        return 0;
    }
    api_reply_ok(reply, ctx, id, b);
    return 1;
}

static int reply_wrote(api_reply_fn reply, void *ctx, long id, const char *path,
                       uint32_t offset, uint32_t size, unsigned n) {
    char *b = api_scratch();
    static char esc[FS_PATH_MAX * 2 + 4];
    unsigned frame;
    api_json_escape(esc, sizeof esc, path);
    snprintf(b, api_scratch_size(),
             "{\"path\":\"%s\",\"offset\":%lu,\"size\":%lu,\"n\":%u}",
             esc, (unsigned long)offset, (unsigned long)size, n);
    frame = (unsigned)strlen(b) + 48u;
    if (!http_ws_bulk_ok(ctx, (uint16_t)frame)) {
        api_reply_err(reply, ctx, id, "busy", "telemetry has the link");
        return 0;
    }
    api_reply_ok(reply, ctx, id, b);
    return 1;
}

static int clamp_read(long *offset, long *len) {
    if (*offset < 0) {
        return -1;
    }
    if (*len <= 0 || *len > (long)WS_FILE_RAW) {
        *len = (long)WS_FILE_RAW;
    }
    return 0;
}

static void op_fs_read_lfs(const char *path, long offset, long len,
                           api_reply_fn reply, void *ctx, long id) {
    size_t sz = 0;
    size_t n;
    int h;
    int got;
    static uint8_t raw[WS_FILE_RAW];
    if (!fs_valid_path(path)) {
        api_reply_err(reply, ctx, id, "bad_request", "bad path");
        return;
    }
    if (!fs_stat(path, &sz)) {
        api_reply_err(reply, ctx, id, fs_is_dir(path) ? "bad_request" : "not_found",
                      fs_is_dir(path) ? "not a file" : "not found");
        return;
    }
    if ((uint32_t)offset >= sz) {
        reply_chunk(reply, ctx, id, path, (uint32_t)offset, (uint32_t)sz, 0, 1, raw);
        return;
    }
    n = (size_t)len;
    if ((size_t)offset + n > sz) {
        n = sz - (size_t)offset;
    }
    h = fs_begin_read(path, &sz);
    if (h == FS_ERR_BUSY) {
        api_reply_err(reply, ctx, id, "busy", "file is open");
        return;
    }
    if (h < 0) {
        api_reply_err(reply, ctx, id, h == -2 ? "not_found" : "io", h == -2 ? "not found" : "read failed");
        return;
    }
    if (offset && fs_read_seek(h, (size_t)offset) < 0) {
        fs_end_read(h);
        api_reply_err(reply, ctx, id, "io", "seek failed");
        return;
    }
    got = fs_read(h, raw, n);
    fs_end_read(h);
    if (got < 0) {
        api_reply_err(reply, ctx, id, "io", "read failed");
        return;
    }
    reply_chunk(reply, ctx, id, path, (uint32_t)offset, (uint32_t)sz, (unsigned)got,
                (size_t)offset + (size_t)got >= sz, raw);
}

static void op_fs_write_lfs(const char *path, long offset, const uint8_t *raw, unsigned n, int trunc,
                            api_reply_fn reply, void *ctx, long id) {
    static uint8_t mem[1024];
    int mode;
    int h;
    int wr;
    int32_t sz;
    if (fs_file_mem() > sizeof mem) {
        api_reply_err(reply, ctx, id, "io", "file handle");
        return;
    }
    if (!fs_valid_path(path) || fs_is_dir(path)) {
        api_reply_err(reply, ctx, id, "bad_request", "bad path");
        return;
    }
    if ((uint32_t)offset > FS_FILE_MAX || n > FS_FILE_MAX || (uint32_t)offset + n > FS_FILE_MAX) {
        api_reply_err(reply, ctx, id, "too_large", "file limit is 1 MiB");
        return;
    }
    mode = (trunc || offset == 0) ? FS_O_WRITE : FS_O_UPDATE;
    h = fs_fopen(path, mode, mem);
    if (h == FS_ERR_BUSY) {
        api_reply_err(reply, ctx, id, "busy", "file is open");
        return;
    }
    if (h < 0) {
        api_reply_err(reply, ctx, id, h == -2 ? "not_found" : "io", h == -2 ? "not found" : "open failed");
        return;
    }
    if (offset && mode != FS_O_WRITE && fs_fseek(h, (uint32_t)offset) < 0) {
        fs_fclose(h);
        api_reply_err(reply, ctx, id, "io", "seek failed");
        return;
    }
    wr = n ? fs_fwrite(h, raw, n) : 0;
    sz = fs_fsize(h);
    fs_fclose(h);
    if (wr < 0) {
        api_reply_err(reply, ctx, id, wr == FS_ERR_TOOLARGE ? "too_large" : "io",
                      wr == FS_ERR_TOOLARGE ? "file limit is 1 MiB" : "write failed");
        return;
    }
    reply_wrote(reply, ctx, id, path, (uint32_t)offset, sz < 0 ? 0 : (uint32_t)sz, (unsigned)wr);
}

#if CONFIG_SD

enum { SW_IDLE = 0, SW_POST = 1, SW_READY = 2 };

static struct {
    int phase;
    int step;
    int file;
    int writing;
    int err;
    int n;
    int inited;
    uint32_t offset;
    uint32_t want;
    uint32_t token;
    uint64_t fsize;
    char path[SDJ_PATH_MAX + 1];
    char held[SDJ_PATH_MAX + 1];
    char full[FS_PATH_MAX + 1];
    uint8_t raw[WS_FILE_RAW];
} sw;

static int sd_match(const char *rel, uint32_t offset, uint32_t want, int writing) {
    return strcmp(sw.path, rel) == 0 && sw.offset == offset && sw.want == want && sw.writing == writing;
}

static int sd_post(void) {
    sdj_t req;
    memset(&req, 0, sizeof req);
    req.op = sw.step;
    if (sw.step == SDJ_MKDIR) {
        snprintf(req.path, sizeof req.path, "%s", sw.path);
        char *slash = strrchr(req.path, '/');
        if (!slash) {
            sw.step = SDJ_OPEN;
            req.op = SDJ_OPEN;
        } else {
            *slash = 0;
        }
    }
    if (sw.step == SDJ_OPEN) {
        req.op = SDJ_OPEN;
        snprintf(req.path, sizeof req.path, "%s", sw.path);
        req.mode = !sw.writing ? LS_RD : (sw.offset == 0 ? LS_WR : LS_RW);
    } else if (sw.step == SDJ_SEEK || sw.step == SDJ_READ || sw.step == SDJ_WRITE || sw.step == SDJ_CLOSE) {
        req.file = sw.file;
        req.pos = sw.offset;
        req.len = sw.want;
    }
    if (!sdj_post(&req, &sw.token)) {
        return 0;
    }
    if (sw.step == SDJ_WRITE && sw.want) {
        memcpy(sdj_buf(), sw.raw, sw.want);
    }
    return 1;
}

static void sd_on_done(const sdj_t *out) {
    int rc = out->rc;
    int step = sw.step;
    if (step == SDJ_READ && rc > 0) {
        sw.n = rc > (int)WS_FILE_RAW ? (int)WS_FILE_RAW : rc;
        memcpy(sw.raw, sdj_buf(), (size_t)sw.n);
        sw.fsize = out->size;
    } else if (step == SDJ_READ && rc == 0) {
        sw.n = 0;
        sw.fsize = out->size;
    } else if (step == SDJ_WRITE && rc >= 0) {
        sw.n = rc;
        sw.fsize = out->size;
    }
    sdj_finish(sw.token);
    if (rc < 0 && step != SDJ_CLOSE) {
        sw.err = rc;
        sw.phase = SW_READY;
        sw.step = 0;
        return;
    }
    if (step == SDJ_CLOSE) {
        sw.file = -1;
        sw.held[0] = 0;
        sw.step = 0;
        sw.phase = SW_IDLE;
        return;
    }
    if (step == SDJ_MKDIR) {
        sw.step = SDJ_OPEN;
        sw.phase = SW_IDLE;
        return;
    }
    if (step == SDJ_OPEN) {
        sw.file = rc;
        sw.fsize = out->size;
        snprintf(sw.held, sizeof sw.held, "%s", sw.path);
        if (sw.writing && sw.want == 0 && sw.offset == 0) {
            sw.n = 0;
            sw.phase = SW_READY;
            sw.step = 0;
            return;
        }
        sw.step = SDJ_SEEK;
        sw.phase = SW_IDLE;
        return;
    }
    if (step == SDJ_SEEK) {
        sw.step = sw.writing ? SDJ_WRITE : SDJ_READ;
        sw.phase = SW_IDLE;
        return;
    }
    sw.step = 0;
    sw.phase = SW_READY;
}

static void sd_begin(const char *rel, const char *full, uint32_t offset, uint32_t want,
                     int writing, const uint8_t *raw_in) {
    snprintf(sw.path, sizeof sw.path, "%s", rel);
    snprintf(sw.full, sizeof sw.full, "%s", full);
    sw.offset = offset;
    sw.want = want;
    sw.writing = writing;
    sw.err = 0;
    sw.n = 0;
    if (writing && raw_in && want) {
        memcpy(sw.raw, raw_in, want);
    }
    if (sw.file >= 0 && (strcmp(sw.held, rel) != 0 || (writing && offset == 0))) {
        sw.step = SDJ_CLOSE;
    } else if (sw.file < 0) {
        sw.step = (writing && strchr(rel, '/')) ? SDJ_MKDIR : SDJ_OPEN;
    } else {
        sw.step = SDJ_SEEK;
    }
    sw.phase = SW_IDLE;
}

/* 1: caller answers busy. 0: sw is READY for this request. */
static int sd_service(const char *rel, const char *full, uint32_t offset, uint32_t want,
                      int writing, const uint8_t *raw_in) {
    if (!sw.inited) {
        sw.file = -1;
        sw.inited = 1;
    }
    if (sw.phase == SW_POST) {
        sdj_t out;
        enum sdj_state st = sdj_poll(sw.token, &out);
        if (st == SDJ_POSTED || st == SDJ_FILLING) {
            return 1;
        }
        if (st != SDJ_DONE) {
            sw.err = LS_EIO;
            sw.phase = SW_READY;
            sw.step = 0;
        } else {
            sd_on_done(&out);
        }
    }
    if (sw.phase == SW_READY) {
        if (sd_match(rel, offset, want, writing)) {
            return 0;
        }
        sw.phase = SW_IDLE;
        sw.step = 0;
        sw.err = 0;
    }
    if (sw.phase == SW_IDLE && sw.step != 0 && !sd_match(rel, offset, want, writing)) {
        /* The client moved on. Drop the queued next step and start this one.
         * A held file for another path is closed by sd_begin. */
        sw.step = 0;
        sw.err = 0;
    }
    if (sw.phase == SW_IDLE && sw.step == 0) {
        sd_begin(rel, full, offset, want, writing, raw_in);
    }
    if (sw.phase == SW_IDLE && sw.step != 0) {
        if (!sd_post()) {
            return 1;
        }
        sw.phase = SW_POST;
        return 1;
    }
    if (sw.phase == SW_READY && sd_match(rel, offset, want, writing)) {
        return 0;
    }
    return 1;
}

static void sd_fail(api_reply_fn reply, void *ctx, long id) {
    int err = sw.err;
    sw.phase = SW_IDLE;
    sw.step = 0;
    sw.err = 0;
    sw.n = 0;
    if (err == LS_ENOENT) {
        api_reply_err(reply, ctx, id, "not_found", "not found");
    } else if (err == LS_ENOMEDIA) {
        api_reply_err(reply, ctx, id, "nomedia", "no sd card");
    } else if (err == LS_EINVAL) {
        api_reply_err(reply, ctx, id, "bad_request", "not a file");
    } else if (err == LS_ENOSPC) {
        api_reply_err(reply, ctx, id, "no_space", "sd card is full");
    } else {
        api_reply_err(reply, ctx, id, "io", "sd io");
    }
}

static void op_fs_sd(const char *path, const char *rel, uint32_t offset, uint32_t want,
                     int writing, const uint8_t *raw_in, api_reply_fn reply, void *ctx, long id) {
    int eof;
    if (!rel[0] || strlen(rel) > SDJ_PATH_MAX || strstr(rel, "..")) {
        api_reply_err(reply, ctx, id, "bad_request", "bad path");
        return;
    }
    if (sd_service(rel, path, offset, want, writing, raw_in)) {
        api_reply_err(reply, ctx, id, "busy", "sd card is busy");
        return;
    }
    if (sw.err) {
        sd_fail(reply, ctx, id);
        return;
    }
    if (writing) {
        if (!reply_wrote(reply, ctx, id, path, sw.offset, (uint32_t)sw.fsize, (unsigned)sw.n)) {
            return;
        }
    } else {
        eof = sw.n < (int)sw.want || (uint64_t)sw.offset + (uint64_t)sw.n >= sw.fsize;
        if (!reply_chunk(reply, ctx, id, path, sw.offset, (uint32_t)sw.fsize,
                         (unsigned)sw.n, eof, sw.raw)) {
            return;
        }
    }
    sw.phase = SW_IDLE;
    sw.step = 0;
}

#endif /* CONFIG_SD */

static void op_fs_stat(const char *params, api_reply_fn reply, void *ctx, long id) {
    /* The path table lives in fs_watch. Copying 32 paths onto this stack
     * does not fit: core0's stack is 2 KB. */
    int rc = fs_watch_stat_begin(params);
    if (rc == -1) {
        api_reply_err(reply, ctx, id, "bad_request", "need \"paths\"");
        return;
    }
    if (rc == -3) {
        api_reply_err(reply, ctx, id, "bad_request", "at most 32 paths");
        return;
    }
    if (rc != 0) {
        api_reply_err(reply, ctx, id, "busy", "a stat is already running");
        return;
    }
    if (fs_watch_stat_state() == 1) {
        api_reply_ok(reply, ctx, id, "{\"pending\":true,\"event\":\"fs.stated\"}");
        return;
    }
    char *b = api_scratch();
    if (!fs_watch_stat_json(b, api_scratch_size())) {
        fs_watch_stat_drop();
        api_reply_err(reply, ctx, id, "too_large", "stat");
        return;
    }
    /* The poll emits fs.stated only for a card read. This reply already
     * carries the LittleFS JSON, so drop the ready flag and do not emit. */
    api_reply_ok(reply, ctx, id, b);
    fs_watch_stat_drop();
}

static int take_write(const char *params, uint8_t *raw, unsigned *n, long *offset, int *trunc) {
    static char data[((WS_FILE_RAW + 2) / 3) * 4 + 1];
    long off = 0;
    int dec;
    bool trunc_b = false;
    if (!api_json_int(params, "offset", &off) || off < 0) {
        off = 0;
    }
    *offset = off;
    if (api_json_bool(params, "truncate", &trunc_b)) {
        *trunc = trunc_b ? 1 : 0;
    } else {
        *trunc = off == 0;
    }
    if (!api_json_str(params, "data", data, sizeof data)) {
        if (params && strstr(params, "\"data\"")) {
            return -1;
        }
        data[0] = 0;
    }
    dec = ws_b64_decode(data, strlen(data), raw, WS_FILE_RAW);
    if (dec < 0) {
        return -1;
    }
    *n = (unsigned)dec;
    return 0;
}

static void op_fs_read(const char *params, api_reply_fn reply, void *ctx, long id) {
    char path[FS_PATH_MAX + 1];
    long offset = 0;
    long len = WS_FILE_RAW;
    if (!api_json_str(params, "path", path, sizeof path) || !path[0]) {
        api_reply_err(reply, ctx, id, "bad_request", "need \"path\"");
        return;
    }
    api_json_int(params, "offset", &offset);
    api_json_int(params, "len", &len);
    if (clamp_read(&offset, &len) < 0) {
        api_reply_err(reply, ctx, id, "bad_request", "bad offset");
        return;
    }
    if (is_sd_path(path)) {
#if CONFIG_SD
        op_fs_sd(path, sd_rel(path), (uint32_t)offset, (uint32_t)len, 0, NULL, reply, ctx, id);
#else
        api_reply_err(reply, ctx, id, "nomedia", "no sd card");
#endif
        return;
    }
    op_fs_read_lfs(path, offset, len, reply, ctx, id);
}

static void op_fs_write(const char *params, api_reply_fn reply, void *ctx, long id) {
    char path[FS_PATH_MAX + 1];
    static uint8_t raw[WS_FILE_RAW];
    unsigned n = 0;
    long offset = 0;
    int trunc = 0;
    if (!api_json_str(params, "path", path, sizeof path) || !path[0]) {
        api_reply_err(reply, ctx, id, "bad_request", "need \"path\"");
        return;
    }
    if (take_write(params, raw, &n, &offset, &trunc) < 0) {
        api_reply_err(reply, ctx, id, "bad_request", "bad data");
        return;
    }
    if (is_sd_path(path)) {
#if CONFIG_SD
        op_fs_sd(path, sd_rel(path), (uint32_t)offset, n, 1, raw, reply, ctx, id);
#else
        api_reply_err(reply, ctx, id, "nomedia", "no sd card");
#endif
        return;
    }
    op_fs_write_lfs(path, offset, raw, n, trunc, reply, ctx, id);
}

static bool fs_ops_init(void) {
    fs_on_watch(fs_watch_on_lfs);
    return true;
}

static const op_def_t fs_ops[] = {
    { "fs.stat", op_fs_stat },
    { "fs.read", op_fs_read },
    { "fs.write", op_fs_write },
    { NULL, NULL }
};

static const char *const fs_topics[] = { "fs.changed", "fs.stated", NULL };

const op_module_t op_module_fs = {
    .ns = "fs", .ops = fs_ops, .topics = fs_topics, .init = fs_ops_init
};
