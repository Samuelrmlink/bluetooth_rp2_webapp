/*
 * spibus_port.h on the ESP32-C5. The only general-purpose controller is
 * SPI2 (index 1). Index 0 is the flash controller and is refused.
 *
 * CS is a GPIO, not a hardware CS pin, so a transfer of several segments
 * stays one chip-select for the W5500. The IDF driver finishes a segment
 * before spi_device_transmit() returns, and it cannot queue the next
 * segment from inside its own completion interrupt. spibus_dma_done()
 * therefore runs here, still in the task, with CS held low, and starts
 * the next segment before this function returns. The caller's completion
 * callback still runs from spibus_poll(), not from here.
 */
#include "io/spibus.h"

#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define SEG_MAX 2048

static uint8_t __attribute__((aligned(64))) zero_tx[SEG_MAX];
static uint8_t __attribute__((aligned(64))) sink_rx[SEG_MAX];

static spi_device_handle_t dev_of[SPIBUS_COUNT];

static bool add_dev(int index, uint32_t baud, uint8_t mode) {
    spi_device_interface_config_t dev = {
        .mode = mode & 3,
        .clock_source = SPI_CLK_SRC_DEFAULT,
        .clock_speed_hz = baud ? (int)baud : 1000000,
        .spics_io_num = -1,
        .queue_size = 1,
    };
    spi_device_handle_t h = NULL;
    esp_err_t e = spi_bus_add_device((spi_host_device_t)index, &dev, &h);
    if (e != ESP_OK) {
        printf("spibus: spi%d device not added (%s)\n", index, esp_err_to_name(e));
        return false;
    }
    dev_of[index] = h;
    return true;
}

void spibus_port_sem_init(spibus_sem_t *s) {
    SemaphoreHandle_t h = xSemaphoreCreateBinaryStatic(s);
    xSemaphoreGive(h);
}
bool spibus_port_sem_try(spibus_sem_t *s) {
    return xSemaphoreTake((SemaphoreHandle_t)s, 0) == pdTRUE;
}
bool spibus_port_sem_take(spibus_sem_t *s, uint32_t timeout_ms) {
    return xSemaphoreTake((SemaphoreHandle_t)s, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}
void spibus_port_sem_give(spibus_sem_t *s) { xSemaphoreGive((SemaphoreHandle_t)s); }

bool spibus_port_open(spibus_t *b) {
    if (!b || b->index != SPI2_HOST) {
        printf("spibus: spi%d is not the C5 general-purpose SPI (SPI2)\n", b ? b->index : -1);
        return false;
    }
    spi_bus_config_t bus = {
        .mosi_io_num = b->mosi,
        .miso_io_num = b->miso,
        .sclk_io_num = b->sck,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .data4_io_num = -1,
        .data5_io_num = -1,
        .data6_io_num = -1,
        .data7_io_num = -1,
        .max_transfer_sz = SEG_MAX,
        .flags = SPICOMMON_BUSFLAG_MASTER | SPICOMMON_BUSFLAG_GPIO_PINS,
    };
    esp_err_t e = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);
    if (e != ESP_OK) {
        printf("spibus: SPI2 did not start (%s)\n", esp_err_to_name(e));
        return false;
    }
    b->dma_tx = 0;
    b->dma_rx = 1;
    b->cur_baud = 1000000;
    b->cur_mode = 0;
    if (!add_dev(b->index, b->cur_baud, 0)) {
        spi_bus_free(SPI2_HOST);
        b->dma_tx = b->dma_rx = -1;
        return false;
    }
    printf("spibus: SPI2 up (sck %d mosi %d miso %d)\n", b->sck, b->mosi, b->miso);
    return true;
}

void spibus_port_format(spibus_t *b, uint32_t baud, uint8_t mode) {
    if (!b || b->index >= SPIBUS_COUNT) { return; }
    if (dev_of[b->index]) {
        spi_bus_remove_device(dev_of[b->index]);
        dev_of[b->index] = NULL;
    }
    if (!add_dev(b->index, baud, mode)) { return; }
    b->cur_baud = baud;
    b->cur_mode = (int8_t)mode;
}

void spibus_port_dma_start(spibus_t *b, const uint8_t *tx, uint8_t *rx, uint16_t len) {
    if (!b || b->index >= SPIBUS_COUNT || !dev_of[b->index] || len == 0 || len > SEG_MAX) {
        printf("spibus: segment refused (len %u)\n", (unsigned)len);
        if (b && b->holder && b->holder->cs >= 0) { spibus_port_gpio_put(b->holder->cs, true); }
        if (b) { b->done = true; }
        return;
    }
    spi_transaction_t tr;
    memset(&tr, 0, sizeof tr);
    tr.length = (size_t)len * 8u;
    tr.tx_buffer = tx ? tx : zero_tx;
    tr.rx_buffer = rx ? rx : sink_rx;
    esp_err_t e = spi_device_transmit(dev_of[b->index], &tr);
    if (e != ESP_OK) {
        printf("spibus: spi%d transfer failed (%s)\n", b->index, esp_err_to_name(e));
        if (b->holder && b->holder->cs >= 0) { spibus_port_gpio_put(b->holder->cs, true); }
        b->done = true;
        return;
    }
    /* Next segment, or raise CS and mark the chain done. Still this task. */
    spibus_dma_done(b);
}

void spibus_port_dma_abort(spibus_t *b) {
    if (!b || b->index >= SPIBUS_COUNT || !dev_of[b->index]) { return; }
    /* Transfers finish inside dma_start, so there is nothing queued to
     * cancel. Drop the device so a following claim reopens a clean one. */
    spi_bus_remove_device(dev_of[b->index]);
    dev_of[b->index] = NULL;
    b->cur_mode = -1;
}

static bool pin_ok(int pin) { return pin >= 0 && pin <= 28; }

void spibus_port_gpio_out(int pin, bool level) {
    if (!pin_ok(pin)) { return; }
    gpio_reset_pin((gpio_num_t)pin);
    gpio_set_direction((gpio_num_t)pin, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)pin, level);
}
void spibus_port_gpio_put(int pin, bool level) {
    if (pin_ok(pin)) { gpio_set_level((gpio_num_t)pin, level); }
}
void spibus_port_gpio_in(int pin, bool pull_up) {
    if (!pin_ok(pin)) { return; }
    gpio_reset_pin((gpio_num_t)pin);
    gpio_set_direction((gpio_num_t)pin, GPIO_MODE_INPUT);
    gpio_set_pull_mode((gpio_num_t)pin, pull_up ? GPIO_PULLUP_ONLY : GPIO_FLOATING);
}
bool spibus_port_gpio_get(int pin) {
    return pin_ok(pin) && gpio_get_level((gpio_num_t)pin);
}
void spibus_port_delay_us(uint32_t us) { esp_rom_delay_us(us); }
uint32_t spibus_port_now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

bool spibus_port_reserve_pin(int pin, const char *owner, bool is_spi) {
    (void)owner; (void)is_spi;
    return pin < 0 || pin_ok(pin);
}
void spibus_port_release_pin(int pin, const char *owner) {
    (void)owner;
    if (pin_ok(pin)) { gpio_reset_pin((gpio_num_t)pin); }
}

void spibus_port_close(spibus_t *b) {
    if (!b || b->index >= SPIBUS_COUNT) { return; }
    if (dev_of[b->index]) {
        spi_bus_remove_device(dev_of[b->index]);
        dev_of[b->index] = NULL;
    }
    if (b->index == SPI2_HOST) { spi_bus_free(SPI2_HOST); }
    b->dma_tx = b->dma_rx = -1;
}

bool spibus_port_adhoc(int index, int sck, int mosi, int miso, int cs, uint32_t hz,
                       const uint8_t *tx, uint8_t *rx, size_t len) {
    if (index != SPI2_HOST || len == 0 || len > SEG_MAX || !pin_ok(cs)) { return false; }
    spibus_t tmp;
    memset(&tmp, 0, sizeof tmp);
    tmp.index = (uint8_t)index;
    tmp.sck = (int8_t)sck;
    tmp.mosi = (int8_t)mosi;
    tmp.miso = (int8_t)miso;
    tmp.dma_tx = tmp.dma_rx = -1;
    if (!spibus_port_open(&tmp)) { return false; }
    spibus_port_format(&tmp, hz, 0);
    spibus_port_gpio_out(cs, false);
    bool ok = dev_of[index] != NULL;
    if (ok) {
        spi_transaction_t tr;
        memset(&tr, 0, sizeof tr);
        tr.length = len * 8u;
        tr.tx_buffer = tx ? tx : zero_tx;
        tr.rx_buffer = rx ? rx : sink_rx;
        ok = spi_device_transmit(dev_of[index], &tr) == ESP_OK;
    }
    spibus_port_gpio_put(cs, true);
    spibus_port_close(&tmp);
    return ok;
}
