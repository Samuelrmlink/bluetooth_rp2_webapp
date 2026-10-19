#ifndef BTGW_SPIBUS_H
#define BTGW_SPIBUS_H

/*
 * The SPI bus layer. Every SPI device in this firmware goes through it; the
 * tier-0 guard in tools/run_tests.py fails the build if anything outside
 * src/io/spibus*.c touches the SDK's spi_* calls.
 *
 * Three rules, and why:
 *
 *   EXCLUSIVE. A bus is claimed through a semaphore (the SDK's semaphore_t,
 *   which is safe across cores and from an IRQ) before any transfer, and a
 *   claim reconfigures the clock and mode for the device that holds it. Two
 *   devices on one bus can then never interleave, and a second core can be
 *   given a device later without revisiting any of this.
 *
 *   IN THE BACKGROUND. Data moves by DMA. A transfer is a CHAIN of segments
 *   (header, then payload pieces) with CS held low across all of it, so a
 *   chip that frames on CS -- the W5500 -- sees one transaction. The DMA IRQ
 *   only starts the next segment; the caller's completion callback runs from
 *   spibus_poll() in the main loop, never in the IRQ and never inside
 *   cyw43_wait.c, so lwIP stays single-threaded.
 *
 *   NON-BLOCKING. spibus_try_claim() and spibus_xfer_async() return at once;
 *   drivers are state machines stepped from the loop. The blocking variants
 *   exist for init and chip probing only.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "io/spibus_port.h"

#define SPIBUS_COUNT    2        /* hardware SPI blocks on the RP2350 */
#define SPIBUS_MAX_SEGS 6

typedef void (*spibus_done_fn)(void *ctx, bool ok);

/* One piece of a transfer. tx NULL clocks out the device's fill byte (0x00
 * unless set: an SD card wants 0xFF); rx NULL discards. */
typedef struct {
    const uint8_t *tx;
    uint8_t       *rx;
    uint16_t       len;
} spiseg_t;

struct spibus;

/* A device on a bus: its chip select, clock and mode. CS is a plain GPIO
 * driven by this layer, so a bus can carry several devices. */
typedef struct spidev {
    struct spibus *bus;
    int8_t         cs;
    uint8_t        mode;         /* SPI mode 0..3 */
    uint32_t       baud;
    uint8_t        fill;         /* clocked when a segment has no tx (default 0x00) */
    const char    *name;
} spidev_t;

typedef struct spibus {
    bool           present;
    uint8_t        index;
    int8_t         sck, mosi, miso;
    const char    *owner;        /* who brought the bus up ("w5500") */
    uint8_t        users;        /* spibus_init()s not yet spibus_deinit()ed */
    spibus_sem_t   sem;
    const spidev_t *holder;      /* the device that holds the claim */
    uint32_t       cur_baud;
    int8_t         cur_mode;
    int            dma_tx, dma_rx;

    /* The transfer in flight. segs/nseg are fixed while busy; seg_i is
     * advanced by the IRQ. */
    spiseg_t       segs[SPIBUS_MAX_SEGS];
    uint8_t        nseg;
    volatile uint8_t seg_i;
    volatile bool  busy;
    volatile bool  done;
    uint8_t        cs_high_mask; /* bit i: segment i is clocked with CS high */
    spibus_done_fn cb;
    void          *cb_ctx;

    uint8_t        zero_tx;      /* source when a segment has no tx */
    uint8_t        sink_rx;      /* destination when it has no rx */

    uint32_t       xfers, bytes, claim_busy;
} spibus_t;

/* Bring a hardware SPI block up on the given pins (reserving them in the pin
 * broker when it is built). A second caller with the same pins shares the bus
 * (counted: each spibus_init() is paired with one spibus_deinit()); different
 * pins on a bus that is already up are refused. NULL on failure. */
spibus_t *spibus_init(int index, int sck, int mosi, int miso, const char *owner);
spibus_t *spibus_get(int index);   /* NULL unless spibus_init()ed */
/* Drop one user of the bus (its device was not there). The block is closed
 * and its DMA channels and pins given back only when the LAST user leaves, so
 * an absent W5500 cannot take down an SD card's shared bus. */
void spibus_deinit(spibus_t *bus);
void spibus_dev_deinit(spidev_t *dev);

/* Attach a device. Configures its CS high and reserves the pin. */
bool spibus_dev_init(spidev_t *dev, spibus_t *bus, int cs, uint32_t baud,
                     uint8_t mode, const char *name);

bool spibus_try_claim(spidev_t *dev);                      /* never waits */
bool spibus_claim_blocking(spidev_t *dev, uint32_t timeout_ms);   /* init only */
void spibus_release(spidev_t *dev);
/* Release after clocking `n` fill bytes with this device's CS high, still
 * holding the claim: an SD card keeps driving MISO until it sees a clock
 * with CS high, so releasing without it would corrupt the next device's
 * first read on a shared bus. False (claim kept) if not held or busy. */
bool spibus_release_trailing(spidev_t *dev, uint16_t n);
bool spibus_holds(const spidev_t *dev);

/* Start a chained transfer; only while claimed and not already busy. The
 * segments are COPIED (the descriptors, not the data: the buffers must stay
 * valid until the callback). cb runs from spibus_poll(). */
bool spibus_xfer_async(spidev_t *dev, const spiseg_t *segs, int nseg,
                       spibus_done_fn cb, void *ctx);
/* The same, with bit i of cs_high_mask clocking segment i with the device's
 * CS HIGH: an SD card's power-up clocks, and the byte after a command that
 * makes it let go of MISO. Still only while claimed. */
bool spibus_xfer_async_cs(spidev_t *dev, const spiseg_t *segs, int nseg, uint8_t cs_high_mask,
                          spibus_done_fn cb, void *ctx);
bool spibus_busy(const spidev_t *dev);

/* The same, waiting for completion (with a timeout). Init and probe only. */
bool spibus_xfer_blocking(spidev_t *dev, const spiseg_t *segs, int nseg);
bool spibus_xfer_blocking_cs(spidev_t *dev, const spiseg_t *segs, int nseg, uint8_t cs_high_mask);

/* Main loop: run completion callbacks. */
void spibus_poll(void);

/* From the port's DMA IRQ (or a host test): the receive DMA of `bus` has
 * finished its segment. Starts the next one or completes the transfer. */
void spibus_dma_done(spibus_t *bus);

/* The spi.xfer op: a one-off blocking transfer on a bus NO driver has
 * brought up. Refuses a bus that spibus_init() owns and pins that are
 * reserved. */
bool spibus_adhoc_xfer(int index, int sck, int mosi, int miso, int cs, uint32_t hz,
                       const uint8_t *tx, uint8_t *rx, size_t len, const char **err);

size_t spibus_status_json(char *dst, size_t cap);

#endif
