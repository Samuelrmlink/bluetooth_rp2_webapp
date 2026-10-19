/*
 * The differential script: the same steps, in the same order, on whichever
 * SD driver this ELF links (backend.h). One JSON line per step over RTT;
 * tools/sdio_diff.py runs it in mcusim against a simulated card and compares
 * the two drivers' lines, the card's final image and its command trace.
 *
 * The faults are the simulated card's, at fixed block addresses (FAULT_*,
 * the same numbers tools/sdio_diff.py gives the card), so both drivers meet
 * them at the same step.
 *
 *   raw: identification, reads at the edges, 1/2/8/64-block writes and
 *        read-backs, then the faults
 *   fs:  FatFs (both R0.15) on top: mkfs, files, rename, delete, listing
 */
#include <stdio.h>
#include <string.h>

#include "backend.h"
#include "pico/stdlib.h"
#if SDDIFF_SCENARIO_fs
#include "ff.h"
#define SCENARIO "fs"
#else
#define SCENARIO "raw"
#endif

#define FAULT_CRC_READ   5000u   /* CRC error on this block's next read (once) */
#define FAULT_CRC_WRITE  5100u   /* CRC-status error on its next write (once) */
#define FAULT_WRITE_ERR  5150u   /* write-error token on its next write (once) */
#define FAULT_MUTE_READ  5200u   /* the next CMD17 for it gets no response (once) */
#define FAULT_BUSY       5300u   /* 1.5 s programming busy after this block */
#define FAULT_PULL       5402u   /* the card is pulled while this block is read; back 300 ms later */

static uint8_t buf[64 * 512], chk[64 * 512];
static int step;
static const char *const CLS[] = { "ok", "timeout", "crc", "io", "nocard", "param" };

static uint64_t fnv(const uint8_t *p, size_t n) {
    uint64_t h = 1469598103934665603ull;
    while (n--) { h ^= *p++; h *= 1099511628211ull; }
    return h;
}

static void pattern(uint8_t *b, uint32_t n, uint32_t seed) {
    uint32_t x = seed * 2654435761u + 1u;
    for (uint32_t i = 0; i < n; i++) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; b[i] = (uint8_t)x; }
}

static void line(const char *name, int res, const char *extra) {
    printf("{\"step\":%d,\"name\":\"%s\",\"res\":\"%s\"%s%s}\n", ++step, name, CLS[res], extra ? "," : "", extra ? extra : "");
}

static void read_step(const char *name, uint32_t lba, uint32_t n) {
    char x[96];
    memset(buf, 0, n * 512);
    uint64_t t0 = time_us_64();
    int r = be_read(lba, buf, n);
    snprintf(x, sizeof x, "\"lba\":%lu,\"n\":%lu,\"hash\":\"%016llx\",\"us\":%llu", (unsigned long)lba,
             (unsigned long)n, r == R_OK ? (unsigned long long)fnv(buf, n * 512) : 0ull,
             (unsigned long long)(time_us_64() - t0));
    line(name, r, x);
}

static void write_step(const char *name, uint32_t lba, uint32_t n) {
    char x[128];
    pattern(buf, n * 512, lba);
    uint64_t t0 = time_us_64();
    int r = be_write(lba, buf, n);
    uint64_t dt = time_us_64() - t0;
    int rb = r == R_OK ? be_read(lba, chk, n) : -1;
    snprintf(x, sizeof x, "\"lba\":%lu,\"n\":%lu,\"readback\":\"%s\",\"match\":%s,\"us\":%llu",
             (unsigned long)lba, (unsigned long)n, rb < 0 ? "-" : CLS[rb],
             rb == R_OK && !memcmp(buf, chk, n * 512) ? "true" : "false", (unsigned long long)dt);
    line(name, r, x);
}

static void init_step(const char *name) {
    be_info_t in;
    int r = be_init(&in);
    char x[96];
    snprintf(x, sizeof x, "\"sectors\":%llu,\"cid\":\"%016llx\"", (unsigned long long)in.sectors,
             in.cid_ok ? (unsigned long long)fnv(in.cid, 15) : 0ull);   /* byte 15 is CRC|1: the same */
    line(name, r, x);
}

#if SDDIFF_SCENARIO_raw
static void scenario(void) {
    init_step("init");
    be_info_t in;
    be_init(&in);
    read_step("read first", 0, 1);
    read_step("read middle x8", (uint32_t)(in.sectors / 2), 8);
    read_step("read last", (uint32_t)(in.sectors - 1), 1);
    write_step("write x1", 1000, 1);
    write_step("write x2", 1001, 2);
    write_step("write x8", 2000, 8);
    write_step("write x64", 3001, 64);
    line("sync", be_sync(), NULL);

    read_step("fault: crc on read", FAULT_CRC_READ, 4);
    write_step("fault: crc status on write", FAULT_CRC_WRITE, 2);
    write_step("fault: write error token", FAULT_WRITE_ERR, 1);
    read_step("fault: no response", FAULT_MUTE_READ, 1);
    write_step("fault: 1.5 s busy", FAULT_BUSY, 1);
    read_step("fault: card pulled mid-read", FAULT_PULL - 2, 8);
    uint64_t t0 = time_us_64();
    while (!be_present() && time_us_64() - t0 < 3000000u) { sleep_ms(10); }
    sleep_ms(50);
    init_step("re-init after reinsertion");
    read_step("read after reinsertion", FAULT_PULL - 2, 8);
}
#endif

#if SDDIFF_SCENARIO_fs
static FATFS fs;
static FIL f;
static int fr(FRESULT r) { return r == FR_OK ? R_OK : r == FR_NOT_READY ? R_NOCARD : R_IO; }

static void fs_write(const char *path, uint32_t size, uint32_t seed) {
    UINT bw = 0;
    FRESULT r = f_open(&f, path, FA_CREATE_ALWAYS | FA_WRITE);
    uint64_t h = 1469598103934665603ull;
    for (uint32_t done = 0; r == FR_OK && done < size;) {
        uint32_t k = size - done > sizeof buf ? sizeof buf : size - done;
        pattern(buf, k, seed + done);
        for (uint32_t i = 0; i < k; i++) { h ^= buf[i]; h *= 1099511628211ull; }
        r = f_write(&f, buf, k, &bw);
        done += k;
    }
    if (r == FR_OK) { r = f_close(&f); }
    char x[96];
    snprintf(x, sizeof x, "\"path\":\"%s\",\"size\":%lu,\"hash\":\"%016llx\"", path, (unsigned long)size, (unsigned long long)h);
    line("fs write", fr(r), x);
}

static void fs_read(const char *path) {
    UINT br = 0;
    uint64_t h = 1469598103934665603ull, n = 0;
    FRESULT r = f_open(&f, path, FA_READ);
    while (r == FR_OK && (r = f_read(&f, buf, sizeof buf, &br)) == FR_OK && br) {
        for (UINT i = 0; i < br; i++) { h ^= buf[i]; h *= 1099511628211ull; }
        n += br;
    }
    if (r == FR_OK) { f_close(&f); }
    char x[96];
    snprintf(x, sizeof x, "\"path\":\"%s\",\"size\":%llu,\"hash\":\"%016llx\"", path, (unsigned long long)n, (unsigned long long)h);
    line("fs read", fr(r), x);
}

static void fs_list(const char *path) {
    DIR d;
    FILINFO fi;
    uint64_t h = 1469598103934665603ull;
    FRESULT r = f_opendir(&d, path);
    while (r == FR_OK && (r = f_readdir(&d, &fi)) == FR_OK && fi.fname[0]) {
        char e[300];
        int k = snprintf(e, sizeof e, "%s:%c:%llu;", fi.fname, (fi.fattrib & AM_DIR) ? 'd' : 'f', (unsigned long long)fi.fsize);
        for (int i = 0; i < k; i++) { h ^= (uint8_t)e[i]; h *= 1099511628211ull; }
    }
    if (r == FR_OK) { f_closedir(&d); }
    char x[96];
    snprintf(x, sizeof x, "\"path\":\"%s\",\"hash\":\"%016llx\"", path, (unsigned long long)h);
    line("fs list", fr(r), x);
}

static void scenario(void) {
    init_step("init");
    MKFS_PARM opt = { .fmt = FM_FAT32, .n_fat = 2, .align = 8192, .n_root = 0, .au_size = 4096 };
    line("mkfs fat32", fr(f_mkfs("", &opt, chk, sizeof chk)), NULL);
    line("mount", fr(f_mount(&fs, "", 1)), NULL);
    line("mkdir", fr(f_mkdir("logs")), NULL);
    fs_write("hello.txt", 13, 1);
    fs_write("logs/a.bin", 200 * 1024, 2);
    fs_write("logs/b.bin", 4096 * 3 + 7, 3);
    fs_read("hello.txt");
    fs_read("logs/a.bin");
    fs_list("logs");
    line("rename", fr(f_rename("logs/b.bin", "logs/c.bin")), NULL);
    line("unlink", fr(f_unlink("hello.txt")), NULL);
    fs_list("");
    fs_list("logs");
    fs_read("logs/c.bin");
    DWORD nfree = 0;
    FATFS *pfs;
    FRESULT r = f_getfree("", &nfree, &pfs);
    char x[48];
    snprintf(x, sizeof x, "\"free_clusters\":%lu", (unsigned long)nfree);
    line("getfree", fr(r), x);
    line("unmount", fr(f_mount(NULL, "", 0)), NULL);
}
#endif

int main(void) {
    stdio_init_all();
    sleep_ms(50);
    printf("{\"start\":\"%s\",\"scenario\":\"%s\"}\n", be_name(), SCENARIO);
    scenario();
    printf("{\"done\":%d}\n", step);
    for (;;) { tight_loop_contents(); }
}
