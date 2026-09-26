/*
 * The I/O subsystems as one registry module: gpio, adc, pwm, i2c, spi, uart.
 *
 * Each is independently Kconfig-gated, and sys.info reports what actually
 * linked, so a client discovers capabilities rather than assuming them.
 * I2C and SPI exist mostly for driver bring-up -- i2c.scan is the reason --
 * so a deployed build that does not need them can drop them and reclaim flash.
 */
#include <stdio.h>
#include <string.h>

#include "api/ops.h"
#include "io/pins.h"
#include "autoconf.h"

#include "hardware/gpio.h"
#if CONFIG_IO_ADC
#include "hardware/adc.h"
#endif
#if CONFIG_IO_I2C
#include "hardware/i2c.h"
#endif
#if CONFIG_IO_SPI
#include "hardware/spi.h"
#endif
#if CONFIG_IO_UART
#include "hardware/uart.h"
#endif
#if CONFIG_IO_PWM
#include "hardware/pwm.h"
#endif

#if CONFIG_IO_PINS

static char io_buf[2048];

static bool pin_arg(const char *p, long *out) {
    return api_json_int(p, "pin", out) && *out >= 0 && *out < PIN_COUNT;
}

/* --- pins ---------------------------------------------------------------- */
static void op_pins_map(const char *params, api_reply_fn reply, void *ctx, long id) {
    (void)params;
    size_t n = (size_t)snprintf(io_buf, sizeof(io_buf), "{\"pins\":");
    n += pins_map_json(io_buf + n, sizeof(io_buf) - n);
    snprintf(io_buf + n, sizeof(io_buf) - n, "}");
    api_reply_ok(reply, ctx, id, io_buf);
}

static void op_pins_claim(const char *params, api_reply_fn reply, void *ctx, long id) {
    long pin;
    if (!pin_arg(params, &pin)) { api_reply_err(reply, ctx, id, "bad_request", "need \"pin\""); return; }
    const char *err = "unknown";
    if (!pins_claim((uint8_t)pin, PIN_FN_GPIO, PIN_OWNER_API, "", &err)) {
        api_reply_err(reply, ctx, id, "claim_refused", err); return;
    }
    api_reply_ok(reply, ctx, id, "{\"claimed\":true}");
}

static void op_pins_release(const char *params, api_reply_fn reply, void *ctx, long id) {
    long pin;
    if (!pin_arg(params, &pin)) { api_reply_err(reply, ctx, id, "bad_request", "need \"pin\""); return; }
    const char *err = "unknown";
    if (!pins_release((uint8_t)pin, &err)) { api_reply_err(reply, ctx, id, "release_failed", err); return; }
    api_reply_ok(reply, ctx, id, "{\"released\":true}");
}

/* --- gpio ---------------------------------------------------------------- */
#if CONFIG_IO_GPIO
static void op_gpio_set(const char *params, api_reply_fn reply, void *ctx, long id) {
    long pin; bool level = false;
    if (!pin_arg(params, &pin) || !api_json_bool(params, "value", &level)) {
        api_reply_err(reply, ctx, id, "bad_request", "need \"pin\" and \"value\""); return;
    }
    if (pins_is_reserved((uint8_t)pin)) {
        api_reply_err(reply, ctx, id, "reserved", "that pin belongs to the radio"); return;
    }
    gpio_init((uint)pin);
    gpio_set_dir((uint)pin, GPIO_OUT);
    gpio_put((uint)pin, level);
    api_reply_ok(reply, ctx, id, "{\"ok\":true}");
}

static void op_gpio_get(const char *params, api_reply_fn reply, void *ctx, long id) {
    long pin;
    if (!pin_arg(params, &pin)) { api_reply_err(reply, ctx, id, "bad_request", "need \"pin\""); return; }
    if (pins_is_reserved((uint8_t)pin)) {
        api_reply_err(reply, ctx, id, "reserved", "that pin belongs to the radio"); return;
    }
    gpio_init((uint)pin);
    gpio_set_dir((uint)pin, GPIO_IN);
    char pull[8] = "none";
    api_json_str(params, "pull", pull, sizeof(pull));
    gpio_set_pulls((uint)pin, strcmp(pull, "up") == 0, strcmp(pull, "down") == 0);
    char r[48];
    snprintf(r, sizeof(r), "{\"value\":%s}", gpio_get((uint)pin) ? "true" : "false");
    api_reply_ok(reply, ctx, id, r);
}
#endif

/* --- adc ----------------------------------------------------------------- */
#if CONFIG_IO_ADC
static void op_adc_read(const char *params, api_reply_fn reply, void *ctx, long id) {
    long ch = 4;
    (void)api_json_int(params, "channel", &ch);
    /* ADC0..2 are GPIO 26..28. ADC3 is GPIO29, which on a Pico 2 W is wired to
     * the radio -- reading it is not useful and claiming it is not allowed.
     * ADC4 is the on-die temperature sensor and always available. */
    if (ch == 3) {
        api_reply_err(reply, ctx, id, "reserved",
                      "ADC3 is GPIO29, which the radio owns on this board"); return;
    }
    if (ch < 0 || ch > 4) { api_reply_err(reply, ctx, id, "bad_request", "channel is 0-2 or 4"); return; }
    adc_init();
    if (ch == 4) { adc_set_temp_sensor_enabled(true); }
    else { adc_gpio_init((uint)(26 + ch)); }
    adc_select_input((uint)ch);
    uint16_t raw = adc_read();
    float volts = (float)raw * 3.3f / 4096.0f;
    char r[160];
    if (ch == 4) {
        /* RP2350 datasheet conversion for the on-die sensor. */
        float t = 27.0f - (volts - 0.706f) / 0.001721f;
        snprintf(r, sizeof(r), "{\"channel\":4,\"raw\":%u,\"volts\":%.4f,\"celsius\":%.2f}",
                 raw, (double)volts, (double)t);
    } else {
        snprintf(r, sizeof(r), "{\"channel\":%ld,\"raw\":%u,\"volts\":%.4f}", ch, raw, (double)volts);
    }
    api_reply_ok(reply, ctx, id, r);
}
#endif

/* --- i2c ----------------------------------------------------------------- */
#if CONFIG_IO_I2C
static i2c_inst_t *i2c_of(long n) { return n == 1 ? i2c1 : i2c0; }

static void i2c_setup(long bus, long sda, long scl, long hz) {
    i2c_init(i2c_of(bus), (uint)hz);
    gpio_set_function((uint)sda, GPIO_FUNC_I2C);
    gpio_set_function((uint)scl, GPIO_FUNC_I2C);
    gpio_pull_up((uint)sda);
    gpio_pull_up((uint)scl);
}

static void op_i2c_scan(const char *params, api_reply_fn reply, void *ctx, long id) {
    long bus = 0, sda = 4, scl = 5, hz = 100000;
    (void)api_json_int(params, "bus", &bus);
    (void)api_json_int(params, "sda", &sda);
    (void)api_json_int(params, "scl", &scl);
    (void)api_json_int(params, "hz", &hz);
    if (pins_is_reserved((uint8_t)sda) || pins_is_reserved((uint8_t)scl)) {
        api_reply_err(reply, ctx, id, "reserved", "sda or scl belongs to the radio"); return;
    }
    i2c_setup(bus, sda, scl, hz);

    size_t n = (size_t)snprintf(io_buf, sizeof(io_buf),
                                "{\"bus\":%ld,\"sda\":%ld,\"scl\":%ld,\"found\":[", bus, sda, scl);
    bool first = true;
    /* 0x08..0x77: everything outside that is reserved by the I2C spec. Every
     * transfer uses the timeout variant, so a stuck bus cannot hang the
     * cooperative loop -- which would take the radio down with it. */
    for (uint8_t a = 0x08; a <= 0x77 && n + 16 < sizeof(io_buf); a++) {
        uint8_t dummy;
        int rc = i2c_read_timeout_us(i2c_of(bus), a, &dummy, 1, false, 2000);
        if (rc >= 0) {
            n += (size_t)snprintf(io_buf + n, sizeof(io_buf) - n, "%s\"0x%02x\"", first ? "" : ",", a);
            first = false;
        }
    }
    snprintf(io_buf + n, sizeof(io_buf) - n, "]}");
    api_reply_ok(reply, ctx, id, io_buf);
}
#endif

/* --- spi ----------------------------------------------------------------- */
#if CONFIG_IO_SPI
static void op_spi_xfer(const char *params, api_reply_fn reply, void *ctx, long id) {
    long bus = 0, sck = 18, mosi = 19, miso = 16, cs = 17, hz = 1000000;
    (void)api_json_int(params, "bus", &bus);
    (void)api_json_int(params, "sck", &sck);
    (void)api_json_int(params, "mosi", &mosi);
    (void)api_json_int(params, "miso", &miso);
    (void)api_json_int(params, "cs", &cs);
    (void)api_json_int(params, "hz", &hz);
    char hex[512];
    if (!api_json_str(params, "value", hex, sizeof(hex))) {
        api_reply_err(reply, ctx, id, "bad_request", "need \"value\" as hex"); return;
    }
    spi_inst_t *s = bus == 1 ? spi1 : spi0;
    spi_init(s, (uint)hz);
    gpio_set_function((uint)sck, GPIO_FUNC_SPI);
    gpio_set_function((uint)mosi, GPIO_FUNC_SPI);
    gpio_set_function((uint)miso, GPIO_FUNC_SPI);
    /* CS is an ordinary claimed GPIO, not a hardware function, so a bus can
     * carry several devices. */
    gpio_init((uint)cs); gpio_set_dir((uint)cs, GPIO_OUT); gpio_put((uint)cs, 1);

    static uint8_t tx[256], rx[256];
    size_t len = strlen(hex) / 2;
    if (len > sizeof(tx)) { len = sizeof(tx); }
    for (size_t i = 0; i < len; i++) { unsigned b = 0; sscanf(hex + i * 2, "%2x", &b); tx[i] = (uint8_t)b; }

    gpio_put((uint)cs, 0);
    spi_write_read_blocking(s, tx, rx, len);
    gpio_put((uint)cs, 1);

    size_t n = (size_t)snprintf(io_buf, sizeof(io_buf), "{\"rx\":\"");
    for (size_t i = 0; i < len && n + 4 < sizeof(io_buf); i++) {
        n += (size_t)snprintf(io_buf + n, sizeof(io_buf) - n, "%02x", rx[i]);
    }
    snprintf(io_buf + n, sizeof(io_buf) - n, "\"}");
    api_reply_ok(reply, ctx, id, io_buf);
}
#endif

static const op_def_t io_ops_tbl[] = {
    { "pins.map",     op_pins_map },
    { "pins.claim",   op_pins_claim },
    { "pins.release", op_pins_release },
#if CONFIG_IO_GPIO
    { "gpio.set",     op_gpio_set },
    { "gpio.get",     op_gpio_get },
#endif
#if CONFIG_IO_ADC
    { "adc.read",     op_adc_read },
#endif
#if CONFIG_IO_I2C
    { "i2c.scan",     op_i2c_scan },
#endif
#if CONFIG_IO_SPI
    { "spi.xfer",     op_spi_xfer },
#endif
    { NULL, NULL }
};

static const char *const io_topics[] = { "gpio.irq", "uart.data", NULL };
static bool io_init(void) { pins_init(); return true; }

const op_module_t op_module_io = {
    .ns = "io", .ops = io_ops_tbl, .topics = io_topics, .init = io_init
};

#endif /* CONFIG_IO_PINS */
