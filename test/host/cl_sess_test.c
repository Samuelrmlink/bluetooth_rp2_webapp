/* Session rules with no radio and no BTstack.
 * gcc -O1 -Wall -Wextra -Isrc -Itest/host test/host/cl_sess_test.c src/bt/classic/cl_sess.c
 */
#include <stdio.h>
#include <string.h>

#include "bt/classic/cl_sess.h"

static int checks, failed;
#define CHECK(c, msg) do { \
    checks++; \
    if (!(c)) { failed++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, msg); } \
} while (0)

static cl_open_t opt_of(const uint8_t *addr, uintptr_t owner) {
    cl_open_t o;
    memset(&o, 0, sizeof o);
    memcpy(o.addr, addr, 6);
    o.level = CL_LEVEL_DEFAULT;
    o.io = CL_IO_NONE;
    o.bond = CL_BOND_GENERAL;
    o.owner = owner;
    return o;
}

static void put_pin(cl_open_t *o, const char *pin) {
    snprintf(o->pin[o->npin], CL_PIN_LEN, "%s", pin);
    o->npin++;
}

int main(void) {
    const uint8_t a[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x01};
    const uint8_t b[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x02};
    cl_info_t ia, ib;
    char pin[CL_PIN_LEN];

    /* Two owners. Releasing one leaves the other up. */
    cl_sess_reset();
    cl_open_t oa = opt_of(a, 1);
    cl_open_t ob = opt_of(b, 2);
    oa.io = CL_IO_KEYBOARD;
    ob.io = CL_IO_KEYBOARD;
    int ha = cl_open(&oa, 1000);
    int hb = cl_open(&ob, 1000);
    CHECK(ha > 0 && hb > 0 && ha != hb, "two sessions");
    CHECK(cl_release_owner(1) == 1, "one owner released");
    CHECK(cl_info(ha, &ia) == CL_OK && ia.state == CL_CLOSING, "released session closing");
    CHECK(cl_info(hb, &ib) == CL_OK && ib.state == CL_OPENING && ib.owner == 2, "other session stays");
    CHECK(cl_rfcomm_up(hb, 127) == CL_OK, "survivor still accepts rfcomm");

    /* PIN list is per session, in order. */
    cl_sess_reset();
    oa = opt_of(a, 1);
    ob = opt_of(b, 2);
    oa.io = CL_IO_KEYBOARD;
    ob.io = CL_IO_KEYBOARD;
    put_pin(&oa, "1111");
    put_pin(&oa, "2222");
    put_pin(&ob, "9999");
    ha = cl_open(&oa, 1000);
    hb = cl_open(&ob, 1000);
    CHECK(cl_pin_pull(ha, pin, sizeof pin) == 1 && strcmp(pin, "1111") == 0, "first pin on A");
    CHECK(cl_pin_pull(ha, pin, sizeof pin) == 1 && strcmp(pin, "2222") == 0, "second pin on A");
    CHECK(cl_pin_pull(hb, pin, sizeof pin) == 1 && strcmp(pin, "9999") == 0, "B keeps its own pin");
    CHECK(cl_pin_pull(ha, pin, sizeof pin) == 0 && pin[0] == 0, "A waits after its list");
    CHECK(cl_info(ha, &ia) == CL_OK && ia.ask == CL_ASK_PIN, "A ask is pin");
    CHECK(strcmp(pin, "0000") != 0 && strcmp(pin, "1234") != 0, "no invented pin");

    /* No list: the request stays outstanding. A preloaded code answers,
     * and a rejection after that code makes the next request outstanding. */
    cl_sess_reset();
    oa = opt_of(a, 1);
    oa.io = CL_IO_KEYBOARD;
    ha = cl_open(&oa, 1000);
    strcpy(pin, "sentinel");
    CHECK(cl_pin_pull(ha, pin, sizeof pin) == 0, "empty list waits");
    CHECK(pin[0] == 0, "empty list writes no code");
    CHECK(strcmp(pin, "0000") != 0 && strcmp(pin, "1234") != 0, "still no 0000 or 1234");
    CHECK(cl_submit_pin(ha, "4242") == CL_OK, "submit on that handle");
    int kind = 0, neg = 0;
    char ans[CL_PIN_LEN];
    CHECK(cl_take_answer(ha, &kind, &neg, ans, sizeof ans, NULL) == 1, "answer queued");
    CHECK(kind == CL_ASK_PIN && !neg && strcmp(ans, "4242") == 0, "submitted code");

    cl_sess_reset();
    oa = opt_of(a, 1);
    oa.io = CL_IO_KEYBOARD;
    put_pin(&oa, "1111");
    ha = cl_open(&oa, 1000);
    CHECK(cl_pin_pull(ha, pin, sizeof pin) == 1 && strcmp(pin, "1111") == 0, "preloaded answers");
    CHECK(cl_info(ha, &ia) == CL_OK && ia.ask == CL_ASK_NONE, "no wait while the list answered");
    CHECK(cl_note_pin_rejected(ha) == 0, "rejection after the last code");
    CHECK(cl_info(ha, &ia) == CL_OK && ia.ask == CL_ASK_PIN, "next request is outstanding");

    /* Two codes: rejection leaves the next preloaded code for the next pull. */
    cl_sess_reset();
    oa = opt_of(a, 1);
    oa.io = CL_IO_KEYBOARD;
    put_pin(&oa, "1111");
    put_pin(&oa, "2222");
    ha = cl_open(&oa, 1000);
    CHECK(cl_pin_pull(ha, pin, sizeof pin) == 1, "first of two");
    CHECK(cl_note_pin_rejected(ha) == 1, "another preloaded pin remains");
    CHECK(cl_pin_pull(ha, pin, sizeof pin) == 1 && strcmp(pin, "2222") == 0, "next pin in order");

    /* A numeric preloaded PIN answers a passkey. A word does not. */
    cl_sess_reset();
    oa = opt_of(a, 1);
    oa.io = CL_IO_KEYBOARD;
    put_pin(&oa, "4242");
    ha = cl_open(&oa, 1000);
    uint32_t pk = 1;
    CHECK(cl_passkey_pull(ha, &pk) == 1 && pk == 4242, "numeric pin is a passkey");
    ob = opt_of(b, 1);
    ob.io = CL_IO_KEYBOARD;
    put_pin(&ob, "abcd");
    hb = cl_open(&ob, 1000);
    CHECK(cl_passkey_pull(hb, &pk) == 0, "non-numeric pin leaves passkey outstanding");
    CHECK(cl_info(hb, &ib) == CL_OK && ib.ask == CL_ASK_PASSKEY, "passkey ask");

    /* Auth failure retries the challenged session once. */
    cl_sess_reset();
    oa = opt_of(a, 1);
    ob = opt_of(b, 2);
    oa.io = CL_IO_KEYBOARD;
    ob.io = CL_IO_KEYBOARD;
    oa.drop_key = 1;
    put_pin(&oa, "1111");
    ha = cl_open(&oa, 1000);
    hb = cl_open(&ob, 1000);
    CHECK(cl_pin_pull(ha, pin, sizeof pin) == 1, "used the first pin");
    CHECK(cl_auth_fail(ha) == CL_RETRY, "drop-key retries once");
    CHECK(cl_info(ha, &ia) == CL_OK && ia.state == CL_OPENING && ia.retried, "A still opening");
    CHECK(cl_info(hb, &ib) == CL_OK && ib.state == CL_OPENING && !ib.retried, "B was not retried");
    CHECK(cl_pin_pull(ha, pin, sizeof pin) == 1 && strcmp(pin, "1111") == 0, "retry uses A's list again");
    CHECK(cl_auth_fail(ha) == 0, "second failure sticks");
    CHECK(cl_info(ha, &ia) == CL_OK && ia.state == CL_FAILED && ia.why == CL_WHY_AUTH, "A failed");
    CHECK(cl_info(hb, &ib) == CL_OK && ib.state == CL_OPENING, "B still opening");

    /* Bytes queued on A do not make B ineligible. */
    cl_sess_reset();
    oa = opt_of(a, 1);
    ob = opt_of(b, 2);
    oa.io = CL_IO_NONE;
    ob.io = CL_IO_NONE;
    ha = cl_open(&oa, 1000);
    hb = cl_open(&ob, 1000);
    CHECK(cl_rfcomm_up(ha, 64) == CL_OK && cl_rfcomm_up(hb, 64) == CL_OK, "both channels");
    CHECK(cl_security(ha, 2, 0, 0, 1) == CL_OK, "A level 2");
    CHECK(cl_security(hb, 2, 0, 0, 1) == CL_OK, "B level 2");
    uint8_t blob[CL_TX_CAP];
    memset(blob, 0xAB, sizeof blob);
    CHECK(cl_tx_put(ha, blob, sizeof blob) == (int)sizeof blob, "A takes a full buffer");
    CHECK(cl_tx_put(ha, blob, 1) == CL_EBUSY, "A is full");
    CHECK(cl_tx_put(hb, "xy", 2) == 2, "B still accepts");
    CHECK(cl_tx_eligible(hb), "B stays eligible");
    int n1 = cl_tx_next();
    int n2 = cl_tx_next();
    CHECK(n1 == ha && n2 == hb, "round robin visits B");
    CHECK(cl_tx_next() == ha, "then back to A");

    /* Separate RX rings and drop counts. */
    cl_sess_reset();
    oa = opt_of(a, 1);
    ob = opt_of(b, 2);
    ha = cl_open(&oa, 1000);
    hb = cl_open(&ob, 1000);
    cl_rfcomm_up(ha, 32);
    cl_rfcomm_up(hb, 32);
    cl_security(ha, 2, 0, 0, 1);
    cl_security(hb, 2, 0, 0, 1);
    uint8_t big[CL_RX_CAP + 10];
    memset(big, 0x11, sizeof big);
    CHECK(cl_rx_put(ha, big, sizeof big) == CL_RX_CAP, "A stores one ring");
    CHECK(cl_info(ha, &ia) == CL_OK && ia.rx_drop == 10, "A counts its own drops");
    uint8_t few[4] = {1, 2, 3, 4};
    CHECK(cl_rx_put(hb, few, sizeof few) == 4, "B stores its own");
    CHECK(cl_info(hb, &ib) == CL_OK && ib.rx_drop == 0, "B did not take A's drops");
    uint8_t got[8];
    CHECK(cl_listen(hb, got, sizeof got) == 4 && memcmp(got, few, 4) == 0, "B's bytes");
    CHECK(cl_listen(ha, got, 3) == 3 && got[0] == 0x11, "A's bytes");
    uint8_t chunk[300];
    int ln = cl_listen(ha, chunk, sizeof chunk);
    CHECK(ln == CL_LISTEN_MAX, "listen caps at 252");

    /* A filter changes one caller's copy, not the shared table. */
    cl_sess_reset();
    const uint8_t d1[6] = {1, 0, 0, 0, 0, 1};
    const uint8_t d2[6] = {2, 0, 0, 0, 0, 2};
    CHECK(cl_inq_start() == CL_OK, "inquiry starts");
    CHECK(cl_inq_note(d1, -40, 0x000400, "Alpha") == 1, "first device");
    CHECK(cl_inq_note(d2, -50, 0x000500, "Beta") == 1, "second device");
    CHECK(cl_inq_start() == CL_EBUSY, "second inquiry is busy");
    CHECK(cl_inq_count() == 2, "busy start did not clear the table");
    cl_dev_t filt[4];
    int nf = cl_inq_filter(0x1F00, 0x0400, NULL, filt, 4);
    CHECK(nf == 1 && memcmp(filt[0].addr, d1, 6) == 0, "cod filter is one device");
    CHECK(cl_inq_count() == 2, "shared table unchanged by cod filter");
    nf = cl_inq_filter(0, 0, "Be", filt, 4);
    CHECK(nf == 1 && strcmp(filt[0].name, "Beta") == 0, "name prefix is the other device");
    CHECK(cl_inq_count() == 2, "shared table unchanged by name filter");
    cl_inq_stop();
    oa = opt_of(a, 1);
    ha = cl_open(&oa, 1000);
    CHECK(ha > 0 && cl_inq_start() == CL_OK, "an open session does not forbid inquiry");

    /* Level 3 does not open on an unauthenticated result. Level 2 does. */
    cl_sess_reset();
    oa = opt_of(a, 1);
    oa.level = 3;
    oa.io = CL_IO_KEYBOARD;
    ha = cl_open(&oa, 1000);
    cl_rfcomm_up(ha, 40);
    CHECK(cl_security(ha, 2, 0, 0, 1) == CL_ESTATE, "level 3 rejects unauthenticated");
    CHECK(cl_info(ha, &ia) == CL_OK && ia.state == CL_FAILED && !ia.secure, "level 3 not open");
    ob = opt_of(b, 2);
    ob.level = 2;
    ob.io = CL_IO_NONE;
    hb = cl_open(&ob, 1000);
    cl_rfcomm_up(hb, 40);
    CHECK(cl_security(hb, 2, 0, 0, 1) == CL_OK, "level 2 accepts encryption");
    CHECK(cl_info(hb, &ib) == CL_OK && ib.state == CL_OPEN, "level 2 is open");

    /* Level 4 also needs a Secure Connections key. */
    cl_sess_reset();
    oa = opt_of(a, 1);
    oa.level = 4;
    oa.io = CL_IO_YESNO;
    ha = cl_open(&oa, 1000);
    cl_rfcomm_up(ha, 40);
    CHECK(cl_security(ha, 3, 1, 0, 1) == CL_ESTATE, "authenticated without SC is not level 4");
    cl_sess_reset();
    ha = cl_open(&oa, 1000);
    cl_rfcomm_up(ha, 40);
    CHECK(cl_security(ha, 4, 1, 1, 0) == CL_OK, "level 4 with SC opens");
    CHECK(cl_info(ha, &ia) == CL_OK && ia.state == CL_OPEN, "level 4 open");

    /* No-input/no-output with level 3 or 4 fails before any code. */
    cl_sess_reset();
    oa = opt_of(a, 1);
    oa.level = 3;
    oa.io = CL_IO_NONE;
    CHECK(cl_open(&oa, 1000) == CL_EINVAL, "level 3 with no io");
    oa.level = 4;
    CHECK(cl_open(&oa, 1000) == CL_EINVAL, "level 4 with no io");
    CHECK(cl_handles(NULL, 8) == 0, "no session was allocated");

    /* A different IO mode during an unfinished exchange is busy. */
    cl_sess_reset();
    oa = opt_of(a, 1);
    oa.io = CL_IO_KEYBOARD;
    oa.level = 3;
    ha = cl_open(&oa, 1000);
    ob = opt_of(b, 2);
    ob.io = CL_IO_YESNO;
    ob.level = 3;
    CHECK(cl_open(&ob, 1000) == CL_EBUSY, "other io while exchange is unfinished");
    ob.io = CL_IO_KEYBOARD;
    hb = cl_open(&ob, 1000);
    CHECK(hb > 0, "same io is allowed");
    CHECK(cl_io_done(ha) == CL_OK && cl_io_done(hb) == CL_OK, "exchange finished");
    const uint8_t c[6] = {3, 3, 3, 3, 3, 3};
    cl_open_t oc = opt_of(c, 3);
    oc.io = CL_IO_DISPLAY;
    oc.level = 2;
    CHECK(cl_open(&oc, 1000) > 0, "a new io is free once the exchange is done");

    /* Channel 0 stays 0. UUID and fallback are the caller's. */
    cl_sess_reset();
    oa = opt_of(a, 1);
    oa.uuid = 0x1234;
    oa.channel = 0;
    oa.fallback = 0;
    ha = cl_open(&oa, 1000);
    CHECK(cl_info(ha, &ia) == CL_OK, "opened");
    CHECK(ia.channel == 0 && ia.uuid == 0x1234 && ia.fallback == 0, "no assumed spp channel");
    CHECK(ia.uuid != 0x1101, "uuid was not rewritten");
    oa.fallback = 5;
    oa.addr[5] = 9;
    hb = cl_open(&oa, 1000);
    CHECK(cl_info(hb, &ib) == CL_OK && ib.channel == 0 && ib.fallback == 5, "fallback stored, channel still 0");
    CHECK(cl_set_channel(hb, 5) == CL_OK, "sdp may fill the channel");
    CHECK(cl_info(hb, &ib) == CL_OK && ib.channel == 5, "channel is what sdp returned");

    /* Omitted level is 2. The table fills at CL_SESS_MAX. */
    cl_sess_reset();
    oa = opt_of(a, 1);
    oa.level = CL_LEVEL_OMIT;
    oa.bond = 0xFF;
    ha = cl_open(&oa, 1000);
    CHECK(cl_info(ha, &ia) == CL_OK && ia.level == 2 && ia.bond == CL_BOND_GENERAL, "defaults");
    int opened = 1;
    for (int i = 1; i < CL_SESS_MAX; i++) {
        uint8_t addr[6] = {9, 9, 9, 9, 9, (uint8_t)i};
        cl_open_t o = opt_of(addr, 1);
        o.io = CL_IO_NONE;
        if (cl_open(&o, 1000) > 0) { opened++; }
    }
    CHECK(opened == CL_SESS_MAX, "table length");
    uint8_t extra[6] = {8, 8, 8, 8, 8, 8};
    cl_open_t ox = opt_of(extra, 1);
    CHECK(cl_open(&ox, 1000) == CL_ENOMEM, "full table");

    /* Send before the level is met is busy. Deadline fails only that session. */
    cl_sess_reset();
    oa = opt_of(a, 1);
    oa.level = 3;
    oa.io = CL_IO_KEYBOARD;
    oa.deadline_ms = 50;
    ha = cl_open(&oa, 1000);
    CHECK(cl_tx_put(ha, "z", 1) == CL_EBUSY, "not open yet");
    CHECK(cl_listen(ha, got, 4) == CL_EBUSY, "no payload before the level");
    ob = opt_of(b, 2);
    ob.io = CL_IO_KEYBOARD;
    ob.level = 3;
    hb = cl_open(&ob, 1000);
    cl_tick(1049);
    CHECK(cl_info(ha, &ia) == CL_OK && ia.state == CL_OPENING, "deadline not yet");
    cl_tick(1050);
    CHECK(cl_info(ha, &ia) == CL_OK && ia.state == CL_FAILED && ia.why == CL_WHY_TIMEOUT, "A hit its deadline");
    CHECK(cl_info(hb, &ib) == CL_OK && ib.state == CL_OPENING, "B has no deadline");

    /* Confirm carries the six-digit number, and a decline fails only that session. */
    cl_sess_reset();
    oa = opt_of(a, 1);
    oa.io = CL_IO_YESNO;
    oa.level = 3;
    ha = cl_open(&oa, 1000);
    CHECK(cl_confirm_ask(ha, 123456) == 0, "confirm waits");
    CHECK(cl_info(ha, &ia) == CL_OK && ia.ask == CL_ASK_CONFIRM && ia.confirm_num == 123456, "number stored");
    CHECK(cl_submit_confirm(ha, 0) == CL_OK, "decline");
    CHECK(cl_info(ha, &ia) == CL_OK && ia.state == CL_FAILED && ia.why == CL_WHY_CANCEL, "decline fails the session");

    printf("%d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
