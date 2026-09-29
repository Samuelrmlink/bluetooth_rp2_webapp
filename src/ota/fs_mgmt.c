/*
 * MCUmgr fs group (8) over LittleFS: download, upload, status, hash, close.
 *
 * Paths. SMP names are absolute ("/lfs/lua/foo.lua"); the leading "/" and an
 * optional "lfs/" are stripped and the rest goes through fs_valid_path(),
 * so the same rules and the same three-tier overlay (staged/ etc.) apply as
 * for the HTTP file API.
 *
 * Writes are EXPENSIVE (a 4 KB sector erase is ~45 ms with everything else
 * stalled) and this runs from the main loop, so: one chunk per request,
 * never more than one outstanding, and the reply goes out only after the
 * chunk is on flash -- SMP is lock-step, so that throttles the client to
 * the flash's pace rather than the network's. A session nobody finishes is
 * aborted after 4 s idle (Zephyr's default), which deletes the partial file
 * exactly as http.c's conn_cleanup does.
 */
#include "ota/smp.h"
#include "ota/crc32.h"
#include "crypto/sha256.h"
#include "fs/fs.h"

#include <stdio.h>
#include <string.h>
#include "pico/time.h"

enum {
    FS_ERR_INVALID_NAME = 2, FS_ERR_NOT_FOUND = 3, FS_ERR_IS_DIRECTORY = 4, FS_ERR_OPEN_FAILED = 5,
    FS_ERR_SEEK_FAILED = 6, FS_ERR_READ_FAILED = 7, FS_ERR_TRUNCATE_FAILED = 8, FS_ERR_DELETE_FAILED = 9,
    FS_ERR_WRITE_FAILED = 10, FS_ERR_OFFSET_NOT_VALID = 11, FS_ERR_OFFSET_LARGER_THAN_FILE = 12,
    FS_ERR_CHECKSUM_HASH_NOT_FOUND = 13, FS_ERR_MOUNT_POINT_NOT_FOUND = 14,
    FS_ERR_READ_ONLY_FILESYSTEM = 15, FS_ERR_FILE_EMPTY = 16,
};

#define FS_IDLE_CLOSE_MS 4000
#define DL_CHUNK 1024           /* the server picks the download chunk size */

static struct {
    bool active;
    char name[FS_PATH_MAX + 1];
    uint32_t expected;          /* from `len` at off 0 */
    uint32_t off;
    uint32_t last_ms;
} wr;

static bool map_name(const smp_req_t *req, char *out, size_t cap) {
    char raw[FS_PATH_MAX + 8];
    if (!cbor_get_tstr(req->payload, req->plen, "name", raw, sizeof raw)) return false;
    const char *p = raw;
    while (*p == '/') p++;
    if (strncmp(p, "lfs/", 4) == 0) p += 4;
    if (strlen(p) >= cap) return false;
    strcpy(out, p);
    return fs_valid_path(out);
}

static void abort_write(void) {
    if (!wr.active) return;
    fs_abort_write();
    fs_delete(wr.name);
    wr.active = false;
}

static int h_download(const smp_req_t *req, cbor_enc_t *e) {
    char name[FS_PATH_MAX + 1];
    if (!map_name(req, name, sizeof name)) return SMP_GERR(FS_ERR_INVALID_NAME);
    uint64_t off = 0;
    cbor_get_uint(req->payload, req->plen, "off", &off);
    if (fs_is_dir(name)) return SMP_GERR(FS_ERR_IS_DIRECTORY);
    size_t size;
    int h = fs_begin_read(name, &size);
    if (h < 0) return SMP_GERR(FS_ERR_NOT_FOUND);
    if (off > size) { fs_end_read(h); return SMP_GERR(FS_ERR_OFFSET_LARGER_THAN_FILE); }
    static uint8_t buf[DL_CHUNK];
    /* Files are small (FS_FILE_MAX) and fs_read is a memcpy from XIP, so
     * seeking by reading forward is cheaper than a seek API would be. */
    size_t skip = (size_t)off;
    while (skip) {
        size_t take = skip > sizeof buf ? sizeof buf : skip;
        int n = fs_read(h, buf, take);
        if (n <= 0) break;
        skip -= (size_t)n;
    }
    int n = fs_read(h, buf, sizeof buf);
    fs_end_read(h);
    if (n < 0) return SMP_GERR(FS_ERR_READ_FAILED);
    cbor_kv_uint(e, "off", off);
    cbor_kv_bstr(e, "data", buf, (size_t)n);
    if (off == 0) cbor_kv_uint(e, "len", size);
    return 0;
}

static int h_upload(const smp_req_t *req, cbor_enc_t *e) {
    char name[FS_PATH_MAX + 1];
    if (!map_name(req, name, sizeof name)) return SMP_GERR(FS_ERR_INVALID_NAME);
    uint64_t off = 0, len = 0;
    if (!cbor_get_uint(req->payload, req->plen, "off", &off)) return MGMT_ERR_EINVAL;
    const uint8_t *data = NULL; size_t dlen = 0;
    cbor_get_bstr(req->payload, req->plen, "data", &data, &dlen);
    uint32_t now = to_ms_since_boot(get_absolute_time());

    if (off == 0) {
        if (!cbor_get_uint(req->payload, req->plen, "len", &len)) return MGMT_ERR_EINVAL;
        if (len > FS_FILE_MAX) return SMP_GERR(FS_ERR_WRITE_FAILED);
        abort_write();                      /* a new upload replaces a stale one */
        if (fs_begin_write(name) != 0) return SMP_GERR(FS_ERR_OPEN_FAILED);
        wr.active = true;
        strcpy(wr.name, name);
        wr.expected = (uint32_t)len;
        wr.off = 0;
    } else if (!wr.active || strcmp(wr.name, name) != 0) {
        return SMP_GERR(FS_ERR_OFFSET_NOT_VALID);
    } else if (off != wr.off) {
        /* The resume protocol: the error plus where we are. */
        cbor_kv_uint(e, "len", wr.off);
        return SMP_GERR(FS_ERR_OFFSET_NOT_VALID);
    }
    if (dlen) {
        if (fs_write(data, dlen) < 0) { abort_write(); return SMP_GERR(FS_ERR_WRITE_FAILED); }   /* BYTE COUNT */
        wr.off += dlen;
    }
    wr.last_ms = now;
    if (wr.off >= wr.expected) {
        int rc = fs_end_write();
        wr.active = false;
        if (rc != 0) { fs_delete(wr.name); return SMP_GERR(FS_ERR_WRITE_FAILED); }
    }
    cbor_kv_uint(e, "off", wr.off);
    return 0;
}

static int h_status(const smp_req_t *req, cbor_enc_t *e) {
    char name[FS_PATH_MAX + 1];
    if (!map_name(req, name, sizeof name)) return SMP_GERR(FS_ERR_INVALID_NAME);
    size_t size;
    if (!fs_stat(name, &size)) return SMP_GERR(FS_ERR_NOT_FOUND);
    cbor_kv_uint(e, "len", size);
    return 0;
}

static int h_hash(const smp_req_t *req, cbor_enc_t *e) {
    char name[FS_PATH_MAX + 1];
    if (!map_name(req, name, sizeof name)) return SMP_GERR(FS_ERR_INVALID_NAME);
    char type[16] = "crc32";
    cbor_get_tstr(req->payload, req->plen, "type", type, sizeof type);
    bool sha = strcmp(type, "sha256") == 0;
    if (!sha && strcmp(type, "crc32") != 0) return SMP_GERR(FS_ERR_CHECKSUM_HASH_NOT_FOUND);
    uint64_t off = 0, len = 0;
    cbor_get_uint(req->payload, req->plen, "off", &off);
    cbor_get_uint(req->payload, req->plen, "len", &len);
    size_t size;
    int h = fs_begin_read(name, &size);
    if (h < 0) return SMP_GERR(FS_ERR_NOT_FOUND);
    if (off > size) { fs_end_read(h); return SMP_GERR(FS_ERR_OFFSET_LARGER_THAN_FILE); }
    size_t want = len ? (size_t)len : size - (size_t)off;
    if (off + want > size) want = size - (size_t)off;
    static uint8_t buf[512];
    size_t skip = (size_t)off;
    uint32_t crc = 0; sha256_ctx_t sc; sha256_init(&sc);
    size_t done = 0;
    for (;;) {
        int n = fs_read(h, buf, sizeof buf);
        if (n <= 0) break;
        size_t p = 0;
        if (skip) { size_t s = skip > (size_t)n ? (size_t)n : skip; skip -= s; p = s; }
        size_t take = (size_t)n - p;
        if (take > want - done) take = want - done;
        if (sha) sha256_update(&sc, buf + p, take); else crc = crc32_update(crc, buf + p, take);
        done += take;
        if (done >= want) break;
    }
    fs_end_read(h);
    cbor_kv_tstr(e, "type", type);
    if (off) cbor_kv_uint(e, "off", off);
    cbor_kv_uint(e, "len", done);
    if (sha) { uint8_t d[32]; sha256_final(&sc, d); cbor_kv_bstr(e, "output", d, 32); }
    else cbor_kv_uint(e, "output", crc);
    return 0;
}

static int h_supported(const smp_req_t *req, cbor_enc_t *e) {
    (void)req;
    cbor_tstr(e, "types");
    cbor_map_open(e);
    cbor_tstr(e, "crc32");  cbor_map_open(e); cbor_kv_uint(e, "format", 0); cbor_kv_uint(e, "size", 4);  cbor_close(e);
    cbor_tstr(e, "sha256"); cbor_map_open(e); cbor_kv_uint(e, "format", 1); cbor_kv_uint(e, "size", 32); cbor_close(e);
    cbor_close(e);
    return 0;
}

static int h_close(const smp_req_t *req, cbor_enc_t *e) {
    (void)req; (void)e;
    abort_write();
    return 0;
}

void smp_fs_tick(uint32_t now) {
    if (wr.active && (uint32_t)(now - wr.last_ms) > FS_IDLE_CLOSE_MS) {
        printf("smp: fs upload of %s abandoned at %lu bytes\n", wr.name, (unsigned long)wr.off);
        abort_write();
    }
}

const smp_cmd_t smp_fs_cmds[] = {
    { SMP_GROUP_FS, 0, false, h_download },
    { SMP_GROUP_FS, 0, true,  h_upload },
    { SMP_GROUP_FS, 1, false, h_status },
    { SMP_GROUP_FS, 2, false, h_hash },
    { SMP_GROUP_FS, 3, false, h_supported },
    { SMP_GROUP_FS, 4, true,  h_close },
    { 0, 0, false, NULL },
};
