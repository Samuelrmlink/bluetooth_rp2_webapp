/* Stream a card file into pex_install. The firmware build uses the real SD
 * job slot. The host unit test defines PEX_SD_READ_TEST and supplies sdj_post,
 * sdj_poll, sdj_finish, sdj_buf, and sdw_service, because sd_jobs.h pulls in
 * the firmware autoconf. */
#include "pex_sd_read.h"

#include <string.h>

#if !defined(PEX_SD_READ_TEST)
#include "sd/sd_jobs.h"
#include "sd/sd_worker.h"
#endif

static int job_wait(sdj_t *j, void *copy, uint32_t copy_cap, uint32_t *nout) {
    uint32_t token = 0;
    if (!sdj_post(j, &token)) { return -1; }
    for (;;) {
        enum sdj_state st = sdj_poll(token, j);
        if (st == SDJ_POSTED || st == SDJ_FILLING) {
            sdw_service();
            continue;
        }
        if (st != SDJ_DONE || j->rc < 0) {
            if (st == SDJ_DONE) { sdj_finish(token); }
            return -1;
        }
        if (nout) {
            uint32_t n = (uint32_t)j->rc;
            if (n > copy_cap) { n = copy_cap; }
            if (n && copy) { memcpy(copy, sdj_buf(), n); }
            *nout = n;
        }
        sdj_finish(token);
        return 0;
    }
}

int pex_sd_pread(void *ctx, uint32_t off, void *dst, uint32_t n) {
    pex_sd_cursor_t *c = ctx;
    if (!c || !dst) { return -1; }
    if (!n) { return 0; }
    if (c->size && (off > c->size || n > c->size - off)) { return -1; }
    uint8_t *out = dst;
    uint32_t got = 0;
    while (got < n) {
        uint32_t at = off + got;
        if (!c->pos_valid || c->pos != at) {
            sdj_t j;
            memset(&j, 0, sizeof j);
            j.op = SDJ_SEEK;
            j.file = c->file;
            j.pos = at;
            if (job_wait(&j, NULL, 0, NULL) != 0) { return -1; }
            c->pos = at;
            c->pos_valid = 1;
        }
        uint32_t chunk = n - got;
        if (chunk > SDJ_BUF) { chunk = SDJ_BUF; }
        sdj_t j;
        memset(&j, 0, sizeof j);
        j.op = SDJ_READ;
        j.file = c->file;
        j.len = chunk;
        uint32_t nr = 0;
        if (job_wait(&j, out + got, chunk, &nr) != 0) { return -1; }
        if (nr == 0) { return -1; }
        c->pos = at + nr;
        got += nr;
    }
    return (int)got;
}
