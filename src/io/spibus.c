/*
 * The SPI bus layer: claim, chain, complete. See spibus.h for the rules.
 *
 * Platform-free on purpose (everything hardware is behind spibus_port.h), so
 * test/host/spibus_test.c runs this exact file against a fake DMA.
 */
#include "io/spibus.h"

#include <stdio.h>
#include <string.h>

static spibus_t buses[SPIBUS_COUNT];

spibus_t *spibus_get(int index) {
    if (index < 0 || index >= SPIBUS_COUNT || !buses[index].present) { return NULL; }
    return &buses[index];
}

spibus_t *spibus_init(int index, int sck, int mosi, int miso, const char *owner) {
    if (index < 0 || index >= SPIBUS_COUNT) { return NULL; }
    spibus_t *b = &buses[index];
    if (b->present) {
        if (b->sck == sck && b->mosi == mosi && b->miso == miso) { return b; }
        printf("spibus: spi%d is already up on other pins\n", index);
        return NULL;
    }
    memset(b, 0, sizeof *b);
    b->index = (uint8_t)index;
    b->sck = (int8_t)sck;
    b->mosi = (int8_t)mosi;
    b->miso = (int8_t)miso;
    b->owner = owner ? owner : "spibus";
    b->cur_mode = -1;
    b->dma_tx = b->dma_rx = -1;
    if (!spibus_port_reserve_pin(sck, b->owner, true) ||
        !spibus_port_reserve_pin(mosi, b->owner, true) ||
        !spibus_port_reserve_pin(miso, b->owner, true)) {
        printf("spibus: spi%d pins %d/%d/%d are not free\n", index, sck, mosi, miso);
        return NULL;
    }
    spibus_port_sem_init(&b->sem);
    if (!spibus_port_open(b)) {
        printf("spibus: spi%d could not be opened (no DMA channel?)\n", index);
        return NULL;
    }
    b->present = true;
    return b;
}

bool spibus_dev_init(spidev_t *dev, spibus_t *bus, int cs, uint32_t baud,
                     uint8_t mode, const char *name) {
    if (!dev || !bus || !bus->present) { return false; }
    memset(dev, 0, sizeof *dev);
    dev->bus = bus;
    dev->cs = (int8_t)cs;
    dev->baud = baud;
    dev->mode = mode & 3;
    dev->name = name ? name : "?";
    if (cs >= 0) {
        if (!spibus_port_reserve_pin(cs, dev->name, false)) { return false; }
        spibus_port_gpio_out(cs, true);
    }
    return true;
}

static void took(spidev_t *dev) {
    spibus_t *b = dev->bus;
    b->holder = dev;
    if (b->cur_baud != dev->baud || b->cur_mode != (int8_t)dev->mode) {
        spibus_port_format(b, dev->baud, dev->mode);
        b->cur_baud = dev->baud;
        b->cur_mode = (int8_t)dev->mode;
    }
}

bool spibus_try_claim(spidev_t *dev) {
    if (!dev || !dev->bus) { return false; }
    if (dev->bus->holder == dev) { return true; }
    if (!spibus_port_sem_try(&dev->bus->sem)) {
        dev->bus->claim_busy++;
        return false;
    }
    took(dev);
    return true;
}

bool spibus_claim_blocking(spidev_t *dev, uint32_t timeout_ms) {
    if (!dev || !dev->bus) { return false; }
    if (dev->bus->holder == dev) { return true; }
    if (!spibus_port_sem_take(&dev->bus->sem, timeout_ms)) { return false; }
    took(dev);
    return true;
}

bool spibus_holds(const spidev_t *dev) {
    return dev && dev->bus && dev->bus->holder == dev;
}

void spibus_release(spidev_t *dev) {
    if (!spibus_holds(dev)) { return; }
    spibus_t *b = dev->bus;
    /* Releasing mid-transfer would hand the bus over with CS still low; the
     * transfer owns the bus until its callback has run. */
    if (b->busy) { return; }
    b->holder = NULL;
    spibus_port_sem_give(&b->sem);
}

bool spibus_busy(const spidev_t *dev) {
    return dev && dev->bus && dev->bus->busy;
}

/* Start segment seg_i, skipping empty ones. False when there is none left. */
static bool start_from(spibus_t *b) {
    while (b->seg_i < b->nseg && b->segs[b->seg_i].len == 0) { b->seg_i++; }
    if (b->seg_i >= b->nseg) { return false; }
    const spiseg_t *s = &b->segs[b->seg_i];
    spibus_port_dma_start(b, s->tx, s->rx, s->len);
    return true;
}

bool spibus_xfer_async(spidev_t *dev, const spiseg_t *segs, int nseg,
                       spibus_done_fn cb, void *ctx) {
    if (!spibus_holds(dev) || nseg <= 0 || nseg > SPIBUS_MAX_SEGS) { return false; }
    spibus_t *b = dev->bus;
    if (b->busy) { return false; }
    memcpy(b->segs, segs, (size_t)nseg * sizeof *segs);
    b->nseg = (uint8_t)nseg;
    b->seg_i = 0;
    b->cb = cb;
    b->cb_ctx = ctx;
    b->done = false;
    b->busy = true;
    b->xfers++;
    for (int i = 0; i < nseg; i++) { b->bytes += segs[i].len; }
    if (dev->cs >= 0) { spibus_port_gpio_put(dev->cs, false); }
    if (!start_from(b)) {
        /* Nothing to clock: complete through the loop like any other. */
        if (dev->cs >= 0) { spibus_port_gpio_put(dev->cs, true); }
        b->done = true;
    }
    return true;
}

/* IRQ context (or a test standing in for it): register writes only. */
void spibus_dma_done(spibus_t *b) {
    if (!b || !b->busy || b->done) { return; }
    b->seg_i++;
    if (start_from(b)) { return; }
    if (b->holder && b->holder->cs >= 0) { spibus_port_gpio_put(b->holder->cs, true); }
    b->done = true;
}

void spibus_poll(void) {
    for (int i = 0; i < SPIBUS_COUNT; i++) {
        spibus_t *b = &buses[i];
        if (!b->present || !b->busy || !b->done) { continue; }
        b->done = false;
        b->busy = false;
        spibus_done_fn cb = b->cb;
        b->cb = NULL;
        if (cb) { cb(b->cb_ctx, true); }
    }
}

#define SPIBUS_BLOCKING_TIMEOUT_MS 50

static void blocking_done(void *ctx, bool ok) { *(int *)ctx = ok ? 1 : -1; }

bool spibus_xfer_blocking(spidev_t *dev, const spiseg_t *segs, int nseg) {
    volatile int result = 0;
    if (!spibus_xfer_async(dev, segs, nseg, blocking_done, (void *)&result)) { return false; }
    spibus_t *b = dev->bus;
    uint32_t t0 = spibus_port_now_ms();
    while (!b->done) {
        if ((uint32_t)(spibus_port_now_ms() - t0) > SPIBUS_BLOCKING_TIMEOUT_MS) {
            printf("spibus: spi%u blocking transfer timed out\n", b->index);
            /* Leave the bus usable: a transfer that never completes must not
             * pin it busy (and CS low) for the life of the boot. */
            spibus_port_dma_abort(b);
            if (dev->cs >= 0) { spibus_port_gpio_put(dev->cs, true); }
            b->busy = false;
            b->done = false;
            b->cb = NULL;
            return false;
        }
        spibus_port_delay_us(1);
    }
    /* Complete just this bus, here, rather than spibus_poll() running every
     * other bus's callback from inside an init path. */
    b->done = false;
    b->busy = false;
    b->cb = NULL;
    blocking_done((void *)&result, true);
    return result == 1;
}

bool spibus_adhoc_xfer(int index, int sck, int mosi, int miso, int cs, uint32_t hz,
                       const uint8_t *tx, uint8_t *rx, size_t len, const char **err) {
    if (index < 0 || index >= SPIBUS_COUNT) { *err = "bus is 0 or 1"; return false; }
    if (buses[index].present) {
        static char msg[64];
        snprintf(msg, sizeof msg, "spi%d is in use by %s", index, buses[index].owner);
        *err = msg;
        return false;
    }
    if (!spibus_port_adhoc(index, sck, mosi, miso, cs, hz, tx, rx, len)) {
        *err = "a pin is reserved";
        return false;
    }
    return true;
}

size_t spibus_status_json(char *dst, size_t cap) {
    size_t n = (size_t)snprintf(dst, cap, "[");
    bool first = true;
    for (int i = 0; i < SPIBUS_COUNT && n + 160 < cap; i++) {
        const spibus_t *b = &buses[i];
        if (!b->present) { continue; }
        n += (size_t)snprintf(dst + n, cap - n,
            "%s{\"bus\":%d,\"owner\":\"%s\",\"sck\":%d,\"mosi\":%d,\"miso\":%d,"
            "\"baud\":%lu,\"holder\":\"%s\",\"busy\":%s,\"xfers\":%lu,\"bytes\":%lu,"
            "\"claim_busy\":%lu}",
            first ? "" : ",", i, b->owner, b->sck, b->mosi, b->miso,
            (unsigned long)b->cur_baud, b->holder ? b->holder->name : "",
            b->busy ? "true" : "false", (unsigned long)b->xfers,
            (unsigned long)b->bytes, (unsigned long)b->claim_busy);
        first = false;
    }
    return n + (size_t)snprintf(dst + n, cap - n, "]");
}
