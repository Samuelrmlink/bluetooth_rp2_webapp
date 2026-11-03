/*
 * Host stand-in for src/io/pio_port_pico.c: 3 blocks, 4 machines, 32 words.
 * Instruction memory can be dropped, the way the real INSTR_MEM cannot be read.
 * GPIO base moves, so a test can refuse one window and land on another.
 */
#include "io/pio_port.h"

#include <stdio.h>
#include <string.h>

#define BLOCKS 3
#define SMS    4
#define WORDS  32

static uint16_t s_mem[BLOCKS][WORDS];
static int s_dropped;
static int s_base[BLOCKS];
static uint8_t s_sm[BLOCKS];
static char s_pin_owner[48][24];
static uint8_t s_pin_sticky[48];

static uint8_t s_tx_n[BLOCKS][SMS];
static uint8_t s_rx_n[BLOCKS][SMS];
static uint8_t s_tx_cap[BLOCKS][SMS];
static uint8_t s_rx_cap[BLOCKS][SMS];
static uint32_t s_tx[BLOCKS][SMS][8];
static uint32_t s_rx[BLOCKS][SMS][8];
static int s_ready;

static void ensure(void) {
    if (s_ready) { return; }
    s_ready = 1;
    for (int b = 0; b < BLOCKS; b++) {
        s_base[b] = -1;
        for (int sm = 0; sm < SMS; sm++) {
            s_tx_cap[b][sm] = 4;
            s_rx_cap[b][sm] = 4;
        }
    }
}

void pio_port_fake_reserve(int pin, const char *owner) {
    if (pin < 0 || pin >= 48) { return; }
    snprintf(s_pin_owner[pin], sizeof s_pin_owner[pin], "%s", owner ? owner : "");
    s_pin_sticky[pin] = 1;
}

void pio_port_fake_drop_instr(void) {
    s_dropped = 1;
    memset(s_mem, 0, sizeof s_mem);
}

int pio_port_fake_dropped(void) { return s_dropped; }

uint16_t pio_port_fake_mem(int block, int offset) {
    if (block < 0 || block >= BLOCKS || offset < 0 || offset >= WORDS) { return 0; }
    return s_mem[block][offset];
}

int pio_port_nblocks(void) { return BLOCKS; }
int pio_port_base_movable(void) { return 1; }
void *pio_port_instance(int block) { (void)block; return NULL; }

int pio_port_set_gpio_base(int block, int base) {
    ensure();
    if (block < 0 || block >= BLOCKS || (base != 0 && base != 16)) { return -1; }
    s_base[block] = base;
    return 0;
}

static int classify(int pin, const char *owner, char *who, size_t who_cap) {
    if (pin < 0 || pin >= 48) { return -2; }
    if (!s_pin_owner[pin][0]) { return 0; }
    if (owner && strcmp(s_pin_owner[pin], owner) == 0) { return 1; }
    if (who && who_cap) { snprintf(who, who_cap, "%s", s_pin_owner[pin]); }
    return -1;
}

int pio_port_pin_check(int pin, const char *owner, char *who, size_t who_cap) {
    int c = classify(pin, owner, who, who_cap);
    return c < 0 ? c : 0;
}

int pio_port_pin_take(int pin, const char *owner, int *took, char *who, size_t who_cap) {
    if (took) { *took = 0; }
    int c = classify(pin, owner, who, who_cap);
    if (c < 0) { return c; }
    if (c == 1) { return 0; }
    snprintf(s_pin_owner[pin], sizeof s_pin_owner[pin], "%s", owner ? owner : "");
    if (took) { *took = 1; }
    return 0;
}

void pio_port_pin_give(int pin, const char *owner) {
    if (pin < 0 || pin >= 48 || s_pin_sticky[pin]) { return; }
    if (owner && strcmp(s_pin_owner[pin], owner) == 0) { s_pin_owner[pin][0] = 0; }
}

int pio_port_sm_claim(int block, int sm) {
    ensure();
    if (block < 0 || block >= BLOCKS || sm < 0 || sm >= SMS) { return -1; }
    if (s_sm[block] & (1u << sm)) { return -1; }
    s_sm[block] = (uint8_t)(s_sm[block] | (1u << sm));
    return 0;
}

void pio_port_sm_unclaim(int block, int sm) {
    if (block < 0 || block >= BLOCKS || sm < 0 || sm >= SMS) { return; }
    s_sm[block] = (uint8_t)(s_sm[block] & ~(1u << sm));
    s_tx_n[block][sm] = 0;
    s_rx_n[block][sm] = 0;
    s_tx_cap[block][sm] = 4;
    s_rx_cap[block][sm] = 4;
}

void pio_port_write_instr(int block, int offset, const uint16_t *words, int n) {
    if (s_dropped || block < 0 || block >= BLOCKS || !words) { return; }
    if (offset < 0 || n < 0 || offset + n > WORDS) { return; }
    memcpy(&s_mem[block][offset], words, (size_t)n * sizeof(uint16_t));
}

void pio_port_clear_instr(int block, int offset, int n) {
    if (block < 0 || block >= BLOCKS || offset < 0 || n < 0 || offset + n > WORDS) { return; }
    memset(&s_mem[block][offset], 0, (size_t)n * sizeof(uint16_t));
}

int pio_port_sm_init(int block, int sm, const pio_port_smcfg_t *cfg) {
    ensure();
    if (block < 0 || block >= BLOCKS || sm < 0 || sm >= SMS || !cfg) { return -1; }
    if (cfg->fifo_join == 1) { s_tx_cap[block][sm] = 8; s_rx_cap[block][sm] = 0; }
    else if (cfg->fifo_join == 2) { s_tx_cap[block][sm] = 0; s_rx_cap[block][sm] = 8; }
    else { s_tx_cap[block][sm] = 4; s_rx_cap[block][sm] = 4; }
    s_tx_n[block][sm] = 0;
    s_rx_n[block][sm] = 0;
    return 0;
}

void pio_port_sm_enable(int block, int sm, int on) { (void)block; (void)sm; (void)on; }

int pio_port_put(int block, int sm, uint32_t word) {
    ensure();
    if (block < 0 || block >= BLOCKS || sm < 0 || sm >= SMS) { return -1; }
    if (s_tx_n[block][sm] >= s_tx_cap[block][sm]) { return -1; }
    s_tx[block][sm][s_tx_n[block][sm]++] = word;
    return 0;
}

int pio_port_get(int block, int sm, uint32_t *word) {
    ensure();
    if (block < 0 || block >= BLOCKS || sm < 0 || sm >= SMS || !word) { return -1; }
    if (s_rx_n[block][sm] == 0) { return -1; }
    *word = s_rx[block][sm][0];
    s_rx_n[block][sm]--;
    memmove(&s_rx[block][sm][0], &s_rx[block][sm][1], s_rx_n[block][sm] * sizeof(uint32_t));
    return 0;
}

void pio_port_levels(int block, int sm, uint32_t *tx, uint32_t *rx) {
    ensure();
    if (tx) { *tx = (block >= 0 && sm >= 0) ? s_tx_n[block][sm] : 0; }
    if (rx) { *rx = (block >= 0 && sm >= 0) ? s_rx_n[block][sm] : 0; }
}

void pio_port_irq_arm(int block, int line, int sm, uint32_t sources, uint32_t instr_flags) {
    (void)block; (void)line; (void)sm; (void)sources; (void)instr_flags;
}
void pio_port_irq_disarm(int block, int line) { (void)block; (void)line; }
void pio_port_irq_unmask(int block, int line) { (void)block; (void)line; }
void pio_port_gpio_init(int block, int pin) { (void)block; (void)pin; }
void pio_port_gpio_release(int pin) { (void)pin; }
