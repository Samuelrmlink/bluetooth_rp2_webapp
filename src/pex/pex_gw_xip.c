/*
 * The gateway's PEX flash partition (docs/pex-format.md 16): the runtime's
 * XIP (pex/runtime/pex_xip.c) over FS_PEX_OFF..+FS_PEX_SIZE, the install ops,
 * and loading from the SD card.
 *
 * Where the partition is seen (RP2350): booted from an A/B slot, the ROM's
 * address translation maps the running slot at 0x10000000 through QMI
 * ATRANS0 and leaves the other windows of the first 4 MB empty -- a plain
 * read of 0x10000000 + FS_PEX_OFF faults (the hole fs.h describes). So:
 *   - reads (the index, verification) use the untranslated alias, as
 *     LittleFS does (FS_READ_BASE + offset);
 *   - code runs through ATRANS1: the window at 0x10400000 is pointed at the
 *     partition (256 KB at storage 0x3BC000), cached like the firmware's own
 *     code. The ROM uses ATRANS1 only for a slot larger than 4 MB, and
 *     nothing in the firmware or the SDK's flash paths touches ATRANS; the
 *     window is set again after every flash operation regardless.
 * Writes go through the same path as LittleFS and OTA: flash_range_* inside
 * flash_safe_execute (core 1 parked), whole 4 KB sectors, 256-byte pages; the
 * SDK flushes the XIP cache after each. The watchdog is fed per operation (a
 * compaction of the whole partition is several seconds of flash writes).
 */
#include "pex/pex_gw.h"

#include <stdio.h>
#include <string.h>

#include "autoconf.h"
#include "fs/fs.h"
#include "lua/lfile_path.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "hardware/watchdog.h"
#include "pico/flash.h"
#include "pico/stdlib.h"
#if !PICO_RP2040
#include "hardware/structs/qmi.h"
#endif
#if CONFIG_SD
#include "sd/sd_api.h"
#include "sd/sd_jobs.h"
#endif

#if CONFIG_PEX_XIP

#if PICO_RP2040
#define PEX_EXEC_BASE (XIP_BASE + FS_PEX_OFF)
#else
#define PEX_EXEC_BASE (XIP_BASE + 0x400000u)            /* ATRANS1's window */
#endif

void pex_gw_xip_window(void) {
#if !PICO_RP2040
    qmi_hw->atrans[1] = ((FS_PEX_SIZE / 4096u) << QMI_ATRANS1_SIZE_LSB) | (FS_PEX_OFF >> 12);
#endif
}

typedef struct { bool erase; uint32_t off; const uint8_t *src; uint32_t n; } xjob_t;

static void __not_in_flash_func(xjob_run)(void *p) {
    xjob_t *j = p;
    if (j->erase) {
        flash_range_erase(j->off, 4096);
    } else {
        flash_range_program(j->off, j->src, j->n);
    }
}

static uint32_t s_ops, s_slow;
static int xjob(xjob_t *j) {
    if (j->off < FS_PEX_OFF || j->off + (j->erase ? 4096u : j->n) > FS_PEX_OFF + FS_PEX_SIZE) { return -1; }
    watchdog_update();
    uint64_t t0 = time_us_64();
    int rc = flash_safe_execute(xjob_run, j, 2000);
    if (rc != PICO_OK) {                 /* core 1 never armed (diag/test builds), as fs.c does */
        uint32_t ints = save_and_disable_interrupts();
        xjob_run(j);
        restore_interrupts(ints);
    }
    pex_gw_xip_window();
    uint32_t us = (uint32_t)(time_us_64() - t0);
    s_ops++;
    if (us > 200000u && s_slow++ < 4) {
        printf("pex: xip: flash %s @%06lx took %lu us\n", j->erase ? "erase" : "program", (unsigned long)j->off,
               (unsigned long)us);
    }
    return 0;
}

static int fl_erase(void *ctx, uint32_t off) {
    (void)ctx;
    xjob_t j = { true, FS_PEX_OFF + off, NULL, 4096 };
    return xjob(&j);
}

static int fl_prog(void *ctx, uint32_t off, const void *src, uint32_t n) {
    (void)ctx;
    xjob_t j = { false, FS_PEX_OFF + off, src, n };
    return xjob(&j);
}

static const pex_flash_t s_flash = {
    .size = FS_PEX_SIZE,
    .exec_base = PEX_EXEC_BASE,
    .read_base = FS_READ_BASE + FS_PEX_OFF,
    .erase = fl_erase,
    .prog = fl_prog,
};

const pex_flash_t *pex_gw_flash(void) {
    /* Until the LittleFS migration has run, the partition is still INSIDE the
     * old LittleFS (fs_legacy): writing it would destroy files. No XIP then
     * -- modules load into RAM as before. */
    if (fs_legacy() || !fs_mounted_size()) {
        printf("pex: xip: off -- the PEX partition is %s\n", fs_legacy() ? "inside the old LittleFS until it migrates"
                                                                         : "not known (no filesystem mounted)");
        return NULL;
    }
    pex_gw_xip_window();
    return &s_flash;
}

uint32_t pex_gw_xip_flash_ops(void) { return s_ops; }

#else
const pex_flash_t *pex_gw_flash(void) { return NULL; }
void pex_gw_xip_window(void) {}
uint32_t pex_gw_xip_flash_ops(void) { return 0; }
#endif

/* ---- install from a path ------------------------------------------------------------ */
/* lfs:/...  (or a bare name: lfs:/pex/<name>.<arch>.pex). sd:/ goes through
 * pex_gw_sd_begin (asynchronous). */
int pex_gw_install_path(const char *name_or_path) {
    char path[128];
    if (!pex_gw_ensure()) { return PEX_ENOMEM; }
    if (!strchr(name_or_path, '/') && !strchr(name_or_path, ':')) {
        snprintf(path, sizeof path, "pex/%s.%s.pex", name_or_path, pex_gw_arch());
    } else {
        char rel[LFILE_PATH_MAX + 1];
        int vol = lfile_parse_path(name_or_path, strlen(name_or_path), rel, sizeof rel);
        if (vol == LFILE_SD) {
            pex_seterr(PEX_EPENDING, "sd:/ is read asynchronously: use the pex.install op");
            return PEX_ENOTSUP;
        }
        if (strncmp(name_or_path, "lfs:", 4) == 0 || name_or_path[0] == '/') {
            if (!vol) { pex_seterr(PEX_EINVAL, "bad path"); return PEX_EINVAL; }
            snprintf(path, sizeof path, "%s", rel);
        } else {
            snprintf(path, sizeof path, "%s", name_or_path);
        }
    }
    return pex_gw_with_file(path, NULL, NULL, true);
}

/* ---- the SD card ------------------------------------------------------------------- */
/* Load reads the whole file into the pool. Install pushes each completed read
 * (at most SDJ_BUF) into pex_install_write and does not allocate the file.
 * A session with no completed read for 8 seconds is aborted. */
#if CONFIG_SD
extern int pex__errcode;

typedef struct {
    int stage;                      /* 0 idle, 1 open, 2 read */
    bool install;
    bool posted;
    uint32_t token;
    int file;
    uint32_t size, got;
    uint8_t *buf;                   /* load only: pex_alloc_host, freed at the end */
    uint64_t idle_us;               /* last completed read, or begin */
    pex_install_t *sess;
    char path[SDJ_PATH_MAX + 1];
    char as[24];
} sdload_t;
static sdload_t s_sd;

static void sd_done(int rc, const char *name) {
    if (s_sd.sess) {
        pex_install_t *s = s_sd.sess;
        s_sd.sess = NULL;
        pex_install_abort(s);
    }
    char ev[220];
    if (rc >= 0) {
        snprintf(ev, sizeof ev, "{\"op\":\"%s\",\"path\":\"sd:/%s\",\"ok\":true,\"name\":\"%s\"}",
                 s_sd.install ? "install" : "load", s_sd.path, name ? name : "");
    } else {
        char msg[120];
        snprintf(msg, sizeof msg, "%s", pex_errmsg());
        for (char *p = msg; *p; p++) {
            if (*p == '"' || *p == '\\') { *p = '\''; }
        }
        snprintf(ev, sizeof ev, "{\"op\":\"%s\",\"path\":\"sd:/%s\",\"ok\":false,\"error\":\"%s\",\"msg\":\"%s\"}",
                 s_sd.install ? "install" : "load", s_sd.path, pex_strerror(rc), msg);
    }
    printf("pex: sd:/%s: %s %s\n", s_sd.path, s_sd.install ? "install" : "load", rc >= 0 ? "done" : pex_errmsg());
    api_emit("pex.sd_done", ev);
    if (s_sd.file >= 0) { sdj_close_async(s_sd.file); }
    if (s_sd.buf) { pex_free(s_sd.buf); }
    memset(&s_sd, 0, sizeof s_sd);
    s_sd.file = -1;
}

int pex_gw_sd_begin(const char *path, const char *as, bool install) {
    char rel[LFILE_PATH_MAX + 1];
    if (lfile_parse_path(path, strlen(path), rel, sizeof rel) != LFILE_SD || !rel[0]) {
        pex_seterr(PEX_EINVAL, "not an sd:/ file path");
        return PEX_EINVAL;
    }
    if (s_sd.stage) { pex_seterr(PEX_EBUSY, "another sd:/ load is in progress (sd:/%s)", s_sd.path); return PEX_EBUSY; }
    if (strcmp(sd_card_state(), "ok") != 0) { pex_seterr(PEX_EIO, "SD card: %s", sd_card_state()); return PEX_EIO; }
    if (!pex_gw_ensure()) { return PEX_ENOMEM; }
    memset(&s_sd, 0, sizeof s_sd);
    s_sd.file = -1;
    s_sd.stage = 1;
    s_sd.install = install;
    snprintf(s_sd.path, sizeof s_sd.path, "%s", rel);
    snprintf(s_sd.as, sizeof s_sd.as, "%s", as ? as : "");
    return PEX_EPENDING;
}

void pex_gw_sd_poll(void) {
    if (!s_sd.stage) { return; }
    if (s_sd.sess && s_sd.idle_us && time_us_64() - s_sd.idle_us > 8000000ull) {
        if (s_sd.posted) {
            sdj_abandon(s_sd.token);
            s_sd.posted = false;
        }
        pex_seterr(PEX_EIO, "no SD read for 8 seconds");
        sd_done(PEX_EIO, NULL);
        return;
    }
    sdj_t j;
    if (!s_sd.posted) {
        memset(&j, 0, sizeof j);
        if (s_sd.stage == 1) {
            j.op = SDJ_OPEN;
            j.mode = LS_RD;
            snprintf(j.path, sizeof j.path, "%s", s_sd.path);
        } else {
            j.op = SDJ_READ;
            j.file = s_sd.file;
            uint32_t left = s_sd.size - s_sd.got;
            j.len = left < SDJ_BUF ? left : SDJ_BUF;
        }
        if (!sdj_post(&j, &s_sd.token)) { return; }     /* the slot is busy: next pass */
        s_sd.posted = true;
        return;
    }
    enum sdj_state st = sdj_poll(s_sd.token, &j);
    if (st == SDJ_POSTED || st == SDJ_FILLING) { return; }
    s_sd.posted = false;
    if (st != SDJ_DONE || j.rc < 0) {
        if (st == SDJ_DONE) { sdj_finish(s_sd.token); }
        pex_seterr(PEX_EIO, "reading the card failed (%d)", st == SDJ_DONE ? j.rc : -1);
        sd_done(st == SDJ_DONE && s_sd.stage == 1 ? PEX_ENOENT : PEX_EIO, NULL);
        return;
    }
    if (s_sd.stage == 1) {
        s_sd.file = j.rc;
        sdj_finish(s_sd.token);
        if (j.size < PEX_HEADER_SIZE || j.size > 512u * 1024u) {
            pex_seterr(PEX_EFORMAT, "%lu bytes: not a module", (unsigned long)j.size);
            sd_done(PEX_EFORMAT, NULL);
            return;
        }
        s_sd.size = (uint32_t)j.size;
        if (s_sd.install) {
            s_sd.sess = pex_install_begin(0);
            if (!s_sd.sess) {
                int rc = pex__errcode ? pex__errcode : PEX_ENOMEM;
                sd_done(rc, NULL);
                return;
            }
            s_sd.idle_us = time_us_64();
            s_sd.stage = 2;
            return;
        }
        s_sd.buf = pex_alloc_host(s_sd.size);
        if (!s_sd.buf) {
            pex_mem_stats_t ps;
            pex_pool_stats(&ps);
            pex_seterr(PEX_ENOMEM, "the file is %lu bytes; the PEX pool has %lu in one piece", (unsigned long)s_sd.size,
                       (unsigned long)ps.pool_largest);
            sd_done(PEX_ENOMEM, NULL);
            return;
        }
        s_sd.stage = 2;
        return;
    }
    uint32_t n = (uint32_t)j.rc;
    if (n > s_sd.size - s_sd.got) { n = s_sd.size - s_sd.got; }
    if (s_sd.install) {
        /* The job buffer is valid until sdj_finish. PEX_DONE means the build
         * is already installed: do not read the rest of the card. */
        int rc = pex_install_write(s_sd.sess, sdj_buf(), n);
        sdj_finish(s_sd.token);
        s_sd.idle_us = time_us_64();
        s_sd.got += n;
        if (rc != PEX_OK && rc != PEX_DONE) {
            pex_install_t *s = s_sd.sess;
            s_sd.sess = NULL;
            int e = pex_install_end(s);
            sd_done(e < 0 ? e : rc, NULL);
            return;
        }
        if (n == 0 && s_sd.got < s_sd.size && rc != PEX_DONE) {
            pex_seterr(PEX_EIO, "short read at %lu of %lu", (unsigned long)s_sd.got, (unsigned long)s_sd.size);
            sd_done(PEX_EIO, NULL);
            return;
        }
        if (rc == PEX_DONE || s_sd.got >= s_sd.size) {
            pex_install_t *s = s_sd.sess;
            s_sd.sess = NULL;
            int slot = pex_install_end(s);
            char name[24] = "";
            pex_installed_t in;
            if (slot >= 0 && pex_installed(slot, &in) == PEX_OK) { snprintf(name, sizeof name, "%s", in.name); }
            sd_done(slot, name);
        }
        return;
    }
    memcpy(s_sd.buf + s_sd.got, sdj_buf(), n);
    sdj_finish(s_sd.token);
    s_sd.got += n;
    if (n == 0 && s_sd.got < s_sd.size) {
        pex_seterr(PEX_EIO, "short read at %lu of %lu", (unsigned long)s_sd.got, (unsigned long)s_sd.size);
        sd_done(PEX_EIO, NULL);
        return;
    }
    if (s_sd.got < s_sd.size) { return; }
    pex_load_opts_t o = { .name = s_sd.as[0] ? s_sd.as : NULL };
    int rc;
    char name[24] = "";
    pex_mod_t *m = pex_load_mem(s_sd.buf, s_sd.size, &o, &rc);
    if (m) { snprintf(name, sizeof name, "%s", pex_name(m)); rc = 0; }
    sd_done(rc, name);
}

bool pex_gw_sd_busy(void) { return s_sd.stage != 0; }
#else
int pex_gw_sd_begin(const char *path, const char *as, bool install) {
    (void)path; (void)as; (void)install;
    pex_seterr(PEX_ENOTSUP, "this build has no SD card");
    return PEX_ENOTSUP;
}
void pex_gw_sd_poll(void) {}
bool pex_gw_sd_busy(void) { return false; }
#endif
