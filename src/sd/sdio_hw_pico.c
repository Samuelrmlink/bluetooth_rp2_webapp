/*
 * sdio_hw.h on the RP2350: two PIO state machines (src/sd/sdio.pio) and four
 * DMA channels. Our own implementation; see sdio.pio for the clocking.
 *
 * FIFO protocol: every FIFO word carries 16 bits in its top half (autopull /
 * autopush at 16, shift left). The command machine takes a jmp to `xfer`
 * (bits out and in on CMD, clocked) or `clk` (bare clock bursts); the data
 * machine a jmp to `rxd` (receive blocks) or `txd` (send one block).
 *
 * Flow control: the clock is ours, so nothing is ever lost by being late --
 * the command machine stalls with CLK held when its FIFO runs dry, and the SD
 * bus allows the host to stop the clock. The one thing that cannot pause is
 * the data machine's receive (it follows the clock), so a read's DMA is set
 * up for every block and every CRC before the command goes out: a control
 * channel walks a list of {write address, count} pairs into the data
 * channel, which chains back to it.
 *
 * Every wait runs in the storage worker and yields (sd_port_wait_step); a
 * yield just pauses the clock.
 */
#include "sd/sdio_hw.h"

#include <stdio.h>
#include <string.h>

#include "autoconf.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "pico/stdlib.h"
#include "sd/sd_card.h"
#include "sd/sd_port.h"
#include "sdio.pio.h"
#if CONFIG_IO_PINS
#include "io/pins.h"
#endif

#define CLK_PER_BIT 8u                  /* PIO cycles per SD clock (sdio.pio) */
#define BLOCK_NIBBLES (1024u + 16u)     /* 512 data bytes + 4 x CRC16, 4-bit */

static sdio_pins_t P;
static PIO  pc, pd;                     /* command and data machines' PIO blocks */
static uint smc, smd, offc, offd;
static int  dma_rx = -1, dma_ctl = -1, dma_tx = -1, dma_txcrc = -1;
static uint32_t khz_now;
static uint32_t dma_blocks;
static bool in_clk;                     /* the command machine is in `clk` */

static uint16_t data_instr[32];
static pio_program_t data_prog;

/* The read's DMA list: {write address, count} per segment, then {0, 0}. */
static uint32_t ctl_list[2 * (2 * SDIO_MAX_BLOCKS + 1)] __attribute__((aligned(8)));
static uint16_t crc_in[SDIO_MAX_BLOCKS][4];
static uint32_t armed;
static uint16_t crc_out[4];

/* --- FIFO helpers -------------------------------------------------------- */

static bool cmd_put(uint16_t h, uint64_t t0) {
    while (pio_sm_is_tx_fifo_full(pc, smc)) {
        if (sd_port_now_us() - t0 > 100000u) { return false; }
        sd_port_wait_step(t0);
    }
    pc->txf[smc] = (uint32_t)h << 16;
    return true;
}

static void data_put(uint16_t h) {
    while (pio_sm_is_tx_fifo_full(pd, smd)) { tight_loop_contents(); }
    pd->txf[smd] = (uint32_t)h << 16;
}

static bool sm_idle(PIO pio, uint sm) {
    /* FIFO empty and stalled on `out` (at top, or at clk's `out x`). */
    uint32_t stall = 1u << (PIO_FDEBUG_TXSTALL_LSB + sm);
    pio->fdebug = stall;                                 /* clear, then see it again */
    for (int i = 0; i < 1000; i++) {
        if (pio_sm_is_tx_fifo_empty(pio, sm) && (pio->fdebug & stall)) { return true; }
        busy_wait_us_32(1);
        pio->fdebug = stall;
        busy_wait_us_32(1);
        if (pio_sm_is_tx_fifo_empty(pio, sm) && (pio->fdebug & stall)) { return true; }
    }
    return false;
}

static void clk_enter(uint64_t t0) {
    if (in_clk) { return; }
    cmd_put((uint16_t)pio_encode_jmp(offc + sdio_cmd_offset_clk), t0);
    in_clk = true;
}

static void clk_leave(void) {
    if (!in_clk) { return; }
    /* Let the queued bursts finish, then pull the machine out of `out x`. */
    uint64_t t0 = sd_port_now_us();
    while (!pio_sm_is_tx_fifo_empty(pc, smc) && sd_port_now_us() - t0 < 100000u) { tight_loop_contents(); }
    sm_idle(pc, smc);
    pio_sm_exec(pc, smc, pio_encode_jmp(offc));
    in_clk = false;
}

/* Run exactly n clocks (n >= 1) and wait for them. */
static void clocks_exact(uint32_t n) {
    uint64_t t0 = sd_port_now_us();
    clk_enter(t0);
    while (n) {
        uint32_t k = n > 65536u ? 65536u : n;
        cmd_put((uint16_t)(k - 1), t0);
        n -= k;
    }
    clk_leave();
}

static void dma_stop(void) {
    if (dma_ctl >= 0) { dma_channel_abort((uint)dma_ctl); }
    if (dma_rx >= 0) { dma_channel_abort((uint)dma_rx); }
    if (dma_tx >= 0) { dma_channel_abort((uint)dma_tx); }
    if (dma_txcrc >= 0) { dma_channel_abort((uint)dma_txcrc); }
}

/* Data machine back to `top`, lines released, FIFOs empty. */
static void data_reset(void) {
    dma_stop();
    pio_sm_set_enabled(pd, smd, false);
    pio_sm_clear_fifos(pd, smd);
    pio_sm_restart(pd, smd);
    pio_sm_exec(pd, smd, pio_encode_set(pio_pindirs, 0));
    pio_sm_exec(pd, smd, pio_encode_jmp(offd));
    pio_sm_set_enabled(pd, smd, true);
}

static uint16_t bswap16(uint16_t v) { return (uint16_t)((v << 8) | (v >> 8)); }

/* --- init ---------------------------------------------------------------- */

static bool reserve(int pin) {
#if CONFIG_IO_PINS
    return pins_reserve((uint8_t)pin, PIN_FN_GPIO, "sd");
#else
    (void)pin;
    return true;
#endif
}

bool sdio_hw_init(const sdio_pins_t *pins) {
    P = *pins;
    if (P.d0 < 0 || P.d0 + 3 > 29 || P.clk < 0 || P.cmd < 0) { return false; }
    if (!reserve(P.clk) || !reserve(P.cmd)) { return false; }
    for (int i = 0; i < 4; i++) { if (!reserve(P.d0 + i)) { return false; } }

    /* The data program, its `wait gpio 0` pointed at CLK. */
    memcpy(data_instr, sdio_data_program.instructions, sdio_data_program.length * sizeof(uint16_t));
    for (uint i = 0; i < sdio_data_program.length; i++) {
        uint16_t in = data_instr[i];
        if ((in & 0xE000u) == 0x2000u && ((in >> 5) & 3u) == 0) {      /* WAIT, source GPIO */
            data_instr[i] = (uint16_t)((in & ~0x1Fu) | (uint16_t)P.clk);
        }
    }
    data_prog = sdio_data_program;
    data_prog.instructions = data_instr;

    uint lo = (uint)(P.clk < P.cmd ? P.clk : P.cmd), hi = (uint)(P.clk < P.cmd ? P.cmd : P.clk);
    if (!pio_claim_free_sm_and_add_program_for_gpio_range(&sdio_cmd_program, &pc, &smc, &offc,
                                                           lo, hi - lo + 1, true)) { return false; }
    if (!pio_claim_free_sm_and_add_program_for_gpio_range(&data_prog, &pd, &smd, &offd,
                                                           (uint)P.d0, 4, true)) { return false; }

    dma_rx = dma_claim_unused_channel(false);
    dma_ctl = dma_claim_unused_channel(false);
    dma_tx = dma_claim_unused_channel(false);
    dma_txcrc = dma_claim_unused_channel(false);
    if (dma_rx < 0 || dma_ctl < 0 || dma_tx < 0 || dma_txcrc < 0) { return false; }

    pio_gpio_init(pc, (uint)P.clk);
    pio_gpio_init(pc, (uint)P.cmd);
    for (int i = 0; i < 4; i++) { pio_gpio_init(pd, (uint)(P.d0 + i)); }
    /* Pull-ups (CONFIG_SD_SDIO_PULLUPS) or none -- never pull-downs: the A2
     * silicon's RP2350-E9 latch-up is what an internal pull-down cannot
     * hold against (see src/sd/Kconfig). */
#if CONFIG_SD_SDIO_PULLUPS
    gpio_pull_up((uint)P.cmd);
    for (int i = 0; i < 4; i++) { gpio_pull_up((uint)(P.d0 + i)); }
#else
    gpio_disable_pulls((uint)P.cmd);
    for (int i = 0; i < 4; i++) { gpio_disable_pulls((uint)(P.d0 + i)); }
#endif

    pio_sm_config c = sdio_cmd_program_get_default_config(offc);
    sm_config_set_sideset_pins(&c, (uint)P.clk);
    sm_config_set_out_pins(&c, (uint)P.cmd, 1);
    sm_config_set_set_pins(&c, (uint)P.cmd, 1);
    sm_config_set_in_pins(&c, (uint)P.cmd);
    sm_config_set_out_shift(&c, false, true, 16);
    sm_config_set_in_shift(&c, false, true, 16);
    pio_sm_init(pc, smc, offc, &c);
    pio_sm_set_pins_with_mask(pc, smc, 1u << P.cmd, (1u << P.cmd) | (1u << P.clk));
    pio_sm_set_pindirs_with_mask(pc, smc, (1u << P.cmd) | (1u << P.clk), (1u << P.cmd) | (1u << P.clk));
    sdio_hw_set_clock_khz(400);
    pio_sm_set_enabled(pc, smc, true);

    pio_sm_config d = sdio_data_program_get_default_config(offd);
    sm_config_set_out_pins(&d, (uint)P.d0, 4);
    sm_config_set_set_pins(&d, (uint)P.d0, 4);
    sm_config_set_in_pins(&d, (uint)P.d0);
    sm_config_set_jmp_pin(&d, (uint)P.d0);
    sm_config_set_out_shift(&d, false, true, 16);
    sm_config_set_in_shift(&d, false, true, 16);
    sm_config_set_clkdiv(&d, 1.0f);
    pio_sm_init(pd, smd, offd, &d);
    pio_sm_set_pindirs_with_mask(pd, smd, 0, 0xFu << P.d0);
    pio_sm_set_enabled(pd, smd, true);
    printf("sdio: cmd pio%u sm%u @%u, data pio%u sm%u @%u, dma rx %d ctl %d tx %d crc %d\n",
           pio_get_index(pc), smc, offc, pio_get_index(pd), smd, offd, dma_rx, dma_ctl, dma_tx, dma_txcrc);
    return true;
}

void sdio_hw_set_clock_khz(uint32_t k) {
    uint32_t sys = clock_get_hz(clk_sys);
    float div = (float)sys / ((float)CLK_PER_BIT * (float)k * 1000.0f);
    if (div < 1.0f) { div = 1.0f; }
    pio_sm_set_clkdiv(pc, smc, div);
    khz_now = (uint32_t)((float)sys / ((float)CLK_PER_BIT * div) / 1000.0f);
}

uint32_t sdio_hw_clock_khz(void) { return khz_now; }
void sdio_hw_set_width(int w) { (void)w; }               /* data is always 4-bit */
uint32_t sdio_hw_dma_blocks(void) { return dma_blocks; }


void sdio_hw_idle_clocks(uint32_t n) {
    uint64_t t0 = sd_port_now_us();
    cmd_put((uint16_t)pio_encode_set(pio_pins, 1), t0);   /* CMD high */
    cmd_put((uint16_t)pio_encode_set(pio_pindirs, 1), t0);
    clocks_exact(n ? n : 1);
}

/* --- commands ------------------------------------------------------------- */

int sdio_hw_command(const uint8_t c[6], int resp_bits, uint8_t *resp) {
    uint64_t t0 = sd_port_now_us();
    clk_leave();
    while (!pio_sm_is_rx_fifo_empty(pc, smc)) { (void)pc->rxf[smc]; }

    /* Phase 1: 48 bits out on CMD. Its 3 echoed halfwords are dropped. */
    uint16_t out[3] = { (uint16_t)(c[0] << 8 | c[1]), (uint16_t)(c[2] << 8 | c[3]), (uint16_t)(c[4] << 8 | c[5]) };
    if (!cmd_put((uint16_t)pio_encode_set(pio_pindirs, 1), t0) ||
        !cmd_put((uint16_t)pio_encode_jmp(offc + sdio_cmd_offset_xfer), t0) || !cmd_put(47, t0)) { return SD_EIO; }
    for (int i = 0; i < 3; i++) { cmd_put(out[i], t0); }

    /* Phase 2: CMD released; clock the response window (Ncr <= 64, then the
     * response, then the 8 clocks Nrc the card needs before the next one). */
    uint32_t n = resp_bits == 136 ? 224u : resp_bits == 48 ? 128u : 16u;
    cmd_put((uint16_t)pio_encode_set(pio_pindirs, 0), t0);
    cmd_put((uint16_t)pio_encode_jmp(offc + sdio_cmd_offset_xfer), t0);
    cmd_put((uint16_t)(n - 1), t0);
    uint16_t in[3 + 14];
    uint32_t want = 3 + n / 16, got = 0, pushed = 0;
    while (got < want) {
        if (pushed < n / 16 && !pio_sm_is_tx_fifo_full(pc, smc)) { pc->txf[smc] = 0xFFFFu << 16; pushed++; }
        if (!pio_sm_is_rx_fifo_empty(pc, smc)) { in[got++] = (uint16_t)pc->rxf[smc]; continue; }
        if (sd_port_now_us() - t0 > 50000u) { return SD_ETIMEOUT; }   /* the machine stopped: not the card */
    }
    if (!resp_bits) { return SD_OK; }

    /* Find the start bit in the window and take the response from there. */
    const uint16_t *w = in + 3;
    uint32_t nbits = n, s = 0;
#define BIT(i) ((w[(i) / 16] >> (15 - (i) % 16)) & 1u)
    while (s < nbits && BIT(s)) { s++; }
    if (s > 64 + 2 || s + (uint32_t)resp_bits > nbits) { return SD_ETIMEOUT; }
    memset(resp, 0, resp_bits == 136 ? 17 : 6);
    for (uint32_t i = 0; i < (uint32_t)resp_bits; i++) {
        if (BIT(s + i)) { resp[i / 8] |= (uint8_t)(0x80u >> (i % 8)); }
    }
#undef BIT
    return SD_OK;
}

/* --- data ------------------------------------------------------------------ */

void sdio_hw_read_arm(uint8_t *buf, uint32_t blocks) {
    data_reset();
    if (blocks == 0 || blocks > SDIO_MAX_BLOCKS) { armed = 0; return; }
    uint32_t *l = ctl_list;
    for (uint32_t i = 0; i < blocks; i++) {
        *l++ = (uint32_t)(uintptr_t)(buf + 512u * i); *l++ = 256;
        *l++ = (uint32_t)(uintptr_t)crc_in[i];       *l++ = 4;
    }
    *l++ = 0; *l++ = 0;                                  /* null trigger: the chain ends */
    armed = blocks;

    dma_channel_config r = dma_channel_get_default_config((uint)dma_rx);
    channel_config_set_transfer_data_size(&r, DMA_SIZE_16);
    channel_config_set_read_increment(&r, false);
    channel_config_set_write_increment(&r, true);
    channel_config_set_bswap(&r, true);                  /* first nibble pair = first byte */
    channel_config_set_dreq(&r, pio_get_dreq(pd, smd, false));
    channel_config_set_chain_to(&r, (uint)dma_ctl);
    channel_config_set_high_priority(&r, true);
    dma_channel_configure((uint)dma_rx, &r, NULL, &pd->rxf[smd], 0, false);

    dma_channel_config k = dma_channel_get_default_config((uint)dma_ctl);
    channel_config_set_transfer_data_size(&k, DMA_SIZE_32);
    channel_config_set_read_increment(&k, true);
    channel_config_set_write_increment(&k, true);
    channel_config_set_ring(&k, true, 3);                /* write address, count: 8 bytes */
    dma_channel_configure((uint)dma_ctl, &k, &dma_hw->ch[dma_rx].al1_write_addr, ctl_list, 2, true);

    data_put((uint16_t)pio_encode_jmp(offd + sdio_data_offset_rxd));
    data_put((uint16_t)(BLOCK_NIBBLES - 1));
}

static bool read_done(void) {
    return !dma_channel_is_busy((uint)dma_ctl) && !dma_channel_is_busy((uint)dma_rx) &&
           dma_hw->ch[dma_ctl].read_addr == (uint32_t)(uintptr_t)(ctl_list + 4 * armed + 2);
}

int sdio_hw_read_wait(uint64_t *crcs, uint32_t timeout_us) {
    if (!armed) { return SD_EPARAM; }
    uint64_t t0 = sd_port_now_us(), last = t0;
    uint32_t mark = dma_hw->ch[dma_rx].write_addr;
    clk_enter(t0);
    int e = SD_OK;
    while (!read_done()) {
        if (!pio_sm_is_tx_fifo_full(pc, smc)) { pc->txf[smc] = 63u << 16; }   /* 64 more clocks */
        uint32_t now_mark = dma_hw->ch[dma_rx].write_addr;
        uint64_t now = sd_port_now_us();
        if (now_mark != mark) { mark = now_mark; last = now; }
        if (now - last > timeout_us) { e = SD_ETIMEOUT; break; }
        sd_port_wait_step(last);
    }
    clk_leave();
    if (e != SD_OK) {
        static uint32_t told;
        if (told++ < 3) {          /* the first few, with the hardware's own view */
            printf("sdio: read timeout: rx ctrl %08lx count %lu waddr %08lx, ctl raddr %08lx (end %08lx) ctrl %08lx, "
                   "pio flevel %08lx fdebug %08lx sm addr %lu\n",
                   (unsigned long)dma_hw->ch[dma_rx].ctrl_trig, (unsigned long)dma_hw->ch[dma_rx].transfer_count,
                   (unsigned long)dma_hw->ch[dma_rx].write_addr, (unsigned long)dma_hw->ch[dma_ctl].read_addr,
                   (unsigned long)(uintptr_t)(ctl_list + 4 * armed + 2), (unsigned long)dma_hw->ch[dma_ctl].ctrl_trig,
                   (unsigned long)pd->flevel, (unsigned long)pd->fdebug, (unsigned long)pio_sm_get_pc(pd, smd));
        }
    }
    if (e == SD_OK) {
        for (uint32_t i = 0; i < armed; i++) {
            crcs[i] = ((uint64_t)bswap16(crc_in[i][0]) << 48) | ((uint64_t)bswap16(crc_in[i][1]) << 32) |
                      ((uint64_t)bswap16(crc_in[i][2]) << 16) | bswap16(crc_in[i][3]);
        }
        dma_blocks += armed;
    }
    armed = 0;
    data_reset();
    return e;
}

int sdio_hw_write_block(const uint8_t *buf, uint32_t len, uint64_t crc, uint8_t *token) {
    if (len != 512) { return SD_EPARAM; }
    uint64_t t0 = sd_port_now_us();
    data_reset();
    crc_out[0] = (uint16_t)(crc >> 48);
    crc_out[1] = (uint16_t)(crc >> 32);
    crc_out[2] = (uint16_t)(crc >> 16);
    crc_out[3] = (uint16_t)crc;

    dma_channel_config cc = dma_channel_get_default_config((uint)dma_txcrc);
    channel_config_set_transfer_data_size(&cc, DMA_SIZE_16);
    channel_config_set_read_increment(&cc, true);
    channel_config_set_write_increment(&cc, false);
    channel_config_set_dreq(&cc, pio_get_dreq(pd, smd, true));
    dma_channel_configure((uint)dma_txcrc, &cc, &pd->txf[smd], crc_out, 4, false);

    dma_channel_config t = dma_channel_get_default_config((uint)dma_tx);
    channel_config_set_transfer_data_size(&t, DMA_SIZE_16);
    channel_config_set_read_increment(&t, true);
    channel_config_set_write_increment(&t, false);
    channel_config_set_bswap(&t, true);
    channel_config_set_dreq(&t, pio_get_dreq(pd, smd, true));
    channel_config_set_chain_to(&t, (uint)dma_txcrc);
    dma_channel_configure((uint)dma_tx, &t, &pd->txf[smd], buf, 256, false);

    data_put((uint16_t)pio_encode_jmp(offd + sdio_data_offset_txd));
    data_put((uint16_t)(BLOCK_NIBBLES - 1));
    dma_channel_start((uint)dma_tx);

    /* Start bit, 1040 nibbles, end bit, release: 1043 clocks exactly, so the
     * status token's start bit (2 clocks later) is still ahead of us. */
    clocks_exact(1 + BLOCK_NIBBLES + 1 + 1);
    if (!sm_idle(pd, smd)) {
        data_reset();
        return SD_EIO;                                   /* the block did not go out whole */
    }
    /* The CRC status token: start bit, 3 status bits, end bit, on D0. */
    data_put((uint16_t)pio_encode_jmp(offd + sdio_data_offset_rxd));
    data_put(3);
    clk_enter(t0);
    int e = SD_ETIMEOUT;
    uint64_t t1 = sd_port_now_us();
    while (sd_port_now_us() - t1 < 10000u) {
        if (!pio_sm_is_rx_fifo_empty(pd, smd)) {
            uint16_t h = (uint16_t)pd->rxf[smd];
            *token = (uint8_t)((((h >> 12) & 1u) << 2) | (((h >> 8) & 1u) << 1) | ((h >> 4) & 1u));
            e = SD_OK;
            break;
        }
        if (!pio_sm_is_tx_fifo_full(pc, smc)) { pc->txf[smc] = 7u << 16; }   /* 8 more clocks */
    }
    clk_leave();
    data_reset();
    if (e == SD_OK) { dma_blocks++; }
    return e;
}

int sdio_hw_wait_busy(uint32_t timeout_us) {
    uint64_t t0 = sd_port_now_us();
    /* The card starts busy on D0 a couple of clocks after the CRC status
     * token, and signals it while clocked: give it clocks before believing a
     * high D0. On the bench (2026-10-19) one look right after the token read
     * "not busy", and the next CMD24 met a card still programming -- which
     * ignores it (illegal outside the transfer state), so every later write
     * timed out. */
    clocks_exact(16);
    if (gpio_get((uint)P.d0)) { return SD_OK; }
    clk_enter(t0);
    int e = SD_OK;
    while (!gpio_get((uint)P.d0)) {
        if (sd_port_now_us() - t0 > timeout_us) { e = SD_ETIMEOUT; break; }
        if (!pio_sm_is_tx_fifo_full(pc, smc)) { pc->txf[smc] = 7u << 16; }
        sd_port_wait_step(t0);
    }
    clk_leave();
    return e;
}

void sdio_hw_abort(void) {
    clk_leave();
    armed = 0;
    data_reset();
}
