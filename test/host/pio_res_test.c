/* Ledger over the fake port. Prints "N checks, M failed". */
#include <stdio.h>
#include <string.h>

#include "io/pio_dma.h"
#include "io/pio_res.h"

void pio_port_fake_reserve(int pin, const char *owner);
int pio_port_fake_dma_starts(void);
int pio_port_fake_dma_aborts(void);
int pio_port_fake_dma_drains(void);
int pio_port_fake_dma_last(void);
const uint32_t *pio_port_fake_dma_src(int ch);
int pio_port_fake_dma_inc(int ch);
int pio_port_fake_dma_active(int ch);
void pio_port_fake_drop_instr(void);
int pio_port_fake_dropped(void);
uint16_t pio_port_fake_mem(int block, int offset);

static int checks, failures;

static void expect(int cond, const char *what) {
    checks++;
    if (!cond) {
        printf("FAIL: %s\n", what);
        if (pio_res_last_error()[0]) { printf("  last: %s\n", pio_res_last_error()); }
        failures++;
    }
}

static void release_all(void) {
    for (int h = 1; h <= 12; h++) {
        if (pio_res_owner(h)) { pio_res_release(h); }
    }
}

static int claim(const char *owner, uint8_t pin, uint32_t nwords, int origin) {
    return pio_res_claim(owner, &pin, 1, nwords, origin);
}

static void deliver(void *mod, pio_res_irq_fn fn, void *arg) {
    (void)mod;
    fn(arg);
}

static int s_hits;

static void on_irq(void *arg) { s_hits += (int)(uintptr_t)arg; }

static void test_empty_and_fit(void) {
    pio_res_block_t st[3];
    size_t n = 0;
    expect(pio_res_stat(st, 3, &n) == PIO_OK && n == 3, "three blocks");
    expect(st[0].instr_used == 0 && st[0].instr_free == 32 && st[0].instr_map == 0, "no words yet");
    expect(st[0].sm_free == 0xfu && st[0].gpio_base == -1 && st[1].gpio_base == -1, "machines free, base unset");
    expect(pio_res_last_error()[0] == 0, "stat is quiet");
    uint8_t pin = 9;
    expect(pio_res_fit("anker", &pin, 1, 4, -1) == 0, "fit picks pio0");
    expect(pio_res_stat(st, 3, &n) == PIO_OK && st[0].sm_free == 0xfu && st[0].instr_used == 0,
           "fit does not claim");
}

static void test_load_and_shadow(void) {
    release_all();
    int h = claim("anker", 2, 4, -1);
    expect(h > 0 && pio_res_block(h) == 0 && pio_res_offset(h) == 0, "first program at 0");
    pio_res_block_t st[3];
    expect(pio_res_stat(st, 1, NULL) == PIO_OK && st[0].instr_used == 4 && st[0].instr_map == 0xfu,
           "claim reserves the four words");
    uint16_t words[4] = { 0x1111, 0x2222, 0x3333, 0x4444 };
    expect(pio_res_program(h, words, 4) == PIO_OK, "program loads");
    expect(pio_res_last_error()[0] == 0, "load is quiet");
    expect(pio_port_fake_mem(0, 0) == 0x1111, "a jmp at offset 0 is unchanged");
    int gap = claim("ws2812", 5, 4, -1);
    expect(gap > 0 && pio_res_block(gap) == 0 && pio_res_offset(gap) == 4, "next program sits at 4");
    uint16_t jmpw[4] = { 0x0002, 0x0042, 0x60f0, 0x0000 };
    expect(pio_res_program(gap, jmpw, 4) == PIO_OK, "jmp program loads");
    expect(pio_port_fake_mem(0, 4) == 0x0006 && pio_port_fake_mem(0, 5) == 0x0046 &&
           pio_port_fake_mem(0, 6) == 0x60f0 && pio_port_fake_mem(0, 7) == 0x0004,
           "jmp targets move by the load offset");
    uint16_t jback[4] = { 0 };
    expect(pio_res_instr(0, 4, jback, 4) == 4 && memcmp(jback, jmpw, sizeof jmpw) == 0,
           "shadow keeps the assembled words");
    pio_res_release(gap);
    expect(pio_res_stat(st, 1, NULL) == PIO_OK && st[0].instr_used == 4 && st[0].instr_free == 28 &&
           st[0].instr_map == 0xfu, "map after load");
    pio_port_fake_drop_instr();
    expect(pio_port_fake_dropped() && pio_port_fake_mem(0, 0) == 0, "fake dropped its write-only memory");
    uint16_t back[4] = { 0 };
    expect(pio_res_instr(0, 0, back, 4) == 4 && memcmp(back, words, sizeof words) == 0,
           "instr reads the shadow");
    pio_res_prog_t prog[4];
    size_t np = 0;
    expect(pio_res_programs(0, prog, 4, &np) == PIO_OK && np == 1, "one range");
    expect(prog[0].offset == 0 && prog[0].length == 4 && prog[0].origin == -1 &&
           prog[0].sm_mask == 0x1u && strcmp(prog[0].owner, "anker") == 0, "range record");

    int h2 = claim("anker", 3, 4, 0);
    expect(h2 > 0 && pio_res_block(h2) == 0 && pio_res_sm(h2) != pio_res_sm(h), "second machine, same copy");
    expect(pio_res_program(h2, words, 4) == PIO_OK, "same words attach");
    expect(pio_res_stat(st, 1, NULL) == PIO_OK && st[0].instr_used == 4 && st[0].instr_map == 0xfu,
           "shared copy is not double-counted");
    expect(pio_res_programs(0, prog, 4, &np) == PIO_OK && np == 1 && prog[0].sm_mask == 0x3u,
           "both machines on the one range");
    uint16_t other[4] = { 9, 9, 9, 9 };
    expect(pio_res_program(h2, other, 4) == PIO_EINVAL, "a different program is refused");
    expect(strcmp(pio_res_last_error(),
                  "pio anker: program at offset 0 on pio0 does not match the 4 words loaded") == 0,
           "mismatch names the offset");

    int fresh = claim("anker", 4, 4, -1);
    expect(fresh > 0 && pio_res_offset(fresh) == 4, "origin -1 takes a new gap");
    expect(pio_res_stat(st, 1, NULL) == PIO_OK && st[0].instr_used == 8, "the new gap counts");
    pio_res_release(h2);
    expect(pio_res_stat(st, 1, NULL) == PIO_OK && st[0].instr_used == 8, "one sharer leaving keeps the words");
    pio_res_release(h);
    expect(pio_res_instr(0, 0, back, 4) == PIO_ENOENT, "last sharer clears the shadow");
    pio_res_release(fresh);
}

static void test_exact_lines(void) {
    release_all();
    expect(claim("a", 1, 32, -1) > 0, "pio0 full");
    expect(claim("a", 2, 28, -1) > 0, "pio1 holds 28");
    expect(claim("a", 3, 32, -1) > 0, "pio2 full");
    uint8_t pin = 4;
    expect(pio_res_claim("anker", &pin, 1, 8, -1) == PIO_EBUSY, "eight more words do not fit");
    expect(strcmp(pio_res_last_error(),
                  "pio anker: instruction memory full on pio1 (used 28 of 32, need 8)") == 0,
           "full line names pio1");
    release_all();

    for (int i = 0; i < 4; i++) { expect(claim("a", 40, 1, -1) > 0, "fill pio0 at base 16"); }
    for (int i = 0; i < 4; i++) { expect(claim("a", 41, 1, -1) > 0, "fill pio1 at base 16"); }
    for (int i = 0; i < 4; i++) { expect(claim("a", 42, 1, -1) > 0, "fill pio2 at base 16"); }
    uint8_t low[4] = { 2, 3, 4, 5 };
    expect(pio_res_claim("anker", low, 4, 1, -1) == PIO_EBUSY, "low pins miss base 16");
    expect(strcmp(pio_res_last_error(),
                  "pio anker: pins 2-5 outside pio0 gpio base 16") == 0,
           "window line names pio0");
    release_all();

    expect(claim("hold", 40, 1, -1) > 0 && pio_res_block(claim("tmp", 0, 1, -1)) == 1, "pin 0 lands on pio1");
    /* claim() above consumed a handle inside the expect. Release the tmp by owner. */
    pio_res_release_owner("tmp");
    int sd = claim("sd", 0, 1, -1);
    int anker = claim("anker", 1, 1, -1);
    expect(sd > 0 && anker > 0 && pio_res_block(sd) == 1 && pio_res_block(anker) == 1, "both on pio1");
    expect(pio_res_irq(sd, 0, PIO_IRQ_RX, on_irq, (void *)(uintptr_t)1, NULL) == PIO_OK, "sd takes line 0");
    expect(pio_res_irq(anker, 0, PIO_IRQ_RX, on_irq, (void *)(uintptr_t)1, NULL) == PIO_EBUSY,
           "second handler is refused");
    expect(strcmp(pio_res_last_error(),
                  "pio anker: cpu irq 0 on pio1 already owned by sd") == 0,
           "irq line names the owner");
    release_all();
}

static void test_base_and_flags(void) {
    release_all();
    int hi = claim("a", 40, 1, -1);
    int lo = claim("b", 0, 1, -1);
    int again = claim("c", 41, 1, -1);
    expect(hi > 0 && lo > 0 && again > 0, "three claims");
    expect(pio_res_block(hi) == 0 && pio_res_block(lo) == 1 && pio_res_block(again) == 0,
           "an open window is preferred, then an empty block");
    pio_res_block_t st[3];
    expect(pio_res_stat(st, 3, NULL) == PIO_OK && st[0].gpio_base == 16 && st[1].gpio_base == 0 &&
           st[2].gpio_base == -1, "bases recorded");
    expect(pio_res_last_error()[0] == 0, "a base that fits is quiet");
    uint8_t span[2] = { 0, 40 };
    expect(pio_res_claim("anker", span, 2, 1, -1) == PIO_EINVAL, "a 40-pin span is refused");
    uint8_t straddle[2] = { 10, 40 };
    expect(pio_res_claim("anker", straddle, 2, 1, -1) == PIO_EINVAL &&
           strcmp(pio_res_last_error(),
                  "pio anker: pins 10-40 do not fit one gpio window") == 0,
           "pins below 16 and above 31 fit neither window");
    release_all();
    expect(pio_res_stat(st, 3, NULL) == PIO_OK && st[0].gpio_base == -1 && st[0].instr_used == 0 &&
           st[0].sm_free == 0xfu, "the last machine clears the base");

    int h1 = claim("a", 2, 1, -1);
    int h2 = claim("b", 3, 1, -1);
    uint16_t irq = 0xC000; /* IRQ set 0 */
    expect(pio_res_program(h1, &irq, 1) == PIO_OK && pio_res_program(h2, &irq, 1) == PIO_OK,
           "two programs may both reference flag 0");
    expect(pio_res_stat(st, 1, NULL) == PIO_OK && (st[0].irq_used & 1u) == 1u, "flag 0 is in use");
    pio_res_prog_t pr[4];
    size_t np = 0;
    expect(pio_res_programs(0, pr, 4, &np) == PIO_OK && np == 2 &&
           (pr[0].irq_flags & 1u) && (pr[1].irq_flags & 1u), "each range names flag 0");

    release_all();
    int s0 = claim("a", 4, 1, 0);
    int s1 = claim("b", 5, 1, 0);
    uint16_t rel = 0xC010; /* IRQ set 0 rel */
    expect(pio_res_program(s0, &rel, 1) == PIO_OK && pio_res_program(s1, &rel, 1) == PIO_OK, "relative irq");
    expect(pio_res_programs(0, pr, 4, &np) == PIO_OK && np == 1 && (pr[0].irq_flags & 0x3u) == 0x3u,
           "relative flag follows each machine");
    release_all();
}

static void test_pins_and_owner(void) {
    release_all();
    pio_port_fake_reserve(8, "w5500");
    pio_port_fake_reserve(2, "sd");
    expect(claim("anker", 8, 1, -1) == PIO_EINVAL, "w5500 pin is refused");
    expect(strstr(pio_res_last_error(), "pin 8 reserved by w5500") != NULL, "w5500 is named");
    expect(claim("anker", 2, 1, -1) == PIO_EINVAL, "sd pin is refused to anker");
    int sd = claim("sd", 2, 2, -1);
    expect(sd > 0, "sd may use its own pin");
    uint16_t words[2] = { 0xA5A5, 0x5A5A };
    expect(pio_res_program(sd, words, 2) == PIO_OK, "sd program");
    int mod = claim("anker", 6, 1, -1);
    expect(mod > 0, "anker takes a free pin");
    pio_res_release_owner("anker");
    expect(pio_res_owner(mod) == NULL, "anker is gone");
    expect(pio_res_owner(sd) != NULL && strcmp(pio_res_owner(sd), "sd") == 0, "sd stays");
    uint16_t back[2] = { 0 };
    expect(pio_res_instr(pio_res_block(sd), (uint32_t)pio_res_offset(sd), back, 2) == 2 &&
           back[0] == 0xA5A5 && back[1] == 0x5A5A, "sd words stay");
    pio_res_release_owner("sd");
    expect(pio_res_owner(sd) != NULL, "unload does not drop sd");
    expect(pio_res_check(sd, "anker") == PIO_EINVAL, "anker cannot drive sd");
    expect(strstr(pio_res_last_error(), "owned by sd") != NULL, "the check names sd");
    pio_res_release(sd);
    expect(pio_res_owner(sd) == NULL, "the card's own release works");
}

static void test_fifo_and_irq(void) {
    release_all();
    int h = claim("anker", 7, 1, -1);
    uint16_t w = 0x0000;
    expect(pio_res_program(h, &w, 1) == PIO_OK, "fifo program");
    expect(pio_res_put(h, 1) == PIO_OK && pio_res_put(h, 2) == PIO_OK &&
           pio_res_put(h, 3) == PIO_OK && pio_res_put(h, 4) == PIO_OK, "four words fit");
    expect(pio_res_put(h, 5) == PIO_EAGAIN, "the fifth does not spin");
    expect(strstr(pio_res_last_error(), "tx fifo full on pio") != NULL, "full fifo line");
    uint32_t tx = 0, rx = 99;
    expect(pio_res_levels(h, &tx, &rx) == PIO_OK && tx == 4 && rx == 0, "levels");
    expect(pio_res_get(h, &tx) == PIO_EAGAIN, "empty rx");

    pio_res_cfg_t cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.out_base = 7;
    cfg.out_count = 1;
    cfg.fifo_join = PIO_JOIN_TX;
    cfg.in_base = -1;
    cfg.jmp_pin = -1;
    expect(pio_res_init(h, &cfg) == PIO_OK, "join tx");
    int n = 0;
    for (; n < 8; n++) { if (pio_res_put(h, (uint32_t)n) != PIO_OK) { break; } }
    expect(n == 8 && pio_res_put(h, 9) == PIO_EAGAIN, "joined tx holds eight");

    s_hits = 0;
    expect(pio_res_irq(h, 1, PIO_IRQ_TX, on_irq, (void *)(uintptr_t)3, NULL) == PIO_OK, "line 1");
    pio_res_set_irq_bit(pio_res_block(h), 1);
    expect(s_hits == 0, "the bit does not call the handler");
    pio_res_poll(deliver);
    expect(s_hits == 3, "poll delivers on the main loop");
    pio_res_poll(deliver);
    expect(s_hits == 3, "a quiet poll does not deliver again");
    release_all();
}

static void drop_hw(void) {
    for (int b = 0; b < 3; b++) {
        for (int sm = 0; sm < 4; sm++) { pio_res_release_hw(b, sm); }
    }
}

static void test_protect(void) {
    uint16_t irq = 0xC000;
    uint8_t radio_pin = 24;
    int radio = pio_res_load("cyw43", &radio_pin, 1, &irq, 1, PIO_LOAD_PROTECT | PIO_LOAD_TOP);
    expect(radio > 0 && pio_res_block(radio) == 2, "the radio prefers the highest block");
    pio_res_prog_t row[4];
    size_t np = 0;
    expect(pio_res_programs(2, row, 4, &np) == PIO_OK && np == 1 && row[0].prot == 1,
           "the radio copy is marked protected");
    drop_hw();

    uint8_t a = 1, b = 3;
    radio = pio_res_load("cyw43", &a, 1, &irq, 1, PIO_LOAD_PROTECT);
    expect(radio > 0 && pio_res_block(radio) == 0 && pio_res_sm(radio) == 0, "radio on pio0 sm0");
    int led = pio_res_load("ws2812", &b, 1, &irq, 1, 0);
    expect(led == PIO_EBUSY &&
           strcmp(pio_res_last_error(),
                  "pio ws2812: irq flag on pio0 is used by protected cyw43 on sm0; a new program must not clear it") == 0,
           "a new program must not clear the radio's irq flag");
    uint16_t back = 0;
    expect(pio_res_instr(0, 0, &back, 1) == 1 && back == irq, "the refused load did not rewrite the radio");

    uint16_t grb = 0x6321;
    led = pio_res_load("ws2812", &b, 1, &grb, 1, 0);
    expect(led > 0 && pio_res_block(led) == 0 && pio_res_offset(led) == 1, "a strip fits beside the radio");
    expect(pio_res_release(led) == PIO_OK, "a strip can be released");
    expect(pio_res_instr(0, 1, &back, 1) == PIO_ENOENT, "releasing the strip frees its words");

    expect(pio_res_release(radio) == PIO_EBUSY &&
           strcmp(pio_res_last_error(),
                  "pio cyw43: cannot release protected cyw43 on pio0 sm0 (offset 0 length 1); that would stop it") == 0,
           "the radio cannot be released");
    expect(pio_res_program(radio, &irq, 1) == PIO_EBUSY, "the radio program cannot be rewritten");
    pio_res_cfg_t cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.in_base = -1;
    cfg.jmp_pin = -1;
    expect(pio_res_init(radio, &cfg) == PIO_EBUSY, "the radio machine cannot be reconfigured");
    expect(pio_res_enable(radio, 0) == PIO_EBUSY, "the radio machine cannot be stopped");
    expect(pio_res_put(radio, 1) == PIO_EBUSY, "the radio fifo cannot be filled");
    uint32_t word = 0;
    expect(pio_res_get(radio, &word) == PIO_EBUSY, "the radio fifo cannot be drained");
    uint32_t tx = 9, rx = 9;
    expect(pio_res_levels(radio, &tx, &rx) == PIO_OK, "reading fifo levels does not stop the radio");
    pio_res_release_owner("cyw43");
    expect(pio_res_owner(radio) != NULL, "unloading does not drop a protected machine");
    expect(pio_res_instr(0, 0, &back, 1) == 1 && back == irq, "the radio words are still there");

    uint16_t big[32];
    memset(big, 0, sizeof big);
    uint8_t p1 = 4, p2 = 5;
    expect(pio_res_load("fill", &p1, 1, big, 32, 0) > 0, "fill pio1");
    expect(pio_res_load("fill", &p2, 1, big, 32, 0) > 0, "fill pio2");
    uint8_t p3 = 6;
    expect(pio_res_claim("anker", &p3, 1, 1, 0) == PIO_EBUSY &&
           strcmp(pio_res_last_error(),
                  "pio anker: origin 0 overlaps protected cyw43 at offset 0 length 1 on pio0 sm0; those words stay") == 0,
           "an explicit origin on the radio is refused");
    expect(pio_res_instr(0, 0, &back, 1) == 1 && back == irq, "the overlap wrote nothing");
    release_all();
    expect(pio_res_owner(radio) != NULL, "release_all leaves the protected machine");
    drop_hw();
    expect(pio_res_instr(0, 0, &back, 1) == PIO_ENOENT, "the radio deinit drops its own machine");

    uint16_t one = 1;
    uint8_t hi = 40;
    expect(pio_res_load("cyw43", &hi, 1, &one, 1, PIO_LOAD_PROTECT) > 0, "base 16 held by the radio");
    for (int n = 0; n < 11; n++) {
        uint8_t pin = 40;
        expect(pio_res_claim("cyw43", &pin, 1, 1, -1) > 0, "fill every machine at base 16");
    }
    uint8_t low[4] = { 0, 1, 3, 6 };
    expect(pio_res_claim("anker", low, 4, 1, -1) == PIO_EBUSY &&
           strcmp(pio_res_last_error(),
                  "pio anker: pins 0-6 outside pio0 gpio base 16; protected cyw43 on sm0 holds that base") == 0,
           "a protected machine holds the gpio base");
    drop_hw();
}

/* The firmware card driver loads both programs with PIO_LOAD_PROTECT.
 * A module may still take a disjoint gap; it may not stop the card. */
static void test_sd_protect(void) {
    uint8_t cmd_pins[2] = { 2, 3 };
    uint16_t cmd[11];
    memset(cmd, 0xA0, sizeof cmd);
    int hc = pio_res_load("sd", cmd_pins, 2, cmd, 11, PIO_LOAD_PROTECT);
    expect(hc > 0, "sd command loads protected");
    uint8_t data_pins[4] = { 4, 5, 6, 7 };
    uint16_t data[28];
    memset(data, 0xA0, sizeof data);
    int hd = pio_res_load("sd", data_pins, 4, data, 28, PIO_LOAD_PROTECT);
    expect(hd > 0, "sd data loads protected");

    pio_res_prog_t row[4];
    size_t np = 0;
    int saw_cmd = 0, saw_data = 0;
    for (int b = 0; b < 3; b++) {
        expect(pio_res_programs(b, row, 4, &np) == PIO_OK, "programs read");
        for (size_t i = 0; i < np && i < 4; i++) {
            if (strcmp(row[i].owner, "sd") != 0 || row[i].prot != 1) { continue; }
            if (row[i].length == 11) { saw_cmd = 1; }
            if (row[i].length == 28) { saw_data = 1; }
        }
    }
    expect(saw_cmd && saw_data, "both sd programs are marked protected");
    expect(pio_res_release(hc) == PIO_EBUSY &&
           strstr(pio_res_last_error(), "cannot release protected sd") != NULL,
           "the command machine cannot be released");
    expect(pio_res_release(hd) == PIO_EBUSY, "the data machine cannot be released");
    expect(pio_res_program(hd, data, 28) == PIO_EBUSY &&
           strstr(pio_res_last_error(), "cannot rewrite protected sd") != NULL,
           "the data program cannot be rewritten");
    pio_res_cfg_t cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.in_base = -1;
    cfg.jmp_pin = -1;
    expect(pio_res_init(hd, &cfg) == PIO_EBUSY, "the data machine cannot be reconfigured");
    expect(pio_res_enable(hc, 0) == PIO_EBUSY, "the command machine cannot be stopped");
    uint32_t tx = 9, rx = 9;
    expect(pio_res_levels(hd, &tx, &rx) == PIO_OK, "reading levels does not stop the card");
    pio_res_release_owner("sd");
    expect(pio_res_owner(hc) != NULL && pio_res_owner(hd) != NULL, "unload leaves the card");

    uint8_t pin = 0;
    uint16_t ledw[4] = { 0x6321, 0x1223, 0x1200, 0xa242 };
    int led = pio_res_load("ws2812", &pin, 1, ledw, 4, 0);
    expect(led > 0 && pio_res_programs(pio_res_block(led), row, 4, &np) == PIO_OK, "a strip fits beside protected sd");
    int led_prot = 0;
    for (size_t i = 0; i < np && i < 4; i++) {
        if (row[i].offset == (uint32_t)pio_res_offset(led) && row[i].prot) { led_prot = 1; }
    }
    expect(!led_prot, "the strip is not protected");
    expect(pio_res_release(led) == PIO_OK, "the strip can be released");
    int data_block = pio_res_block(hd);
    uint32_t data_off = (uint32_t)pio_res_offset(hd);
    uint16_t back = 0;
    expect(pio_res_instr(data_block, data_off, &back, 1) == 1 && back == 0xA0A0,
           "releasing the strip leaves the card's words");
    release_all();
    expect(pio_res_owner(hc) != NULL && pio_res_owner(hd) != NULL, "release_all leaves protected sd");
    drop_hw();
    expect(pio_res_instr(data_block, data_off, &back, 1) == PIO_ENOENT,
           "hardware release drops the card");
}

static int dma_tx(int handle, const char *owner, uint32_t *src, uint32_t n) {
    return pio_dma_tx(handle, owner, src, n, (uintptr_t)src, (uintptr_t)(src + n));
}

static int dma_arm(int handle, const char *owner, uint32_t *src, uint32_t n) {
    return pio_dma_arm(handle, owner, src, n, (uintptr_t)src, (uintptr_t)(src + n));
}

static int s_dma_hits;

static void on_dma(void *arg) {
    (void)arg;
    s_dma_hits++;
}

static void deliver_dma(void *mod, pio_dma_fn fn, void *arg) {
    (void)mod;
    if (fn) { fn(arg); }
}

static void test_dma(void) {
    drop_hw();
    release_all();
    for (int h = 1; h <= 12; h++) { pio_dma_abort(h); }
    int starts = pio_port_fake_dma_starts();
    uint16_t prog[4] = { 0x6321, 0x1223, 0x1200, 0xa242 };
    uint8_t pin = 22;
    int other = pio_res_load("other", &pin, 1, prog, 4, 0);
    uint32_t word = 0x11;
    expect(other > 0, "other machine loads");
    expect(dma_tx(other, "ws", &word, 1) == PIO_EINVAL &&
           strstr(pio_res_last_error(), "owned by other") != NULL,
           "a transfer on someone else's machine is refused");
    expect(pio_port_fake_dma_starts() == starts, "a refused transfer does not start");
    pio_res_release(other);

    pin = 21;
    int h = pio_res_load("ws", &pin, 1, prog, 4, 0);
    expect(h > 0, "ws machine loads");
    expect(dma_tx(h, "ws", &word, 0) == PIO_EINVAL &&
           strstr(pio_dma_last_error(), "not 1-65535") != NULL,
           "zero words are refused");
    uint32_t outside = 7;
    expect(pio_dma_tx(h, "ws", &outside, 1, (uintptr_t)&outside + 16,
                      (uintptr_t)&outside + 32) == PIO_EINVAL &&
           strstr(pio_dma_last_error(), "outside module memory") != NULL,
           "a source outside the module is refused");
    expect(pio_port_fake_dma_starts() == starts, "refused sources do not start");

    expect(pio_dma_listen(h, "ws", on_dma, 0, 0) == PIO_OK, "listen records a callback");
    s_dma_hits = 0;
    uint32_t sec_a[2] = { 1, 2 };
    uint32_t sec_b[2] = { 3, 4 };
    expect(dma_tx(h, "ws", sec_a, 2) == PIO_OK, "tx while idle starts");
    expect(pio_dma_idle(h, "ws") == 0, "a running transfer is not idle");
    expect(pio_port_fake_dma_starts() == starts + 1, "tx starts one transfer");
    int ch = pio_port_fake_dma_last();
    expect(ch >= 0 && pio_port_fake_dma_src(ch) == sec_a && pio_port_fake_dma_inc(ch) == 1,
           "the first section is the one that started");
    expect(dma_tx(h, "ws", sec_b, 2) == PIO_EBUSY, "tx while busy is refused");
    expect(dma_arm(h, "ws", sec_b, 2) == PIO_OK, "arm while busy stores one section");
    expect(pio_port_fake_dma_src(ch) == sec_a, "arm does not replace the running section");
    expect(dma_arm(h, "ws", sec_a, 2) == PIO_EAGAIN &&
           strstr(pio_dma_last_error(), "already armed") != NULL,
           "a second arm is refused");
    pio_dma_hw_done(ch);
    expect(pio_port_fake_dma_starts() == starts + 2 && pio_port_fake_dma_src(ch) == sec_b,
           "completion starts the armed section");
    expect(pio_dma_idle(h, "ws") == 0, "the armed section is now running");
    pio_dma_poll(deliver_dma);
    expect(s_dma_hits == 1, "poll delivers the completion on the main loop");
    pio_dma_hw_done(ch);
    expect(pio_dma_idle(h, "ws") == 1, "completion with nothing armed goes idle");
    pio_dma_poll(deliver_dma);
    expect(s_dma_hits == 2, "the last section is delivered too");

    expect(dma_tx(h, "ws", sec_a, 2) == PIO_OK, "tx after idle starts again");
    expect(dma_arm(h, "ws", sec_b, 2) == PIO_OK, "arm stores the section abort will drop");
    int aborts = pio_port_fake_dma_aborts();
    int running_starts = pio_port_fake_dma_starts();
    pio_dma_abort(h);
    pio_dma_abort(h);
    expect(pio_port_fake_dma_aborts() == aborts + 1, "abort stops the channel once");
    expect(pio_port_fake_dma_starts() == running_starts, "abort does not start the armed section");
    expect(pio_dma_idle(h, "ws") == 1, "abort leaves the handle idle");
    pio_dma_hw_done(ch);
    expect(pio_port_fake_dma_starts() == running_starts, "a stale completion does not start");

    expect(dma_tx(h, "ws", sec_a, 2) == PIO_OK, "color transfer before a blank");
    int drains = pio_port_fake_dma_drains();
    expect(pio_dma_zero(h, "ws", 20) == PIO_OK, "zero aborts color and starts");
    ch = pio_port_fake_dma_last();
    expect(pio_port_fake_dma_inc(ch) == 0 && pio_port_fake_dma_drains() == drains + 1,
           "zeros hold the read address and resync the machine");
    expect(pio_dma_idle(h, "ws") == 0, "the zero transfer is running");
    pio_dma_hw_done(ch);
    expect(pio_dma_idle(h, "ws") == 1, "the zero transfer can finish");
    pio_dma_abort(h);
    pio_res_release(h);

    pin = 20;
    int prot = pio_res_load("sd", &pin, 1, prog, 4, PIO_LOAD_PROTECT);
    expect(prot > 0, "protected machine loads");
    starts = pio_port_fake_dma_starts();
    expect(dma_tx(prot, "sd", &word, 1) == PIO_EBUSY &&
           strstr(pio_dma_last_error(), "protected") != NULL,
           "a protected machine is refused");
    expect(pio_port_fake_dma_starts() == starts, "a protected machine does not start dma");
    pio_res_release_hw(pio_res_block(prot), pio_res_sm(prot));

    int handles[4];
    uint8_t pins[4] = { 16, 17, 18, 19 };
    uint32_t secs[4] = { 1, 2, 3, 4 };
    int got = 0;
    for (int i = 0; i < 4; i++) {
        handles[i] = pio_res_load("many", &pins[i], 1, prog, 4, 0);
        if (handles[i] > 0 && dma_tx(handles[i], "many", &secs[i], 1) == PIO_OK) { got++; }
    }
    expect(got == 4, "four module channels start");
    pin = 15;
    int fifth = pio_res_load("many", &pin, 1, prog, 4, 0);
    uint32_t extra = 9;
    expect(fifth > 0 && dma_tx(fifth, "many", &extra, 1) == PIO_ENOMEM &&
           strstr(pio_dma_last_error(), "limited to 4") != NULL,
           "a fifth channel is refused");
    for (int i = 0; i < 4; i++) {
        if (handles[i] > 0) { pio_dma_abort(handles[i]); pio_res_release(handles[i]); }
    }
    if (fifth > 0) { pio_dma_abort(fifth); pio_res_release(fifth); }

    pin = 14;
    int idle_h = pio_res_load("ws", &pin, 1, prog, 4, 0);
    expect(idle_h > 0 && pio_dma_idle(idle_h, "ws") == 1, "a machine with no dma is idle");
    expect(dma_arm(idle_h, "ws", sec_a, 2) == PIO_OK && pio_dma_idle(idle_h, "ws") == 0,
           "arm while idle starts immediately");
    pio_dma_abort(idle_h);
    pio_res_release(idle_h);
}

int main(void) {
    test_empty_and_fit();
    test_load_and_shadow();
    test_exact_lines();
    test_base_and_flags();
    test_pins_and_owner();
    test_fifo_and_irq();
    test_protect();
    test_sd_protect();
    test_dma();
    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
