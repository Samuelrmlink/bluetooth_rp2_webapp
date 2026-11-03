#ifndef BTGW_PIO_DMA_H
#define BTGW_PIO_DMA_H
/*
 * Paced DMA into a PIO TX FIFO the caller already owns.
 * pio_dma.c keeps the arm / idle / abort state. The port claims the
 * channel and programs the transfer. The interrupt only acknowledges
 * the channel and starts an armed section. The module hears about it
 * from pio_dma_poll, on the main loop.
 */
#include <stdint.h>

typedef void (*pio_dma_fn)(void *arg);

/* rw is the inclusive start of the caller's RAM and rw_end is one past
 * the end. src must lie in that range, 4-byte aligned. nwords is 1..65535.
 * 0 or a PIO_E* from pio_res.h. */
int pio_dma_tx(int handle, const char *owner, const uint32_t *src, uint32_t nwords,
               uintptr_t rw, uintptr_t rw_end);
int pio_dma_arm(int handle, const char *owner, const uint32_t *src, uint32_t nwords,
                uintptr_t rw, uintptr_t rw_end);
/* 1 when this handle has nothing running and nothing armed. */
int pio_dma_idle(int handle, const char *owner);
/* Abort a color transfer, resync the machine, and shift nwords zeros. */
int pio_dma_zero(int handle, const char *owner, uint32_t nwords);
/* Completion runs on the main loop. fn NULL clears it. */
int pio_dma_listen(int handle, const char *owner, pio_dma_fn fn, void *arg, void *mod);

const char *pio_dma_last_error(void);

void pio_dma_abort(int handle);
void pio_dma_abort_owner(const char *owner);
/* The port calls this from the DMA interrupt after it has acknowledged. */
void pio_dma_hw_done(int ch);
void pio_dma_poll(void (*deliver)(void *mod, pio_dma_fn fn, void *arg));

#endif
