/*
 * src/io/spibus.c against a fake DMA (fake_spi.c).
 *
 * What it pins down: the bus is exclusive (a second device cannot claim it,
 * cannot transfer on it, and the holder cannot release it mid-transfer); a
 * chained transfer keeps CS low across every segment and skips empty ones;
 * completion is reported ONLY from spibus_poll(), never from the IRQ; and a
 * claim reprograms clock and mode for the device that takes the bus.
 *
 *   gcc -DSPIBUS_HOST -Isrc -Itest/host test/host/spibus_test.c src/io/spibus.c test/host/fake_spi.c
 */
#include <stdio.h>
#include <string.h>

#include "io/spibus.h"
#include "fake_spi.h"

static int checks, failed;
#define CHECK(c, msg) do { checks++; if (!(c)) { failed++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, msg); } } while (0)

static int cb_calls;
static void *cb_ctx_seen;
static void done_cb(void *ctx, bool ok) { (void)ok; cb_calls++; cb_ctx_seen = ctx; }

static uint8_t echo_dev(void *ctx, int ev, uint8_t mosi) { (void)ctx; return ev == FAKE_BYTE ? (uint8_t)(mosi ^ 0xFF) : 0; }

int main(void) {
    fake_reset();
    spibus_t *b = spibus_init(1, 14, 15, 8, "test");
    CHECK(b != NULL, "bus comes up");
    CHECK(spibus_init(1, 14, 15, 8, "test") == b, "same pins again is idempotent");
    CHECK(spibus_init(1, 2, 3, 4, "other") == NULL, "different pins on a live bus are refused");
    CHECK(spibus_get(0) == NULL, "an unopened bus is absent");
    CHECK(fake_reserved_count() == 3, "sck/mosi/miso reserved");

    spidev_t a, c;
    CHECK(spibus_dev_init(&a, b, 13, 30000000, 0, "a"), "device a");
    CHECK(spibus_dev_init(&c, b, 9, 1000000, 3, "c"), "device c");
    CHECK(!spibus_dev_init(&c, b, 13, 1000000, 3, "dup"), "a CS pin already reserved is refused");
    spibus_dev_init(&c, b, 9, 1000000, 3, "c");   /* restore (pin 9 stays reserved) */
    CHECK(fake_gpio(13) == 1 && fake_gpio(9) == 1, "CS idles high");
    fake_attach(13, echo_dev, NULL);
    fake_attach(9, echo_dev, NULL);

    /* exclusivity */
    uint8_t tx[4] = { 1, 2, 3, 4 }, rx[4];
    spiseg_t one = { tx, rx, 4 };
    CHECK(!spibus_xfer_async(&a, &one, 1, done_cb, NULL), "no transfer without a claim");
    CHECK(spibus_try_claim(&a), "a claims");
    CHECK(fake_baud(1) == 30000000 && fake_mode(1) == 0, "claim set a's clock and mode");
    CHECK(!spibus_try_claim(&c), "c cannot claim while a holds it");
    CHECK(b->claim_busy == 1, "the refusal is counted");
    CHECK(!spibus_xfer_async(&c, &one, 1, done_cb, NULL), "c cannot transfer on a's bus");

    /* a chained transfer */
    uint8_t hdr[3] = { 0xAA, 0xBB, 0xCC }, pay[5] = { 9, 8, 7, 6, 5 }, in[5];
    spiseg_t chain[4] = { { hdr, NULL, 3 }, { NULL, NULL, 0 }, { NULL, in, 5 }, { pay, NULL, 5 } };
    int starts0 = fake_dma_starts();
    cb_calls = 0;
    CHECK(spibus_xfer_async(&a, chain, 4, done_cb, (void *)0x55), "chain starts");
    CHECK(fake_gpio(13) == 0, "CS low for the transfer");
    CHECK(!spibus_xfer_async(&a, chain, 4, done_cb, NULL), "one transfer at a time");
    spibus_release(&a);
    CHECK(spibus_holds(&a), "release refused mid-transfer");
    int segs = 0;
    while (fake_dma_pending()) {
        CHECK(fake_gpio(13) == 0, "CS stays low between segments");
        fake_dma_complete();
        segs++;
        CHECK(cb_calls == 0, "the IRQ never runs the callback");
    }
    CHECK(segs == 3 && fake_dma_starts() - starts0 == 3, "three DMA segments, the empty one skipped");
    CHECK(in[0] == (uint8_t)~0 && in[4] == (uint8_t)~0, "rx-only segment clocked zeros out and stored what came back");
    CHECK(fake_gpio(13) == 1, "CS high once the last segment is done");
    CHECK(!fake_cs_glitched(), "CS never rose while a segment was in flight");
    CHECK(cb_calls == 0, "still not delivered before the loop");
    spibus_poll();
    CHECK(cb_calls == 1 && cb_ctx_seen == (void *)0x55, "delivered from spibus_poll with its context");
    spibus_poll();
    CHECK(cb_calls == 1, "delivered exactly once");

    /* hand-over */
    spibus_release(&a);
    CHECK(!spibus_holds(&a), "released");
    CHECK(spibus_try_claim(&c), "c claims now");
    CHECK(fake_baud(1) == 1000000 && fake_mode(1) == 3, "c's clock and mode applied on claim");
    /* blocking variant without an IRQ ever arriving: times out, no hang */
    CHECK(!spibus_xfer_blocking(&c, &one, 1), "a blocking transfer whose DMA never completes times out");
    CHECK(!b->busy && fake_gpio(9) == 1, "and leaves the bus idle with CS high");
    spibus_release(&c);
    CHECK(!spibus_holds(&c), "so the holder can still release it");

    /* the blocking variant (init only) with a DMA that completes */
    fake_autocomplete(true);
    CHECK(spibus_claim_blocking(&a, 10), "blocking claim");
    memset(rx, 0, sizeof rx);
    cb_calls = 0;
    CHECK(spibus_xfer_blocking(&a, &one, 1), "blocking transfer completes");
    CHECK(rx[0] == (uint8_t)~1 && rx[3] == (uint8_t)~4, "and moved the data");
    spibus_poll();
    CHECK(cb_calls == 0, "a blocking transfer leaves nothing for the loop");
    spibus_release(&a);
    fake_autocomplete(false);

    /* zero-length transfer completes through the loop */
    spibus_try_claim(&a);
    spiseg_t empty = { NULL, NULL, 0 };
    cb_calls = 0;
    CHECK(spibus_xfer_async(&a, &empty, 1, done_cb, NULL), "empty transfer accepted");
    CHECK(fake_gpio(13) == 1, "and CS is back high");
    spibus_poll();
    CHECK(cb_calls == 1, "empty transfer still completes via the loop");
    spibus_release(&a);

    /* spi.xfer's ad-hoc path refuses a bus a driver owns */
    const char *err = NULL;
    uint8_t o[2];
    CHECK(!spibus_adhoc_xfer(1, 14, 15, 8, 13, 1000000, tx, o, 2, &err) && err && strstr(err, "test"),
          "spi.xfer refused on an owned bus, naming the owner");
    CHECK(spibus_adhoc_xfer(0, 18, 19, 16, 17, 1000000, tx, o, 2, &err), "a free bus is allowed");

    char js[512];
    spibus_status_json(js, sizeof js);
    CHECK(strstr(js, "\"bus\":1") && strstr(js, "\"owner\":\"test\""), "status json");

    printf("%d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
