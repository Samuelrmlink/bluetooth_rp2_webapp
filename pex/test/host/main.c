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
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "hardware/regs/addressmap.h"
#if !PICO_RP2040
#include "hardware/structs/qmi.h"
#endif
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
        uint32_t want = 0x01 | 0x02 | 0x04 | 0x10 | 0x20 | 0x40 | 0x80;    /* NOBOARD */
#else
        uint32_t want = 0x01 | 0x02 | 0x04 | 0x08 | 0x20 | 0x40 | 0x80;    /* BOARD */
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

    /* slice: one call never returns. It is faulted; a cooperative app keeps ticking. */
    {
        pex_mod_t *buddy = load(p_of(model, "app", TGT), NULL, &err);
        pex_mod_t *sp = load(p_of(model, "spin", TGT), NULL, &err);
        if (buddy && sp) {
            int rc = pex_start(buddy, "{\"ticks\":100000,\"period\":20}");
            poll_for(40);
            int before = (int)pex_call0(buddy, sym(buddy, "app_ticks"));
            uint32_t t0 = to_ms_since_boot(get_absolute_time());
            uint32_t r = pex_call0(sp, sym(sp, "spin_forever"));
            uint32_t dt = to_ms_since_boot(get_absolute_time()) - t0;
            pex_info_t is;
            pex_info(sp, &is);
            poll_for(80);
            int after = (int)pex_call0(buddy, sym(buddy, "app_ticks"));
            const pex_fault_t *f = pex_last_fault();
            result("slice.recovered", r == (uint32_t)PEX_EFAULT && is.state == PEX_ST_FAULTED && is.active == 0 &&
                                       is.tasks == 0 && dt >= 30 && dt < 500,
                   "rc %lu dt %lu state %s", (unsigned long)r, (unsigned long)dt, pex_state_name(is.state));
            result("slice.named", f && f->hang && !f->in_host && strcmp(f->name, is.name) == 0, "%s", f ? f->name : "");
            result("slice.others_run", rc == PEX_OK && after > before, "ticks %d -> %d", before, after);
            result("slice.unload", pex_stop(buddy) == PEX_OK && pex_unload(buddy) == PEX_OK && pex_unload(sp) == PEX_OK,
                   "%s", pex_errmsg());
        } else {
            result("slice.load", false, "%s", pex_errmsg());
        }
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
}

/* ---- XIP: the PEX flash partition on this host ------------------------------------
 * RP2350: storage 2 MB..2 MB+256 KB, executed through QMI ATRANS1 -- the window at
 * 0x10400000 mapped onto it, as the gateway does (its slot boot leaves window 1
 * free) -- and read through the untranslated uncached alias. RP2040: 1.5 MB, at
 * its plain XIP address. Power cuts: the cut operation is not done (or, for a
 * multi-page program, only its first pages) and every later one fails, until
 * the "reboot" (pex_xip_mount). */
#if PICO_RP2040
#define PART_OFF (1536u * 1024u)
#define PART_EXEC (XIP_BASE + PART_OFF)
#define PART_READ (XIP_NOCACHE_NOALLOC_BASE + PART_OFF)
#else
#define PART_OFF (2048u * 1024u)
#define PART_EXEC 0x10400000u
#define PART_READ (XIP_NOCACHE_NOALLOC_NOTRANSLATE_BASE + PART_OFF)
#endif
#define PART_SIZE (256u * 1024u)
static long s_fops, s_fcut = -1;
static int s_fdead;

static int part_erase(void *ctx, uint32_t off) {
    (void)ctx;
    if (s_fdead) { return -1; }
    if (s_fops++ == s_fcut) { s_fdead = 1; return -1; }
    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(PART_OFF + off, 4096);
    restore_interrupts(ints);
    return 0;
}
static int part_prog(void *ctx, uint32_t off, const void *src, uint32_t n) {
    (void)ctx;
    if (s_fdead) { return -1; }
    uint32_t k = n;
    if (s_fops++ == s_fcut) { s_fdead = 1; k = (n / 2) & ~255u; }
    if (k) {
        uint32_t ints = save_and_disable_interrupts();
        flash_range_program(PART_OFF + off, src, k);
        restore_interrupts(ints);
    }
    return k == n ? 0 : -1;
}
static const pex_flash_t s_part = { PART_SIZE, PART_EXEC, PART_READ, part_erase, part_prog, NULL };

static void part_window(void) {
#if !PICO_RP2040
    qmi_hw->atrans[1] = (0x40u << QMI_ATRANS1_SIZE_LSB) | (PART_OFF >> 12);   /* 256 KB at 2 MB */
#endif
}

static int install_b(const char *bundle_name, uint32_t flags) {
    uint32_t n;
    const uint8_t *b = bfind(bundle_name, &n);
    if (!b) { printf("PEXT %s - missing %s in the bundle\n", TGT, bundle_name); return PEX_ENOENT; }
    return pex_install_mem(b, n, flags);
}

static bool in_part(uintptr_t a) { return a >= PART_EXEC && a < PART_EXEC + PART_SIZE; }

static void xip_suite(const char *model) {
    s_model = model;
    int err;
    char nb[64], why[200];
    pex_mem_stats_t base;
    pex_pool_stats(&base);
    bool pic = strcmp(model, "pic") == 0;

    /* big: 41 KB (more than the gateway's whole v1 pool), signed: verified in place */
    snprintf(nb, sizeof nb, "sig/%s_big.%s.pex", model, TGT);
    uint64_t t0 = time_us_64();
    int slot = install_b(nb, 0);
    uint32_t us = (uint32_t)(time_us_64() - t0);
    pex_installed_t in;
    result("xip.install_big_signed", slot >= 0 && pex_installed_find("big", &in) >= 0 && in.signed_ && in.file_size > 40000,
           "slot %d, %lu bytes in %lu ms %s", slot, (unsigned long)in.file_size, (unsigned long)(us / 1000), slot < 0 ? pex_errmsg() : "");
    result("xip.verify_in_place", pex_xip_verify("big") == PEX_OK, "%s", pex_errmsg());
    pex_mod_t *m = pex_load_installed("big", NULL, &err);
    pex_info_t inf;
    if (m) { pex_info(m, &inf); }
    if (!pic) {
        /* model A: its RW address is reserved at mount -- before a "reboot" it loads into RAM */
        result("xip.reloc_ram_until_reboot", m && !inf.xip && pex_call0(m, sym(m, "big_check")) == 0, "%s",
               m ? (inf.xip ? "in place?" : "") : pex_errmsg());
        if (m) { pex_unload(m); }
        result("xip.remount", pex_xip_mount() == PEX_OK, "%s", pex_errmsg());
        m = pex_load_installed("big", NULL, &err);
        if (m) { pex_info(m, &inf); }
    }
    uint32_t chk = m ? pex_call0(m, sym(m, "big_check")) : 99;
    uintptr_t ca = m ? pex_call0(m, sym(m, "big_code_addr")) : 0, ra = m ? pex_call0(m, sym(m, "big_rw_addr")) : 0;
    result("xip.big_runs_in_place", m && inf.xip && chk == 0 && in_part(ca) && !in_part(ra) && inf.code == in.code,
           "check 0x%lx code %08lx rw %08lx %s", (unsigned long)chk, (unsigned long)ca, (unsigned long)ra, m ? "" : pex_errmsg());
    pex_mem_stats_t ms;
    pex_pool_stats(&ms);
    result("xip.big_costs_rw_only", m && base.pool_free - ms.pool_free < 2048 + (pic ? inf.rw_size : 0),
           "pool used by the load: %lu bytes (code %lu in flash)", (unsigned long)(base.pool_free - ms.pool_free),
           (unsigned long)inf.code_size);
    result("xip.uninstall_refused_loaded", pex_uninstall("big") == PEX_EBUSY, "%s", pex_errmsg());
    if (m) { pex_unload(m); }

    /* the same file into RAM: a module larger than the old pool */
    m = load(p_of(model, "big", TGT), "bigram", &err);
    result("big.ram_load", m && pex_call0(m, sym(m, "big_check")) == 0, "%s", m ? "" : pex_errmsg());
    if (m) { pex_unload(m); }

    /* arith in place: every relocation kind, ctor/dtor, libgcc -- signed */
    snprintf(nb, sizeof nb, "sig/%s_arith.%s.pex", model, TGT);
    slot = install_b(nb, 0);
    if (!pic) { pex_xip_mount(); }
    m = slot >= 0 ? pex_load_installed("arith", NULL, &err) : NULL;
    if (m) { pex_info(m, &inf); }
    result("xip.arith_in_place", m && inf.xip && pex_call0(m, sym(m, "arith_selftest")) == 0 &&
                                 (int)pex_call(m, sym(m, "arith_apply"), 2, 6, 7, 0) == 42,
           "%s", m ? (inf.xip ? "" : "not in place") : pex_errmsg());
    if (m) { pex_unload(m); }

    /* counter: model B instances share the flash copy */
    snprintf(nb, sizeof nb, "%s/counter.%s.pex", model, TGT);
    slot = install_b(nb, 0);
    if (!pic) { pex_xip_mount(); }
    pex_mod_t *a = pex_load_installed("counter", NULL, &err), *b = pex_load_installed("counter", NULL, &err);
    if (a && b) {
        pex_info_t ia, ib;
        pex_info(a, &ia);
        pex_info(b, &ib);
        int x = (int)pex_call1(a, sym(a, "counter_add"), 5), y = (int)pex_call1(b, sym(b, "counter_add"), 7);
        result(pic ? "xip.instances_share_flash" : "xip.second_instance_ram",
               x == 5 && y == 7 && (pic ? (ia.xip && ib.xip && ia.code == ib.code) : (ia.xip && !ib.xip)),
               "xip %d/%d code %08lx %08lx", ia.xip, ib.xip, (unsigned long)ia.code, (unsigned long)ib.code);
    } else {
        result("xip.instances", false, "%s", pex_errmsg());
    }
    if (a) { pex_unload(a); }
    if (b) { pex_unload(b); }

    /* app on the loop, from flash */
    snprintf(nb, sizeof nb, "%s/app.%s.pex", model, TGT);
    install_b(nb, 0);
    if (!pic) { pex_xip_mount(); }
    m = pex_load_installed("app", NULL, &err);
    if (m) {
        pex_start(m, "{\"ticks\":4,\"period\":5}");
        for (int i = 0; i < 100 && pex_call0(m, sym(m, "app_ticks")) < 4; i++) { poll_for(5); }
        poll_for(20);
        pex_info(m, &inf);
        result("xip.app_runs", inf.xip && pex_call0(m, sym(m, "app_ticks")) == 4 && inf.state == PEX_ST_EXITED, "%s",
               pex_state_name(inf.state));
        pex_unload(m);
    } else {
        result("xip.app_runs", false, "%s", pex_errmsg());
    }

    /* a fault in flash code is attributed */
    snprintf(nb, sizeof nb, "%s/crash.%s.pex", model, TGT);
    install_b(nb, 0);
    if (!pic) { pex_xip_mount(); }
    m = pex_load_installed("crash", NULL, &err);
    if (m) {
        pex_info(m, &inf);
        uint32_t where = pex_call0(m, sym(m, "crash_where"));
        bool faulted = guarded(m, sym(m, "crash_now"), 1, NULL);
        const pex_fault_t *f = pex_last_fault();
        uint32_t lv2 = (where & ~1u) - (uint32_t)inf.code;
        result("xip.fault_attributed", inf.xip && faulted && f && in_part(f->pc) && f->offset >= lv2 && f->offset < lv2 + 16,
               "pc %08lx offset 0x%lx", f ? (unsigned long)f->pc : 0, f ? (unsigned long)f->offset : 0);
        pex_unload(m);
    }

    /* compaction: free a hole at the bottom, then something that only fits after moving the others */
    result("xip.uninstall", pex_uninstall("big") == PEX_OK && pex_installed_find("big", NULL) == PEX_ENOENT, "%s", pex_errmsg());
    pex_xip_stats_t st;
    pex_xip_stats(&st);
    uint32_t free0 = st.free, moves0 = st.moves;
    snprintf(nb, sizeof nb, "%s/big.%s.pex", model, TGT);
    slot = install_b(nb, 0);
    pex_xip_stats(&st);
    result("xip.reinstall_after_uninstall", slot >= 0, "free %lu -> %lu, moves %lu %s", (unsigned long)free0,
           (unsigned long)st.free, (unsigned long)(st.moves - moves0), slot < 0 ? pex_errmsg() : "");
    pex_uninstall("arith");
    pex_uninstall("counter");
    int mv = pex_xip_compact();
    pex_xip_stats(&st);
    if (!pic) { pex_xip_mount(); }
    m = pex_load_installed("big", NULL, &err);
    result("xip.compacted_still_runs", mv >= 1 && st.largest == st.free && m && pex_call0(m, sym(m, "big_check")) == 0,
           "moved %d, free %lu largest %lu %s", mv, (unsigned long)st.free, (unsigned long)st.largest, m ? "" : pex_errmsg());
    if (m) { pex_unload(m); }

    /* power cuts during an install: the index is the old or the new one, and what is in it runs */
    snprintf(nb, sizeof nb, "%s/arith.%s.pex", model, TGT);
    pex_uninstall("arith");
    long f0 = s_fops;
    install_b(nb, 0);
    long nops = s_fops - f0;          /* erase, pages, then the index (erase + program) */
    const long cuts[] = { 0, 1, nops / 2, nops - 3, nops - 2, nops - 1 };
    int good = 0, before = 0, after = 0;
    for (unsigned c = 0; c < sizeof cuts / sizeof cuts[0]; c++) {
        pex_uninstall("arith");
        s_fdead = 0;
        s_fcut = s_fops + cuts[c];
        install_b(nb, 0);
        s_fcut = -1;
        s_fdead = 0;
        int rc = pex_xip_mount();
        bool has = pex_installed_find("arith", NULL) >= 0;
        bool ok = rc == PEX_OK && pex_installed_find("big", NULL) >= 0;
        if (has) {
            ok = ok && pex_xip_verify("arith") == PEX_OK;
            pex_mod_t *x = pex_load_installed("arith", NULL, &err);
            ok = ok && x && pex_call0(x, sym(x, "arith_selftest")) == 0;
            if (x) { pex_unload(x); }
            after++;
        } else {
            before++;
        }
        pex_mod_t *y = pex_load_installed("big", NULL, &err);
        ok = ok && y && pex_call0(y, sym(y, "big_check")) == 0;
        if (y) { pex_unload(y); }
        good += ok;
    }
    result("xip.power_cut_install", good == (int)(sizeof cuts / sizeof cuts[0]) && before >= 4,
           "%d/%u consistent (%d before, %d after; %ld flash operations)", good, (unsigned)(sizeof cuts / sizeof cuts[0]),
           before, after, nops);

    /* policy: an unsigned module is never installed on a signed-only host; a bad signature never */
    pex_set_policy(0);
    snprintf(nb, sizeof nb, "%s/hello.%s.pex", model, TGT);
    int r1 = install_b(nb, 0);
    snprintf(nb, sizeof nb, "bad/hello_badsig.%s.pex", TGT);
    int r2 = install_b(nb, 0);
    snprintf(nb, sizeof nb, "bad/hello_signed.%s.pex", TGT);
    int r3 = install_b(nb, 0);
    pex_set_policy(PEX_POLICY_ALLOW_UNSIGNED);
    result("xip.signed_only_policy", r1 == PEX_EUNSIGNED && r2 == PEX_ESIG && r3 >= 0, "%d %d %d", r1, r2, r3);

    /* clean up for the next model */
    pex_installed_t e;
    while (pex_installed(0, &e) == PEX_OK) {
        if (pex_uninstall(e.name) != PEX_OK) { break; }
    }
    pex_xip_mount();
    pex_pool_stats(&ms);
    snprintf(why, sizeof why, "free %lu (baseline %lu)", (unsigned long)ms.pool_free, (unsigned long)base.pool_free);
    result("xip.pool_clean", ms.pool_free >= base.pool_free && ms.scratch_bytes == 0, "%s", why);
}

static uint8_t s_pool[96 * 1024] __attribute__((aligned(8)));

int main(void) {
    stdio_init_all();
    sleep_ms(20);
    printf("PEXT BEGIN target=%s board=%s\n", TGT, PICO_BOARD);
    pex_sig_uecc_set_keys(PEX_TEST_PUBKEY, 1);
    /* a fresh partition: both index copies erased */
    part_window();
    part_erase(NULL, 0);
    part_erase(NULL, 4096);
    pex_config_t cfg = {
        .pool = s_pool, .pool_size = sizeof s_pool, .policy = PEX_POLICY_ALLOW_UNSIGNED,
        .verify = pex_sig_uecc_verify, .host_name = "pex-test", .flash = &s_part,
        .slice_ms = 50,
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
    xip_suite("reloc");
    xip_suite("pic");
    pex_mem_stats_t s;
    pex_pool_stats(&s);
    s_model = "-";
    /* what stays: the XIP index (3 KB, runtime-owned) */
    result("final.pool_empty", s.long_blocks == 1 && s.scratch_bytes == 0 && s.pool_size - s.pool_free <= 3200,
           "free %lu/%lu, %lu blocks", (unsigned long)s.pool_free, (unsigned long)s.pool_size, (unsigned long)s.long_blocks);
    printf("PEXT DONE target=%s pass=%d fail=%d ms=%lu\n", TGT, s_pass, s_fail,
           (unsigned long)(to_ms_since_boot(get_absolute_time()) - t0));
    for (;;) { sleep_ms(1000); }
}
