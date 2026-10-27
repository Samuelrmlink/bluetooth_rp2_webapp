/*
 * The whole SD stack on the host, on both buses: the storage worker
 * (ucontext) -> sd_api transfers -> sd_store -> FatFs -> sd_card -> sd_sdio
 * or sd_spi -> the simulated card. What the HTTP layer sees, without HTTP:
 * format (FAT32 and exFAT), upload, download, listing, mkdir, rename,
 * delete, the error map, card pulled and reinserted (settle), and the bench.
 * Each finished card image is written out for fsck (tools/run_tests.py runs
 * fsck.fat / fsck.exfat on it).
 *
 *   gcc -DSPIBUS_HOST -DCONFIG_SD_DETECT=22 -Isrc -Isrc/sd -Ithird_party/fatfs -Itest/host \
 *       test/host/sd_stack_test.c <SD_STACK_SRCS>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "fake_spi.h"
#include "io/spibus.h"
#include "sd/sd_api.h"
#include "sd/sd_card.h"
#include "sd/sd_core.h"
#include "sd/sd_jobs.h"
#include "sd/sd_store.h"
#include "sd/sd_worker.h"
#include "sdcard_sim.h"

/* The editor watcher lives in the firmware image. This test does not link it. */
void fs_watch_note_sd(const char *rel, const char *op) { (void)rel; (void)op; }
bool fs_watch_sd_slice(void) { return false; }

static int checks, failed;
static const char *tag = "";
#define CHECK(c, msg) do { checks++; if (!(c)) { failed++; printf("FAIL [%s] %s:%d %s\n", tag, __FILE__, __LINE__, msg); } } while (0)

extern void (*sd_host_yield_hook)(void);
extern void (*sd_host_maybe_yield_hook)(uint32_t budget_us);
void sdw_host_meter(uint64_t *calls_max, void **calls_site, uint64_t *us_max, void **us_site, bool reset);
extern sdsim_t *sd_host_detect_card;

static sdsim_t card;
static bool spi;

static void worker_yield(void) { sdw_yield(); }

/* The worker must never keep the main loop for long: between two yield
 * opportunities (a yield, or a check where the board yields once its slice
 * has used SD_SLICE_US) it may do only so much. The meter (sd_worker_uc.c)
 * counts firmware function calls (CPU work) and simulated microseconds
 * (waits, wire time) per stretch.
 *
 * Seen here (2026-10-20): at most ~2050 calls (an SPI block: its bytes go
 * through the bus layer) and ~470 us (one 400 kHz command and its response).
 * Before the fixes: 69977 calls in f_mkfs's exFAT up-case table (227 ms of
 * main-loop stall on the board) and ~3 ms of back-to-back commands in
 * identification and multi-block writes. */
#define METER_CALLS_MAX 4000u
#define METER_US_MAX    1000u

static const char *site_name(void *site) {
    static char buf[160];
    char cmd[128];
    snprintf(cmd, sizeof cmd, "addr2line -f -s -e /proc/%d/exe %p", (int)getpid(), site);
    FILE *f = popen(cmd, "r");
    buf[0] = 0;
    if (f) {
        char fn[64] = "", line[64] = "";
        if (fgets(fn, sizeof fn, f) && fgets(line, sizeof line, f)) {
            fn[strcspn(fn, "\n")] = 0;
            line[strcspn(line, "\n")] = 0;
            snprintf(buf, sizeof buf, "%s (%s)", fn, line);
        }
        pclose(f);
    }
    return buf;
}

static void meter(const char *what) {
    uint64_t calls, us;
    void *cs, *us_site;
    sdw_host_meter(&calls, &cs, &us, &us_site, true);
    printf("METER [%s] %-10s calls %6llu  us %6llu\n", tag, what, (unsigned long long)calls, (unsigned long long)us);
    char msg[256];
    snprintf(msg, sizeof msg, "%s: %llu calls without a chance to yield, in %s", what,
             (unsigned long long)calls, calls > METER_CALLS_MAX ? site_name(cs) : "");
    CHECK(calls <= METER_CALLS_MAX, msg);
    snprintf(msg, sizeof msg, "%s: %llu us without a chance to yield, ending at %s", what,
             (unsigned long long)us, us > METER_US_MAX ? site_name(us_site) : "");
    CHECK(us <= METER_US_MAX, msg);
}

/* One pass of the main loop. */
static void pass(void) {
    sdsim_advance_us(1000);
    if (spi) { spibus_poll(); }
    sdw_service();
}
static void passes(int n) { while (n--) { pass(); } }

static bool until_state(const char *want, int max) {
    for (int i = 0; i < max; i++) {
        if (strcmp(sd_card_state(), want) == 0) { return true; }
        pass();
    }
    return strcmp(sd_card_state(), want) == 0;
}

static enum sd_xstate settle(int x) {
    for (int i = 0; i < 20000; i++) {
        enum sd_xstate s = sd_xfer_state(x);
        if (s == SDX_STREAM || s == SDX_DONE || s == SDX_FAIL) { return s; }
        pass();
    }
    return sd_xfer_state(x);
}

static uint8_t big[300000], back[300000];

/* One job (sd_jobs.h), run to completion; returns its result. The data
 * buffer is filled by `fill` right after posting, as lfile.c does. */
static sdj_t job(int op, const char *path, const char *path2, int file, int mode, uint32_t len,
                 uint64_t pos, const uint8_t *fill, uint8_t *out) {
    sdj_t q;
    memset(&q, 0, sizeof q);
    q.op = op; q.file = file; q.mode = mode; q.len = len; q.pos = pos;
    if (path) { snprintf(q.path, sizeof q.path, "%s", path); }
    if (path2) { snprintf(q.path2, sizeof q.path2, "%s", path2); }
    uint32_t tok = 0;
    int tries = 0;
    while (!sdj_post(&q, &tok) && tries++ < 1000) { pass(); }
    if (fill) { memcpy(sdj_buf(), fill, len); }
    sdj_t r;
    memset(&r, 0, sizeof r);
    r.rc = -99;
    for (int i = 0; i < 20000; i++) {
        if (sdj_poll(tok, &r) == SDJ_DONE) { break; }
        pass();
    }
    if (out && r.rc > 0) { memcpy(out, sdj_buf(), (size_t)r.rc > SDJ_BUF ? SDJ_BUF : (size_t)r.rc); }
    sdj_finish(tok);
    return r;
}

/* The Lua file module's path: jobs through the worker. */
static int s_stale_file = -1;
static void jobs(void) {
    static uint8_t rd[SDJ_BUF];
    sdj_t r = job(SDJ_OPEN, "lua/sub/f.bin", NULL, 0, LS_WR, 0, 0, NULL, NULL);
    CHECK(r.rc >= 0, "job: open for write, parents made");
    int f = r.rc;
    size_t total = 0;
    for (int i = 0; i < 10; i++) {
        r = job(SDJ_WRITE, NULL, NULL, f, 0, SDJ_BUF, 0, big + total, NULL);
        if (r.rc != SDJ_BUF) { CHECK(false, "job: write a buffer"); break; }
        total += SDJ_BUF;
    }
    CHECK(r.size == total && r.tell == total, "job: size and position follow the writes");
    CHECK(job(SDJ_CLOSE, NULL, NULL, f, 0, 0, 0, NULL, NULL).rc == LS_OK, "job: close");
    r = job(SDJ_STAT, "lua/sub/f.bin", NULL, 0, 0, 0, 0, NULL, NULL);
    CHECK(r.rc == LS_OK && !r.is_dir && r.size == total, "job: stat a file");
    r = job(SDJ_STAT, "lua/sub", NULL, 0, 0, 0, 0, NULL, NULL);
    CHECK(r.rc == LS_OK && r.is_dir, "job: stat a folder");
    CHECK(job(SDJ_STAT, "lua/nope", NULL, 0, 0, 0, 0, NULL, NULL).rc == LS_ENOENT, "job: stat missing");
    CHECK(job(SDJ_STAT, "", NULL, 0, 0, 0, 0, NULL, NULL).is_dir, "job: the root is a folder");

    r = job(SDJ_OPEN, "lua/sub/f.bin", NULL, 0, LS_RD, 0, 0, NULL, NULL);
    CHECK(r.rc >= 0 && r.size == total, "job: open for read reports the size");
    f = r.rc;
    r = job(SDJ_SEEK, NULL, NULL, f, 0, 0, 12345, NULL, NULL);
    CHECK(r.rc == LS_OK && r.tell == 12345, "job: seek");
    r = job(SDJ_READ, NULL, NULL, f, 0, 1000, 0, NULL, rd);
    CHECK(r.rc == 1000 && !memcmp(rd, big + 12345, 1000) && r.tell == 13345, "job: read after a seek");
    r = job(SDJ_SEEK, NULL, NULL, f, 0, 0, total - 10, NULL, NULL);
    r = job(SDJ_READ, NULL, NULL, f, 0, SDJ_BUF, 0, NULL, rd);
    CHECK(r.rc == 10, "job: a read at the end is short");
    r = job(SDJ_READ, NULL, NULL, f, 0, SDJ_BUF, 0, NULL, rd);
    CHECK(r.rc == 0, "job: then empty");
    s_stale_file = f;          /* left open: the pull below must make it stale */

    r = job(SDJ_OPEN, "lua/missing.bin", NULL, 0, LS_RD, 0, 0, NULL, NULL);
    CHECK(r.rc == LS_ENOENT, "job: opening a missing file for read: ENOENT");
    r = job(SDJ_OPEN, "lua/sub/f.bin", NULL, 0, LS_APPEND, 0, 0, NULL, NULL);
    CHECK(r.rc >= 0 && r.tell == total, "job: append starts at the end");
    int a = r.rc;
    r = job(SDJ_WRITE, NULL, NULL, a, 0, 5, 0, (const uint8_t *)"tail!", NULL);
    CHECK(r.rc == 5 && r.size == total + 5, "job: append grows the file");
    job(SDJ_CLOSE, NULL, NULL, a, 0, 0, 0, NULL, NULL);

    /* Listing, paged through the 4 KiB buffer. */
    CHECK(job(SDJ_MKDIR, "lua/many", NULL, 0, 0, 0, 0, NULL, NULL).rc == LS_OK, "job: mkdir");
    CHECK(job(SDJ_MKDIR, "lua/many", NULL, 0, 0, 0, 0, NULL, NULL).rc == LS_OK, "job: mkdir of an existing folder");
    for (int i = 0; i < 120; i++) {
        char fn[64];
        snprintf(fn, sizeof fn, "lua/many/a_rather_long_file_name_number_%03d.txt", i);
        r = job(SDJ_OPEN, fn, NULL, 0, LS_WR, 0, 0, NULL, NULL);
        if (r.rc < 0) { CHECK(false, "job: create in a folder"); break; }
        job(SDJ_CLOSE, NULL, NULL, r.rc, 0, 0, 0, NULL, NULL);
    }
    int seen = 0, pages = 0;
    bool more = true;
    while (more && pages < 20) {
        r = job(SDJ_LIST, "lua/many", NULL, 0, 0, 0, (uint64_t)seen, NULL, NULL);
        if (r.rc < 0) { break; }
        seen += r.rc;
        more = r.more;
        pages++;
    }
    CHECK(seen == 120 && pages > 1, "job: a listing larger than the buffer pages");
    CHECK(job(SDJ_REMOVE, "lua/many", NULL, 0, 0, 0, 0, NULL, NULL).rc == SDJ_ENOTEMPTY,
          "job: removing a folder with files in it: ENOTEMPTY");
    CHECK(job(SDJ_RENAME, "lua/sub/f.bin", "lua/many/a_rather_long_file_name_number_000.txt", 0, 0, 0, 0, NULL, NULL).rc
          == LS_EEXIST, "job: rename onto an existing name: EEXIST");
    CHECK(job(SDJ_RENAME, "lua/sub/f.bin", "lua/moved/g.bin", 0, 0, 0, 0, NULL, NULL).rc == LS_OK,
          "job: rename (target parents made)");
    CHECK(job(SDJ_STAT, "lua/moved/g.bin", NULL, 0, 0, 0, 0, NULL, NULL).size == total + 5, "job: the renamed file");
    r = job(SDJ_STATFS, NULL, NULL, 0, 0, 0, 0, NULL, NULL);
    CHECK(r.rc == LS_OK && r.total_bytes > r.free_bytes && r.free_bytes > 0, "job: statfs");

    /* An abandoned open closes its file; a second post while busy fails. */
    sdj_t q;
    memset(&q, 0, sizeof q);
    q.op = SDJ_OPEN; q.mode = LS_RD;
    snprintf(q.path, sizeof q.path, "lua/moved/g.bin");
    uint32_t t1 = 0, t2 = 0;
    CHECK(sdj_post(&q, &t1), "job: post");
    CHECK(!sdj_post(&q, &t2), "job: a second post while one is in flight fails");
    sdj_abandon(t1);
    passes(50);
    CHECK(sdj_poll(t1, NULL) == SDJ_IDLE, "job: an abandoned job frees itself");
    /* Every file closed again: SDJ_FILES opens must all succeed now (one is
     * still held by the stale read handle). */
    int got = 0, fh[SDJ_FILES];
    for (int i = 0; i < SDJ_FILES; i++) {
        r = job(SDJ_OPEN, "lua/moved/g.bin", NULL, 0, LS_RD, 0, 0, NULL, NULL);
        fh[i] = r.rc;
        got += r.rc >= 0;
    }
    CHECK(got == SDJ_FILES - 1, "job: the abandoned open did not leak a file");
    for (int i = 0; i < SDJ_FILES; i++) { if (fh[i] >= 0) { sdj_close_async(fh[i]); } }
    passes(20);
    CHECK(sd_xfer_active() == 0, "job: the borrowed transfer ring went back");
}

/* PUT `n` bytes; returns the HTTP status the transfer ended with. */
static int put(const char *path, const uint8_t *data, size_t n) {
    struct sd_reply r;
    char p[SD_PATH_MAX];
    snprintf(p, sizeof p, "%s", path);
    if (!sd_path_clean(p, sizeof p)) { return 400; }
    if (!sd_api_gate(&r)) { return r.status; }
    int x = sd_xfer_start(SD_OP_PUT, p, NULL, n, &r);
    if (x < 0) { return r.status; }
    enum sd_xstate s = settle(x);
    size_t done = 0;
    while (s == SDX_STREAM && done < n) {
        size_t k = sd_xfer_put(x, data + done, n - done > 700 ? 700 : n - done);   /* odd chunks, as TCP gives */
        done += k;
        pass();
        s = sd_xfer_state(x);
    }
    if (s == SDX_STREAM) { sd_xfer_end_input(x); }
    for (int i = 0; i < 20000 && sd_xfer_state(x) == SDX_STREAM; i++) { pass(); }
    sd_xfer_result(x, &r);
    int st = sd_xfer_state(x) == SDX_DONE ? 200 : r.status;
    sd_xfer_release(x);
    return st;
}

/* GET into out (cap); returns status, *len bytes. site resolves like /sd. */
static int get_ex(const char *path, uint8_t *out, size_t cap, size_t *len, bool site, bool *gzip,
                  const char **mime) {
    struct sd_reply r;
    char p[SD_PATH_MAX];
    snprintf(p, sizeof p, "%s", path);
    *len = 0;
    if (gzip) { *gzip = false; }
    if (mime) { *mime = NULL; }
    if (!sd_path_clean(p, sizeof p)) { return 400; }
    if (!sd_api_gate(&r)) { return r.status; }
    int x = sd_xfer_start(SD_OP_GET, p, NULL, 0, &r);
    if (x < 0) { return r.status; }
    if (site) { sd_xfer_as_site(x); }
    enum sd_xstate s = settle(x);
    if (s == SDX_FAIL) {
        sd_xfer_result(x, &r);
        sd_xfer_release(x);
        return r.status;
    }
    for (int i = 0; i < 200000; i++) {
        const uint8_t *q;
        size_t k = sd_xfer_peek(x, &q);
        if (k) {
            if (*len + k <= cap) { memcpy(out + *len, q, k); }
            *len += k;
            sd_xfer_consume(x, k);
            continue;
        }
        s = sd_xfer_state(x);
        if (s == SDX_DONE || s == SDX_FAIL) {
            if (sd_xfer_peek(x, &q) == 0) { break; }
            continue;
        }
        pass();
    }
    bool g = sd_xfer_gzip(x);
    const char *m = sd_xfer_mime(x);
    sd_xfer_result(x, &r);
    int st = sd_xfer_state(x) == SDX_DONE ? 200 : r.status;
    sd_xfer_release(x);
    if (gzip) { *gzip = g; }
    if (mime) { *mime = m; }
    return st;
}

static int get(const char *path, uint8_t *out, size_t cap, size_t *len) {
    return get_ex(path, out, cap, len, false, NULL, NULL);
}

static int get_site(const char *path, uint8_t *out, size_t cap, size_t *len, bool *gzip, const char **mime) {
    return get_ex(path, out, cap, len, true, gzip, mime);
}

static int get_meta(const char *path, uint8_t *out, size_t cap, size_t *len, bool *gzip, const char **mime) {
    return get_ex(path, out, cap, len, false, gzip, mime);
}

static int simple(enum sd_op op, const char *a, const char *b) {
    struct sd_reply r;
    char p[SD_PATH_MAX], q[SD_PATH_MAX];
    snprintf(p, sizeof p, "%s", a);
    snprintf(q, sizeof q, "%s", b ? b : "");
    if (!sd_path_clean(p, sizeof p) || (b && !sd_path_clean(q, sizeof q))) { return 400; }
    if (!sd_api_gate(&r)) { return r.status; }
    int x = sd_xfer_start(op, p, b ? q : NULL, 0, &r);
    if (x < 0) { return r.status; }
    settle(x);
    sd_xfer_result(x, &r);
    int st = sd_xfer_state(x) == SDX_DONE ? 200 : r.status;
    sd_xfer_release(x);
    return st;
}

static bool format(const char *fs) {
    struct sd_reply r;
    if (!sd_format_start(fs, 0, &r)) { printf("  format refused: %d %s\n", r.status, r.error); return false; }
    for (int i = 0; i < 100000; i++) {
        struct sd_format_status f;
        sd_format_get(&f);
        if (f.state == SDF_DONE) { return true; }
        if (f.state == SDF_ERROR) { printf("  format error: %s\n", f.error); return false; }
        pass();
    }
    return false;
}

static void dump_image(const char *name) {
    /* the partition, for fsck */
    const uint8_t *m = card.img;
    uint32_t start = m[454] | m[455] << 8 | m[456] << 16 | (uint32_t)m[457] << 24;
    uint32_t count = m[458] | m[459] << 8 | m[460] << 16 | (uint32_t)m[461] << 24;
    const char *dir = getenv("SD_IMG_DIR");
    if (!dir || m[510] != 0x55 || m[511] != 0xAA || !count) { return; }
    char path[256];
    snprintf(path, sizeof path, "%s/%s.img", dir, name);
    FILE *f = fopen(path, "wb");
    if (!f) { return; }
    fwrite(card.img + (uint64_t)start * 512, 512, count, f);
    fclose(f);
    printf("IMAGE %s\n", path);
}


static void stack(const char *fs) {
    sdsim_free(&card);
    sdsim_init(&card, 300 * 2048, true);                 /* 300 MiB SDHC, blank */
    sd_host_detect_card = &card;
    if (spi) { sdsim_attach_spi(&card, 9); } else { sdsim_attach_sdio(&card); }
    sd_card_use(spi ? sd_bus_spi() : sd_bus_sdio());
    ls_unmount();                                        /* the previous card's mount */

    /* A blank card: present, but no filesystem. */
    meter("(before)");
    passes(2600);                                        /* past the 2 s settle */
    CHECK(strcmp(sd_card_state(), "error") == 0, "a blank card is 'error' (present, not mountable)");
    char js[4096];
    CHECK(sd_api_status_json(js, sizeof js) > 0 && strstr(js, "\"card\":{\"bytes\":314572800") &&
          strstr(js, "\"type\":\"SDHC\""), "status names the card even before it is formatted");

    meter("identify");
    CHECK(format(fs), "format");
    CHECK(until_state("ok", 5000), "mounted after the format");
    meter("format");
    sd_api_status_json(js, sizeof js);
    char want[32];
    snprintf(want, sizeof want, "\"fs\":\"%s\"", fs);
    CHECK(strstr(js, want) != NULL, "status shows the new filesystem");
    CHECK(strstr(js, spi ? "\"type\":\"spi\"" : "\"type\":\"sdio\""), "status names the bus");

    /* files */
    for (size_t i = 0; i < sizeof big; i++) { big[i] = (uint8_t)(i * 13 + (i >> 11)); }
    size_t n = 0;
    CHECK(put("logs/2026/big.bin", big, sizeof big) == 200, "PUT 300 KB, parents made");
    CHECK(get("logs/2026/big.bin", back, sizeof back, &n) == 200 && n == sizeof big && !memcmp(back, big, n),
          "GET it back, byte for byte");
    meter("put+get");
    CHECK(put("hello.txt", (const uint8_t *)"hi there\n", 9) == 200, "PUT a small file");
    CHECK(get("", back, sizeof back, &n) == 200 && n < sizeof back, "list the root");
    back[n < sizeof back ? n : sizeof back - 1] = 0;
    CHECK(strstr((char *)back, "{\"name\":\"hello.txt\",\"size\":9}") && strstr((char *)back, "{\"name\":\"logs\",\"dir\":true}"),
          "the listing has the file and the folder");
    CHECK(simple(SD_OP_MKDIR, "a/b/c", NULL) == 200, "mkdir -p");
    CHECK(get("a/b", back, sizeof back, &n) == 200 && memmem(back, n, "\"c\",\"dir\":true", 14), "the new folder is there");
    CHECK(simple(SD_OP_RENAME, "hello.txt", "a/b/c/hi.txt") == 200, "rename into the folder");
    CHECK(get("a/b/c/hi.txt", back, sizeof back, &n) == 200 && n == 9 && !memcmp(back, "hi there\n", 9), "renamed file reads");
    CHECK(get("hello.txt", back, sizeof back, &n) == 404, "the old name is gone (404)");
    {
        /* Public /sd: a folder serves index.html, and the .gz copy wins. */
        static const uint8_t page[] = {0x1f, 0x8b, 'p', 'a', 'g', 'e'};
        static const uint8_t script[] = {0x1f, 0x8b, 'j', 's'};
        static const uint8_t plain_page[] = {'<', 'p', '>', 'h', 'i', '<', '/', 'p', '>'};
        bool gz = false;
        const char *mime = NULL;
        CHECK(put("apps/printer/index.html.gz", page, sizeof page) == 200, "PUT a gzipped index");
        CHECK(put("apps/printer/app.js.gz", script, sizeof script) == 200, "PUT a gzipped script");
        CHECK(put("apps/printer/plain.txt", (const uint8_t *)"plain\n", 6) == 200, "PUT a plain card file");
        CHECK(put("apps/other/index.html", plain_page, sizeof plain_page) == 200, "PUT an uncompressed index");
        CHECK(put("apps/both/index.html", plain_page, sizeof plain_page) == 200, "PUT a plain index beside a gz");
        CHECK(put("apps/both/index.html.gz", page, sizeof page) == 200, "PUT the gzipped twin");
        CHECK(get_site("apps/printer", back, sizeof back, &n, &gz, &mime) == 200 && n == sizeof page &&
              !memcmp(back, page, n) && gz && mime && strstr(mime, "text/html"),
              "/sd/apps/printer serves index.html.gz");
        CHECK(get_site("apps/printer/", back, sizeof back, &n, &gz, &mime) == 200 && n == sizeof page && gz,
              "a trailing slash is the same folder");
        CHECK(get_site("apps/printer/app.js", back, sizeof back, &n, &gz, &mime) == 200 && n == sizeof script &&
              !memcmp(back, script, n) && gz && mime && strstr(mime, "javascript"),
              "/sd prefers app.js.gz and types it as javascript");
        CHECK(get_site("apps/printer/plain.txt", back, sizeof back, &n, &gz, &mime) == 200 && n == 6 && !gz &&
              !memcmp(back, "plain\n", 6),
              "a file with no .gz twin is served as stored");
        CHECK(get_site("apps/other", back, sizeof back, &n, &gz, &mime) == 200 && n == sizeof plain_page && !gz &&
              !memcmp(back, plain_page, n),
              "a folder whose index is not gzipped still serves it");
        CHECK(get_site("apps/both", back, sizeof back, &n, &gz, &mime) == 200 && gz && n == sizeof page &&
              !memcmp(back, page, n),
              "index.html.gz wins over index.html");
        CHECK(simple(SD_OP_MKDIR, "apps/empty", NULL) == 200 &&
              get_site("apps/empty", back, sizeof back, &n, &gz, &mime) == 404,
              "a folder with no index is 404");
        CHECK(get("apps/printer", back, sizeof back, &n) == 200 && memmem(back, n, "index.html.gz", 13),
              "the file API still lists the folder");
        CHECK(get_meta("apps/printer/index.html.gz", back, sizeof back, &n, &gz, &mime) == 200 && gz && n == sizeof page &&
              mime && strstr(mime, "text/html"),
              "a direct .gz read is marked gzip and typed as html");
        /* Two slots, and the third is the 503 the HTTP server waits through. */
        struct sd_reply busy;
        char holdp[SD_PATH_MAX];
        snprintf(holdp, sizeof holdp, "apps/printer/plain.txt");
        CHECK(sd_path_clean(holdp, sizeof holdp) && sd_api_gate(&busy), "the card takes another GET");
        int h0 = sd_xfer_start(SD_OP_GET, holdp, NULL, 0, &busy);
        int h1 = sd_xfer_start(SD_OP_GET, holdp, NULL, 0, &busy);
        CHECK(h0 >= 0 && h1 >= 0 && h0 != h1, "two GETs can hold the two slots");
        enum sd_xstate hs = settle(h0);
        for (int i = 0; i < 1000 && hs != SDX_DONE && hs != SDX_FAIL; i++) {
            const uint8_t *q;
            if (sd_xfer_peek(h0, &q)) { sd_xfer_consume(h0, sd_xfer_peek(h0, &q)); }
            else { pass(); }
            hs = sd_xfer_state(h0);
        }
        hs = settle(h1);
        for (int i = 0; i < 1000 && hs != SDX_DONE && hs != SDX_FAIL; i++) {
            const uint8_t *q;
            if (sd_xfer_peek(h1, &q)) { sd_xfer_consume(h1, sd_xfer_peek(h1, &q)); }
            else { pass(); }
            hs = sd_xfer_state(h1);
        }
        int h2 = sd_xfer_start(SD_OP_GET, holdp, NULL, 0, &busy);
        CHECK(h2 < 0 && busy.status == 503 && busy.retry_after == 1 && strcmp(busy.error, "card busy") == 0,
              "a third GET is card busy, which HTTP queues");
        sd_xfer_release(h0);
        h2 = sd_xfer_start(SD_OP_GET, holdp, NULL, 0, &busy);
        CHECK(h2 >= 0, "releasing a slot lets the next GET start");
        sd_xfer_release(h1);
        sd_xfer_release(h2);
    }
    CHECK(simple(SD_OP_DELETE, "a/b", NULL) == 409, "deleting a non-empty folder: 409");
    CHECK(simple(SD_OP_DELETE, "a/b/c/hi.txt", NULL) == 200 && simple(SD_OP_DELETE, "a/b/c", NULL) == 200, "delete file, then folder");
    CHECK(simple(SD_OP_DELETE, "nope.txt", NULL) == 404, "delete a missing file: 404");
    CHECK(put("../etc/passwd", big, 10) == 400, "a path out of the tree: 400");
    CHECK(put("logs", big, 10) == 409, "PUT onto a folder: 409");
    CHECK(put("bad:name.txt", big, 10) == 400, "a name FAT cannot hold: 400");
    meter("file ops");

    /* a big folder: listed, then emptied */
    for (int i = 0; i < 300; i++) {
        char fn[48];
        snprintf(fn, sizeof fn, "many/file-with-a-long-name-%03d.txt", i);
        if (put(fn, big, 1 + i % 3) != 200) { CHECK(false, "PUT into a big folder"); break; }
    }
    meter("300 files");
    CHECK(get("many", back, sizeof back, &n) == 200 && n < sizeof back &&
          memmem(back, n, "file-with-a-long-name-299.txt", 29), "list a 300-file folder");
    meter("list 300");
    int gone = 0;
    for (int i = 0; i < 300; i++) {
        char fn[48];
        snprintf(fn, sizeof fn, "many/file-with-a-long-name-%03d.txt", i);
        gone += simple(SD_OP_DELETE, fn, NULL) == 200;
    }
    CHECK(gone == 300 && simple(SD_OP_DELETE, "many", NULL) == 200, "empty it and remove it");
    meter("delete 300");

    /* bench */
    struct sd_reply r;
    CHECK(sd_bench_start(1, &r), "bench starts");
    for (int i = 0; i < 100000; i++) {
        sd_api_status_json(js, sizeof js);
        if (strstr(js, "\"bench\":{\"state\":\"done\"") || strstr(js, "\"bench\":{\"state\":\"error\"")) { break; }
        pass();
    }
    CHECK(strstr(js, "\"bench\":{\"state\":\"done\"") != NULL, "bench finishes");
    CHECK(get("BTGW-BENCH.bin", back, sizeof back, &n) == 404, "and removes its file");
    meter("bench");

    jobs();
    meter("jobs");

    /* pulled, then back */
    sdsim_pull(&card);
    passes(20);
    CHECK(strcmp(sd_card_state(), "none") == 0, "pulled: state none");
    CHECK(get("logs/2026/big.bin", back, sizeof back, &n) == 503, "a GET while out: 503");
    sdsim_insert(&card);
    passes(100);
    CHECK(strcmp(sd_card_state(), "settling") == 0, "reinserted: settling first");
    CHECK(until_state("ok", 5000), "then mounted again");
    CHECK(get("logs/2026/big.bin", back, sizeof back, &n) == 200 && n == sizeof big && !memcmp(back, big, n),
          "the file survived the pull");
    meter("remount");
    {
        static uint8_t rd[64];
        sdj_t r = job(SDJ_READ, NULL, NULL, s_stale_file, 0, 64, 0, NULL, rd);
        CHECK(r.rc == LS_EIO, "job: a handle from before the pull is stale (EIO), not someone else's file");
        CHECK(job(SDJ_STAT, "lua/moved/g.bin", NULL, 0, 0, 0, 0, NULL, NULL).rc == LS_OK, "job: works after the remount");
    }
    CHECK(sdw_stack_ok(), "worker stack intact");

    char name[32];
    snprintf(name, sizeof name, "%s_%s", spi ? "spi" : "sdio", fs);
    dump_image(name);
}

int main(void) {
    fake_reset();
    fake_autocomplete(true);
    sd_host_yield_hook = worker_yield;
    sd_host_maybe_yield_hook = sdw_maybe_yield;
    sd_init();

    tag = "sdio fat32"; spi = false; stack("fat32");
    tag = "sdio exfat"; stack("exfat");

    spibus_t *b = spibus_init(1, 14, 15, 8, "w5500");
    (void)b;
    tag = "spi fat32"; spi = true; stack("fat32");
    tag = "spi exfat"; stack("exfat");
    CHECK(fake_bus_conflicts() == 0, "no MISO conflict");

    sdsim_free(&card);
    printf("%d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
