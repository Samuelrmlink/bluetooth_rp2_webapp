#ifndef BTGW_SPIBUS_PORT_H
#define BTGW_SPIBUS_PORT_H

/*
 * What spibus.c needs from the platform. src/io/spibus_pico.c implements it
 * on the RP2350 (hardware_spi + two DMA channels + DMA_IRQ_1); the host tests
 * in test/host implement it with a fake DMA and, for the W5500, a simulated
 * chip -- which is why the bus logic and the drivers above it never include
 * an SDK header themselves.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef SPIBUS_HOST
typedef struct { volatile int permits; } spibus_sem_t;
#else
#include "pico/sem.h"
typedef semaphore_t spibus_sem_t;
#endif

struct spibus;

void     spibus_port_sem_init(spibus_sem_t *s);
bool     spibus_port_sem_try(spibus_sem_t *s);
bool     spibus_port_sem_take(spibus_sem_t *s, uint32_t timeout_ms);
void     spibus_port_sem_give(spibus_sem_t *s);

/* Bring the block up on its pins (and claim its two DMA channels). */
bool     spibus_port_open(struct spibus *bus);
void     spibus_port_format(struct spibus *bus, uint32_t baud, uint8_t mode);
/* Start one segment: both channels, the receive one raising the IRQ that
 * ends in spibus_dma_done(). tx/rx may be NULL (see spiseg_t). */
void     spibus_port_dma_start(struct spibus *bus, const uint8_t *tx, uint8_t *rx, uint16_t len);
/* Stop both channels and drain the FIFO (a blocking transfer that timed out). */
void     spibus_port_dma_abort(struct spibus *bus);

void     spibus_port_gpio_out(int pin, bool level);   /* configure + drive */
void     spibus_port_gpio_put(int pin, bool level);
void     spibus_port_gpio_in(int pin, bool pull_up);
bool     spibus_port_gpio_get(int pin);
void     spibus_port_delay_us(uint32_t us);
uint32_t spibus_port_now_ms(void);

/* Pin broker hook: reserve a pin for a system driver. True when the pin was
 * free (or the broker is not built). */
bool     spibus_port_reserve_pin(int pin, const char *owner, bool is_spi);

/* spi.xfer's one-off transfer, on a bus no driver owns. */
bool     spibus_port_adhoc(int index, int sck, int mosi, int miso, int cs, uint32_t hz,
                           const uint8_t *tx, uint8_t *rx, size_t len);

#endif
