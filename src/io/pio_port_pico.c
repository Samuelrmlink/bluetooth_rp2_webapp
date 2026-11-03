/*
 * PIO port for the RP2040 and RP2350. The ledger decides. This file only
 * moves the decision into the SDK, and masks the CPU IRQ until the main
 * loop has delivered it.
 */
#include "io/pio_port.h"

#include <stdio.h>
#include <string.h>

#include "autoconf.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/pio.h"
#include "hardware/sync.h"
#include "io/pio_dma.h"
#include "io/pio_res.h"
#if CONFIG_IO_PINS
#include "io/pins.h"
#endif

static PIO pio_of(int block) {
    if (block == 0) { return pio0; }
    if (block == 1) { return pio1; }
#if NUM_PIOS > 2
    if (block == 2) { return pio2; }
#endif
    return NULL;
}

static void service(int block, int line) {
    PIO pio = pio_of(block);
    if (!pio) { return; }
    irq_set_enabled(PIO_IRQ_NUM(pio, (uint)line), false);
    pio_res_set_irq_bit(block, line);
}

static void h00(void) { service(0, 0); }
static void h01(void) { service(0, 1); }
static void h10(void) { service(1, 0); }
static void h11(void) { service(1, 1); }
#if NUM_PIOS > 2
static void h20(void) { service(2, 0); }
static void h21(void) { service(2, 1); }
#endif

static void (*handler(int block, int line))(void) {
    static void (*const table[3][2])(void) = {
        { h00, h01 },
        { h10, h11 },
#if NUM_PIOS > 2
        { h20, h21 },
#else
        { NULL, NULL },
#endif
    };
    if (block < 0 || block >= NUM_PIOS || line < 0 || line > 1) { return NULL; }
    return table[block][line];
}

static uint32_t s_inte[3][2];
static uint8_t s_hooked[3][2];

int pio_port_nblocks(void) { return NUM_PIOS; }

int pio_port_base_movable(void) {
#if PICO_PIO_USE_GPIO_BASE
    return 1;
#else
    return 0;
#endif
}

void *pio_port_instance(int block) { return pio_of(block); }

int pio_port_set_gpio_base(int block, int base) {
    PIO pio = pio_of(block);
    if (!pio || (base != 0 && base != 16)) { return -1; }
#if PICO_PIO_USE_GPIO_BASE
    pio_set_gpio_base(pio, (uint)base);
    return 0;
#else
    return base == 0 ? 0 : -1;
#endif
}

#if CONFIG_IO_PINS
/* 0 free, 1 already this owner, -1 someone else, -2 no such pin. */
static int classify(int pin, const char *owner, char *who, size_t who_cap) {
    if (pin < 0 || pin >= PIN_COUNT) { return -2; }
    pins_init();
    const char *held = pins_owner((uint8_t)pin);
    if (!held) { return 0; }
    if (owner && strcmp(held, owner) == 0) { return 1; }
    if (who && who_cap) { snprintf(who, who_cap, "%s", held); }
    return -1;
}
#endif

int pio_port_pin_check(int pin, const char *owner, char *who, size_t who_cap) {
#if CONFIG_IO_PINS
    int c = classify(pin, owner, who, who_cap);
    if (c < 0) { return c; }
    return 0;
#else
    (void)owner; (void)who; (void)who_cap;
    if (pin < 0 || pin >= 30) { return -2; }
    return 0;
#endif
}

int pio_port_pin_take(int pin, const char *owner, int *took, char *who, size_t who_cap) {
    if (took) { *took = 0; }
#if CONFIG_IO_PINS
    int c = classify(pin, owner, who, who_cap);
    if (c < 0) { return c; }
    if (c == 1) { return 0; }
    const char *err = NULL;
    if (!pins_claim((uint8_t)pin, PIN_FN_GPIO, PIN_OWNER_API, owner ? owner : "", &err)) {
        if (who && who_cap) { snprintf(who, who_cap, "%s", err ? err : "claimed"); }
        return -1;
    }
    if (took) { *took = 1; }
    return 0;
#else
    (void)owner; (void)who; (void)who_cap;
    if (pin < 0 || pin >= 30) { return -2; }
    return 0;
#endif
}

void pio_port_pin_give(int pin, const char *owner) {
#if CONFIG_IO_PINS
    /* A system reservation (the card, the radio, the W5500) stays. */
    if (pin < 0 || pin >= PIN_COUNT || pins_is_reserved((uint8_t)pin)) { return; }
    if (!pins_owned_by((uint8_t)pin, owner)) { return; }
    const char *err = NULL;
    pins_release((uint8_t)pin, &err);
#else
    (void)pin; (void)owner;
#endif
}

int pio_port_sm_claim(int block, int sm) {
    PIO pio = pio_of(block);
    if (!pio || sm < 0 || sm >= NUM_PIO_STATE_MACHINES) { return -1; }
    if (pio_sm_is_claimed(pio, (uint)sm)) { return -1; }
    pio_sm_claim(pio, (uint)sm);
    return 0;
}

void pio_port_sm_unclaim(int block, int sm) {
    PIO pio = pio_of(block);
    if (!pio) { return; }
    pio_sm_set_enabled(pio, (uint)sm, false);
    pio_sm_clear_fifos(pio, (uint)sm);
    if (pio_sm_is_claimed(pio, (uint)sm)) { pio_sm_unclaim(pio, (uint)sm); }
}

void pio_port_write_instr(int block, int offset, const uint16_t *words, int n) {
    PIO pio = pio_of(block);
    if (!pio || !words || offset < 0 || n < 0 || offset + n > PIO_INSTRUCTION_COUNT) { return; }
    for (int i = 0; i < n; i++) { pio->instr_mem[offset + i] = words[i]; }
}

void pio_port_clear_instr(int block, int offset, int n) {
    PIO pio = pio_of(block);
    if (!pio || offset < 0 || n < 0 || offset + n > PIO_INSTRUCTION_COUNT) { return; }
    for (int i = 0; i < n; i++) { pio->instr_mem[offset + i] = 0; }
}

/* Mark a run of pins as outputs of this state machine. Pins at or above
 * 32 need the 64-bit helper and a movable GPIO base; this board's user
 * pins are 0-29, and a range that does not fit is left alone. */
static void pio_drive_outputs(PIO pio, uint sm, int base, int count) {
    if (!pio || count <= 0 || base < 0 || base >= 32) { return; }
    if (base + count > 32) { count = 32 - base; }
    pio_sm_set_consecutive_pindirs(pio, sm, (uint)base, (uint)count, true);
}

int pio_port_sm_init(int block, int sm, const pio_port_smcfg_t *cfg) {
    PIO pio = pio_of(block);
    if (!pio || !cfg) { return -1; }
    pio_sm_config c = pio_get_default_sm_config();
    sm_config_set_wrap(&c, cfg->wrap_target, cfg->wrap);
    if (cfg->out_count) { sm_config_set_out_pins(&c, cfg->out_base, cfg->out_count); }
    if (cfg->set_count) { sm_config_set_set_pins(&c, cfg->set_base, cfg->set_count); }
    if (cfg->sideset_count) {
        sm_config_set_sideset_pins(&c, cfg->sideset_base);
        sm_config_set_sideset(&c, cfg->sideset_count, cfg->sideset_opt != 0, false);
    }
    if (cfg->in_base >= 0) { sm_config_set_in_pins(&c, (uint)cfg->in_base); }
    if (cfg->jmp_pin >= 0) { sm_config_set_jmp_pin(&c, (uint)cfg->jmp_pin); }
    uint pull = cfg->pull_thresh ? cfg->pull_thresh : 32u;
    uint push = cfg->push_thresh ? cfg->push_thresh : 32u;
    sm_config_set_out_shift(&c, cfg->shift_right != 0, cfg->autopull != 0, pull);
    sm_config_set_in_shift(&c, cfg->shift_right != 0, cfg->autopush != 0, push);
    enum pio_fifo_join join = PIO_FIFO_JOIN_NONE;
    if (cfg->fifo_join == 1) { join = PIO_FIFO_JOIN_TX; }
    else if (cfg->fifo_join == 2) { join = PIO_FIFO_JOIN_RX; }
    sm_config_set_fifo_join(&c, join);
    sm_config_set_clkdiv_int_frac(&c, cfg->div_int ? cfg->div_int : 1u, cfg->div_frac);
    pio_sm_init(pio, (uint)sm, cfg->wrap_target, &c);
    /* The config names out/set/sideset pins but does not enable them.
     * A sideset program (WS2812) otherwise leaves the pin an input. */
    if (cfg->out_count) { pio_drive_outputs(pio, (uint)sm, cfg->out_base, cfg->out_count); }
    if (cfg->set_count) { pio_drive_outputs(pio, (uint)sm, cfg->set_base, cfg->set_count); }
    if (cfg->sideset_count) { pio_drive_outputs(pio, (uint)sm, cfg->sideset_base, cfg->sideset_count); }
    return 0;
}

void pio_port_sm_enable(int block, int sm, int on) {
    PIO pio = pio_of(block);
    if (!pio) { return; }
    pio_sm_set_enabled(pio, (uint)sm, on != 0);
}

int pio_port_put(int block, int sm, uint32_t word) {
    PIO pio = pio_of(block);
    if (!pio || pio_sm_is_tx_fifo_full(pio, (uint)sm)) { return -1; }
    pio_sm_put(pio, (uint)sm, word);
    return 0;
}

int pio_port_get(int block, int sm, uint32_t *word) {
    PIO pio = pio_of(block);
    if (!pio || !word || pio_sm_is_rx_fifo_empty(pio, (uint)sm)) { return -1; }
    *word = pio_sm_get(pio, (uint)sm);
    return 0;
}

void pio_port_levels(int block, int sm, uint32_t *tx, uint32_t *rx) {
    PIO pio = pio_of(block);
    if (tx) { *tx = pio ? pio_sm_get_tx_fifo_level(pio, (uint)sm) : 0; }
    if (rx) { *rx = pio ? pio_sm_get_rx_fifo_level(pio, (uint)sm) : 0; }
}

static void apply_sources(PIO pio, int line, uint32_t bits, bool on) {
    for (uint32_t src = 0; src < 32; src++) {
        if (bits & (1u << src)) { pio_set_irqn_source_enabled(pio, (uint)line, (pio_interrupt_source_t)src, on); }
    }
}

void pio_port_irq_arm(int block, int line, int sm, uint32_t sources, uint32_t instr_flags) {
    PIO pio = pio_of(block);
    void (*fn)(void) = handler(block, line);
    if (!pio || !fn || line < 0 || line > 1) { return; }
    uint32_t bits = 0;
    if (sources & 1u) { bits |= 1u << (pis_sm0_rx_fifo_not_empty + sm); }
    if (sources & 2u) { bits |= 1u << (pis_sm0_tx_fifo_not_full + sm); }
    if (sources & 4u) {
        for (int f = 0; f < 4; f++) {
            if (instr_flags & (1u << f)) { bits |= 1u << (pis_interrupt0 + f); }
        }
    }
    uint irq = PIO_IRQ_NUM(pio, (uint)line);
    if (!s_hooked[block][line]) {
        irq_set_exclusive_handler(irq, fn);
        s_hooked[block][line] = 1;
    }
    s_inte[block][line] |= bits;
    apply_sources(pio, line, bits, true);
    irq_set_enabled(irq, true);
}

void pio_port_irq_disarm(int block, int line) {
    PIO pio = pio_of(block);
    if (!pio || line < 0 || line > 1) { return; }
    apply_sources(pio, line, s_inte[block][line], false);
    s_inte[block][line] = 0;
    irq_set_enabled(PIO_IRQ_NUM(pio, (uint)line), false);
}

void pio_port_irq_unmask(int block, int line) {
    PIO pio = pio_of(block);
    if (!pio || line < 0 || line > 1 || s_inte[block][line] == 0) { return; }
    irq_set_enabled(PIO_IRQ_NUM(pio, (uint)line), true);
}

void pio_port_gpio_init(int block, int pin) {
    PIO pio = pio_of(block);
    if (!pio || pin < 0) { return; }
    pio_gpio_init(pio, (uint)pin);
}

void pio_port_gpio_release(int pin) {
    if (pin < 0) { return; }
    gpio_set_function((uint)pin, GPIO_FUNC_SIO);
    gpio_set_dir((uint)pin, GPIO_IN);
    gpio_disable_pulls((uint)pin);
}

/* DMA_IRQ_0, shared. DMA_IRQ_1 belongs to the SPI bus. */
static uint32_t s_dma_ours;
static uint8_t s_dma_irq;

static void dma_irq(void) {
    for (int ch = 0; ch < 16; ch++) {
        if ((s_dma_ours & (1u << ch)) == 0) { continue; }
        if (!dma_irqn_get_channel_status(0, (uint)ch)) { continue; }
        dma_irqn_acknowledge_channel(0, (uint)ch);
        pio_dma_hw_done(ch);
    }
}

uint32_t pio_port_lock(void) { return save_and_disable_interrupts(); }
void pio_port_unlock(uint32_t key) { restore_interrupts(key); }

int pio_port_dma_claim(void) {
    int ch = dma_claim_unused_channel(false);
    if (ch < 0 || ch >= 16) { return -1; }
    s_dma_ours |= 1u << ch;
    dma_irqn_set_channel_enabled(0, (uint)ch, true);
    if (!s_dma_irq) {
        irq_add_shared_handler(DMA_IRQ_0, dma_irq, PICO_SHARED_IRQ_HANDLER_DEFAULT_ORDER_PRIORITY);
        irq_set_enabled(DMA_IRQ_0, true);
        s_dma_irq = 1;
    }
    return ch;
}

void pio_port_dma_unclaim(int ch) {
    if (ch < 0 || ch >= 16) { return; }
    dma_irqn_set_channel_enabled(0, (uint)ch, false);
    dma_channel_unclaim((uint)ch);
    s_dma_ours &= ~(1u << ch);
}

int pio_port_dma_start(int ch, int block, int sm, const uint32_t *src, uint32_t nwords, int inc) {
    PIO pio = pio_of(block);
    if (ch < 0 || ch >= 16 || !pio || sm < 0 || sm >= 4 || !src || nwords < 1 || nwords > 65535u) {
        return -1;
    }
    dma_channel_config c = dma_channel_get_default_config((uint)ch);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, inc ? true : false);
    channel_config_set_write_increment(&c, false);
    channel_config_set_dreq(&c, pio_get_dreq(pio, (uint)sm, true));
    dma_channel_configure((uint)ch, &c, &pio->txf[sm], src, nwords, true);
    return 0;
}

void pio_port_dma_abort(int ch) {
    if (ch < 0 || ch >= 16) { return; }
    dma_channel_abort((uint)ch);
}

void pio_port_dma_ack(int ch) {
    if (ch < 0 || ch >= 16) { return; }
    dma_irqn_acknowledge_channel(0, (uint)ch);
}

void pio_port_tx_resync(int block, int sm, int pc) {
    PIO pio = pio_of(block);
    if (!pio || sm < 0 || sm >= 4) { return; }
    pio_sm_set_enabled(pio, (uint)sm, false);
    pio_sm_clear_fifos(pio, (uint)sm);
    pio_sm_restart(pio, (uint)sm);
    pio_sm_clkdiv_restart(pio, (uint)sm);
    if (pc >= 0 && pc < 32) { pio_sm_exec(pio, (uint)sm, pio_encode_jmp((uint)pc)); }
    pio_sm_set_enabled(pio, (uint)sm, true);
}
