/*
 * The PIO ledger. pio_port_* is the hardware. Nothing here includes the SDK.
 *
 * A claim reserves its instruction words immediately, so a second caller
 * cannot see the same gap as free. The words themselves are copied into
 * the shadow by pio_res_program. Release clears that shadow only when the
 * last machine on the range is gone. The last machine on a block also
 * clears the GPIO base, so the block can take either window later.
 */
#include "io/pio_res.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "io/pio_port.h"

#define BLOCKS  3
#define SMS     4
#define WORDS   32
#define HANDLES (BLOCKS * SMS)
#define PROGS   32
#define OWNER_N 24

typedef struct {
    uint8_t used;
    uint8_t block;
    uint8_t sm;
    uint8_t join;
    int prog;
    char owner[OWNER_N];
    uint32_t pins;          /* bit (gpio - gpio_base) of each claimed pin */
    uint32_t pin_took;      /* bits this handle claimed in the pin broker */
} slot_t;

typedef struct {
    uint8_t used;
    uint8_t block;
    uint8_t loaded;
    uint8_t sm_mask;
    uint16_t offset;
    uint16_t length;
    int16_t origin;
    uint8_t irq_here;       /* flags 0-3 on this block */
    uint8_t irq_next;       /* flags 0-3 on the next block (absolute IRQ 4-7) */
    uint8_t prot;           /* refuse rewrite, release, and block changes */
    char owner[OWNER_N];
} prog_t;

typedef struct {
    uint8_t used;
    uint8_t sm;
    int handle;
    char owner[OWNER_N];
    pio_res_irq_fn fn;
    void *arg;
    void *mod;
    uint32_t sources;
    uint32_t instr_flags;
} line_t;

typedef struct {
    int gpio_base;
    uint32_t map;
    uint16_t shadow[WORDS];
    uint8_t sm_used;
    volatile uint8_t irq_pending;
    line_t line[2];
} block_t;

typedef struct {
    int block;
    int base;
    int sm;
    int prog;               /* >= 0: attach. -1: a new range at offset */
    int offset;
} choice_t;

static block_t s_blk[BLOCKS] = {
    {.gpio_base = -1}, {.gpio_base = -1}, {.gpio_base = -1},
};
static slot_t s_slot[HANDLES];
static prog_t s_prog[PROGS];
static char s_err[192];

static void clear_err(void) { s_err[0] = 0; }

const char *pio_res_last_error(void) { return s_err; }

static int fail(int code, const char *owner, const char *fmt, ...) {
    char body[160];
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

static int nblocks(void) {
    int n = pio_port_nblocks();
    if (n < 1) { return 1; }
    if (n > BLOCKS) { return BLOCKS; }
    return n;
}

static uint32_t range_mask(int off, int n) {
    if (n <= 0) { return 0; }
    if (n >= WORDS) { return 0xffffffffu; }
    return ((1u << n) - 1u) << off;
}

static int find_run(uint32_t map, int n) {
    if (n <= 0 || n > WORDS) { return -1; }
    if (n == WORDS) { return map == 0 ? 0 : -1; }
    uint32_t need = (1u << n) - 1u;
    for (int off = 0; off <= WORDS - n; off++) {
        if ((map & (need << off)) == 0) { return off; }
    }
    return -1;
}

static unsigned popcount(uint32_t m) {
    unsigned n = 0;
    while (m) { n += m & 1u; m >>= 1u; }
    return n;
}

static slot_t *slot_of(int handle) {
    if (handle < 1 || handle > HANDLES) { return NULL; }
    slot_t *s = &s_slot[handle - 1];
    return s->used ? s : NULL;
}

static unsigned lowest_sm(const prog_t *p) {
    for (unsigned sm = 0; sm < SMS; sm++) {
        if (p->sm_mask & (1u << sm)) { return sm; }
    }
    return 0;
}

static int prog_covering(int block, int offset, int nwords) {
    uint32_t need = range_mask(offset, nwords);
    for (int i = 0; i < PROGS; i++) {
        if (!s_prog[i].used || s_prog[i].block != (uint8_t)block) { continue; }
        if (s_prog[i].length == 0) { continue; }
        if (range_mask(s_prog[i].offset, s_prog[i].length) & need) { return i; }
    }
    return -1;
}

static prog_t *protected_on(int block) {
    for (int i = 0; i < PROGS; i++) {
        if (s_prog[i].used && s_prog[i].prot && s_prog[i].block == (uint8_t)block) {
            return &s_prog[i];
        }
    }
    return NULL;
}

static int slot_prot(const slot_t *s) {
    return s && s->prog >= 0 && s->prog < PROGS && s_prog[s->prog].used && s_prog[s->prog].prot;
}

static int refuse_prot(const slot_t *s, const char *verb) {
    prog_t *p = &s_prog[s->prog];
    return fail(PIO_EBUSY, s->owner,
                "cannot %s protected %s on pio%d sm%d (offset %u length %u); that would stop it",
                verb, p->owner, s->block, s->sm, p->offset, (unsigned)p->length);
}

static int find_exact(int block, int offset, int length) {
    for (int i = 0; i < PROGS; i++) {
        if (s_prog[i].used && s_prog[i].block == (uint8_t)block &&
            s_prog[i].offset == (uint16_t)offset && s_prog[i].length == (uint16_t)length) {
            return i;
        }
    }
    return -1;
}

static int free_prog(void) {
    for (int i = 0; i < PROGS; i++) { if (!s_prog[i].used) { return i; } }
    return -1;
}

static int free_sm(int block) {
    uint8_t used = s_blk[block].sm_used;
    for (int sm = 0; sm < SMS; sm++) {
        if ((used & (1u << sm)) == 0) { return sm; }
    }
    return -1;
}

/* IRQ opcode: bits 15:13 are 0b110. Bit 4 is relative. Bits 2:0 are the index.
 * Relative resolves with the machine that executes the word. Absolute 4-7 is
 * the next block's flags 0-3. WAIT GPIO is a different opcode and is not scanned. */
static void scan_word(uint16_t in, int sm, uint8_t *here, uint8_t *next) {
    if ((in & 0xE000u) != 0xC000u) { return; }
    int rel = (in & 0x10u) != 0;
    int idx = in & 7;
    if (rel) { *here |= (uint8_t)(1u << ((sm + idx) & 3)); return; }
    if (idx < 4) { *here |= (uint8_t)(1u << idx); return; }
    *next |= (uint8_t)(1u << (idx - 4));
}

static void scan_words(const uint16_t *words, int n, int sm, uint8_t *here, uint8_t *next) {
    *here = 0;
    *next = 0;
    for (int i = 0; i < n; i++) { scan_word(words[i], sm, here, next); }
}

/* A protected program's IRQ flag is part of its machine. Setting or clearing
 * that flag from a new program on this block, or from the neighbouring block
 * via an absolute IRQ 4-7, changes what the protected machine waits on. */
static int prot_irq(int block, int self, uint8_t here, uint8_t next, const char *owner) {
    int nb = nblocks();
    int nextb = (block + 1) % nb;
    int prev = (block + nb - 1) % nb;
    for (int i = 0; i < PROGS; i++) {
        if (!s_prog[i].used || !s_prog[i].prot || i == self) { continue; }
        if (s_prog[i].block == (uint8_t)block && (s_prog[i].irq_here & here)) {
            return fail(PIO_EBUSY, owner,
                        "irq flag on pio%d is used by protected %s on sm%u; a new program must not clear it",
                        block, s_prog[i].owner, lowest_sm(&s_prog[i]));
        }
        if (s_prog[i].block == (uint8_t)nextb && (s_prog[i].irq_here & next)) {
            return fail(PIO_EBUSY, owner,
                        "irq flag on pio%d is used by protected %s on sm%u; a new program must not clear it",
                        nextb, s_prog[i].owner, lowest_sm(&s_prog[i]));
        }
        if (s_prog[i].block == (uint8_t)prev && (s_prog[i].irq_next & here)) {
            return fail(PIO_EBUSY, owner,
                        "irq flag on pio%d is used by protected %s on pio%d sm%u; a new program must not clear it",
                        block, s_prog[i].owner, prev, lowest_sm(&s_prog[i]));
        }
    }
    return 0;
}

static void rescan(prog_t *p) {
    p->irq_here = 0;
    p->irq_next = 0;
    if (!p->loaded) { return; }
    const uint16_t *w = s_blk[p->block].shadow + p->offset;
    for (int sm = 0; sm < SMS; sm++) {
        if ((p->sm_mask & (1u << sm)) == 0) { continue; }
        for (int i = 0; i < p->length; i++) { scan_word(w[i], sm, &p->irq_here, &p->irq_next); }
    }
}

static uint32_t irq_used_of(int block) {
    int nb = nblocks();
    int prev = (block + nb - 1) % nb;
    uint32_t here = 0, from_prev = 0;
    for (int i = 0; i < PROGS; i++) {
        if (!s_prog[i].used) { continue; }
        if (s_prog[i].block == (uint8_t)block) { here |= s_prog[i].irq_here; }
        if (s_prog[i].block == (uint8_t)prev) { from_prev |= s_prog[i].irq_next; }
    }
    return here | from_prev;
}

static int pin_still_used(int pin) {
    for (int i = 0; i < HANDLES; i++) {
        if (!s_slot[i].used) { continue; }
        int base = s_blk[s_slot[i].block].gpio_base;
        int rel = pin - base;
        if (rel >= 0 && rel < WORDS && (s_slot[i].pins & (1u << rel))) { return 1; }
    }
    return 0;
}

static void release_slot(int idx) {
    slot_t *s = &s_slot[idx];
    int b = s->block;
    int sm = s->sm;
    int base = s_blk[b].gpio_base;
    uint32_t pins = s->pins;
    uint32_t took = s->pin_took;
    char owner[OWNER_N];
    memcpy(owner, s->owner, sizeof owner);

    pio_port_sm_enable(b, sm, 0);
    pio_port_sm_unclaim(b, sm);
    s_blk[b].sm_used = (uint8_t)(s_blk[b].sm_used & ~(1u << sm));

    if (s->prog >= 0 && s->prog < PROGS && s_prog[s->prog].used) {
        prog_t *p = &s_prog[s->prog];
        p->sm_mask = (uint8_t)(p->sm_mask & ~(1u << sm));
        if (p->sm_mask == 0) {
            for (int i = 0; i < p->length; i++) { s_blk[b].shadow[p->offset + i] = 0; }
            pio_port_clear_instr(b, p->offset, p->length);
            s_blk[b].map &= ~range_mask(p->offset, p->length);
            memset(p, 0, sizeof *p);
        } else {
            rescan(p);
        }
    }
    for (int line = 0; line < 2; line++) {
        if (s_blk[b].line[line].used && s_blk[b].line[line].handle == idx + 1) {
            pio_port_irq_disarm(b, line);
            memset(&s_blk[b].line[line], 0, sizeof s_blk[b].line[line]);
        }
    }
    memset(s, 0, sizeof *s);
    if (base >= 0) {
        for (int bit = 0; bit < WORDS; bit++) {
            if ((pins & (1u << bit)) == 0) { continue; }
            int pin = base + bit;
            if (took & (1u << bit)) { pio_port_pin_give(pin, owner); }
            if (!pin_still_used(pin)) { pio_port_gpio_release(pin); }
        }
    }
    if (s_blk[b].sm_used == 0) { s_blk[b].gpio_base = -1; }
}

static int base_for(int lo, int hi, int current, int *use) {
    if (current >= 0) {
        if (lo >= current && hi < current + WORDS) { *use = current; return 0; }
        return -1;
    }
    if (!pio_port_base_movable()) {
        if (hi <= 31) { *use = 0; return 0; }
        return -1;
    }
    if (hi > 31) {
        if (lo >= 16 && hi <= 47) { *use = 16; return 0; }
        return -1;
    }
    *use = 0;
    return 0;
}

static int choose(const char *owner, const uint8_t *pins, size_t npins,
                  uint32_t nwords, int origin, int from_top, choice_t *out) {
    if (!pins || npins == 0 || npins > WORDS) {
        return fail(PIO_EINVAL, owner, "need 1 to %d pins", WORDS);
    }
    if (nwords == 0 || nwords > WORDS) {
        return fail(PIO_EINVAL, owner, "need %u instruction words", (unsigned)nwords);
    }
    if (origin < -1 || origin >= WORDS ||
        (origin >= 0 && origin + (int)nwords > WORDS)) {
        return fail(PIO_EINVAL, owner, "origin %d length %u does not fit in 32 words",
                    origin, (unsigned)nwords);
    }
    int lo = 255, hi = -1;
    for (size_t i = 0; i < npins; i++) {
        int pin = pins[i];
        if (pin < lo) { lo = pin; }
        if (pin > hi) { hi = pin; }
    }
    if (lo < 0 || hi > 47 || hi - lo >= WORDS) {
        return fail(PIO_EINVAL, owner, "pins %d-%d do not fit one gpio window", lo, hi);
    }
    if (!pio_port_base_movable() && hi > 31) {
        return fail(PIO_EINVAL, owner, "pin %d outside gpio base 0", hi);
    }
    /* 10 and 40 are 30 apart, so the span check lets them through, and neither
     * window holds both: base 0 stops at 31, base 16 starts at 16. */
    if (pio_port_base_movable() && hi > 31 && lo < 16) {
        return fail(PIO_EINVAL, owner, "pins %d-%d do not fit one gpio window", lo, hi);
    }
    for (size_t i = 0; i < npins; i++) {
        char who[48];
        who[0] = 0;
        int rc = pio_port_pin_check(pins[i], owner, who, sizeof who);
        if (rc == -2) { return fail(PIO_EINVAL, owner, "pin %u does not exist", pins[i]); }
        if (rc < 0) { return fail(PIO_EINVAL, owner, "pin %u reserved by %s", pins[i], who[0] ? who : "someone"); }
    }

    int nb = nblocks();
    int have_share = 0, have_set = 0, have_empty = 0;
    choice_t on_share, on_set, on_empty;
    int saw_mem = 0, mem_block = 0, mem_used = 0, mem_free = -1;
    int saw_overlap = 0, overlap_block = 0, overlap_prog = -1;
    int saw_prot = 0, prot_block = 0, prot_prog = -1;
    int saw_sm = 0, sm_block = 0;
    int saw_base = 0, base_block = 0, base_val = 0;
    int saw_slot = 0, slot_block = 0;

    memset(&on_share, 0, sizeof on_share);
    memset(&on_set, 0, sizeof on_set);
    memset(&on_empty, 0, sizeof on_empty);
    for (int step = 0; step < nb; step++) {
        int b = from_top ? (nb - 1 - step) : step;
        int use = 0;
        if (base_for(lo, hi, s_blk[b].gpio_base, &use) != 0) {
            /* Name a protected holder over an earlier unlocked block. */
            if (!saw_base || (protected_on(b) && !protected_on(base_block))) {
                saw_base = 1; base_block = b; base_val = s_blk[b].gpio_base;
            }
            continue;
        }
        int sm = free_sm(b);
        if (sm < 0) {
            if (!saw_sm) { saw_sm = 1; sm_block = b; }
            continue;
        }
        int prog = -1, offset = 0;
        if (origin >= 0) {
            prog = find_exact(b, origin, (int)nwords);
            if (prog >= 0 && s_prog[prog].prot) {
                /* Attaching another machine would share the running copy. */
                if (!saw_prot) { saw_prot = 1; prot_prog = prog; prot_block = b; }
                continue;
            }
            if (prog >= 0) {
                offset = origin;
            } else if (s_blk[b].map & range_mask(origin, (int)nwords)) {
                int hit = prog_covering(b, origin, (int)nwords);
                if (hit >= 0 && s_prog[hit].prot) {
                    if (!saw_prot) { saw_prot = 1; prot_prog = hit; prot_block = b; }
                } else if (!saw_overlap) {
                    saw_overlap = 1; overlap_block = b; overlap_prog = hit;
                }
                continue;
            } else if (free_prog() < 0) {
                if (!saw_slot) { saw_slot = 1; slot_block = b; }
                continue;
            } else {
                offset = origin;
            }
        } else {
            offset = find_run(s_blk[b].map, (int)nwords);
            if (offset < 0) {
                int used = (int)popcount(s_blk[b].map);
                int free = WORDS - used;
                if (!saw_mem || free > mem_free) {
                    saw_mem = 1; mem_block = b; mem_used = used; mem_free = free;
                }
                continue;
            }
            if (free_prog() < 0) {
                if (!saw_slot) { saw_slot = 1; slot_block = b; }
                continue;
            }
        }
        choice_t c = { .block = b, .base = use, .sm = sm, .prog = prog, .offset = offset };
        /* An existing copy of this origin wins over a free gap on an earlier block. */
        if (prog >= 0) {
            if (!have_share) { on_share = c; have_share = 1; }
        } else if (s_blk[b].gpio_base >= 0) {
            if (!have_set) { on_set = c; have_set = 1; }
        } else if (!have_empty) {
            on_empty = c; have_empty = 1;
        }
    }
    if (have_share) { *out = on_share; return 0; }
    if (have_set) { *out = on_set; return 0; }
    if (have_empty) { *out = on_empty; return 0; }
    if (saw_prot && prot_prog >= 0) {
        prog_t *p = &s_prog[prot_prog];
        return fail(PIO_EBUSY, owner,
                    "origin %d overlaps protected %s at offset %u length %u on pio%d sm%u; those words stay",
                    origin, p->owner, p->offset, (unsigned)p->length, prot_block, lowest_sm(p));
    }
    if (saw_mem) {
        return fail(PIO_EBUSY, owner,
                    "instruction memory full on pio%d (used %d of 32, need %u)",
                    mem_block, mem_used, (unsigned)nwords);
    }
    if (saw_overlap) {
        if (overlap_prog >= 0) {
            prog_t *p = &s_prog[overlap_prog];
            return fail(PIO_EBUSY, owner,
                        "origin %d overlaps %s program at offset %u length %u on pio%d sm%u; those words stay",
                        origin, p->owner, p->offset, (unsigned)p->length, overlap_block, lowest_sm(p));
        }
        return fail(PIO_EBUSY, owner, "origin %d overlaps a loaded range on pio%d", origin, overlap_block);
    }
    if (saw_slot) {
        return fail(PIO_ENOMEM, owner, "no program slot on pio%d", slot_block);
    }
    /* A block whose window already fits, but has no machine, is the reason.
     * A window mismatch is only the answer when no block's base could fit. */
    if (saw_sm) {
        return fail(PIO_EBUSY, owner, "no free state machine on pio%d", sm_block);
    }
    if (saw_base) {
        prog_t *hold = protected_on(base_block);
        if (hold) {
            return fail(PIO_EBUSY, owner,
                        "pins %d-%d outside pio%d gpio base %d; protected %s on sm%u holds that base",
                        lo, hi, base_block, base_val, hold->owner, lowest_sm(hold));
        }
        return fail(PIO_EBUSY, owner, "pins %d-%d outside pio%d gpio base %d",
                    lo, hi, base_block, base_val);
    }
    return fail(PIO_EBUSY, owner, "no pio block can take %u words", (unsigned)nwords);
}

static int fill_block(pio_res_block_t *d, int b) {
    unsigned used = popcount(s_blk[b].map);
    d->gpio_base = s_blk[b].gpio_base;
    d->instr_used = used;
    d->instr_free = WORDS - used;
    d->instr_map = s_blk[b].map;
    d->sm_free = (uint32_t)((~s_blk[b].sm_used) & 0xfu);
    d->irq_used = irq_used_of(b);
    d->irq_line_used = (s_blk[b].line[0].used ? 1u : 0u) | (s_blk[b].line[1].used ? 2u : 0u);
    return 0;
}

static void fill_prog(pio_res_prog_t *d, const prog_t *p) {
    memset(d, 0, sizeof *d);
    d->offset = p->offset;
    d->length = p->length;
    d->origin = p->origin;
    d->sm_mask = p->sm_mask;
    d->irq_flags = (uint32_t)p->irq_here | ((uint32_t)p->irq_next << 4);
    d->prot = p->prot;
    memcpy(d->owner, p->owner, OWNER_N);
}

int pio_res_stat(pio_res_block_t *out, size_t cap, size_t *n) {
    clear_err();
    int nb = nblocks();
    if (n) { *n = (size_t)nb; }
    if (cap == 0) { return PIO_OK; }
    if (!out) { return fail(PIO_EINVAL, "?", "stat buffer is missing"); }
    size_t w = cap < (size_t)nb ? cap : (size_t)nb;
    for (size_t i = 0; i < w; i++) { fill_block(&out[i], (int)i); }
    return PIO_OK;
}

int pio_res_programs(int block, pio_res_prog_t *out, size_t cap, size_t *n) {
    clear_err();
    if (block < 0 || block >= nblocks()) {
        return fail(PIO_EINVAL, "?", "no pio%d", block);
    }
    size_t count = 0;
    for (int i = 0; i < PROGS; i++) {
        if (s_prog[i].used && s_prog[i].block == (uint8_t)block) { count++; }
    }
    if (n) { *n = count; }
    if (cap == 0) { return PIO_OK; }
    if (!out) { return fail(PIO_EINVAL, "?", "programs buffer is missing"); }
    size_t w = 0;
    for (int i = 0; i < PROGS && w < cap; i++) {
        if (!s_prog[i].used || s_prog[i].block != (uint8_t)block) { continue; }
        fill_prog(&out[w], &s_prog[i]);
        w++;
    }
    return PIO_OK;
}

int pio_res_instr(int block, uint32_t offset, uint16_t *words, size_t cap) {
    clear_err();
    if (block < 0 || block >= nblocks()) { return fail(PIO_EINVAL, "?", "no pio%d", block); }
    int idx = -1;
    for (int i = 0; i < PROGS; i++) {
        if (s_prog[i].used && s_prog[i].block == (uint8_t)block &&
            s_prog[i].offset == offset) { idx = i; break; }
    }
    if (idx < 0) { return fail(PIO_ENOENT, "?", "no program at offset %u on pio%d", offset, block); }
    prog_t *p = &s_prog[idx];
    if (!p->loaded) {
        return fail(PIO_ENOENT, p->owner, "program at offset %u on pio%d is not loaded", offset, block);
    }
    if (cap == 0) { return (int)p->length; }
    if (!words) { return fail(PIO_EINVAL, p->owner, "instr buffer is missing"); }
    size_t n = cap < p->length ? cap : p->length;
    memcpy(words, s_blk[block].shadow + p->offset, n * sizeof(uint16_t));
    return (int)n;
}

int pio_res_fit(const char *owner, const uint8_t *pins, size_t npins,
                uint32_t nwords, int origin) {
    clear_err();
    choice_t c;
    int rc = choose(owner, pins, npins, nwords, origin, 0, &c);
    return rc < 0 ? rc : c.block;
}

static int claim_with(const char *owner, const uint8_t *pins, size_t npins,
                      uint32_t nwords, int origin, int from_top) {
    choice_t c;
    int attempts = 0;
    for (;;) {
        int rc = choose(owner, pins, npins, nwords, origin, from_top, &c);
        if (rc < 0) { return rc; }
        if (pio_port_sm_claim(c.block, c.sm) == 0) { break; }
        /* The SDK already holds this machine. Remember that and pick again. */
        s_blk[c.block].sm_used = (uint8_t)(s_blk[c.block].sm_used | (1u << c.sm));
        if (++attempts >= HANDLES) {
            return fail(PIO_EBUSY, owner, "no free state machine on pio%d", c.block);
        }
    }

    int set_base = 0;
    int prev_base = s_blk[c.block].gpio_base;
    if (prev_base < 0) {
        if (pio_port_set_gpio_base(c.block, c.base) != 0) {
            pio_port_sm_unclaim(c.block, c.sm);
            return fail(PIO_EINVAL, owner, "gpio base %d refused on pio%d", c.base, c.block);
        }
        s_blk[c.block].gpio_base = c.base;
        set_base = 1;
    }

    uint32_t mask = 0, took = 0;
    for (size_t i = 0; i < npins; i++) {
        int rel = (int)pins[i] - s_blk[c.block].gpio_base;
        if (rel < 0 || rel >= WORDS) {
            /* choose() agreed this window fits. A pin outside it is a bug. */
            continue;
        }
        if (mask & (1u << rel)) { continue; }
        char who[48];
        int did = 0;
        who[0] = 0;
        int tr = pio_port_pin_take(pins[i], owner, &did, who, sizeof who);
        if (tr < 0) {
            for (int bit = 0; bit < WORDS; bit++) {
                if (took & (1u << bit)) { pio_port_pin_give(s_blk[c.block].gpio_base + bit, owner); }
            }
            pio_port_sm_unclaim(c.block, c.sm);
            if (set_base) { s_blk[c.block].gpio_base = -1; }
            if (tr == -2) { return fail(PIO_EINVAL, owner, "pin %u does not exist", pins[i]); }
            return fail(PIO_EINVAL, owner, "pin %u reserved by %s", pins[i], who[0] ? who : "someone");
        }
        mask |= 1u << rel;
        if (did) { took |= 1u << rel; }
    }

    int prog = c.prog;
    if (prog < 0) {
        prog = free_prog();
        if (prog < 0) {
            for (int bit = 0; bit < WORDS; bit++) {
                if (took & (1u << bit)) { pio_port_pin_give(s_blk[c.block].gpio_base + bit, owner); }
            }
            pio_port_sm_unclaim(c.block, c.sm);
            if (set_base) { s_blk[c.block].gpio_base = -1; }
            return fail(PIO_ENOMEM, owner, "no program slot on pio%d", c.block);
        }
        prog_t *p = &s_prog[prog];
        memset(p, 0, sizeof *p);
        p->used = 1;
        p->block = (uint8_t)c.block;
        p->offset = (uint16_t)c.offset;
        p->length = (uint16_t)nwords;
        p->origin = (int16_t)origin;
        p->sm_mask = (uint8_t)(1u << c.sm);
        copy_owner(p->owner, owner);
        s_blk[c.block].map |= range_mask(c.offset, (int)nwords);
    } else {
        s_prog[prog].sm_mask = (uint8_t)(s_prog[prog].sm_mask | (1u << c.sm));
        if (s_prog[prog].loaded) { rescan(&s_prog[prog]); }
    }

    int idx = -1;
    for (int i = 0; i < HANDLES; i++) { if (!s_slot[i].used) { idx = i; break; } }
    if (idx < 0) {
        /* 12 handles and 12 machines: this is unreachable once sm_used is honest. */
        s_prog[prog].sm_mask = (uint8_t)(s_prog[prog].sm_mask & ~(1u << c.sm));
        if (s_prog[prog].sm_mask == 0) {
            s_blk[c.block].map &= ~range_mask(c.offset, (int)nwords);
            memset(&s_prog[prog], 0, sizeof s_prog[prog]);
        }
        for (int bit = 0; bit < WORDS; bit++) {
            if (took & (1u << bit)) { pio_port_pin_give(s_blk[c.block].gpio_base + bit, owner); }
        }
        pio_port_sm_unclaim(c.block, c.sm);
        if (set_base) { s_blk[c.block].gpio_base = -1; }
        return fail(PIO_EBUSY, owner, "no free state machine");
    }
    slot_t *s = &s_slot[idx];
    memset(s, 0, sizeof *s);
    s->used = 1;
    s->block = (uint8_t)c.block;
    s->sm = (uint8_t)c.sm;
    s->prog = prog;
    s->pins = mask;
    s->pin_took = took;
    copy_owner(s->owner, owner);
    s_blk[c.block].sm_used = (uint8_t)(s_blk[c.block].sm_used | (1u << c.sm));
    for (int bit = 0; bit < WORDS; bit++) {
        if (mask & (1u << bit)) { pio_port_gpio_init(c.block, s_blk[c.block].gpio_base + bit); }
    }
    return idx + 1;
}

int pio_res_claim(const char *owner, const uint8_t *pins, size_t npins,
                  uint32_t nwords, int origin) {
    clear_err();
    return claim_with(owner, pins, npins, nwords, origin, 0);
}

int pio_res_load(const char *owner, const uint8_t *pins, size_t npins,
                 const uint16_t *words, size_t nwords, uint32_t flags) {
    clear_err();
    if (flags & ~(PIO_LOAD_PROTECT | PIO_LOAD_TOP)) {
        return fail(PIO_EINVAL, owner, "load flags 0x%x are not protect or top", flags);
    }
    if (!words || nwords == 0) { return fail(PIO_EINVAL, owner, "program is empty"); }
    /* origin -1 is a fresh gap. An existing copy is left where it is. */
    int h = claim_with(owner, pins, npins, nwords, -1, (flags & PIO_LOAD_TOP) ? 1 : 0);
    if (h < 0) { return h; }
    int rc = pio_res_program(h, words, nwords);
    if (rc < 0) {
        char saved[sizeof s_err];
        memcpy(saved, s_err, sizeof saved);
        pio_res_release(h);
        memcpy(s_err, saved, sizeof s_err);
        return rc;
    }
    if (flags & PIO_LOAD_PROTECT) {
        slot_t *s = slot_of(h);
        if (s && s->prog >= 0) { s_prog[s->prog].prot = 1; }
    }
    return h;
}

int pio_res_program(int handle, const uint16_t *words, size_t nwords) {
    clear_err();
    slot_t *s = slot_of(handle);
    if (!s) { return fail(PIO_EINVAL, "?", "bad handle %d", handle); }
    if (!words || nwords == 0) { return fail(PIO_EINVAL, s->owner, "program is empty"); }
    if (s->prog < 0 || !s_prog[s->prog].used) {
        return fail(PIO_EINVAL, s->owner, "handle %d has no instruction range", handle);
    }
    prog_t *p = &s_prog[s->prog];
    if (p->prot) { return refuse_prot(s, "rewrite"); }
    if (nwords != p->length) {
        return fail(PIO_EINVAL, s->owner,
                    "program length %u does not match the %u words reserved on pio%d",
                    (unsigned)nwords, p->length, s->block);
    }
    uint16_t *shadow = s_blk[s->block].shadow + p->offset;
    uint8_t here = 0, next = 0;
    scan_words(words, (int)nwords, s->sm, &here, &next);
    if (prot_irq(s->block, s->prog, here, next, s->owner) < 0) { return PIO_EBUSY; }
    if (p->loaded) {
        if (memcmp(shadow, words, nwords * sizeof(uint16_t)) != 0) {
            return fail(PIO_EINVAL, s->owner,
                        "program at offset %u on pio%d does not match the %u words loaded",
                        p->offset, s->block, (unsigned)nwords);
        }
    } else {
        /* JMP addresses are assembled for a program that starts at word 0.
         * pio_add_program adds the load offset; a verbatim copy jumps into
         * whatever was loaded earlier on this block. The shadow keeps the
         * assembled words, so a second machine still matches them. */
        uint16_t placed[WORDS];
        for (size_t i = 0; i < nwords; i++) {
            uint16_t w = words[i];
            placed[i] = (w & 0xe000u) == 0 ? (uint16_t)(w + p->offset) : w;
        }
        memcpy(shadow, words, nwords * sizeof(uint16_t));
        pio_port_write_instr(s->block, p->offset, placed, (int)nwords);
        p->loaded = 1;
    }
    rescan(p);
    return PIO_OK;
}

static int pins_in_claim(const slot_t *s, int base, int count, const char *what) {
    if (count == 0) { return 0; }
    int window = s_blk[s->block].gpio_base;
    int lo = base, hi = base + count - 1;
    if (count < 0 || base < 0 || hi > 47 || lo < window || hi >= window + WORDS) {
        return fail(PIO_EINVAL, s->owner, "%s pins %d-%d outside pio%d gpio base %d",
                    what, lo, hi, s->block, window);
    }
    for (int pin = lo; pin <= hi; pin++) {
        int rel = pin - window;
        if ((s->pins & (1u << rel)) == 0) {
            return fail(PIO_EINVAL, s->owner, "%s pin %d outside claim on pio%d", what, pin, s->block);
        }
    }
    return 0;
}

int pio_res_init(int handle, const pio_res_cfg_t *cfg) {
    clear_err();
    slot_t *s = slot_of(handle);
    if (!s) { return fail(PIO_EINVAL, "?", "bad handle %d", handle); }
    if (slot_prot(s)) { return refuse_prot(s, "reconfigure"); }
    if (!cfg) { return fail(PIO_EINVAL, s->owner, "init config is missing"); }
    if (s->prog < 0 || !s_prog[s->prog].used || !s_prog[s->prog].loaded) {
        return fail(PIO_EINVAL, s->owner, "machine on pio%d has no program", s->block);
    }
    if (cfg->fifo_join > PIO_JOIN_RX) {
        return fail(PIO_EINVAL, s->owner, "fifo join %u is not none, tx, or rx", cfg->fifo_join);
    }
    if (cfg->out_count > 32 || cfg->set_count > 5 || cfg->sideset_count > 5) {
        return fail(PIO_EINVAL, s->owner, "pin count does not fit a state machine");
    }
    if (pins_in_claim(s, cfg->out_base, cfg->out_count, "out") < 0) { return PIO_EINVAL; }
    if (pins_in_claim(s, cfg->set_base, cfg->set_count, "set") < 0) { return PIO_EINVAL; }
    if (pins_in_claim(s, cfg->sideset_base, cfg->sideset_count, "sideset") < 0) { return PIO_EINVAL; }
    if (cfg->in_base >= 0 && pins_in_claim(s, cfg->in_base, 1, "in") < 0) { return PIO_EINVAL; }
    if (cfg->jmp_pin >= 0 && pins_in_claim(s, cfg->jmp_pin, 1, "jmp") < 0) { return PIO_EINVAL; }
    if (cfg->clkdiv > 0x00ffffffu) {
        return fail(PIO_EINVAL, s->owner, "clock divider uses 16.8 bits");
    }
    uint32_t div = cfg->clkdiv ? cfg->clkdiv : 0x100u;
    uint16_t div_int = (uint16_t)(div >> 8);
    uint8_t div_frac = (uint8_t)div;
    if (div_int == 0) {
        if (div_frac != 0) { return fail(PIO_EINVAL, s->owner, "clock divider must be at least 1"); }
        div_int = 1;
    }
    prog_t *p = &s_prog[s->prog];
    pio_port_smcfg_t hw;
    memset(&hw, 0, sizeof hw);
    hw.out_base = cfg->out_base;
    hw.out_count = cfg->out_count;
    hw.set_base = cfg->set_base;
    hw.set_count = cfg->set_count;
    hw.sideset_base = cfg->sideset_base;
    hw.sideset_count = cfg->sideset_count;
    hw.sideset_opt = cfg->sideset_opt;
    hw.in_base = cfg->in_base;
    hw.jmp_pin = cfg->jmp_pin;
    hw.shift_right = cfg->shift_right;
    hw.autopush = cfg->autopush;
    hw.autopull = cfg->autopull;
    hw.push_thresh = cfg->push_thresh;
    hw.pull_thresh = cfg->pull_thresh;
    hw.fifo_join = cfg->fifo_join;
    hw.div_int = div_int;
    hw.div_frac = div_frac;
    hw.wrap_target = p->offset;
    hw.wrap = (uint16_t)(p->offset + p->length - 1);
    if (pio_port_sm_init(s->block, s->sm, &hw) != 0) {
        return fail(PIO_EINVAL, s->owner, "state machine init refused on pio%d sm%d", s->block, s->sm);
    }
    s->join = cfg->fifo_join;
    return PIO_OK;
}

int pio_res_enable(int handle, int on) {
    clear_err();
    slot_t *s = slot_of(handle);
    if (!s) { return fail(PIO_EINVAL, "?", "bad handle %d", handle); }
    if (slot_prot(s)) { return refuse_prot(s, "enable"); }
    pio_port_sm_enable(s->block, s->sm, on ? 1 : 0);
    return PIO_OK;
}

int pio_res_put(int handle, uint32_t word) {
    clear_err();
    slot_t *s = slot_of(handle);
    if (!s) { return fail(PIO_EINVAL, "?", "bad handle %d", handle); }
    if (slot_prot(s)) { return refuse_prot(s, "fill"); }
    if (pio_port_put(s->block, s->sm, word) != 0) {
        return fail(PIO_EAGAIN, s->owner, "tx fifo full on pio%d sm%d", s->block, s->sm);
    }
    return PIO_OK;
}

int pio_res_get(int handle, uint32_t *word) {
    clear_err();
    slot_t *s = slot_of(handle);
    if (!s) { return fail(PIO_EINVAL, "?", "bad handle %d", handle); }
    if (slot_prot(s)) { return refuse_prot(s, "drain"); }
    if (!word) { return fail(PIO_EINVAL, s->owner, "get buffer is missing"); }
    if (pio_port_get(s->block, s->sm, word) != 0) {
        return fail(PIO_EAGAIN, s->owner, "rx fifo empty on pio%d sm%d", s->block, s->sm);
    }
    return PIO_OK;
}

int pio_res_levels(int handle, uint32_t *tx, uint32_t *rx) {
    clear_err();
    slot_t *s = slot_of(handle);
    if (!s) { return fail(PIO_EINVAL, "?", "bad handle %d", handle); }
    uint32_t a = 0, b = 0;
    pio_port_levels(s->block, s->sm, &a, &b);
    if (tx) { *tx = a; }
    if (rx) { *rx = b; }
    return PIO_OK;
}

int pio_res_irq(int handle, int line, uint32_t sources,
                pio_res_irq_fn fn, void *arg, void *mod) {
    clear_err();
    slot_t *s = slot_of(handle);
    if (!s) { return fail(PIO_EINVAL, "?", "bad handle %d", handle); }
    if (slot_prot(s)) { return refuse_prot(s, "hook"); }
    if (line < 0 || line > 1) { return fail(PIO_EINVAL, s->owner, "cpu irq %d is not 0 or 1", line); }
    if (!fn) { return fail(PIO_EINVAL, s->owner, "cpu irq %d on pio%d has no handler", line, s->block); }
    if (sources == 0) { return fail(PIO_EINVAL, s->owner, "cpu irq %d on pio%d has no sources", line, s->block); }
    line_t *L = &s_blk[s->block].line[line];
    if (L->used) {
        return fail(PIO_EBUSY, s->owner, "cpu irq %d on pio%d already owned by %s",
                    line, s->block, L->owner);
    }
    uint32_t flags = 0;
    if (s->prog >= 0 && s_prog[s->prog].used) { flags = s_prog[s->prog].irq_here; }
    memset(L, 0, sizeof *L);
    L->used = 1;
    L->sm = s->sm;
    L->handle = handle;
    L->fn = fn;
    L->arg = arg;
    L->mod = mod;
    L->sources = sources;
    L->instr_flags = flags;
    copy_owner(L->owner, s->owner);
    pio_port_irq_arm(s->block, line, s->sm, sources, flags);
    return PIO_OK;
}

int pio_res_release(int handle) {
    clear_err();
    slot_t *s = slot_of(handle);
    if (!s) { return fail(PIO_EINVAL, "?", "bad handle %d", handle); }
    if (slot_prot(s)) { return refuse_prot(s, "release"); }
    release_slot(handle - 1);
    return PIO_OK;
}

int pio_res_release_hw(int block, int sm) {
    if (block < 0 || block >= BLOCKS || sm < 0 || sm >= SMS) { return -1; }
    for (int i = 0; i < HANDLES; i++) {
        if (!s_slot[i].used) { continue; }
        if (s_slot[i].block == (uint8_t)block && s_slot[i].sm == (uint8_t)sm) {
            release_slot(i);
            return 0;
        }
    }
    return -1;
}

void pio_res_release_owner(const char *owner) {
    if (!owner || !owner[0] || strcmp(owner, "sd") == 0) { return; }
    for (int i = 0; i < HANDLES; i++) {
        if (s_slot[i].used && !slot_prot(&s_slot[i]) && strcmp(s_slot[i].owner, owner) == 0) {
            release_slot(i);
        }
    }
}

int pio_res_check(int handle, const char *owner) {
    clear_err();
    slot_t *s = slot_of(handle);
    if (!s) { return fail(PIO_EINVAL, owner, "bad handle %d", handle); }
    if (!owner || strcmp(s->owner, owner) != 0) {
        return fail(PIO_EINVAL, owner, "handle %d owned by %s", handle, s->owner);
    }
    return PIO_OK;
}

int pio_res_protected(int handle) {
    slot_t *s = slot_of(handle);
    if (!s) { return -1; }
    return slot_prot(s) ? 1 : 0;
}

int pio_res_block(int handle) {
    slot_t *s = slot_of(handle);
    return s ? s->block : -1;
}

int pio_res_sm(int handle) {
    slot_t *s = slot_of(handle);
    return s ? s->sm : -1;
}

int pio_res_offset(int handle) {
    slot_t *s = slot_of(handle);
    if (!s || s->prog < 0 || !s_prog[s->prog].used) { return -1; }
    return s_prog[s->prog].offset;
}

const char *pio_res_owner(int handle) {
    slot_t *s = slot_of(handle);
    return s ? s->owner : NULL;
}

void pio_res_set_irq_bit(int block, int line) {
    if (block < 0 || block >= BLOCKS || line < 0 || line > 1) { return; }
    s_blk[block].irq_pending = (uint8_t)(s_blk[block].irq_pending | (1u << line));
}

void pio_res_poll(void (*deliver)(void *mod, pio_res_irq_fn fn, void *arg)) {
    int nb = nblocks();
    for (int b = 0; b < nb; b++) {
        uint8_t pend = s_blk[b].irq_pending;
        if (!pend) { continue; }
        s_blk[b].irq_pending = 0;
        for (int line = 0; line < 2; line++) {
            if ((pend & (1u << line)) == 0) { continue; }
            line_t *L = &s_blk[b].line[line];
            if (L->used && L->fn && deliver) { deliver(L->mod, L->fn, L->arg); }
            pio_port_irq_unmask(b, line);
        }
    }
}
