/* Send-buffer gate and a 2 MiB transfer beside a telemetry frame every turn.
 * Prints "N checks, M failed". The pump is the test, not the firmware: the
 * firmware calls ws_link_allow_bulk and sends at most one bulk frame. */
#include <stdio.h>
#include <string.h>

#include "net/ws_link.h"

static int checks, failures;

static void expect(int cond, const char *what) {
    checks++;
    if (!cond) {
        printf("FAIL: %s\n", what);
        failures++;
    }
}

enum { SND = 5840, MSS = 1460, TEL = 64, FRAME = 900 };

static void drain(unsigned *avail) {
    unsigned used = SND - *avail;
    unsigned d = MSS;
    if (d > used) {
        d = used;
    }
    *avail += d;
}

/* Telemetry is produced and sent before a file frame is considered.
 * `late` counts a file frame that went out on a turn whose telemetry did not. */
static void turn_fair(unsigned *avail, unsigned *left, unsigned *tel_sent, unsigned *tel_drop,
                      unsigned *raw_sent, unsigned *late) {
    int tel_out = 0;
    if (*avail >= TEL) {
        *avail -= TEL;
        (*tel_sent)++;
        tel_out = 1;
    } else {
        (*tel_drop)++;
    }
    if (*left > 0 && ws_link_allow_bulk(SND, *avail, FRAME)) {
        unsigned n = *left > WS_FILE_RAW ? WS_FILE_RAW : *left;
        if (!tel_out) {
            (*late)++;
        }
        *avail -= FRAME;
        *left -= n;
        *raw_sent += n;
    }
    drain(avail);
}

/* Fills the send buffer with file frames, then tries telemetry. */
static void turn_greedy(unsigned *avail, unsigned *left, unsigned *tel_drop) {
    while (*left > 0 && *avail >= FRAME) {
        unsigned n = *left > WS_FILE_RAW ? WS_FILE_RAW : *left;
        *avail -= FRAME;
        *left -= n;
    }
    if (*avail >= TEL) {
        *avail -= TEL;
    } else {
        (*tel_drop)++;
    }
    drain(avail);
}

static void test_gate(void) {
    expect(ws_link_allow_bulk(SND, SND, FRAME) == true, "full buffer allows one 900-byte frame");
    expect(ws_link_allow_bulk(SND, 4793, FRAME) == true, "exactly the reserve still allows it");
    expect(ws_link_allow_bulk(SND, 4792, FRAME) == false, "one byte under the reserve refuses");
    expect(ws_link_allow_bulk(SND, 4000, FRAME) == false, "a half-full buffer refuses a file frame");
    expect(ws_link_allow_bulk(SND, 800, FRAME) == false, "frame larger than the hole is refused");
    expect(ws_link_allow_bulk(SND, SND, 0) == false, "a zero-length frame is not bulk");
    expect(ws_link_allow_bulk(0, 0, FRAME) == false, "no send buffer");
    expect(ws_link_allow_bulk(1000, 1000, 400) == true, "small buffer uses half as the reserve");
    expect(ws_link_allow_bulk(1000, 800, 400) == false, "small buffer refuses when half would be eaten");
    expect(ws_link_allow_bulk(20000, 20000, FRAME) == true, "reserve caps at 4096");
    expect(ws_link_allow_bulk(WS_LINK_SND_BUF, 0, FRAME) == false, "queued telemetry is an empty buffer");
}

static void test_b64(void) {
    char enc[16];
    uint8_t dec[8];
    uint8_t raw[WS_FILE_RAW];
    char big[700];
    uint8_t back[WS_FILE_RAW];
    int n;
    size_t i;
    expect(ws_b64_encode((const uint8_t *)"foo", 3, enc, sizeof enc) == 4, "encode length");
    expect(strcmp(enc, "Zm9v") == 0, "encode foo");
    n = ws_b64_decode("Zm9v", 4, dec, sizeof dec);
    expect(n == 3 && memcmp(dec, "foo", 3) == 0, "decode foo");
    expect(ws_b64_decode("Zm9v=", 5, dec, sizeof dec) == -1, "bad length");
    expect(ws_b64_decode("Zm9@", 4, dec, sizeof dec) == -1, "bad alphabet");
    expect(ws_b64_decode("", 0, dec, sizeof dec) == 0, "empty");
    for (i = 0; i < sizeof raw; i++) {
        raw[i] = (uint8_t)(i * 17u + 3u);
    }
    expect(ws_b64_encode(raw, sizeof raw, big, sizeof big) > 0, "encode a chunk");
    n = ws_b64_decode(big, strlen(big), back, sizeof back);
    expect(n == (int)sizeof raw && memcmp(raw, back, sizeof raw) == 0, "chunk round trip");
    expect(ws_b64_decode(big, strlen(big), back, 10) == -1, "decode does not truncate");
}

static void test_transfer(void) {
    unsigned left = 2u * 1024u * 1024u;
    unsigned avail = SND;
    unsigned tel_sent = 0, tel_drop = 0, raw_sent = 0, late = 0;
    unsigned guard = 0;
    while (left && guard < 2000000u) {
        turn_fair(&avail, &left, &tel_sent, &tel_drop, &raw_sent, &late);
        guard++;
    }
    expect(left == 0, "2 MiB file finishes");
    expect(raw_sent == 2u * 1024u * 1024u, "every file byte is sent once");
    expect(tel_drop == 0, "telemetry is not dropped during the transfer");
    expect(late == 0, "a file frame never goes out ahead of that turn's telemetry");
    expect(tel_sent >= raw_sent / WS_FILE_RAW, "telemetry keeps up with every chunk");
    expect(guard < 2000000u, "the fair pump finishes");
}

static void test_abort(void) {
    unsigned left = 2u * 1024u * 1024u;
    unsigned avail = SND;
    unsigned tel_sent = 0, tel_drop = 0, raw_sent = 0, late = 0;
    while (raw_sent < 200000u && left) {
        turn_fair(&avail, &left, &tel_sent, &tel_drop, &raw_sent, &late);
    }
    expect(raw_sent >= 200000u, "abort runs until 200 KB have moved");
    expect(left > 0, "abort leaves the rest of the file");
    expect(tel_drop == 0 && late == 0, "abort does not drop or delay telemetry");
}

static void test_greedy(void) {
    unsigned left = 2u * 1024u * 1024u;
    unsigned avail = SND;
    unsigned tel_drop = 0;
    unsigned guard = 0;
    while (left && guard < 2000000u) {
        turn_greedy(&avail, &left, &tel_drop);
        guard++;
    }
    expect(tel_drop > 0, "ignoring the reserve drops telemetry");
}

int main(void) {
    test_gate();
    test_b64();
    test_transfer();
    test_abort();
    test_greedy();
    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
