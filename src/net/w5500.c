/*
 * W5500 MACRAW driver: one non-blocking state machine, one SPI transaction
 * per step. See w5500.h for the split between this file and the lwIP glue.
 *
 * The sequences, one transaction each:
 *
 *   RX   [Sn_IR read -> clear]  RX_RSR  ->  2-byte header at RX_RD
 *        -> frame (DMA straight into the glue's buffers) -> RX_RD += len
 *        -> CR=RECV  -> next header while RSR says there is more
 *   TX   frame at TX_WR -> TX_WR += len -> CR=SEND; SENDOK is collected by
 *        the next Sn_IR read before another frame goes. MACRAW sends one
 *        frame at a time, so the 16 KB TX buffer is always empty when a
 *        frame is written and TX_FSR never needs reading.
 *   PHY  PHYCFGR every 500 ms -> link up/down, speed, duplex.
 *
 * RX_RSR is read ONCE, not twice-until-stable as WIZnet's library does: it
 * only grows while we are not consuming, it is read high byte first, and a
 * torn read of a growing 16-bit value read that way can only come out LOW.
 * An underestimate is harmless (the frame it splits is already complete in
 * the ring; the chip counts whole frames), so the second read buys nothing.
 *
 * The bus is claimed per transaction and released after it, so another
 * device on the same SPI block gets a turn between any two of ours.
 */
#include "net/w5500.h"

#include <stdio.h>
#include <string.h>

static void on_xfer(void *ctx, bool ok) {
    w5500_t *w = (w5500_t *)ctx;
    (void)ok;
    w->xfer_done = true;
}

static void mkhdr(w5500_t *w, uint8_t bsb, uint16_t addr, bool write) {
    w->hdr[0] = (uint8_t)(addr >> 8);
    w->hdr[1] = (uint8_t)addr;
    w->hdr[2] = (uint8_t)((bsb << 3) | (write ? 0x04 : 0x00));
}

/* --- blocking register access, init only --------------------------------- */

static bool breg(w5500_t *w, uint8_t bsb, uint16_t addr, bool write, uint8_t *data, uint16_t n) {
    mkhdr(w, bsb, addr, write);
    spiseg_t s[2] = { { w->hdr, NULL, 3 }, { write ? data : NULL, write ? NULL : data, n } };
    return spibus_xfer_blocking(&w->dev, s, 2);
}

static bool bw8(w5500_t *w, uint8_t bsb, uint16_t addr, uint8_t v) {
    return breg(w, bsb, addr, true, &v, 1);
}

static int br8(w5500_t *w, uint8_t bsb, uint16_t addr) {
    uint8_t v = 0;
    return breg(w, bsb, addr, false, &v, 1) ? v : -1;
}

static uint8_t sock_mr(const w5500_t *w) {
    /* IPv6 blocked: lwIP here is IPv4-only, so those frames would only cost
     * SPI time. Multicast is NOT blocked (MMB clear): mDNS needs it. */
    return (uint8_t)(W5500_MR_MACRAW | W5500_MR_MIP6B | (w->mac_filter ? W5500_MR_MFEN : 0));
}

bool w5500_init(w5500_t *w, spibus_t *bus, int cs, uint32_t baud, int int_pin, int rst_pin,
                const uint8_t mac[6], bool mac_filter, const w5500_ops_t *ops, void *ctx) {
    memset(w, 0, sizeof *w);
    w->ops = ops;
    w->ctx = ctx;
    w->int_pin = int_pin;
    w->mac_filter = mac_filter;
    w->speed = 0;
    memcpy(w->mac, mac, 6);
    if (!spibus_dev_init(&w->dev, bus, cs, baud, 0, "w5500")) { return false; }
    if (int_pin >= 0) {
        spibus_port_reserve_pin(int_pin, "w5500", false);
        spibus_port_gpio_in(int_pin, true);
    }
    if (!spibus_claim_blocking(&w->dev, 100)) { return false; }

    bool ok = false;
    if (rst_pin >= 0) {
        spibus_port_reserve_pin(rst_pin, "w5500", false);
        spibus_port_gpio_out(rst_pin, false);
        spibus_port_delay_us(1000);           /* >= 500 us low */
        spibus_port_gpio_put(rst_pin, true);
        spibus_port_delay_us(2000);           /* PLL lock < 1 ms */
    }
    int ver = br8(w, W5500_BSB_COMMON, W5500_VERSIONR);
    if (ver != W5500_VERSION) {
        printf("w5500: no chip (VERSIONR %d, want %d)\n", ver, W5500_VERSION);
        goto out;
    }
    if (rst_pin < 0) {
        bw8(w, W5500_BSB_COMMON, W5500_MR, 0x80);
        for (int i = 0; i < 20 && (br8(w, W5500_BSB_COMMON, W5500_MR) & 0x80); i++) {
            spibus_port_delay_us(100);
        }
    }
    uint8_t m[6];
    memcpy(m, mac, 6);
    breg(w, W5500_BSB_COMMON, W5500_SHAR, true, m, 6);
    for (int s = 0; s < 8; s++) {
        uint8_t kb = s == 0 ? 16 : 0;
        bw8(w, W5500_BSB_REG(s), W5500_Sn_RXBUF_SIZE, kb);
        bw8(w, W5500_BSB_REG(s), W5500_Sn_TXBUF_SIZE, kb);
    }
    bw8(w, W5500_BSB_REG(0), W5500_Sn_MR, sock_mr(w));
    bw8(w, W5500_BSB_REG(0), W5500_Sn_CR, W5500_CR_OPEN);
    int sr = -1;
    for (int i = 0; i < 20; i++) {
        sr = br8(w, W5500_BSB_REG(0), W5500_Sn_SR);
        if (sr == W5500_SR_MACRAW) { break; }
        spibus_port_delay_us(100);
    }
    if (sr != W5500_SR_MACRAW) {
        printf("w5500: socket 0 did not open in MACRAW (SR 0x%02x)\n", sr & 0xff);
        goto out;
    }
    /* INT asserts on RECV only; SENDOK is collected by polling Sn_IR. */
    bw8(w, W5500_BSB_REG(0), W5500_Sn_IMR, W5500_IR_RECV);
    bw8(w, W5500_BSB_COMMON, W5500_SIMR, 0x01);
    bw8(w, W5500_BSB_COMMON, W5500_IMR, 0x00);
    uint8_t p[2];
    breg(w, W5500_BSB_REG(0), W5500_Sn_TX_WR, false, p, 2);
    w->tx_wr = (uint16_t)((p[0] << 8) | p[1]);
    breg(w, W5500_BSB_REG(0), W5500_Sn_RX_RD, false, p, 2);
    w->rx_rd = (uint16_t)((p[0] << 8) | p[1]);
    w->present = true;
    ok = true;
    printf("w5500: present, MACRAW open, %02x:%02x:%02x:%02x:%02x:%02x\n",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
out:
    spibus_release(&w->dev);
    return ok;
}

/* --- the state machine --------------------------------------------------- */

static w5500_buf_t rxb[SPIBUS_MAX_SEGS - 1], txb[SPIBUS_MAX_SEGS - 1];
static int n_rxb, n_txb;

static bool reg_xfer(w5500_t *w, uint8_t bsb, uint16_t addr, bool write, uint16_t n) {
    mkhdr(w, bsb, addr, write);
    spiseg_t s[2] = { { w->hdr, NULL, 3 }, { write ? w->reg : NULL, write ? NULL : w->reg, n } };
    return spibus_xfer_async(&w->dev, s, 2, on_xfer, w);
}

static bool reg_w16(w5500_t *w, uint8_t bsb, uint16_t addr, uint16_t v) {
    w->reg[0] = (uint8_t)(v >> 8);
    w->reg[1] = (uint8_t)v;
    return reg_xfer(w, bsb, addr, true, 2);
}

static bool reg_w8(w5500_t *w, uint8_t bsb, uint16_t addr, uint8_t v) {
    w->reg[0] = v;
    return reg_xfer(w, bsb, addr, true, 1);
}

static bool buf_xfer(w5500_t *w, uint8_t bsb, uint16_t ptr, bool write, const w5500_buf_t *b, int n) {
    mkhdr(w, bsb, ptr, write);
    spiseg_t s[SPIBUS_MAX_SEGS];
    s[0] = (spiseg_t){ w->hdr, NULL, 3 };
    for (int i = 0; i < n; i++) {
        s[1 + i] = (spiseg_t){ write ? b[i].p : NULL, write ? NULL : b[i].p, b[i].len };
    }
    return spibus_xfer_async(&w->dev, s, 1 + n, on_xfer, w);
}

static bool begin(w5500_t *w, w5500_phase_t p) {
    switch (p) {
    case W_IR_READ:  return reg_xfer(w, W5500_BSB_REG(0), W5500_Sn_IR, false, 1);
    case W_IR_CLEAR: return reg_xfer(w, W5500_BSB_REG(0), W5500_Sn_IR, true, 1);  /* reg[0] = bits */
    case W_RX_RSR:   return reg_xfer(w, W5500_BSB_REG(0), W5500_Sn_RX_RSR, false, 2);
    case W_RX_HDR:   return reg_xfer(w, W5500_BSB_RX(0), w->rx_rd, false, 2);
    case W_RX_DATA:  return buf_xfer(w, W5500_BSB_RX(0), (uint16_t)(w->rx_rd + 2), false, rxb, n_rxb);
    case W_RX_ADV:   return reg_w16(w, W5500_BSB_REG(0), W5500_Sn_RX_RD, (uint16_t)(w->rx_rd + w->frame_len));
    case W_RX_RECV:  return reg_w8(w, W5500_BSB_REG(0), W5500_Sn_CR, W5500_CR_RECV);
    case W_TX_DATA:  return buf_xfer(w, W5500_BSB_TX(0), w->tx_wr, true, txb, n_txb);
    case W_TX_WR:    return reg_w16(w, W5500_BSB_REG(0), W5500_Sn_TX_WR, (uint16_t)(w->tx_wr + w->tx_len));
    case W_TX_SEND:  return reg_w8(w, W5500_BSB_REG(0), W5500_Sn_CR, W5500_CR_SEND);
    case W_PHY:      return reg_xfer(w, W5500_BSB_COMMON, W5500_PHYCFGR, false, 1);
    case W_RS_CLOSE: return reg_w8(w, W5500_BSB_REG(0), W5500_Sn_CR, W5500_CR_CLOSE);
    case W_RS_MR:    return reg_w8(w, W5500_BSB_REG(0), W5500_Sn_MR, sock_mr(w));
    case W_RS_OPEN:  return reg_w8(w, W5500_BSB_REG(0), W5500_Sn_CR, W5500_CR_OPEN);
    case W_RS_PTRS:  return reg_xfer(w, W5500_BSB_REG(0), W5500_Sn_TX_RD, false, 8);
    default:         return false;
    }
}

static w5500_phase_t resync(w5500_t *w) {
    if (w->rx_have_buf) { w->ops->rx_end(w->ctx, false); w->rx_have_buf = false; }
    w->resyncs++;
    return W_RS_CLOSE;
}

/* What the transaction that just finished means, and what comes next. */
static w5500_phase_t after(w5500_t *w, w5500_phase_t p, uint32_t now) {
    switch (p) {
    case W_IR_READ: {
        uint8_t ir = w->reg[0];
        if ((ir & W5500_IR_SENDOK) && w->send_busy) { w->send_busy = false; }
        if (ir) { w->reg[0] = ir; return W_IR_CLEAR; }   /* write-1-to-clear */
        return w->want_ir ? W_RX_RSR : W_IDLE;
    }
    case W_IR_CLEAR:
        /* Cleared BEFORE reading RSR: a frame that lands after this
         * re-asserts INT, so none can slip between the read and the clear. */
        return w->want_ir ? W_RX_RSR : W_IDLE;
    case W_RX_RSR:
        w->want_ir = false;
        w->last_rx_poll_ms = now;
        w->rsr = (uint16_t)((w->reg[0] << 8) | w->reg[1]);
        return w->rsr >= 2 ? W_RX_HDR : W_IDLE;
    case W_RX_HDR: {
        uint16_t len = (uint16_t)((w->reg[0] << 8) | w->reg[1]);
        if (len < 2 + W5500_FRAME_MIN || len > 2 + W5500_FRAME_MAX) {
            printf("w5500: bad frame length %u at %u, resync\n", len, w->rx_rd);
            return resync(w);
        }
        w->frame_len = len;
        n_rxb = w->ops->rx_begin(w->ctx, (uint16_t)(len - 2), rxb, SPIBUS_MAX_SEGS - 1);
        if (n_rxb <= 0) {
            w->rx_dropped++;
            return W_RX_ADV;
        }
        w->rx_have_buf = true;
        return W_RX_DATA;
    }
    case W_RX_DATA:
        w->rx_have_buf = false;
        w->rx_frames++;
        w->rx_bytes += (uint32_t)(w->frame_len - 2);
        w->ops->rx_end(w->ctx, true);
        return W_RX_ADV;
    case W_RX_ADV:
        w->rx_rd = (uint16_t)(w->rx_rd + w->frame_len);
        return W_RX_RECV;
    case W_RX_RECV:
        w->rsr = w->rsr > w->frame_len ? (uint16_t)(w->rsr - w->frame_len) : 0;
        return w->rsr >= 2 ? W_RX_HDR : W_IDLE;
    case W_TX_DATA:
        return W_TX_WR;
    case W_TX_WR:
        w->tx_wr = (uint16_t)(w->tx_wr + w->tx_len);
        return W_TX_SEND;
    case W_TX_SEND:
        w->send_busy = true;
        w->send_ms = now;
        w->tx_frames++;
        w->tx_bytes += w->tx_len;
        w->ops->tx_done(w->ctx, true);
        return W_IDLE;
    case W_PHY: {
        uint8_t v = w->reg[0];
        bool up = (v & 0x01) != 0;
        int speed = up ? ((v & 0x02) ? 100 : 10) : 0;
        bool full = up && (v & 0x04);
        w->last_phy_ms = now;
        if (up != w->link_up || speed != w->speed || full != w->full_duplex) {
            w->link_up = up;
            w->speed = speed;
            w->full_duplex = full;
            w->ops->link(w->ctx, up, speed, full);
        }
        return W_IDLE;
    }
    case W_RS_CLOSE: return W_RS_MR;
    case W_RS_MR:    return W_RS_OPEN;
    case W_RS_OPEN:  return W_RS_PTRS;
    case W_RS_PTRS:
        /* TX_RD(2) TX_WR(2) RX_RSR(2) RX_RD(2) */
        w->tx_wr = (uint16_t)((w->reg[2] << 8) | w->reg[3]);
        w->rx_rd = (uint16_t)((w->reg[6] << 8) | w->reg[7]);
        w->send_busy = false;
        return W_IDLE;
    default:
        return W_IDLE;
    }
}

static bool int_asserted(const w5500_t *w) {
    return w->int_pin >= 0 && !spibus_port_gpio_get(w->int_pin);
}

/* Nothing in flight: pick the next job. */
static w5500_phase_t choose(w5500_t *w, uint32_t now) {
    if (w->send_busy && (uint32_t)(now - w->send_ms) > W5500_SEND_TIMEOUT_MS) {
        /* SENDOK never came. Not seen on a healthy chip; reopening the
         * socket is cheaper than reasoning about what state it is in. */
        w->tx_timeouts++;
        return resync(w);
    }
    /* Link down: anything queued has nowhere to go. */
    if (!w->link_up) {
        while (w->ops->tx_peek(w->ctx, txb, SPIBUS_MAX_SEGS - 1) > 0) { w->ops->tx_done(w->ctx, false); }
    } else if (!w->send_busy) {
        n_txb = w->ops->tx_peek(w->ctx, txb, SPIBUS_MAX_SEGS - 1);
        if (n_txb > 0) {
            uint32_t total = 0;
            for (int i = 0; i < n_txb; i++) { total += txb[i].len; }
            if (total < W5500_FRAME_MIN || total > W5500_FRAME_MAX) {
                w->ops->tx_done(w->ctx, false);
            } else {
                w->tx_len = (uint16_t)total;
                return W_TX_DATA;
            }
        }
    }
    uint32_t every = w->int_pin >= 0 ? W5500_RX_POLL_MS : W5500_RX_POLL_NOINT;
    if (int_asserted(w) || (uint32_t)(now - w->last_rx_poll_ms) >= every) {
        w->want_ir = true;
        return W_IR_READ;
    }
    if (w->send_busy) { return W_IR_READ; }
    if ((uint32_t)(now - w->last_phy_ms) >= W5500_PHY_POLL_MS) { return W_PHY; }
    return W_IDLE;
}

bool w5500_step(w5500_t *w, uint32_t now) {
    if (!w->present) { return false; }
    if (w->phase != W_IDLE) {
        if (!w->xfer_done) { return false; }
        w->xfer_done = false;
        w5500_phase_t p = w->phase;
        w->phase = W_IDLE;
        spibus_release(&w->dev);
        w->pending = after(w, p, now);
        return true;
    }
    if (w->pending == W_IDLE) { w->pending = choose(w, now); }
    if (w->pending == W_IDLE) { return false; }
    if (!spibus_try_claim(&w->dev)) { w->bus_busy++; return false; }
    if (!begin(w, w->pending)) {
        spibus_release(&w->dev);
        return false;
    }
    w->phase = w->pending;
    w->pending = W_IDLE;
    return true;
}
