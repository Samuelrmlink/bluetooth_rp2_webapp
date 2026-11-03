/*
 * Arm / idle / abort for a paced PIO TX feed.
 * The port owns the DMA registers. A refusal prints one line here.
 * Success prints nothing. The PEX wrapper does not print another.
 */
#include "io/pio_dma.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "io/pio_port.h"
#include "io/pio_res.h"

#define SLOTS   4
#define OWNER_N 24
#define MAX_N   65535u

typedef struct {
    uint8_t used;
    uint8_t running;
    uint8_t pending;
    int handle;
    int ch;
    int block;
    int sm;
    int pc;
    const uint32_t *armed;
    uint32_t armed_n;
    uint8_t armed_inc;
    char owner[OWNER_N];
    pio_dma_fn fn;
    void *arg;
    void *mod;
} slot_t;

static slot_t s_slot[SLOTS];
static uint32_t s_zero;
static char s_err[160];

static void clear_err(void) { s_err[0] = 0; }

const char *pio_dma_last_error(void) { return s_err; }

static int fail(int code, const char *owner, const char *fmt, ...) {
    char body[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(body, sizeof body, fmt, ap);
    va_end(ap);
    snprintf(s_err, sizeof s_err, "pio %s: %s", (owner && owner[0]) ? owner : "?", body);
    printf("%s\n", s_err);
    return code;
}

static void copy_owner(char *dst, const char *src) {
    snprintf(dst, OWNER_N, "%s", (src && src[0]) ? src : "?");
}

static slot_t *find_handle(int handle) {
    for (int i = 0; i < SLOTS; i++) {
        if (s_slot[i].used && s_slot[i].handle == handle) { return &s_slot[i]; }
    }
    return NULL;
}

static slot_t *find_ch(int ch) {
    if (ch < 0) { return NULL; }
    for (int i = 0; i < SLOTS; i++) {
        if (s_slot[i].used && s_slot[i].ch == ch) { return &s_slot[i]; }
    }
    return NULL;
}

static int length_ok(const char *owner, uint32_t nwords) {
    if (nwords < 1 || nwords > MAX_N) {
        return fail(PIO_EINVAL, owner, "dma length %u is not 1-65535", nwords);
    }
    return PIO_OK;
}

static int range_ok(const char *owner, const uint32_t *src, uint32_t nwords,
                    uintptr_t rw, uintptr_t rw_end) {
    if (!src) { return fail(PIO_EINVAL, owner, "dma source is missing"); }
    uintptr_t p = (uintptr_t)src;
    if ((p & 3u) != 0) {
        return fail(PIO_EINVAL, owner, "dma source is outside module memory");
    }
    uint64_t bytes = (uint64_t)nwords * 4u;
    if (rw_end < rw || p < rw || (uint64_t)(p - rw) + bytes > (uint64_t)(rw_end - rw)) {
        return fail(PIO_EINVAL, owner, "dma source is outside module memory");
    }
    return PIO_OK;
}

/* Owner and protected checks already printed when they fail.
 * *created is 1 when this call took a free slot. */
static int open_slot(int handle, const char *owner, slot_t **out, int *created) {
    clear_err();
    *created = 0;
    int rc = pio_res_check(handle, owner);
    if (rc < 0) { return rc; }
    if (pio_res_protected(handle) == 1) {
        int block = pio_res_block(handle);
        int sm = pio_res_sm(handle);
        return fail(PIO_EBUSY, owner, "cannot feed protected machine on pio%d sm%d", block, sm);
    }
    slot_t *s = find_handle(handle);
    if (!s) {
        for (int i = 0; i < SLOTS; i++) {
            if (!s_slot[i].used) { s = &s_slot[i]; break; }
        }
        if (!s) {
            return fail(PIO_ENOMEM, owner, "dma limited to %d channels", SLOTS);
        }
        memset(s, 0, sizeof *s);
        s->used = 1;
        s->handle = handle;
        s->ch = -1;
        *created = 1;
    }
    s->block = pio_res_block(handle);
    s->sm = pio_res_sm(handle);
    s->pc = pio_res_offset(handle);
    copy_owner(s->owner, owner);
    *out = s;
    return PIO_OK;
}

static void drop_new(slot_t *s, int created) {
    if (created && s && s->ch < 0 && !s->fn) {
        memset(s, 0, sizeof *s);
        s->ch = -1;
    }
}

static int ensure_ch(slot_t *s) {
    if (s->ch >= 0) { return 0; }
    int ch = pio_port_dma_claim();
    if (ch < 0) { return -1; }
    s->ch = ch;
    return 0;
}

/* Caller holds the port lock. Does not print. */
static int kick(slot_t *s, const uint32_t *src, uint32_t nwords, int inc) {
    if (pio_port_dma_start(s->ch, s->block, s->sm, src, nwords, inc) != 0) { return -1; }
    s->running = 1;
    s->armed = NULL;
    s->armed_n = 0;
    return 0;
}

int pio_dma_tx(int handle, const char *owner, const uint32_t *src, uint32_t nwords,
               uintptr_t rw, uintptr_t rw_end) {
    int nrc = length_ok(owner, nwords);
    if (nrc < 0) { return nrc; }
    int rrc = range_ok(owner, src, nwords, rw, rw_end);
    if (rrc < 0) { return rrc; }
    slot_t *s = NULL;
    int created = 0;
    int rc = open_slot(handle, owner, &s, &created);
    if (rc < 0) { return rc; }
    uint32_t key = pio_port_lock();
    if (s->running || s->armed) {
        int block = s->block;
        pio_port_unlock(key);
        drop_new(s, created);
        return fail(PIO_EBUSY, owner, "dma busy on pio%d", block);
    }
    if (ensure_ch(s) < 0) {
        pio_port_unlock(key);
        drop_new(s, created);
        return fail(PIO_ENOMEM, owner, "no free dma channel");
    }
    if (kick(s, src, nwords, 1) < 0) {
        int block = s->block;
        int sm = s->sm;
        pio_port_unlock(key);
        drop_new(s, created);
        return fail(PIO_ENOMEM, owner, "dma did not start on pio%d sm%d", block, sm);
    }
    pio_port_unlock(key);
    return PIO_OK;
}

int pio_dma_arm(int handle, const char *owner, const uint32_t *src, uint32_t nwords,
                uintptr_t rw, uintptr_t rw_end) {
    int nrc = length_ok(owner, nwords);
    if (nrc < 0) { return nrc; }
    int rrc = range_ok(owner, src, nwords, rw, rw_end);
    if (rrc < 0) { return rrc; }
    slot_t *s = NULL;
    int created = 0;
    int rc = open_slot(handle, owner, &s, &created);
    if (rc < 0) { return rc; }
    uint32_t key = pio_port_lock();
    if (s->armed) {
        int block = s->block;
        pio_port_unlock(key);
        drop_new(s, created);
        return fail(PIO_EAGAIN, owner, "dma already armed on pio%d", block);
    }
    if (!s->running) {
        if (ensure_ch(s) < 0) {
            pio_port_unlock(key);
            drop_new(s, created);
            return fail(PIO_ENOMEM, owner, "no free dma channel");
        }
        if (kick(s, src, nwords, 1) < 0) {
            int block = s->block;
            int sm = s->sm;
            pio_port_unlock(key);
            drop_new(s, created);
            return fail(PIO_ENOMEM, owner, "dma did not start on pio%d sm%d", block, sm);
        }
        pio_port_unlock(key);
        return PIO_OK;
    }
    s->armed = src;
    s->armed_n = nwords;
    s->armed_inc = 1;
    pio_port_unlock(key);
    return PIO_OK;
}

int pio_dma_idle(int handle, const char *owner) {
    clear_err();
    int rc = pio_res_check(handle, owner);
    if (rc < 0) { return rc; }
    uint32_t key = pio_port_lock();
    slot_t *s = find_handle(handle);
    int idle = !s || (!s->running && !s->armed);
    pio_port_unlock(key);
    return idle ? 1 : 0;
}

int pio_dma_zero(int handle, const char *owner, uint32_t nwords) {
    int nrc = length_ok(owner, nwords);
    if (nrc < 0) { return nrc; }
    slot_t *s = NULL;
    int created = 0;
    int rc = open_slot(handle, owner, &s, &created);
    if (rc < 0) { return rc; }
    uint32_t key = pio_port_lock();
    if (s->ch >= 0) {
        pio_port_dma_abort(s->ch);
        pio_port_dma_ack(s->ch);
    }
    s->armed = NULL;
    s->armed_n = 0;
    s->running = 0;
    s->pending = 0;
    int block = s->block;
    int sm = s->sm;
    int pc = s->pc;
    if (ensure_ch(s) < 0) {
        pio_port_unlock(key);
        drop_new(s, created);
        return fail(PIO_ENOMEM, owner, "no free dma channel");
    }
    pio_port_tx_resync(block, sm, pc);
    s_zero = 0;
    if (kick(s, &s_zero, nwords, 0) < 0) {
        pio_port_unlock(key);
        drop_new(s, created);
        return fail(PIO_ENOMEM, owner, "dma did not start on pio%d sm%d", block, sm);
    }
    pio_port_unlock(key);
    return PIO_OK;
}

int pio_dma_listen(int handle, const char *owner, pio_dma_fn fn, void *arg, void *mod) {
    slot_t *s = NULL;
    int created = 0;
    int rc = open_slot(handle, owner, &s, &created);
    if (rc < 0) { return rc; }
    uint32_t key = pio_port_lock();
    s->fn = fn;
    s->arg = arg;
    s->mod = mod;
    pio_port_unlock(key);
    return PIO_OK;
}

static void unclaim(slot_t *s) {
    if (s->ch >= 0) {
        pio_port_dma_abort(s->ch);
        pio_port_dma_ack(s->ch);
        pio_port_dma_unclaim(s->ch);
    }
    memset(s, 0, sizeof *s);
    s->ch = -1;
}

void pio_dma_abort(int handle) {
    uint32_t key = pio_port_lock();
    slot_t *s = find_handle(handle);
    if (!s) { pio_port_unlock(key); return; }
    unclaim(s);
    pio_port_unlock(key);
}

void pio_dma_abort_owner(const char *owner) {
    if (!owner || !owner[0]) { return; }
    uint32_t key = pio_port_lock();
    for (int i = 0; i < SLOTS; i++) {
        if (s_slot[i].used && strcmp(s_slot[i].owner, owner) == 0) { unclaim(&s_slot[i]); }
    }
    pio_port_unlock(key);
}

void pio_dma_hw_done(int ch) {
    uint32_t key = pio_port_lock();
    slot_t *s = find_ch(ch);
    if (!s || !s->running) { pio_port_unlock(key); return; }
    s->running = 0;
    if (s->armed && s->armed_n) {
        const uint32_t *src = s->armed;
        uint32_t n = s->armed_n;
        int inc = s->armed_inc;
        s->armed = NULL;
        s->armed_n = 0;
        if (pio_port_dma_start(s->ch, s->block, s->sm, src, n, inc) == 0) {
            s->running = 1;
        }
    }
    if (s->pending < 255) { s->pending++; }
    pio_port_unlock(key);
}

void pio_dma_poll(void (*deliver)(void *mod, pio_dma_fn fn, void *arg)) {
    for (int i = 0; i < SLOTS; i++) {
        uint32_t key = pio_port_lock();
        uint8_t n = s_slot[i].pending;
        pio_dma_fn fn = s_slot[i].fn;
        void *arg = s_slot[i].arg;
        void *mod = s_slot[i].mod;
        s_slot[i].pending = 0;
        pio_port_unlock(key);
        if (!n || !fn || !deliver) { continue; }
        for (uint8_t c = 0; c < n; c++) { deliver(mod, fn, arg); }
    }
}
