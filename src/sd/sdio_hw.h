#ifndef BTGW_SDIO_HW_H
#define BTGW_SDIO_HW_H
/*
 * The SDIO wire, below the protocol (sd_sdio.c) and above the PIO
 * (sdio_hw_pico.c + sdio.pio). Host tests implement it with a simulated card
 * (test/host/sdcard_sim.c), which is how the protocol layer is tested without
 * a board.
 *
 * One clock source: the command state machine drives CLK -- while it sends a
 * command and reads the response, and in counted bursts while data moves.
 * The data state machine never drives CLK; it follows it (absolute GPIO
 * waits on the CLK pin, so CLK may be any pin). A read is ARMED before its
 * command goes out: a card may start the data block while the response is
 * still coming in on CMD, and the data machine must already be listening.
 *
 * All calls run in the storage worker; waits yield through sd_port.
 */
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    int clk, cmd, d0;        /* D1..D3 = d0+1..d0+3 */
    int pio;                 /* -1: any PIO with room */
} sdio_pins_t;

bool sdio_hw_init(const sdio_pins_t *pins);
void sdio_hw_set_clock_khz(uint32_t khz);
void sdio_hw_set_width(int width);          /* 4 (1 is identification only: no data then) */
uint32_t sdio_hw_clock_khz(void);           /* what the clock really runs at */

/* At least `cycles` clocks with CMD high and nothing armed (power-up: 74). */
void sdio_hw_idle_clocks(uint32_t cycles);

/* Send a command -- 6 bytes, CRC7 and end bit included -- and, when
 * resp_bits is 48 or 136, read the response into resp (6 or 17 bytes, the
 * start bit first, MSB first). Waits up to 64 clocks for the response's start
 * bit (Ncr). SD_OK or SD_ETIMEOUT. Anything armed on the data lines samples
 * while this clocks. */
int  sdio_hw_command(const uint8_t cmd[6], int resp_bits, uint8_t *resp);

/* Arm the data lines for `blocks` 512-byte blocks (at most SDIO_MAX_BLOCKS):
 * each is a start bit, 512 bytes and its CRC. All of it is set up -- DMA
 * descriptors for every block and every CRC -- before the command goes out,
 * because the data machine follows the clock and cannot pause it. */
#define SDIO_MAX_BLOCKS 32
void sdio_hw_read_arm(uint8_t *buf, uint32_t blocks);
/* Clock until every armed block is in, or timeout_us passes without
 * progress. crcs[i] gets block i's CRC as the card sent it. */
int  sdio_hw_read_wait(uint64_t *crcs, uint32_t timeout_us);

/* Send one block: start bit, data, crc (as sd_crc16x4 gives it),
 * end bit; then read the card's CRC status token on D0. *token = the 3
 * status bits (0b010 accepted, 0b101 CRC error, 0b110 write error).
 * SD_OK when a token came back (whatever it says), SD_ETIMEOUT if none. */
int  sdio_hw_write_block(const uint8_t *buf, uint32_t len, uint64_t crc, uint8_t *token);

/* Clock while D0 is held low (busy), up to timeout_us. SD_OK when released. */
int  sdio_hw_wait_busy(uint32_t timeout_us);

/* Stop whatever the data lines were doing (an error path). */
void sdio_hw_abort(void);

/* Counters for the status JSON / diagnostics. */
uint32_t sdio_hw_dma_blocks(void);

#endif
