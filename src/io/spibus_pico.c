/*
 * spibus_port.h on the RP2350: hardware_spi, two DMA channels per bus, and a
 * SHARED handler on DMA_IRQ_1.
 *
 * DMA_IRQ_1 and shared, not exclusive: the cyw43 PIO-SPI driver and BTstack
 * own DMA channels of their own (claimed through the same allocator, so there
 * is no clash over channels), and a shared handler costs nothing if anybody
 * else ever wants the line. The channels are claimed with
 * dma_claim_unused_channel() when the bus is opened, which main.c does after
 * cyw43_arch_init() so the radio has already taken what it needs.
 *
 * The receive channel signals the end of a segment: it completes only after
 * the last byte has been clocked IN, by which time the transmit side is
 * necessarily done too. Ending on the transmit channel would raise CS while
 * the final byte was still in the shifter.
 */
#include "io/spibus.h"

#include <stdio.h>

#include "autoconf.h"
#include "pico/stdlib.h"
#include "pico/sem.h"
#include "hardware/spi.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#if CONFIG_IO_PINS
#include "io/pins.h"
#endif

#define SPIBUS_DMA_IRQ 1      /* DMA_IRQ_1 */

static spi_inst_t *inst(int index) { return index == 1 ? spi1 : spi0; }

void spibus_port_sem_init(spibus_sem_t *s) { sem_init(s, 1, 1); }
bool spibus_port_sem_try(spibus_sem_t *s) { return sem_acquire_timeout_us(s, 0); }
bool spibus_port_sem_take(spibus_sem_t *s, uint32_t timeout_ms) { return sem_acquire_timeout_ms(s, timeout_ms); }
void spibus_port_sem_give(spibus_sem_t *s) { sem_release(s); }

static bool irq_installed;

static void dma_irq_handler(void) {
    for (int i = 0; i < SPIBUS_COUNT; i++) {
        spibus_t *b = spibus_get(i);
        if (!b || b->dma_rx < 0) { continue; }
        if (dma_irqn_get_channel_status(SPIBUS_DMA_IRQ, (uint)b->dma_rx)) {
            dma_irqn_acknowledge_channel(SPIBUS_DMA_IRQ, (uint)b->dma_rx);
            spibus_dma_done(b);
        }
    }
}

bool spibus_port_open(spibus_t *b) {
    spi_inst_t *s = inst(b->index);
    int tx = dma_claim_unused_channel(false);
    int rx = dma_claim_unused_channel(false);
    if (tx < 0 || rx < 0) {
        if (tx >= 0) { dma_channel_unclaim((uint)tx); }
        if (rx >= 0) { dma_channel_unclaim((uint)rx); }
        return false;
    }
    b->dma_tx = tx;
    b->dma_rx = rx;
    spi_init(s, 1000000);
    spi_set_format(s, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    b->cur_baud = 1000000;
    b->cur_mode = 0;
    gpio_set_function((uint)b->sck, GPIO_FUNC_SPI);
    gpio_set_function((uint)b->mosi, GPIO_FUNC_SPI);
    gpio_set_function((uint)b->miso, GPIO_FUNC_SPI);

    dma_irqn_set_channel_enabled(SPIBUS_DMA_IRQ, (uint)rx, true);
    if (!irq_installed) {
        irq_add_shared_handler(DMA_IRQ_1, dma_irq_handler,
                               PICO_SHARED_IRQ_HANDLER_DEFAULT_ORDER_PRIORITY);
        irq_set_enabled(DMA_IRQ_1, true);
        irq_installed = true;
    }
    printf("spibus: spi%u up (sck %d mosi %d miso %d), dma tx %d rx %d\n",
           b->index, b->sck, b->mosi, b->miso, tx, rx);
    return true;
}

void spibus_port_format(spibus_t *b, uint32_t baud, uint8_t mode) {
    spi_inst_t *s = inst(b->index);
    spi_set_baudrate(s, baud);
    spi_set_format(s, 8, (mode & 2) ? SPI_CPOL_1 : SPI_CPOL_0,
                   (mode & 1) ? SPI_CPHA_1 : SPI_CPHA_0, SPI_MSB_FIRST);
}

void spibus_port_dma_start(spibus_t *b, const uint8_t *tx, uint8_t *rx, uint16_t len) {
    spi_inst_t *s = inst(b->index);
    b->zero_tx = 0;

    dma_channel_config ct = dma_channel_get_default_config((uint)b->dma_tx);
    channel_config_set_transfer_data_size(&ct, DMA_SIZE_8);
    channel_config_set_dreq(&ct, spi_get_dreq(s, true));
    channel_config_set_read_increment(&ct, tx != NULL);
    channel_config_set_write_increment(&ct, false);
    dma_channel_configure((uint)b->dma_tx, &ct, &spi_get_hw(s)->dr,
                          tx ? tx : &b->zero_tx, len, false);

    dma_channel_config cr = dma_channel_get_default_config((uint)b->dma_rx);
    channel_config_set_transfer_data_size(&cr, DMA_SIZE_8);
    channel_config_set_dreq(&cr, spi_get_dreq(s, false));
    channel_config_set_read_increment(&cr, false);
    channel_config_set_write_increment(&cr, rx != NULL);
    dma_channel_configure((uint)b->dma_rx, &cr, rx ? rx : &b->sink_rx,
                          &spi_get_hw(s)->dr, len, false);

    /* Both at once, receive armed first in the same write. */
    dma_start_channel_mask((1u << (uint)b->dma_rx) | (1u << (uint)b->dma_tx));
}

void spibus_port_dma_abort(spibus_t *b) {
    dma_channel_abort((uint)b->dma_tx);
    dma_channel_abort((uint)b->dma_rx);
    /* An abort can leave the completion flag raised; clear it so the IRQ
     * does not finish a transfer that no longer exists. */
    dma_irqn_acknowledge_channel(SPIBUS_DMA_IRQ, (uint)b->dma_rx);
    spi_inst_t *s = inst(b->index);
    while (spi_is_readable(s)) { (void)spi_get_hw(s)->dr; }
}

void spibus_port_gpio_out(int pin, bool level) {
    gpio_init((uint)pin);
    gpio_put((uint)pin, level);
    gpio_set_dir((uint)pin, GPIO_OUT);
}
void spibus_port_gpio_put(int pin, bool level) { gpio_put((uint)pin, level); }
void spibus_port_gpio_in(int pin, bool pull_up) {
    gpio_init((uint)pin);
    gpio_set_dir((uint)pin, GPIO_IN);
    gpio_set_pulls((uint)pin, pull_up, false);
}
bool spibus_port_gpio_get(int pin) { return gpio_get((uint)pin); }
void spibus_port_delay_us(uint32_t us) { busy_wait_us_32(us); }
uint32_t spibus_port_now_ms(void) { return to_ms_since_boot(get_absolute_time()); }

bool spibus_port_reserve_pin(int pin, const char *owner, bool is_spi) {
#if CONFIG_IO_PINS
    if (pin < 0) { return true; }
    return pins_reserve((uint8_t)pin, is_spi ? PIN_FN_SPI : PIN_FN_GPIO, owner);
#else
    (void)pin; (void)owner; (void)is_spi;
    return true;
#endif
}

bool spibus_port_adhoc(int index, int sck, int mosi, int miso, int cs, uint32_t hz,
                       const uint8_t *tx, uint8_t *rx, size_t len) {
#if CONFIG_IO_PINS
    const int pins[4] = { sck, mosi, miso, cs };
    for (int i = 0; i < 4; i++) {
        if (pins[i] < 0 || pins[i] >= PIN_COUNT || pins_is_reserved((uint8_t)pins[i])) { return false; }
    }
#endif
    spi_inst_t *s = inst(index);
    spi_init(s, hz);
    gpio_set_function((uint)sck, GPIO_FUNC_SPI);
    gpio_set_function((uint)mosi, GPIO_FUNC_SPI);
    gpio_set_function((uint)miso, GPIO_FUNC_SPI);
    /* CS is an ordinary GPIO, not a hardware function, so a bus can carry
     * several devices. */
    spibus_port_gpio_out(cs, true);
    gpio_put((uint)cs, 0);
    spi_write_read_blocking(s, tx, rx, len);
    gpio_put((uint)cs, 1);
    return true;
}
