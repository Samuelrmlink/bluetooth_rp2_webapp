/* gwfs: the fs group. APP_RUN {"sd":bool} runs the LittleFS checks at once
 * (synchronous) and the card checks through callbacks; op pextest.fs reports.
 * Everything it writes is under pextest/ and removed again. */
#include "pex.h"

static char lfs_res[48] = "pending", sd_res[48] = "pending";
static int step, fd, nlist, want_sd;
static char buf[64];
static const char MSG[] = "pex fs test 0123456789";

static int fail(char *res, const char *what, int rc) {
    pex_snprintf(res, 48, "fail:%s:%d", what, rc);
    return rc;
}

static void count(void *arg, const pex_fs_entry_t *e) {
    if (e && !e->is_dir && strcmp(e->name, "b.txt") == 0) { nlist++; }
}

static void lfs_checks(void) {
    int rc;
    pex_fs_remove("lfs:/pextest/b.txt", 0, 0);
    pex_fs_remove("lfs:/pextest/a.txt", 0, 0);
    pex_fs_remove("lfs:/pextest", 0, 0);
    if ((rc = pex_fs_mkdir("lfs:/pextest", 0, 0)) < 0) { fail(lfs_res, "mkdir", rc); return; }
    if ((fd = pex_fs_open("lfs:/pextest/a.txt", PEX_FS_WRITE, 0, 0)) < 0) { fail(lfs_res, "open_w", fd); return; }
    if ((rc = pex_fs_write(fd, MSG, sizeof MSG - 1, 0, 0)) != (int)sizeof MSG - 1) { fail(lfs_res, "write", rc); return; }
    pex_fs_close(fd);
    if ((fd = pex_fs_open("lfs:/pextest/a.txt", PEX_FS_READ, 0, 0)) < 0) { fail(lfs_res, "open_r", fd); return; }
    if (pex_fs_size(fd) != (int)sizeof MSG - 1) { fail(lfs_res, "size", pex_fs_size(fd)); return; }
    pex_fs_seek(fd, 4, 0, 0);
    rc = pex_fs_read(fd, buf, sizeof buf, 0, 0);
    pex_fs_close(fd);
    if (rc != (int)sizeof MSG - 5 || memcmp(buf, MSG + 4, (size_t)rc) != 0) { fail(lfs_res, "read", rc); return; }
    pex_fs_entry_t st;
    if ((rc = pex_fs_stat("lfs:/pextest/a.txt", &st, 0, 0)) < 0 || st.size != sizeof MSG - 1 || st.is_dir) {
        fail(lfs_res, "stat", rc); return;
    }
    if ((rc = pex_fs_rename("lfs:/pextest/a.txt", "lfs:/pextest/b.txt", 0, 0)) < 0) { fail(lfs_res, "rename", rc); return; }
    nlist = 0;
    if ((rc = pex_fs_list("lfs:/pextest", count, 0)) < 1 || nlist != 1) { fail(lfs_res, "list", rc); return; }
    if (pex_fs_open("lfs:/pextest/a.txt", PEX_FS_READ, 0, 0) != PEX_ENOENT) { fail(lfs_res, "enoent", 0); return; }
    if ((rc = pex_fs_remove("lfs:/pextest/b.txt", 0, 0)) < 0) { fail(lfs_res, "remove", rc); return; }
    if ((rc = pex_fs_remove("lfs:/pextest", 0, 0)) < 0) { fail(lfs_res, "rmdir", rc); return; }
    pex_fs_media_t md;
    pex_fs_media(&md);
    if (!md.lfs_ok || !md.lfs_total) { fail(lfs_res, "media", 0); return; }
    pex_snprintf(lfs_res, sizeof lfs_res, "ok");
}

/* the card, one step per callback */
static void sd_next(void *arg, int rc);
static void sd_go(int rc) {
    if (rc == PEX_EPENDING) { return; }
    sd_next(0, rc);                /* completed at once (or failed) */
}
static void sd_next(void *arg, int rc) {
    if (rc < 0 && step != 9) { fail(sd_res, "step", rc * 100 - step); pex_app_exit(1); return; }
    switch (step++) {
    case 0: sd_go(pex_fs_open("sd:/pextest.txt", PEX_FS_WRITE, sd_next, 0)); break;
    case 1: fd = rc; sd_go(pex_fs_write(fd, MSG, sizeof MSG - 1, sd_next, 0)); break;
    case 2: pex_fs_close(fd); sd_go(pex_fs_open("sd:/pextest.txt", PEX_FS_READ, sd_next, 0)); break;
    case 3: fd = rc; memset(buf, 0, sizeof buf); sd_go(pex_fs_read(fd, buf, sizeof buf, sd_next, 0)); break;
    case 4:
        pex_fs_close(fd);
        if (rc != (int)sizeof MSG - 1 || memcmp(buf, MSG, (size_t)rc) != 0) { fail(sd_res, "readback", rc); pex_app_exit(1); return; }
        { static pex_fs_entry_t st; sd_go(pex_fs_stat("sd:/pextest.txt", &st, sd_next, 0)); }
        break;
    case 5: sd_go(pex_fs_remove("sd:/pextest.txt", sd_next, 0)); break;
    case 6: pex_snprintf(sd_res, sizeof sd_res, "ok"); pex_app_exit(0); break;
    }
}

static int op_fs(void *arg, const char *params, char *reply, size_t cap) {
    return pex_snprintf(reply, cap, "{\"lfs\":\"%s\",\"sd\":\"%s\"}", lfs_res, want_sd ? sd_res : "skipped");
}

int pex_main(const pex_host_t *host, uint32_t reason, void *arg) {
    if (reason == PEX_INIT) { return pex_bus_op("pextest.fs", op_fs, 0); }
    if (reason == PEX_APP_RUN) {
        int sd = 0;
        pex_json_bool(arg, "sd", &sd);
        want_sd = sd;
        lfs_checks();
        if (sd) {
            pex_fs_media_t md;
            pex_fs_media(&md);
            if (strcmp(md.sd_state, "ok") != 0) {
                pex_snprintf(sd_res, sizeof sd_res, "no card (%s)", md.sd_state);
                return PEX_OK;
            }
            step = 0;
            sd_next(0, 0);
        }
    }
    return PEX_OK;
}
