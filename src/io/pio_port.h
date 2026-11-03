#ifndef BTGW_PIO_PORT_H
#define BTGW_PIO_PORT_H
/*
 * The only PIO surface pio_res.c uses. The Pico port talks to the SDK.
 * Host tests link a fake of 3 blocks, 4 machines, and 32 instruction words.
 * pio_res.c does not include hardware/pio.h.
 */
#include <stddef.h>
#include <stdint.h>

/* Applied after the ledger has checked the pins. wrap is the program range. */
typedef struct {
    uint8_t out_base, out_count;
    uint8_t set_base, set_count;
    uint8_t sideset_base, sideset_count, sideset_opt;
    int16_t in_base;                 /* -1: leave the input mapping alone */
    int16_t jmp_pin;                 /* -1: leave the jump pin alone */
    uint8_t shift_right, autopush, autopull;
    uint8_t push_thresh, pull_thresh; /* 0 means 32 */
    uint8_t fifo_join;               /* 0 none, 1 TX, 2 RX */
    uint16_t div_int;
    uint8_t div_frac;
    uint16_t wrap_target, wrap;
} pio_port_smcfg_t;

int pio_port_nblocks(void);          /* 2 on RP2040, 3 on RP2350 */
int pio_port_base_movable(void);     /* 0 when the GPIO base is fixed at 0 */
/* SDK PIO*, or NULL. The header stays free of hardware/pio.h. */
void *pio_port_instance(int block);
int pio_port_set_gpio_base(int block, int base);

/* 0: this owner may use the pin. -1: someone else holds it (*who set).
 * -2: no such pin. check does not claim. take claims a free pin for owner
 * (*took is 1) or notices the owner already has it (*took is 0). */
int pio_port_pin_check(int pin, const char *owner, char *who, size_t who_cap);
int pio_port_pin_take(int pin, const char *owner, int *took, char *who, size_t who_cap);
void pio_port_pin_give(int pin, const char *owner);

int pio_port_sm_claim(int block, int sm);
void pio_port_sm_unclaim(int block, int sm);
void pio_port_write_instr(int block, int offset, const uint16_t *words, int n);
void pio_port_clear_instr(int block, int offset, int n);
int pio_port_sm_init(int block, int sm, const pio_port_smcfg_t *cfg);
void pio_port_sm_enable(int block, int sm, int on);
int pio_port_put(int block, int sm, uint32_t word);          /* 0, or -1 full */
int pio_port_get(int block, int sm, uint32_t *word);         /* 0, or -1 empty */
void pio_port_levels(int block, int sm, uint32_t *tx, uint32_t *rx);

/* sources: bit 0 RX not empty, bit 1 TX not full, bit 2 instruction flags.
 * instr_flags is bits 0-3 of this block. */
void pio_port_irq_arm(int block, int line, int sm, uint32_t sources, uint32_t instr_flags);
void pio_port_irq_disarm(int block, int line);
void pio_port_irq_unmask(int block, int line);

void pio_port_gpio_init(int block, int pin);
void pio_port_gpio_release(int pin);

#endif
