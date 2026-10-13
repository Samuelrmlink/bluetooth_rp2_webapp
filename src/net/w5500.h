#ifndef BTGW_W5500_H
#define BTGW_W5500_H

/*
 * WIZnet W5500 in MACRAW mode: the chip moves Ethernet frames, lwIP does
 * everything else (src/net/eth_w5500.c is that glue).
 *
 * This file is the chip, and only the chip: no lwIP and no SDK, so
 * test/host/w5500_test.c drives this exact code against a simulated W5500
 * (registers, a MACRAW ring with wrap-around, SEND/RECV). Everything goes
 * through src/io/spibus.c; nothing here ever waits for the bus or the chip
 * outside w5500_init().
 *
 * Socket 0 takes all 16 KB of RX and all 16 KB of TX buffer; sockets 1..7
 * get none. Each received frame in the ring is prefixed with a 2-byte
 * big-endian length that includes those two bytes.
 */
#include <stdbool.h>
#include <stdint.h>

#include "io/spibus.h"

/* Common registers (block 0). */
#define W5500_MR        0x0000
#define W5500_SHAR      0x0009
#define W5500_IMR       0x0016
#define W5500_SIMR      0x0018
#define W5500_PHYCFGR   0x002E
#define W5500_VERSIONR  0x0039
#define W5500_VERSION   0x04

/* Socket registers. */
#define W5500_Sn_MR         0x0000
#define W5500_Sn_CR         0x0001
#define W5500_Sn_IR         0x0002
#define W5500_Sn_SR         0x0003
#define W5500_Sn_RXBUF_SIZE 0x001E
#define W5500_Sn_TXBUF_SIZE 0x001F
#define W5500_Sn_TX_FSR     0x0020
#define W5500_Sn_TX_RD      0x0022
#define W5500_Sn_TX_WR      0x0024
#define W5500_Sn_RX_RSR     0x0026
#define W5500_Sn_RX_RD      0x0028
#define W5500_Sn_IMR        0x002C

#define W5500_CR_OPEN   0x01
#define W5500_CR_CLOSE  0x10
#define W5500_CR_SEND   0x20
#define W5500_CR_RECV   0x40
#define W5500_IR_SENDOK 0x10
#define W5500_IR_RECV   0x04
#define W5500_MR_MACRAW 0x04
#define W5500_MR_MIP6B  0x10
#define W5500_MR_MFEN   0x80
#define W5500_SR_MACRAW 0x42

/* Block select: common, socket n registers / TX buffer / RX buffer. */
#define W5500_BSB_COMMON   0
#define W5500_BSB_REG(n)   (uint8_t)(((n) << 2) | 1)
#define W5500_BSB_TX(n)    (uint8_t)(((n) << 2) | 2)
#define W5500_BSB_RX(n)    (uint8_t)(((n) << 2) | 3)

#define W5500_BUF_SIZE     16384
#define W5500_FRAME_MAX    1514     /* no FCS in MACRAW */
#define W5500_FRAME_MIN    14

typedef struct { uint8_t *p; uint16_t len; } w5500_buf_t;

/* What the driver needs from the netif glue. All called from the main loop
 * (spibus_poll / w5500_step), never from an IRQ. */
typedef struct {
    /* A frame of `len` bytes is about to be read: hand out up to `max`
     * buffers that together hold exactly `len`. 0 = no memory, drop it. */
    int  (*rx_begin)(void *ctx, uint16_t len, w5500_buf_t *bufs, int max);
    void (*rx_end)(void *ctx, bool ok);
    /* The next frame to send, as up to `max` buffers; 0 = nothing queued.
     * Must return the same frame until tx_done(). */
    int  (*tx_peek)(void *ctx, w5500_buf_t *bufs, int max);
    void (*tx_done)(void *ctx, bool ok);
    void (*link)(void *ctx, bool up, int speed_mbps, bool full_duplex);
} w5500_ops_t;

typedef enum {
    W_IDLE = 0,
    W_IR_READ, W_IR_CLEAR,
    W_RX_RSR, W_RX_HDR, W_RX_DATA, W_RX_ADV, W_RX_RECV,
    W_TX_DATA, W_TX_WR, W_TX_SEND,
    W_PHY,
    W_RS_CLOSE, W_RS_MR, W_RS_OPEN, W_RS_PTRS,
} w5500_phase_t;

typedef struct {
    spidev_t           dev;
    const w5500_ops_t *ops;
    void              *ctx;
    int                int_pin;        /* -1: poll only */
    bool               mac_filter;
    bool               present;
    uint8_t            mac[6];

    w5500_phase_t      phase;          /* the transaction in flight */
    w5500_phase_t      pending;        /* the next one, waiting for the bus */
    volatile bool      xfer_done;
    uint8_t            hdr[3];
    uint8_t            reg[8];         /* register data, both directions */

    uint16_t           rx_rd, tx_wr;
    uint16_t           rsr, rsr_prev;
    uint16_t           frame_len;      /* incl. the 2-byte prefix */
    bool               rx_have_buf;
    bool               rx_more;        /* rsr says another frame is queued */
    bool               send_busy;
    uint16_t           tx_len;
    uint32_t           send_ms;
    uint32_t           last_rx_poll_ms, last_phy_ms;
    bool               want_ir;

    bool               link_up;
    int                speed;
    bool               full_duplex;

    uint32_t           rx_frames, rx_bytes, rx_dropped, tx_frames, tx_bytes;
    uint32_t           tx_timeouts, resyncs, bus_busy;
} w5500_t;

/* Blocking: reset (RST pin pulse, or MR.RST when rst_pin < 0), probe
 * VERSIONR, set the MAC and open socket 0 in MACRAW. False = no chip, and
 * nothing else was touched. Call once, at init. */
bool w5500_init(w5500_t *w, spibus_t *bus, int cs, uint32_t baud, int int_pin, int rst_pin,
                const uint8_t mac[6], bool mac_filter, const w5500_ops_t *ops, void *ctx);

/* Advance the state machine by at most one SPI transaction. Returns true
 * when it did something (so the caller may step again this pass). */
bool w5500_step(w5500_t *w, uint32_t now_ms);

/* Poll cadences. */
#define W5500_RX_POLL_MS     10     /* fallback when INT is idle/absent */
#define W5500_RX_POLL_NOINT  2
#define W5500_PHY_POLL_MS    500
#define W5500_SEND_TIMEOUT_MS 100

#endif
