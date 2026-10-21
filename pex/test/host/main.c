/*
 * PEX test host: a minimal pico-sdk firmware with the PEX runtime and NO Lua
 * and no gateway code, for RP2350 (M33) and RP2040 (M0+). It loads the test
 * modules from a bundle embedded in flash and prints one line per check:
 *
 *   PEXT <target> <model> <case> PASS|FAIL [detail]
 *   PEXT DONE target=<t> pass=<n> fail=<m>
 *
 * pex/test/run_sim.py runs both in mcusim and collects the matrix.
 */
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/unique_id.h"
#include "hardware/clocks.h"
#include "pex_rt.h"
#include "pex_testkey.h"

#if PICO_RP2040
#define TGT "m0p"
#define OTHER_TGT "m33"
#else
#define TGT "m33"
#define OTHER_TGT "m0p"
#endif

int pex_sig_uecc_verify(uint16_t alg, uint16_t key_id, const uint8_t digest[32], const uint8_t *sig, uint32_t len);
void pex_sig_uecc_set_keys(const uint8_t (*keys)[64], uint32_t n);

/* ---- the bundle (bundle.S .incbin's it) ------------------------------------ */
extern const uint8_t pex_bundle[];
typedef struct { char name[40]; uint32_t off, size; } bent_t;

static const uint8_t *bfind(const char *name, uint32_t *size) {
    uint32_t n;
    memcpy(&n, pex_bundle + 4, 4);
    const bent_t *e = (const bent_t *)(pex_bundle + 8);
    for (uint32_t i = 0; i < n; i++) {
        if (strcmp(e[i].name, name) == 0) {
            *size = e[i].size;
            return pex_bundle + e[i].off;
        }
    }
    *size = 0;
    return NULL;
}

/* ---- the optional "board" group: only the RP2350 host provides it -------- */
const char *pex_board_name(void) { return PICO_BOARD; }
uint32_t pex_board_cpu_hz(void) { return clock_get_hz(clk_sys); }
int pex_board_uid(uint8_t *out, size_t cap) {
    pico_unique_board_id_t id;
    pico_get_unique_board_id(&id);
    size_t n = cap < sizeof id.id ? cap : sizeof id.id;
    memcpy(out, id.id, n);
    return (int)n;
}
extern const pex_group_t pex_group_board;

/* ---- results --------------------------------------------------------------- */
static int s_pass, s_fail;
static const char *s_model = "-";

static void result(const char *name, bool ok, const char *fmt, ...) {
    char d[160] = "";
    if (fmt) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(d, sizeof d, fmt, ap);
        va_end(ap);
    }
    printf("PEXT %s %s %s %s %s\n", TGT, s_model, name, ok ? "PASS" : "FAIL", d);
    if (ok) { s_pass++; } else { s_fail++; }
}

/* ---- fault recovery: the HardFault handler resumes here -------------------- */
static jmp_buf s_jb;
static volatile int s_armed;
static volatile int s_faults;

static void __attribute__((noreturn, used)) fault_resume(void) {
    longjmp(s_jb, 1);
}

void __attribute__((used)) test_fault_c(uint32_t *frame) {
    pex_fault_t f;
    bool mine = pex_fault_attribute(frame, &f);
    s_faults++;
    if (mine && s_armed) {
        s_armed = 0;
        frame[6] = (uint32_t)(uintptr_t)fault_resume & ~1u;   /* return into thread mode there */
        frame[7] = (frame[7] & (1u << 9)) | (1u << 24);        /* keep the stack-align bit, Thumb */
#if !PICO_RP2040
        *(volatile uint32_t *)0xE000ED28u = 0xFFFFFFFFu;        /* clear CFSR */
#endif
        return;
    }
    printf("PEXT %s - fault UNEXPECTED pc=0x%08lx lr=0x%08lx\n", TGT, (unsigned long)frame[6], (unsigned long)frame[5]);
    for (;;) { tight_loop_contents(); }
}

__attribute__((naked)) void isr_hardfault(void) {
    __asm volatile(
        "movs r0, #4\n"
        "mov r1, lr\n"
        "tst r0, r1\n"
        "beq 1f\n"
        "mrs r0, psp\n"
        "b test_fault_c\n"
        "1: mrs r0, msp\n"
        "b test_fault_c\n");
}

/* A call into a module that may fault: true if it did (and was recovered). */
static bool guarded(pex_mod_t *m, uint32_t fn, uint32_t a0, uint32_t *ret) {
    static volatile uint32_t depth;
    depth = pex_depth();
    if (setjmp(s_jb)) {
        pex_unwind(depth);
        return true;
    }
    s_armed = 1;
    uint32_t r = pex_call1(m, fn, a0);
    s_armed = 0;
    if (ret) { *ret = r; }
    return false;
}

/* ---- helpers ----------------------------------------------------------------- */
static char path[64];
static const char *p_of(const char *model, const char *name, const char *tgt) {
    snprintf(path, sizeof path, "%s/%s.%s.pex", model, name, tgt);
    return path;
}

static pex_mod_t *load(const char *bundle_name, const char *as, int *err) {
    uint32_t n;
    const uint8_t *b = bfind(bundle_name, &n);
    if (!b) {
        printf("PEXT %s - missing %s in the bundle\n", TGT, bundle_name);
        if (err) { *err = PEX_ENOENT; }
        return NULL;
    }
    pex_load_opts_t o = { .name = as };
    return pex_load_mem(b, n, &o, err);
}

/* A reader that hands out at most 13 bytes per call (a file read in pieces). */
typedef struct { const uint8_t *p; uint32_t n, calls; } chunky_t;
static int chunky_read(void *ctx, uint32_t off, void *dst, uint32_t n) {
    chunky_t *c = ctx;
    uint32_t done = 0;
    while (done < n) {
        uint32_t k = n - done > 13 ? 13 : n - done;
        if (off + done + k > c->n) { return (int)done; }
        memcpy((uint8_t *)dst + done, c->p + off + done, k);
        done += k;
        c->calls++;
    }
    return (int)done;
}

static uint32_t sym(pex_mod_t *m, const char *n) { return m ? pex_sym(m, n) : 0; }

static void poll_for(uint32_t ms) {
    uint32_t t0 = to_ms_since_boot(get_absolute_time());
    while (to_ms_since_boot(get_absolute_time()) - t0 < ms) {
        pex_poll();
        sleep_us(200);
    }
}

static bool pool_clean(pex_mem_stats_t *base, char *why, size_t cap) {
    pex_mem_stats_t s;
    pex_pool_stats(&s);
    snprintf(why, cap, "free %lu/%lu blocks %lu scratch %lu (baseline free %lu blocks %lu)", (unsigned long)s.pool_free,
             (unsigned long)s.pool_size, (unsigned long)s.long_blocks, (unsigned long)s.scratch_bytes,
             (unsigned long)base->pool_free, (unsigned long)base->long_blocks);
    return s.pool_free == base->pool_free && s.long_blocks == base->long_blocks && s.scratch_bytes == 0;
}

/* ---- the suite, per code model ---------------------------------------------- */
static void suite(const char *model) {
    s_model = model;
    int err;
    char why[200];
    pex_mem_stats_t base;
    pex_pool_stats(&base);

    /* hello: load, INIT logs, an export, no Lua on this host (weak group absent), unload */
    pex_mod_t *m = load(p_of(model, "hello", TGT), NULL, &err);
    result("hello.load", m != NULL, "%s", m ? "" : pex_errmsg());
    if (m) {
        result("hello.export", pex_call0(m, sym(m, "hello_answer")) == 42, NULL);
        int r = pex_entry(m, PEX_LUA_OPEN, NULL);
        result("hello.no_lua_weak", r == PEX_ENOTSUP, "LUA_OPEN -> %d", r);
        result("hello.unload", pex_unload(m) == PEX_OK, "%s", pex_errmsg());
    }

    /* arith: relocations of every kind, ctor, libgcc helpers, 4 args, 64-bit return */
    m = load(p_of(model, "arith", TGT), NULL, &err);
    result("arith.load", m != NULL, "%s", m ? "INIT ran the self-test" : pex_errmsg());
    if (m) {
        uint32_t st = pex_call0(m, sym(m, "arith_selftest"));
        result("arith.selftest", st == 0, "check %lu", (unsigned long)st);
        result("arith.fnptr_table", (int)pex_call(m, sym(m, "arith_apply"), 2, 6, 7, 0) == 42 &&
                                    (int)pex_call(m, sym(m, "arith_apply"), 3, 100, 7, 0) == 14, NULL);
        result("arith.four_args", (int)pex_call(m, sym(m, "arith_sum6"), 1, 2, 3, 4) == 30, NULL);
        int64_t v = (int64_t)pex_call(m, sym(m, "arith_mul64"), (uint32_t)-100000, 300000, 0, 0);
        result("arith.int64_return", v == -30000000000LL, "%lld", (long long)v);
        pex_info_t in;
        pex_info(m, &in);
        result("arith.unload", pex_unload(m) == PEX_OK, "%s", pex_errmsg());
    }

    /* streaming reader (13-byte reads), same module */
    {
        uint32_t n;
        const uint8_t *b = bfind(p_of(model, "arith", TGT), &n);
        chunky_t c = { b, n, 0 };
        pex_reader_t rd = { chunky_read, &c, n };
        m = pex_load(&rd, NULL, &err);
        result("reader.load", m != NULL && pex_call0(m, sym(m, "arith_selftest")) == 0, "%lu reads %s",
               (unsigned long)c.calls, m ? "" : pex_errmsg());
        if (m) { pex_unload(m); }
    }

    /* imports: host calls, and the weak board import */
    m = load(p_of(model, "imports", TGT), NULL, &err);
    if (m) {
        uint32_t r = pex_call0(m, sym(m, "imports_run"));
#if PICO_RP2040
        uint32_t want = 0x01 | 0x02 | 0x04 | 0x10 | 0x20 | 0x40;    /* NOBOARD */
#else
        uint32_t want = 0x01 | 0x02 | 0x04 | 0x08 | 0x20 | 0x40;    /* BOARD */
#endif
        result("imports.run", r == want, "mask 0x%02lx want 0x%02lx", (unsigned long)r, (unsigned long)want);
        result("imports.unload", pex_unload(m) == PEX_OK, NULL);
    } else {
        result("imports.load", false, "%s", pex_errmsg());
    }

    /* needboard: a STRONG import of the board group */
    m = load(p_of(model, "needboard", TGT), NULL, &err);
#if PICO_RP2040
    result("needboard.refused", !m && err == PEX_EAPI && strstr(pex_errmsg(), "pex_board_name"), "%d %s", err,
           pex_errmsg());
#else
    result("needboard.loads", m && strcmp((const char *)(uintptr_t)pex_call0(m, sym(m, "needboard_name")), PICO_BOARD) == 0,
           "%s", m ? "" : pex_errmsg());
#endif
    if (m) { pex_unload(m); }

    /* app: cooperative ticks through sched; unload refused while running */
    m = load(p_of(model, "app", TGT), NULL, &err);
    if (m) {
        int rc = pex_start(m, "{\"ticks\":5,\"period\":10}");
        pex_info_t in;
        pex_info(m, &in);
        result("app.start", rc == PEX_OK && in.state == PEX_ST_RUNNING && in.tasks >= 1, "rc %d state %s tasks %d", rc,
               pex_state_name(in.state), in.tasks);
        result("app.unload_refused_running", pex_unload(m) == PEX_EBUSY, "%s", pex_errmsg());
        for (int i = 0; i < 100 && pex_call0(m, sym(m, "app_ticks")) < 5; i++) { poll_for(5); }
        poll_for(30);
        pex_info(m, &in);
        int ticks = (int)pex_call0(m, sym(m, "app_ticks"));
        result("app.ticks", ticks == 5 && in.state == PEX_ST_EXITED && in.tasks == 0 &&
                            pex_call0(m, sym(m, "app_posted")) == 77, "ticks %d state %s", ticks, pex_state_name(in.state));
        result("app.unload_after_exit", pex_unload(m) == PEX_OK, "%s", pex_errmsg());
    }
    m = load(p_of(model, "app", TGT), NULL, &err);
    if (m) {
        pex_start(m, "{\"ticks\":100000,\"period\":5}");
        poll_for(40);
        int t1 = (int)pex_call0(m, sym(m, "app_ticks"));
        int rc = pex_stop(m);
        pex_info_t in;
        pex_info(m, &in);
        result("app.stop", rc == 0 && t1 > 1 && pex_call0(m, sym(m, "app_stopped")) == 1 && in.tasks == 0 &&
                           in.state == PEX_ST_STOPPED, "ticks %d", t1);
        result("app.unload_after_stop", pex_unload(m) == PEX_OK, "%s", pex_errmsg());
    }

    /* memtest: the mem group, then a module that leaks everything */
    m = load(p_of(model, "memtest", TGT), NULL, &err);
    if (m) {
        uint32_t r = pex_call0(m, sym(m, "mem_run"));
        result("mem.run", r == 0, "check %lu", (unsigned long)r);
        r = pex_call0(m, sym(m, "mem_leak"));
        pex_info_t in;
        pex_info(m, &in);
        result("mem.leak_made", r == 0 && in.mem.long_blocks >= 5 && in.mem.scratch_bytes > 0, "blocks %lu",
               (unsigned long)in.mem.long_blocks);
        result("mem.unload", pex_unload(m) == PEX_OK, "%s", pex_errmsg());
        result("mem.leak_free", pool_clean(&base, why, sizeof why), "%s", why);
    }

    /* counter: two instances; model B shares one code copy */
    pex_mod_t *a = load(p_of(model, "counter", TGT), NULL, &err);
    pex_mod_t *b = load(p_of(model, "counter", TGT), NULL, &err);
    if (a && b) {
        uint32_t fa = sym(a, "counter_add"), fb = sym(b, "counter_add");
        int x1 = (int)pex_call1(a, fa, 5), y1 = (int)pex_call1(b, fb, 7), x2 = (int)pex_call1(a, fa, 1);
        uint32_t rwa = pex_call0(a, sym(a, "counter_rw_addr")), rwb = pex_call0(b, sym(b, "counter_rw_addr"));
        uint32_t ca = pex_call0(a, sym(a, "counter_code_addr")), cb = pex_call0(b, sym(b, "counter_code_addr"));
        pex_info_t ia, ib;
        pex_info(a, &ia);
        pex_info(b, &ib);
        bool pic = strcmp(model, "pic") == 0;
        result("instances.separate_rw", x1 == 5 && y1 == 7 && x2 == 6 && rwa != rwb &&
                                        pex_call0(a, sym(a, "counter_calls")) == 102 &&
                                        pex_call0(b, sym(b, "counter_calls")) == 101,
               "%d %d %d rw %08lx %08lx", x1, y1, x2, (unsigned long)rwa, (unsigned long)rwb);
        result(pic ? "instances.shared_code" : "instances.own_code",
               pic ? (ca == cb && ia.code == ib.code && ia.code_shared) : (ca != cb && !ia.code_shared),
               "code %08lx %08lx (%s, %s)", (unsigned long)ca, (unsigned long)cb, ia.name, ib.name);
        result("instances.self", pex_call0(a, sym(a, "counter_self")) == (uint32_t)(uintptr_t)a &&
                                 pex_call0(b, sym(b, "counter_self")) == (uint32_t)(uintptr_t)b, NULL);
        pex_ref(a);
        result("unload.refused_ref", pex_unload(a) == PEX_EBUSY, "%s", pex_errmsg());
        pex_unref(a);
        result("instances.unload_first", pex_unload(a) == PEX_OK, NULL);
        result("instances.second_alive", (int)pex_call1(b, fb, 3) == 10, NULL);
        result("instances.unload_second", pex_unload(b) == PEX_OK, NULL);
    } else {
        result("instances.load", false, "%s", pex_errmsg());
    }

    /* crash: fault attribution */
    m = load(p_of(model, "crash", TGT), NULL, &err);
    if (m) {
        uint32_t where = pex_call0(m, sym(m, "crash_where"));
        pex_info_t in;
        pex_info(m, &in);
        uint32_t r = 0;
        bool faulted = guarded(m, sym(m, "crash_now"), 1, &r);
        const pex_fault_t *f = pex_last_fault();
        uint32_t lv2 = (where & ~1u) - (uint32_t)in.code;
        bool ok = faulted && f && strcmp(f->name, in.name) == 0 && memcmp(f->build_id, in.build_id, 16) == 0 &&
                  !f->in_host && f->offset >= lv2 && f->offset < lv2 + 16;
        result("fault.attributed", ok, "offset 0x%lx, level2 at 0x%lx", f ? (unsigned long)f->offset : 0, (unsigned long)lv2);
        pex_info(m, &in);
        result("fault.state", in.state == PEX_ST_FAULTED && in.active == 0 && pex_depth() == 0, "%s",
               pex_state_name(in.state));
        result("fault.call_refused", (int)pex_call0(m, sym(m, "crash_where")) == PEX_EFAULT, NULL);
        result("fault.unload", pex_unload(m) == PEX_OK, "%s", pex_errmsg());
    }

    /* the other architecture's file */
    m = load(p_of(model, "hello", OTHER_TGT), NULL, &err);
    result("wrong_arch.refused", !m && err == PEX_ETARGET, "%s", pex_errmsg());

    result("pool.clean", pool_clean(&base, why, sizeof why), "%s", why);
}

static void integrity_suite(void) {
    s_model = "-";
    int err;
    pex_mod_t *m;
    struct { const char *name; int want; } cases[] = {
        { "bad/hello_badcrc", PEX_ECRC },   { "bad/hello_badsig", PEX_ESIG },  { "bad/hello_needsig", PEX_EUNSIGNED },
        { "bad/arith_picro", PEX_ERELOC },  { "bad/hello_fmt2", PEX_EVERSION }, { "bad/hello_api99", PEX_EAPI },
        { "bad/hello_trunc", PEX_EFORMAT },
    };
    char nb[64];
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        snprintf(nb, sizeof nb, "%s.%s.pex", cases[i].name, TGT);
        m = load(nb, NULL, &err);
        result(cases[i].name + 4, !m && err == cases[i].want, "err %d (%s): %s", err, pex_strerror(err), pex_errmsg());
        if (m) { pex_unload(m); }
    }
    snprintf(nb, sizeof nb, "bad/hello_signed.%s.pex", TGT);
    m = load(nb, NULL, &err);
    result("signed.loads", m != NULL, "%s", m ? "" : pex_errmsg());
    if (m) { pex_unload(m); }
    pex_set_policy(0);                 /* signed-only */
    m = load(nb, NULL, &err);
    result("signed_only.signed_loads", m != NULL, "%s", m ? "" : pex_errmsg());
    if (m) { pex_unload(m); }
    m = load(p_of("pic", "hello", TGT), NULL, &err);
    result("signed_only.unsigned_refused", !m && err == PEX_EUNSIGNED, "%s", pex_errmsg());
    if (m) { pex_unload(m); }
    pex_set_policy(PEX_POLICY_ALLOW_UNSIGNED);
    result("xip.stub", pex_install(NULL, 0) == PEX_ENOTSUP, NULL);
}

static uint8_t s_pool[56 * 1024] __attribute__((aligned(8)));

int main(void) {
    stdio_init_all();
    sleep_ms(20);
    printf("PEXT BEGIN target=%s board=%s\n", TGT, PICO_BOARD);
    pex_sig_uecc_set_keys(PEX_TEST_PUBKEY, 1);
    pex_config_t cfg = {
        .pool = s_pool, .pool_size = sizeof s_pool, .policy = PEX_POLICY_ALLOW_UNSIGNED,
        .verify = pex_sig_uecc_verify, .host_name = "pex-test",
    };
    int rc = pex_init(&cfg);
    result("init", rc == 0, NULL);
#if !PICO_RP2040
    pex_register_group(&pex_group_board);      /* the RP2040 host leaves the group out */
#endif
    uint32_t t0 = to_ms_since_boot(get_absolute_time());
    suite("reloc");
    suite("pic");
    integrity_suite();
    pex_mem_stats_t s;
    pex_pool_stats(&s);
    s_model = "-";
    result("final.pool_empty", s.long_blocks == 0 && s.scratch_bytes == 0 && s.pool_free == s.pool_size, "free %lu/%lu",
           (unsigned long)s.pool_free, (unsigned long)s.pool_size);
    printf("PEXT DONE target=%s pass=%d fail=%d ms=%lu\n", TGT, s_pass, s_fail,
           (unsigned long)(to_ms_since_boot(get_absolute_time()) - t0));
    for (;;) { sleep_ms(1000); }
}
