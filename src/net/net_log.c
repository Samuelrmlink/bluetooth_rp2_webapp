#include "net/net_log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "autoconf.h"
#include "pico/time.h"
#include "lwip/memp.h"
#include "lwip/stats.h"

#if CONFIG_SD
#include "sd/sd_core.h"
#include "sd/sd_jobs.h"
#include "sd/sd_store.h"
#endif

#define RING 1024
#define LOG_CAP 65536u

static char ring[RING];
static uint16_t head, tail;
static uint32_t dropped, drop_noted, drop_note_ms;

static uint32_t now_ms(void) {
    return to_ms_since_boot(get_absolute_time());
}

static uint16_t ring_used(void) {
    return (uint16_t)((head - tail) & (RING - 1));
}

static void ring_push(const char *s, int n) {
    if (n <= 0) return;
    if (n > RING - 1) {
        s += n - (RING - 1);
        n = RING - 1;
    }
    /* Drop the new line rather than the bytes a flush has already copied. */
    if ((uint16_t)(ring_used() + (uint16_t)n) > RING - 1) {
        dropped++;
        return;
    }
    for (int i = 0; i < n; i++) {
        ring[head] = s[i];
        head = (uint16_t)((head + 1) & (RING - 1));
    }
}

static void ring_copy(uint8_t *dst, uint16_t n) {
    uint16_t t = tail;
    for (uint16_t i = 0; i < n; i++) {
        dst[i] = (uint8_t)ring[t];
        t = (uint16_t)((t + 1) & (RING - 1));
    }
}

static void ring_advance(uint16_t n) {
    tail = (uint16_t)((tail + n) & (RING - 1));
}

void net_log(const char *fmt, ...) {
    char line[180];
    int n = snprintf(line, sizeof line, "%lu ", (unsigned long)now_ms());
    if (n < 0) n = 0;
    if (n >= (int)sizeof line) n = (int)sizeof line - 1;
    va_list ap;
    va_start(ap, fmt);
    int m = vsnprintf(line + n, sizeof line - (size_t)n, fmt, ap);
    va_end(ap);
    if (m > 0) n += m;
    if (n >= (int)sizeof line) n = (int)sizeof line - 1;
    line[n] = 0;
    if (n == 0 || line[n - 1] != '\n') {
        if (n < (int)sizeof line - 1) line[n++] = '\n';
        line[n] = 0;
    }
    printf("%s", line);
    ring_push(line, n);
}

static void watch_pool(void) {
#if MEMP_STATS
    const struct stats_mem *m = lwip_stats.memp[MEMP_PBUF_POOL];
    if (!m) return;
    static STAT_COUNTER prev;
    static int have;
    static int dirty;
    static STAT_COUNTER from;
    static uint32_t logged_at;
    if (!have) {
        prev = m->err;
        have = 1;
        return;
    }
    if (m->err != prev) {
        if (!dirty) from = prev;
        prev = m->err;
        dirty = 1;
    }
    uint32_t now = now_ms();
    if (!dirty || (uint32_t)(now - logged_at) < 1000) return;
    net_log("pbuf pool err %u -> %u used %u/%u",
            (unsigned)from, (unsigned)prev, (unsigned)m->used, (unsigned)m->avail);
    dirty = 0;
    logged_at = now;
#else
    (void)0;
#endif
}

#if CONFIG_SD

enum { W_NONE, W_STAT, W_MKDIR, W_OPEN, W_WRITE, W_CLOSE };

static int inflight, want, open_mode, fd = -1;
static uint32_t token, retry_at, fail_at;
static uint16_t snap;

static void sd_fail(int rc) {
    uint32_t now = now_ms();
    if ((uint32_t)(now - fail_at) > 5000) {
        printf("net log: sd %d\n", rc);
        fail_at = now;
    }
    retry_at = now + 1000;
    want = W_NONE;
    fd = -1;
}

static void post_want(void) {
    if (inflight || want == W_NONE) return;
    sdj_t req;
    memset(&req, 0, sizeof req);
    if (want == W_STAT) {
        req.op = SDJ_STAT;
        snprintf(req.path, sizeof req.path, "log/net.log");
    } else if (want == W_MKDIR) {
        req.op = SDJ_MKDIR;
        snprintf(req.path, sizeof req.path, "log");
    } else if (want == W_OPEN) {
        req.op = SDJ_OPEN;
        req.mode = open_mode;
        snprintf(req.path, sizeof req.path, "log/net.log");
    } else if (want == W_WRITE) {
        req.op = SDJ_WRITE;
        req.file = fd;
        req.len = snap;
    } else if (want == W_CLOSE) {
        req.op = SDJ_CLOSE;
        req.file = fd;
    } else {
        want = W_NONE;
        return;
    }
    if (!sdj_post(&req, &token)) return;
    if (want == W_WRITE) ring_copy(sdj_buf(), snap);
    inflight = want;
    want = W_NONE;
}

static void on_done(int done, int rc, const sdj_t *out) {
    if (done == W_STAT) {
        if (rc == LS_ENOENT) {
            open_mode = LS_APPEND;
            want = W_MKDIR;
        } else if (rc < 0) {
            sd_fail(rc);
        } else {
            open_mode = out->size > LOG_CAP ? LS_WR : LS_APPEND;
            want = W_OPEN;
        }
    } else if (done == W_MKDIR) {
        if (rc < 0) sd_fail(rc);
        else {
            open_mode = LS_APPEND;
            want = W_OPEN;
        }
    } else if (done == W_OPEN) {
        if (rc < 0) sd_fail(rc);
        else {
            fd = rc;
            uint16_t u = ring_used();
            if (!u) want = W_CLOSE;
            else {
                snap = u > 512 ? 512 : u;
                if (snap > SDJ_BUF) snap = (uint16_t)SDJ_BUF;
                want = W_WRITE;
            }
        }
    } else if (done == W_WRITE) {
        if (rc > 0) ring_advance((uint16_t)((uint16_t)rc > snap ? snap : rc));
        else printf("net log: write %d\n", rc);
        want = W_CLOSE;
    } else if (done == W_CLOSE) {
        if (rc < 0) sdj_close_async(fd);
        fd = -1;
    }
}

static void sd_step(void) {
    if (inflight) {
        sdj_t out;
        enum sdj_state ps = sdj_poll(token, &out);
        if (ps == SDJ_POSTED || ps == SDJ_FILLING) return;
        if (ps != SDJ_DONE) {
            inflight = W_NONE;
            return;
        }
        int was = inflight;
        sdj_finish(token);
        inflight = W_NONE;
        on_done(was, out.rc, &out);
        return;
    }
    if (want == W_NONE) {
        uint32_t now = now_ms();
        if ((int32_t)(now - retry_at) < 0) return;
        if (!ring_used()) return;
        const struct sd_pub *pub = sd_pub();
        if (!pub || !pub->present) return;
        want = W_STAT;
    }
    post_want();
}

#endif /* CONFIG_SD */

void net_log_poll(void) {
    watch_pool();
    uint32_t now = now_ms();
    if (dropped != drop_noted && (uint32_t)(now - drop_note_ms) >= 5000) {
        net_log("net log dropped %lu", (unsigned long)dropped);
        drop_noted = dropped;
        drop_note_ms = now;
    }
#if CONFIG_SD
    sd_step();
#endif
}
