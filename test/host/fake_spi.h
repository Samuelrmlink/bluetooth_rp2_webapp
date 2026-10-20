/* A fake spibus port (DMA that completes when the test says so) and a
 * simulated W5500 hanging off one of its chip selects. See fake_spi.c. */
#ifndef FAKE_SPI_H
#define FAKE_SPI_H

#include <stdbool.h>
#include <stdint.h>

#include "io/spibus.h"

/* --- the port ------------------------------------------------------------ */
typedef uint8_t (*fake_dev_fn)(void *ctx, int event, uint8_t mosi);
/* FAKE_IDLE_BYTE: a clock while this device's CS is high; return nonzero if
 * the device is still driving MISO then (a bus conflict). */
enum { FAKE_CS_LOW = 1, FAKE_CS_HIGH = 2, FAKE_BYTE = 3, FAKE_IDLE_BYTE = 4 };

void     fake_reset(void);
void     fake_attach(int cs, fake_dev_fn fn, void *ctx);
/* Complete every DMA segment the moment it starts (init-time blocking paths). */
void     fake_autocomplete(bool on);
int      fake_dma_starts(void);           /* segments started since reset */
int      fake_bus_conflicts(void);        /* bytes where a deselected device drove MISO */
int      fake_cs_overlaps(void);          /* segments clocked with two chip selects low */
/* Called as each segment starts, with its length: a test advances its clock
 * by the bytes' time on the wire. */
extern void (*fake_bytes_hook)(spibus_t *b, uint16_t n);
bool     fake_dma_pending(void);
void     fake_dma_complete(void);         /* the IRQ, for the bus in flight */
int      fake_gpio(int pin);              /* -1 never driven */
void     fake_set_input(int pin, bool level);
uint32_t fake_baud(int bus);
uint8_t  fake_mode(int bus);
void     fake_advance_ms(uint32_t ms);
uint32_t fake_now(void);
int      fake_reserved_count(void);
bool     fake_cs_glitched(void);          /* CS went high mid-transfer */

/* --- the simulated W5500 ------------------------------------------------- */
typedef struct {
    uint8_t  common[0x40];
    uint8_t  sreg[8][0x30];
    uint8_t  txbuf[16384];
    uint8_t  rxbuf[16384];
    uint16_t rx_wr;          /* chip's write pointer into rxbuf */
    bool     link;
    /* SPI framing */
    int      phase;          /* 0..2 header bytes, 3 = data */
    uint16_t addr;
    uint8_t  ctrl;
    /* what the chip "sent" */
    uint8_t  sent[32][1600];
    uint16_t sent_len[32];
    int      n_sent;
    int      dropped;        /* frames the ring had no room for */
    bool     defer_sendok;   /* leave SENDOK unset until the test sets it */
    int      int_pin;
} w5500_sim_t;

void     sim_init(w5500_sim_t *s, int cs, int int_pin);
bool     sim_inject(w5500_sim_t *s, const uint8_t *frame, uint16_t len);
void     sim_set_link(w5500_sim_t *s, bool up);
void     sim_sendok(w5500_sim_t *s);
uint16_t sim_rx_free(const w5500_sim_t *s);
/* Corrupt the next frame header in the ring (for the resync test). */
void     sim_corrupt_next(w5500_sim_t *s);

#endif
