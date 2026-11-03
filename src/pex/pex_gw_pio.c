/*
 * The pio group. The ledger in src/io/pio_res.c owns the resources and prints
 * the one refusal line. Nothing here logs again.
 */
#include "pex/pex_gw.h"

#include <string.h>

#include "autoconf.h"
#include "io/pio_res.h"

_Static_assert(PIO_EINVAL == PEX_EINVAL, "pio EINVAL");
_Static_assert(PIO_ENOMEM == PEX_ENOMEM, "pio ENOMEM");
_Static_assert(PIO_EBUSY == PEX_EBUSY, "pio EBUSY");
_Static_assert(PIO_ENOENT == PEX_ENOENT, "pio ENOENT");
_Static_assert(PIO_EAGAIN == PEX_EAGAIN, "pio EAGAIN");
_Static_assert(PIO_ENOTSUP == PEX_ENOTSUP, "pio ENOTSUP");

static const char *mod_name(void) {
    pex_mod_t *m = pex_self();
    if (!m) { return "pex"; }
    pex_info_t in;
    if (pex_info(m, &in) != PEX_OK || !in.name || !in.name[0]) { return "pex"; }
    return in.name;
}

static int gate(int handle) {
    return pio_res_check(handle, mod_name());
}

static void deliver(void *mod, pio_res_irq_fn fn, void *arg) {
    if (!mod) { fn(arg); return; }
    pex_call((pex_mod_t *)mod, (uint32_t)(uintptr_t)fn, (uint32_t)(uintptr_t)arg, 0, 0, 0);
}

void pex_gw_pio_poll(void) { pio_res_poll(deliver); }

void pex_gw_pio_unload(pex_mod_t *m) {
    pex_info_t in;
    if (!m || pex_info(m, &in) != PEX_OK || !in.name) { return; }
    pio_res_release_owner(in.name);
}

int pex_pio_stat(pex_pio_block_t *out, size_t cap, size_t *n) {
    pio_res_block_t tmp[3];
    size_t nn = 0;
    int rc = pio_res_stat(tmp, 3, &nn);
    if (rc < 0) { return rc; }
    if (n) { *n = nn; }
    if (cap == 0 || !out) { return PEX_OK; }
    size_t w = cap < nn ? cap : nn;
    for (size_t i = 0; i < w; i++) {
        out[i].gpio_base = tmp[i].gpio_base;
        out[i].instr_used = tmp[i].instr_used;
        out[i].instr_free = tmp[i].instr_free;
        out[i].instr_map = tmp[i].instr_map;
        out[i].sm_free = tmp[i].sm_free;
        out[i].irq_used = tmp[i].irq_used;
        out[i].irq_line_used = tmp[i].irq_line_used;
    }
    return PEX_OK;
}

int pex_pio_programs(int block, pex_pio_prog_t *out, size_t cap, size_t *n) {
    /* One row per loaded range. 32 is every instruction word as its own program. */
    static pio_res_prog_t tmp[32];
    size_t nn = 0;
    int rc = pio_res_programs(block, NULL, 0, &nn);
    if (rc < 0) { return rc; }
    if (n) { *n = nn; }
    if (cap == 0 || !out) { return PEX_OK; }
    size_t w = cap < nn ? cap : nn;
    if (w > 32) { w = 32; }
    rc = pio_res_programs(block, tmp, w, NULL);
    if (rc < 0) { return rc; }
    for (size_t i = 0; i < w; i++) {
        out[i].offset = tmp[i].offset;
        out[i].length = tmp[i].length;
        out[i].origin = tmp[i].origin;
        out[i].sm_mask = tmp[i].sm_mask;
        out[i].irq_flags = tmp[i].irq_flags;
        out[i].prot = tmp[i].prot;
        memcpy(out[i].owner, tmp[i].owner, sizeof out[i].owner);
    }
    return PEX_OK;
}

int pex_pio_instr(int block, uint32_t offset, uint16_t *words, size_t cap) {
    return pio_res_instr(block, offset, words, cap);
}

int pex_pio_fit(const uint8_t *pins, size_t npins, uint32_t nwords, int origin) {
    return pio_res_fit(mod_name(), pins, npins, nwords, origin);
}

int pex_pio_claim(const uint8_t *pins, size_t npins, uint32_t nwords, int origin) {
    return pio_res_claim(mod_name(), pins, npins, nwords, origin);
}

int pex_pio_load(const uint8_t *pins, size_t npins, const uint16_t *words, size_t nwords) {
    /* A module does not get PIO_LOAD_PROTECT. The radio and the SD card are
     * protected by their firmware claims. A module program, such as a WS2812
     * strip, can be released. */
    return pio_res_load(mod_name(), pins, npins, words, nwords, 0);
}

int pex_pio_program(int handle, const uint16_t *words, size_t nwords) {
    int rc = gate(handle);
    if (rc < 0) { return rc; }
    return pio_res_program(handle, words, nwords);
}

int pex_pio_init(int handle, const pex_pio_cfg_t *cfg) {
    int rc = gate(handle);
    if (rc < 0) { return rc; }
    if (!cfg) { return pio_res_init(handle, NULL); }
    pio_res_cfg_t c;
    c.out_base = cfg->out_base;
    c.out_count = cfg->out_count;
    c.set_base = cfg->set_base;
    c.set_count = cfg->set_count;
    c.sideset_base = cfg->sideset_base;
    c.sideset_count = cfg->sideset_count;
    c.sideset_opt = cfg->sideset_opt;
    c.shift_right = cfg->shift_right;
    c.autopush = cfg->autopush;
    c.autopull = cfg->autopull;
    c.push_thresh = cfg->push_thresh;
    c.pull_thresh = cfg->pull_thresh;
    c.fifo_join = cfg->fifo_join;
    c.in_base = cfg->in_base;
    c.jmp_pin = cfg->jmp_pin;
    c.clkdiv = cfg->clkdiv;
    return pio_res_init(handle, &c);
}

int pex_pio_enable(int handle, int on) {
    int rc = gate(handle);
    if (rc < 0) { return rc; }
    return pio_res_enable(handle, on);
}

int pex_pio_put(int handle, uint32_t word) {
    int rc = gate(handle);
    if (rc < 0) { return rc; }
    return pio_res_put(handle, word);
}

int pex_pio_get(int handle, uint32_t *word) {
    int rc = gate(handle);
    if (rc < 0) { return rc; }
    return pio_res_get(handle, word);
}

int pex_pio_levels(int handle, uint32_t *tx_level, uint32_t *rx_level) {
    int rc = gate(handle);
    if (rc < 0) { return rc; }
    return pio_res_levels(handle, tx_level, rx_level);
}

int pex_pio_irq(int handle, int line, uint32_t sources, pex_pio_irq_fn fn, void *arg) {
    int rc = gate(handle);
    if (rc < 0) { return rc; }
    return pio_res_irq(handle, line, sources, fn, arg, pex_self());
}

int pex_pio_release(int handle) {
    int rc = gate(handle);
    if (rc < 0) { return rc; }
    return pio_res_release(handle);
}
