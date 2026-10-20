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
#include "sd/sd_store.h"
#include "sd/sd_worker.h"
#include "sdcard_sim.h"

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

/* GET into out (cap); returns status, *len bytes. */
static int get(const char *path, uint8_t *out, size_t cap, size_t *len) {
    struct sd_reply r;
    char p[SD_PATH_MAX];
    snprintf(p, sizeof p, "%s", path);
    *len = 0;
    if (!sd_path_clean(p, sizeof p)) { return 400; }
    if (!sd_api_gate(&r)) { return r.status; }
    int x = sd_xfer_start(SD_OP_GET, p, NULL, 0, &r);
    if (x < 0) { return r.status; }
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
    sd_xfer_result(x, &r);
    int st = sd_xfer_state(x) == SDX_DONE ? 200 : r.status;
    sd_xfer_release(x);
    return st;
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

static uint8_t big[300000], back[300000];

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
