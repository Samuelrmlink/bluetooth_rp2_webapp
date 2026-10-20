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
        if (b->sck == sck && b->mosi == mosi && b->miso == miso) {
            b->users++;
            return b;
        }
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
    b->users = 1;
    return b;
}

void spibus_deinit(spibus_t *b) {
    if (!b || !b->present || b->busy) { return; }
    if (b->users > 1) {          /* another device still uses the bus */
        b->users--;
        return;
    }
    spibus_port_close(b);
    spibus_port_release_pin(b->sck, b->owner);
    spibus_port_release_pin(b->mosi, b->owner);
    spibus_port_release_pin(b->miso, b->owner);
    memset(b, 0, sizeof *b);
}

void spibus_dev_deinit(spidev_t *dev) {
    if (!dev || !dev->bus) { return; }
    spibus_release(dev);
    if (dev->slot < SPIBUS_MAX_DEVS) {
        spibus_t *b = dev->bus;
        b->devs[dev->slot] = NULL;
        b->ticket[dev->slot] = 0;
        b->want_irq[dev->slot] = 0;
    }
    if (dev->cs >= 0) { spibus_port_release_pin(dev->cs, dev->name); }
    memset(dev, 0, sizeof *dev);
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
    dev->slot = 0xFF;
    for (uint8_t i = 0; i < SPIBUS_MAX_DEVS; i++) {   /* re-initialised: its old slot */
        if (bus->devs[i] == dev) { bus->devs[i] = NULL; }
    }
    for (uint8_t i = 0; i < SPIBUS_MAX_DEVS; i++) {
        if (!bus->devs[i]) {
            bus->devs[i] = dev;
            bus->ticket[i] = 0;
            bus->want_irq[i] = 0;
            dev->slot = i;
            break;
        }
    }
    if (dev->slot == 0xFF) { printf("spibus: spi%u has no fairness slot for %s\n", bus->index, dev->name); }
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

/* --- fairness ------------------------------------------------------------
 *
 * Tickets in arrival order; the oldest live one goes first. Main-loop
 * context only, except want_irq (a byte, set by spibus_want_isr). */

static bool has_slot(const spidev_t *dev) { return dev->slot < SPIBUS_MAX_DEVS; }

static void take_ticket(spibus_t *b, uint8_t i, uint32_t now) {
    if (!b->ticket[i]) {
        if (++b->next_ticket == 0) { b->next_ticket = 1; }   /* 0 means none */
        b->ticket[i] = b->next_ticket;
    }
    b->want_ms[i] = now;
}

/* Interrupt wants become tickets; wants not renewed within the TTL lapse. */
static void settle_wants(spibus_t *b, uint32_t now) {
    for (uint8_t i = 0; i < SPIBUS_MAX_DEVS; i++) {
        if (b->want_irq[i]) {
            b->want_irq[i] = 0;
            if (b->devs[i]) { take_ticket(b, i, now); }
        }
        if (b->ticket[i] && (uint32_t)(now - b->want_ms[i]) > SPIBUS_WANT_TTL_MS) { b->ticket[i] = 0; }
    }
}

/* Is some device other than `dev` ahead of it in line? A device with no
 * ticket stands behind everyone who has one. Tickets compare by age, so a
 * wrapped counter still orders correctly. */
static bool someone_ahead(spibus_t *b, const spidev_t *dev) {
    uint32_t mine = has_slot(dev) ? b->ticket[dev->slot] : 0;
    for (uint8_t i = 0; i < SPIBUS_MAX_DEVS; i++) {
        if (i == dev->slot || !b->ticket[i]) { continue; }
        if (!mine || (int32_t)(b->ticket[i] - mine) < 0) { return true; }
    }
    return false;
}

void spibus_want(spidev_t *dev) {
    if (!dev || !dev->bus || !has_slot(dev)) { return; }
    take_ticket(dev->bus, dev->slot, spibus_port_now_ms());
}

void spibus_want_isr(spidev_t *dev) {
    if (dev && dev->bus && has_slot(dev)) { dev->bus->want_irq[dev->slot] = 1; }
}

void spibus_unwant(spidev_t *dev) {
    if (!dev || !dev->bus || !has_slot(dev)) { return; }
    dev->bus->ticket[dev->slot] = 0;
    dev->bus->want_irq[dev->slot] = 0;
}

bool spibus_contended(const spidev_t *dev) {
    if (!dev || !dev->bus) { return false; }
    spibus_t *b = dev->bus;
    settle_wants(b, spibus_port_now_ms());
    for (uint8_t i = 0; i < SPIBUS_MAX_DEVS; i++) {
        if (i != dev->slot && b->ticket[i]) { return true; }
    }
    return false;
}

bool spibus_try_claim(spidev_t *dev) {
    if (!dev || !dev->bus) { return false; }
    spibus_t *b = dev->bus;
    if (b->holder == dev) { return true; }
    uint32_t now = spibus_port_now_ms();
    settle_wants(b, now);
    if (someone_ahead(b, dev)) {
        /* The bus may well be free: it is someone else's turn. */
        if (has_slot(dev)) { take_ticket(b, dev->slot, now); }
        b->claim_yield++;
        return false;
    }
    if (!spibus_port_sem_try(&b->sem)) {
        if (has_slot(dev)) { take_ticket(b, dev->slot, now); }
        b->claim_busy++;
        return false;
    }
    if (has_slot(dev)) { b->ticket[dev->slot] = 0; }
    took(dev);
    return true;
}

bool spibus_claim_blocking(spidev_t *dev, uint32_t timeout_ms) {
    if (!dev || !dev->bus) { return false; }
    if (dev->bus->holder == dev) { return true; }
    if (!spibus_port_sem_take(&dev->bus->sem, timeout_ms)) { return false; }
    if (has_slot(dev)) { dev->bus->ticket[dev->slot] = 0; }
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

bool spibus_release_trailing(spidev_t *dev, uint16_t n) {
    if (!spibus_holds(dev) || dev->bus->busy) { return false; }
    if (dev->cs >= 0) { spibus_port_gpio_put(dev->cs, true); }
    if (n) {
        spiseg_t s = { NULL, NULL, n };
        if (!spibus_xfer_blocking_cs(dev, &s, 1, 1u)) { return false; }
    }
    spibus_release(dev);
    return true;
}

bool spibus_busy(const spidev_t *dev) {
    return dev && dev->bus && dev->bus->busy;
}

/* Start segment seg_i, skipping empty ones. False when there is none left. */
static bool start_from(spibus_t *b) {
    while (b->seg_i < b->nseg && b->segs[b->seg_i].len == 0) { b->seg_i++; }
    if (b->seg_i >= b->nseg) { return false; }
    const spiseg_t *s = &b->segs[b->seg_i];
    const spidev_t *d = b->holder;
    if (d) {
        b->zero_tx = d->fill;
        if (d->cs >= 0) { spibus_port_gpio_put(d->cs, (b->cs_high_mask >> b->seg_i) & 1u); }
    }
    spibus_port_dma_start(b, s->tx, s->rx, s->len);
    return true;
}

bool spibus_xfer_async(spidev_t *dev, const spiseg_t *segs, int nseg,
                       spibus_done_fn cb, void *ctx) {
    return spibus_xfer_async_cs(dev, segs, nseg, 0, cb, ctx);
}

bool spibus_xfer_async_cs(spidev_t *dev, const spiseg_t *segs, int nseg, uint8_t cs_high_mask,
                          spibus_done_fn cb, void *ctx) {
    if (!spibus_holds(dev) || nseg <= 0 || nseg > SPIBUS_MAX_SEGS) { return false; }
    spibus_t *b = dev->bus;
    if (b->busy) { return false; }
    memcpy(b->segs, segs, (size_t)nseg * sizeof *segs);
    b->nseg = (uint8_t)nseg;
    b->seg_i = 0;
    b->cs_high_mask = cs_high_mask;
    b->cb = cb;
    b->cb_ctx = ctx;
    b->done = false;
    b->busy = true;
    b->xfers++;
    for (int i = 0; i < nseg; i++) { b->bytes += segs[i].len; }
    /* start_from() sets CS for each segment (low unless cs_high). */
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
    return spibus_xfer_blocking_cs(dev, segs, nseg, 0);
}

bool spibus_xfer_blocking_cs(spidev_t *dev, const spiseg_t *segs, int nseg, uint8_t cs_high_mask) {
    volatile int result = 0;
    if (!spibus_xfer_async_cs(dev, segs, nseg, cs_high_mask, blocking_done, (void *)&result)) { return false; }
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
    for (int i = 0; i < SPIBUS_COUNT && n + 190 < cap; i++) {
        const spibus_t *b = &buses[i];
        if (!b->present) { continue; }
        n += (size_t)snprintf(dst + n, cap - n,
            "%s{\"bus\":%d,\"owner\":\"%s\",\"sck\":%d,\"mosi\":%d,\"miso\":%d,"
            "\"baud\":%lu,\"holder\":\"%s\",\"busy\":%s,\"xfers\":%lu,\"bytes\":%lu,"
            "\"claim_busy\":%lu,\"claim_yield\":%lu}",
            first ? "" : ",", i, b->owner, b->sck, b->mosi, b->miso,
            (unsigned long)b->cur_baud, b->holder ? b->holder->name : "",
            b->busy ? "true" : "false", (unsigned long)b->xfers,
            (unsigned long)b->bytes, (unsigned long)b->claim_busy,
            (unsigned long)b->claim_yield);
        first = false;
    }
    return n + (size_t)snprintf(dst + n, cap - n, "]");
}
