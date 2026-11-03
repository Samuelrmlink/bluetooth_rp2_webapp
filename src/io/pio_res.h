#ifndef BTGW_PIO_RES_H
#define BTGW_PIO_RES_H
/*
 * PIO claim ledger for the SDIO driver and for PEX modules.
 *
 * INSTR_MEM is write-only and the hardware does not remember where one
 * program ends. Every word, its offset, the origin the caller asked for,
 * the owner, the machines executing it, and the IRQ flags scanned from
 * those words live here. stat / programs / instr read this record.
 *
 * The numeric codes match pex.h. This header does not include it, so a
 * host test can build the ledger without the generated API header.
 * A refusal returns one of these and prints one "pio <owner>: ..." line.
 * Success prints nothing.
 */
#include <stddef.h>
#include <stdint.h>

#define PIO_OK        0
#define PIO_EINVAL    (-1)
#define PIO_ENOMEM    (-2)
#define PIO_EBUSY     (-10)
#define PIO_ENOENT    (-11)
#define PIO_ENOTSUP   (-16)
#define PIO_EAGAIN    (-20)

#define PIO_IRQ_RX    1u     /* this machine's RX FIFO is not empty */
#define PIO_IRQ_TX    2u     /* this machine's TX FIFO is not full */
#define PIO_IRQ_FLAG  4u     /* instruction flags this program references */
#define PIO_JOIN_NONE 0u
#define PIO_JOIN_TX   1u
#define PIO_JOIN_RX   2u

typedef struct {
    int32_t gpio_base;       /* -1 until a machine sets it, else 0 or 16 */
    uint32_t instr_used;
    uint32_t instr_free;
    uint32_t instr_map;      /* bit i set when instruction word i is taken */
    uint32_t sm_free;        /* bit i set when state machine i is free */
    uint32_t irq_used;       /* instruction flags 0-3 a loaded word references */
    uint32_t irq_line_used;  /* bit i set when CPU IRQ line i has a handler */
} pio_res_block_t;

typedef struct {
    uint32_t offset;
    uint32_t length;
    int32_t origin;          /* -1 when the loader picked the offset */
    uint32_t sm_mask;        /* machines on this block executing the copy */
    uint32_t irq_flags;      /* bits 0-3 this block, bits 4-7 the next block's 0-3 */
    char owner[24];
    uint32_t prot;           /* 1: this copy must keep running */
} pio_res_prog_t;

typedef struct {
    uint8_t out_base, out_count;
    uint8_t set_base, set_count;
    uint8_t sideset_base, sideset_count, sideset_opt;
    uint8_t shift_right;     /* 1: shift both directions right */
    uint8_t autopush, autopull;
    uint8_t push_thresh, pull_thresh; /* 0 means 32 */
    uint8_t fifo_join;       /* PIO_JOIN_* */
    int16_t in_base;         /* -1: do not set the input base */
    int16_t jmp_pin;         /* -1: do not set the jump pin */
    uint32_t clkdiv;         /* 16.8 fixed point. 0 and 0x100 both mean 1.0.
                              * Bits 23:8 are the integer, bits 7:0 the fraction. */
} pio_res_cfg_t;

typedef void (*pio_res_irq_fn)(void *arg);

/* *n is how many blocks the chip has. The array receives min(cap, *n). */
int pio_res_stat(pio_res_block_t *out, size_t cap, size_t *n);
int pio_res_programs(int block, pio_res_prog_t *out, size_t cap, size_t *n);
/* Words copied out of the shadow, or the length when cap is 0.
 * PIO_ENOENT when offset is not the start of a loaded range. */
int pio_res_instr(int block, uint32_t offset, uint16_t *words, size_t cap);

/* Block index a claim would pick, or the same PIO_E* claim would return.
 * Does not take anything. */
int pio_res_fit(const char *owner, const uint8_t *pins, size_t npins,
                uint32_t nwords, int origin);

/* Handle (>= 1) or PIO_E*. origin -1 is any free run. An origin that names
 * a range of the same length already reserved on a fitting block attaches
 * this machine to that copy and does not take new words. */
int pio_res_claim(const char *owner, const uint8_t *pins, size_t npins,
                  uint32_t nwords, int origin);
/* Place words in a free gap and allocate a machine. Never attaches to a
 * copy that is already loaded, and never writes a word that copy owns.
 * PIO_LOAD_PROTECT: release, rewrite, reconfigure, FIFO, and IRQ changes
 * are refused, and the block keeps its GPIO base. PIO_LOAD_TOP: prefer the
 * highest block, which is where the radio's own allocator used to look. */
#define PIO_LOAD_PROTECT 1u
#define PIO_LOAD_TOP     2u
int pio_res_load(const char *owner, const uint8_t *pins, size_t npins,
                 const uint16_t *words, size_t nwords, uint32_t flags);
int pio_res_program(int handle, const uint16_t *words, size_t nwords);
int pio_res_init(int handle, const pio_res_cfg_t *cfg);
int pio_res_enable(int handle, int on);
int pio_res_put(int handle, uint32_t word);          /* PIO_EAGAIN when full */
int pio_res_get(int handle, uint32_t *word);         /* PIO_EAGAIN when empty */
int pio_res_levels(int handle, uint32_t *tx, uint32_t *rx);
/* line is 0 or 1. A second handler on that line is PIO_EBUSY.
 * mod is an opaque module pointer the poll deliver hook receives. */
int pio_res_irq(int handle, int line, uint32_t sources,
                pio_res_irq_fn fn, void *arg, void *mod);
int pio_res_release(int handle);
/* The radio's own deinit. Drops that machine even when it is protected.
 * Returns 0 when a ledger machine was released, -1 when block/sm is not one. */
int pio_res_release_hw(int block, int sm);
/* Drop every machine that owner holds. "sd" is left alone, and so is a
 * protected machine: unloading a module must not stop the card or the radio. */
void pio_res_release_owner(const char *owner);

/* PIO_OK when handle belongs to owner. One error line otherwise. */
int pio_res_check(int handle, const char *owner);

int pio_res_block(int handle);     /* -1 when the handle is not live */
int pio_res_sm(int handle);
int pio_res_offset(int handle);
const char *pio_res_owner(int handle);
const char *pio_res_last_error(void);

/* The ISR sets a bit. The main loop delivers the callback. */
void pio_res_set_irq_bit(int block, int line);
void pio_res_poll(void (*deliver)(void *mod, pio_res_irq_fn fn, void *arg));

#endif
