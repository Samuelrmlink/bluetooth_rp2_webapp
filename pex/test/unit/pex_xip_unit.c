/*
 * XIP on the build host: the runtime's install / uninstall / compaction /
 * mount over a simulated NOR flash, with POWER CUTS injected at every flash
 * operation of each update (and a second cut during the recovery).
 *
 * Nothing executes module code here (pex_tramp is a stub): what is checked is
 * the partition -- after any cut and a "reboot" (pex_init again over the same
 * flash) the index is the state before or the state after the update, every
 * installed file verifies in place (CRC over the original bytes, model A
 * un-relocated), and loads. The modules are synthesised (any size, model A
 * with RO relocations to code and to RW across many sectors).
 *
 *   gcc -DPEX_FORCE_TARGET=2 -DPEX_FORCE_FEATURES=5 -DPEX_SD_READ_TEST \
 *       -I pex/runtime -I pex/sdk/include -I src/pex -I <gen> \
 *       pex_xip_unit.c pex_core.c pex_load.c pex_mem.c pex_sched.c pex_hash.c pex_xip.c \
 *       src/pex/pex_sd_read.c groups/pex_json.c \
 *       <gen>/pex_host_{core,log,mem,sched,pex,json}.c
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pex_internal.h"
#include "pex_sd_read.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d %s ", __FILE__, __LINE__, #c); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- the port ------------------------------------------------------------------ */
static int s_quiet = 1;
uint32_t pex_port_ms(void) { return 0; }
uint64_t pex_port_us(void) { return 0; }
void pex_port_log(int level, const char *line) { if (!s_quiet || level == PEX_LOG_ERR) { printf("  | %s\n", line); } }
void pex_port_code_sync(void *p, uint32_t n) { (void)p; (void)n; }
int pex_port_stack_bounds(uintptr_t *lo, uintptr_t *hi) { (void)lo; (void)hi; return 0; }
void pex_port_slice_arm(uint32_t ms) { (void)ms; }
void pex_port_slice_disarm(void) {}
uint64_t pex_tramp(uint32_t sb, uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3) {
    (void)sb; (void)fn; (void)a0; (void)a1; (void)a2; (void)a3;
    return 0;
}
uintptr_t pex_get_sp(void) { return 0; }
size_t pex_scratch_avail(void);

/* Card reader: pex_sd_read.c pumps sdw_service. The job stays posted until then,
 * and each READ is at most 4 KB, including a later seek back to the header. */
static const uint8_t *s_fix;
static uint32_t s_fix_n, s_pos, s_max_read;
static int s_job_on, s_job_done, s_pos_known, s_seeks, s_reads, s_services;
static sdj_t s_job;
static uint8_t s_job_buf[4096];

bool sdj_post(const sdj_t *req, uint32_t *token) {
    if (s_job_on || !req || !token) { return false; }
    s_job = *req;
    s_job.rc = -1;
    s_job_on = 1;
    s_job_done = 0;
    *token = 1;
    return true;
}
enum sdj_state sdj_poll(uint32_t token, sdj_t *out) {
    if (!s_job_on || token != 1) { return SDJ_IDLE; }
    if (!s_job_done) { return SDJ_POSTED; }
    if (out) { *out = s_job; }
    return SDJ_DONE;
}
void sdj_finish(uint32_t token) { (void)token; s_job_on = 0; s_job_done = 0; }
uint8_t *sdj_buf(void) { return s_job_buf; }
void sdw_service(void) {
    if (!s_job_on || s_job_done) { return; }
    s_services++;
    if (s_job.op == SDJ_SEEK) {
        s_seeks++;
        if (s_job.pos > s_fix_n) { s_job.rc = -1; }
        else { s_pos = (uint32_t)s_job.pos; s_pos_known = 1; s_job.rc = 0; }
    } else if (s_job.op == SDJ_READ) {
        s_reads++;
        uint32_t n = s_job.len > 4096 ? 4096 : s_job.len;
        if (n > s_max_read) { s_max_read = n; }
        if (!s_pos_known || s_pos > s_fix_n) { s_job.rc = -1; }
        else {
            if (n > s_fix_n - s_pos) { n = s_fix_n - s_pos; }
            memcpy(s_job_buf, s_fix + s_pos, n);
            s_pos += n;
            s_job.rc = (int)n;
        }
    } else {
        s_job.rc = -1;
    }
    s_job_done = 1;
}

/* ---- NOR flash with power cuts -------------------------------------------------- */
#define FSEC 40u                       /* partition sectors */
#define FSIZE (FSEC * 4096u)
static uint8_t s_flash[FSIZE];
static long s_ops, s_cut = -1;         /* cut at the s_cut-th operation (0-based); -1 never */
static int s_dead;
static unsigned s_rng = 1;
static unsigned rnd(void) { s_rng = s_rng * 1103515245u + 12345u; return s_rng >> 8; }

static int fl_erase(void *ctx, uint32_t off) {
    (void)ctx;
    if (s_dead || off % 4096 || off >= FSIZE) { return -1; }
    if (s_ops++ == s_cut) {            /* torn: part of the sector erased */
        uint32_t n = rnd() % 4096;
        memset(s_flash + off, 0xFF, n);
        s_dead = 1;
        return -1;
    }
    memset(s_flash + off, 0xFF, 4096);
    return 0;
}
static int fl_prog(void *ctx, uint32_t off, const void *src, uint32_t n) {
    (void)ctx;
    if (s_dead || off % 256 || n % 256 || off + n > FSIZE) { return -1; }
    const uint8_t *p = src;
    uint32_t k = n;
    if (s_ops++ == s_cut) { k = rnd() % (n + 1); s_dead = 1; }
    for (uint32_t i = 0; i < k; i++) { s_flash[off + i] &= p[i]; }   /* NOR: 1 -> 0 only */
    return k == n ? 0 : -1;
}
static const pex_flash_t s_fl = { FSIZE, 0x10400000u, (uintptr_t)s_flash, fl_erase, fl_prog, NULL };

/* ---- runtime "boot" ------------------------------------------------------------- */
static uint8_t s_poolmem[160 * 1024 + 64] __attribute__((aligned(8)));
static uint32_t s_pool_shift;          /* moves the pool (a new firmware build) */

static void boot(void) {
    pex_unload_all();
    s_dead = 0;
    s_cut = -1;
    pex_config_t cfg = { .pool = s_poolmem + s_pool_shift, .pool_size = 160 * 1024, .policy = PEX_POLICY_ALLOW_UNSIGNED,
                         .host_name = "unit", .flash = &s_fl };
    int rc = pex_init(&cfg);
    if (rc) { printf("pex_init %d\n", rc); exit(2); }
}

/* ---- synthetic modules --------------------------------------------------------------- */
typedef struct { uint8_t *p; uint32_t n; char name[24]; } file_t;

static void w32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static void w16(uint8_t *p, uint16_t v) { memcpy(p, &v, 2); }

/* text+ro+data bytes; model A gets an RO word every `every` words (to code and to RW alternately). */
static file_t mk(const char *name, uint32_t text, uint32_t ro, uint32_t data, uint32_t bss, int pic, uint32_t seed,
                 uint32_t every) {
    file_t f;
    memset(&f, 0, sizeof f);
    snprintf(f.name, sizeof f.name, "%s", name);
    uint32_t code = text + ro, img = code + data;
    uint32_t nro_c = 0, nro_w = 0;
    uint32_t *roc = calloc(code / 4 + 1, 4), *row = calloc(code / 4 + 1, 4);
    if (!pic && every) {
        for (uint32_t w = 1; w * 4 + 4 <= code; w += every) {
            if ((w / every) & 1) { row[nro_w++] = (w * 4) | PEX_RELOC_TO_RW; } else { roc[nro_c++] = w * 4; }
        }
    }
    uint32_t nl = (uint32_t)strlen(name);
    uint32_t meta = 4 + ((nl + 3) & ~3u);
    uint32_t reloc = 8 + 4 * (nro_c + nro_w);
    uint32_t nsec = 2, sec_off = 96, pay = sec_off + 12 * nsec;
    uint32_t meta_off = pay, reloc_off = meta_off + meta, image_off = (reloc_off + reloc + 7) & ~7u;
    uint32_t size = image_off + img;
    f.p = calloc(size, 1);
    f.n = size;
    uint8_t *b = f.p;
    w32(b, PEX_MAGIC); w16(b + 4, 1); w16(b + 6, 0); w16(b + 8, PEX_HEADER_SIZE); w16(b + 10, PEX_TARGET_ARMV8M_MAIN);
    w32(b + 12, size); w32(b + 16, 5); w16(b + 20, PEX_API_MAJOR); w16(b + 22, PEX_API_MINOR);
    w32(b + 24, pic ? (PEX_F_PIC | PEX_F_XIP_OK) : 0); w32(b + 28, 0x01020003u); w32(b + 32, img);
    w32(b + 36, text); w32(b + 40, ro); w32(b + 44, data); w32(b + 48, bss); w32(b + 52, 0); w32(b + 56, 0);
    w32(b + 60, nsec); w32(b + 64, sec_off); w32(b + 68, image_off);
    w16(b + sec_off, PEX_SEC_META); w32(b + sec_off + 4, meta_off); w32(b + sec_off + 8, meta);
    w16(b + sec_off + 12, PEX_SEC_RELOC); w32(b + sec_off + 16, reloc_off); w32(b + sec_off + 20, reloc);
    w16(b + meta_off, PEX_META_NAME); w16(b + meta_off + 2, (uint16_t)nl); memcpy(b + meta_off + 4, name, nl);
    w32(b + reloc_off, nro_c + nro_w); w32(b + reloc_off + 4, 0);
    for (uint32_t i = 0; i < nro_c; i++) { w32(b + reloc_off + 8 + 4 * i, roc[i]); }
    for (uint32_t i = 0; i < nro_w; i++) { w32(b + reloc_off + 8 + 4 * (nro_c + i), row[i]); }
    uint32_t x = seed;
    for (uint32_t i = 0; i + 4 <= img; i += 4) { x = x * 1664525u + 1013904223u; w32(b + image_off + i, x); }
    /* the words RO relocations point at hold valid segment offsets */
    for (uint32_t i = 0; i < nro_c; i++) { w32(b + image_off + roc[i], (rnd() % code) & ~3u); }
    for (uint32_t i = 0; i < nro_w; i++) { w32(b + image_off + (row[i] & 0x7FFFFFFFu), (rnd() % (data + bss + 4)) & ~3u); }
    pex_sha256_t sh;
    uint8_t d[32];
    pex_sha256_init(&sh);
    pex_sha256_update(&sh, b + image_off, img);
    pex_sha256_final(&sh, d);
    memcpy(b + 76, d, 16);
    w32(b + 72, 0);
    w32(b + 72, pex_crc32_update(0, b, size));
    free(roc);
    free(row);
    return f;
}

/* ---- checks ------------------------------------------------------------------------------ */
typedef struct { int n; char name[16][24]; uint8_t bid[16][16]; } set_t;

static void snap(set_t *s) {
    memset(s, 0, sizeof *s);
    pex_installed_t in;
    for (int i = 0; pex_installed(i, &in) == PEX_OK && i < 16; i++) {
        memcpy(s->name[s->n], in.name, 24);
        memcpy(s->bid[s->n], in.build_id, 16);
        s->n++;
    }
}
static int has(const set_t *s, const char *name, const uint8_t *bid) {
    for (int i = 0; i < s->n; i++) {
        if (strcmp(s->name[i], name) == 0 && memcmp(s->bid[i], bid, 16) == 0) { return 1; }
    }
    return 0;
}
static int same(const set_t *a, const set_t *b) {
    if (a->n != b->n) { return 0; }
    for (int i = 0; i < a->n; i++) {
        if (!has(b, a->name[i], a->bid[i])) { return 0; }
    }
    return 1;
}

/* Every installed file verifies in place and loads (and unloads). */
static int all_good(const char *ctx) {
    pex_installed_t in;
    int ok = 1;
    for (int i = 0; pex_installed(i, &in) == PEX_OK; i++) {
        int rc = pex_xip_verify(in.name);
        CHECK(rc == PEX_OK, "%s: %s does not verify: %d %s", ctx, in.name, rc, pex_errmsg());
        int err = 0;
        pex_mod_t *m = pex_load_installed(in.name, NULL, &err);
        CHECK(m != NULL, "%s: %s does not load: %d %s", ctx, in.name, err, pex_errmsg());
        if (!m) { ok = 0; continue; }
        pex_info_t inf;
        pex_info(m, &inf);
        if (inf.xip) { CHECK(inf.code == in.code, "%s: %s code %lx vs %lx", ctx, in.name, (long)inf.code, (long)in.code); }
        CHECK(pex_unload(m) == PEX_OK, "%s: unload %s", ctx, in.name);
        if (rc) { ok = 0; }
    }
    pex_xip_stats_t st;
    pex_xip_stats(&st);
    CHECK(st.mounted && st.used + st.free == st.data_size, "%s: accounting %u+%u != %u", ctx, st.used, st.free, st.data_size);
    return ok;
}

typedef int (*op_fn)(void *);
typedef struct { file_t *f; uint32_t flags; } inst_arg_t;
static int op_install(void *a) { inst_arg_t *x = a; return pex_install_mem(x->f->p, x->f->n, x->flags); }
static int op_uninstall(void *a) { return pex_uninstall((const char *)a); }
static int op_compact(void *a) { (void)a; return pex_xip_compact(); }

static uint8_t s_snapshot[FSIZE];

/* Run op once cleanly (expect "after"), then from the same start with a cut at
 * every flash operation: after the reboot the index is "before" or "after". */
static void cut_everywhere(const char *what, op_fn op, void *arg, int double_cut) {
    memcpy(s_snapshot, s_flash, FSIZE);
    boot();
    set_t before, after, got;
    snap(&before);
    long o0 = s_ops;
    int rc = op(arg);
    long n = s_ops - o0;
    CHECK(rc >= 0, "%s: clean run failed: %d %s", what, rc, pex_errmsg());
    boot();
    snap(&after);
    all_good(what);
    int seen_before = 0, seen_after = 0, recovered = 0;
    for (long k = 0; k < n; k++) {
        memcpy(s_flash, s_snapshot, FSIZE);
        boot();
        s_cut = s_ops + k;
        op(arg);
        if (double_cut && (k % 3 == 0)) {
            /* the reboot itself is cut too (during a resumed move), then a clean one */
            s_dead = 0;
            long base = s_ops;
            s_cut = base + (long)(rnd() % 40);
            pex_unload_all();
            pex_config_t cfg = { .pool = s_poolmem + s_pool_shift, .pool_size = 160 * 1024,
                                 .policy = PEX_POLICY_ALLOW_UNSIGNED, .host_name = "unit", .flash = &s_fl };
            pex_init(&cfg);
        }
        boot();
        pex_xip_stats_t st;
        pex_xip_stats(&st);
        recovered += st.recovered;
        snap(&got);
        int sb = same(&got, &before), sa = same(&got, &after);
        CHECK(sb || sa, "%s: cut at op %ld/%ld: the index is neither before nor after (%d entries)", what, k, n, got.n);
        seen_before += sb;
        seen_after += sa;
        if (!all_good(what)) { printf("  (%s: cut at op %ld of %ld)\n", what, k, n); }
    }
    printf("%-44s %4ld cut points: %3d left it before, %3d after, %3d resumed a move\n", what, n, seen_before,
           seen_after, recovered);
    memcpy(s_flash, s_snapshot, FSIZE);
    boot();
    op(arg);                           /* leave the "after" state for the next step */
    boot();
}

int main(void) {
    memset(s_flash, 0xFF, sizeof s_flash);
    boot();
    pex_xip_stats_t st;
    CHECK(pex_xip_stats(&st) == PEX_OK && st.mounted && st.entries == 0 && st.free == (FSEC - 2) * 4096u, "blank mount");

    /* 1. basics, both models */
    file_t b1 = mk("b1", 6000, 3000, 400, 200, 1, 11, 0);          /* model B, 3 sectors */
    file_t a1 = mk("a1", 9000, 4000, 600, 100, 0, 22, 7);          /* model A, 4 sectors, RO relocs */
    int s = pex_install_mem(b1.p, b1.n, 0);
    CHECK(s == 0, "install b1 -> %d %s", s, pex_errmsg());
    long o = s_ops;
    CHECK(pex_install_mem(b1.p, b1.n, 0) == 0 && s_ops == o, "the same build again: no flash write");
    s = pex_install_mem(a1.p, a1.n, 0);
    CHECK(s == 1, "install a1 -> %d %s", s, pex_errmsg());
    pex_installed_t in;
    CHECK(pex_installed_find("a1", &in) == 1 && in.xip_ready, "a1 installed, RW reserved without a reboot");
    /* model A in flash differs from the file (relocated) but verifies */
    CHECK(memcmp(s_flash + in.offset, a1.p, a1.n) != 0, "a1 is relocated in flash");
    CHECK(pex_xip_verify("a1") == PEX_OK, "a1 verifies in place: %s", pex_errmsg());
    CHECK(pex_installed_find("b1", &in) == 0 && memcmp(s_flash + in.offset, b1.p, b1.n) == 0, "b1 stored as is");
    int err;
    CHECK(pex_installed_find("a1", &in) >= 0, "a1 record");
    pex_mod_t *m = pex_load_installed("a1", NULL, &err);
    pex_info_t inf;
    CHECK(m && pex_info(m, &inf) == 0 && inf.xip && inf.code == in.code, "a1 in place without a reboot");
    /* a second model A while the first is loaded */
    file_t a2 = mk("a2", 4000, 1000, 800, 200, 0, 23, 5);
    pex_xip_stats_t room;
    pex_xip_stats(&room);
    uint32_t area_one = room.rw_area, room_one = room.rw_room;
    s = pex_install_mem(a2.p, a2.n, 0);
    CHECK(s >= 0, "install a2 while a1 is loaded -> %d %s", s, pex_errmsg());
    CHECK(pex_installed_find("a1", &in) >= 0 && in.xip_ready, "a1 still ready");
    CHECK(pex_installed_find("a2", &in) >= 0 && in.xip_ready, "a2 ready without a reboot");
    pex_mod_t *ma = pex_load_installed("a2", NULL, &err);
    CHECK(ma && pex_info(ma, &inf) == 0 && inf.xip, "a2 in place while a1 is loaded: %s", pex_errmsg());
    if (ma) { pex_unload(ma); }
    pex_xip_stats(&room);
    CHECK(room.rw_area > area_one && room.rw_room < room_one, "a2 grew the suffix (%u -> %u, room %u -> %u)",
          area_one, room.rw_area, room_one, room.rw_room);
    uint32_t area_both = room.rw_area, room_both = room.rw_room;
    CHECK(pex_uninstall("a2") == PEX_OK, "uninstall the highest: %s", pex_errmsg());
    pex_xip_stats(&room);
    CHECK(room.rw_area == area_one && room.rw_room > room_both, "highest uninstall returns the tail (%u, room %u)",
          room.rw_area, room.rw_room);
    CHECK(pex_install_mem(a2.p, a2.n, 0) >= 0, "a2 again: %s", pex_errmsg());
    if (m) { pex_unload(m); }
    CHECK(pex_uninstall("a1") == PEX_OK, "uninstall the lower module: %s", pex_errmsg());
    pex_xip_stats(&room);
    CHECK(room.rw_area == area_both, "a hole in the middle does not shrink the suffix (%u vs %u)", room.rw_area, area_both);
    CHECK(pex_uninstall("a2") == PEX_OK, "uninstall a2: %s", pex_errmsg());
    CHECK(pex_install_mem(a1.p, a1.n, 0) >= 0, "a1 reinstalled: %s", pex_errmsg());
    boot();
    CHECK(pex_installed_find("a1", &in) == 1 && in.xip_ready, "a1 after a reboot: RW reserved");
    m = pex_load_installed("a1", NULL, &err);
    CHECK(m && pex_info(m, &inf) == 0 && inf.xip && inf.code == in.code, "a1 in place");
    pex_mod_t *m2 = pex_load_installed("a1", NULL, &err);
    CHECK(m2 && pex_info(m2, &inf) == 0 && !inf.xip, "a second a1: RAM copy (the flash copy is bound to one RW)");
    if (m2) { pex_unload(m2); }
    CHECK(pex_uninstall("a1") == PEX_EBUSY, "uninstall refused while loaded");
    if (m) { pex_unload(m); }
    m = pex_load_installed("b1", NULL, &err);
    m2 = pex_load_installed("b1", NULL, &err);
    CHECK(m && m2 && pex_info(m2, &inf) == 0 && inf.xip, "two b1 instances, both in place");
    if (m) { pex_unload(m); }
    if (m2) { pex_unload(m2); }
    all_good("basics");

    /* 2. a new firmware moved the pool: model A is relocated to a new copy */
    s_pool_shift = 64;
    boot();
    CHECK(pex_installed_find("a1", &in) == 1, "a1 still installed");
    uint32_t off_before = in.offset;
    m = pex_load_installed("a1", NULL, &err);
    CHECK(m && pex_info(m, &inf) == 0 && inf.xip, "a1 re-placed for the moved pool, in place: %s", pex_errmsg());
    pex_installed_find("a1", &in);
    CHECK(in.offset != off_before, "a1 has a new copy (%x -> %x)", off_before, in.offset);
    if (m) { pex_unload(m); }
    all_good("re-placed");
    s_pool_shift = 0;
    boot();
    m = pex_load_installed("a1", NULL, &err);       /* and back */
    CHECK(m && pex_info(m, &inf) == 0 && inf.xip, "a1 re-placed again");
    if (m) { pex_unload(m); }

    /* 3. replace by name */
    file_t b1v2 = mk("b1", 6100, 3000, 400, 200, 0, 33, 5);        /* now model A */
    cut_everywhere("replace b1 (model B -> A)", op_install, &(inst_arg_t){ &b1v2, 0 }, 0);
    CHECK(pex_installed_find("b1", &in) >= 0 && !(in.flags & PEX_F_PIC), "b1 replaced by the model A build");

    /* 4. fill, fragment, then an install that needs an overlapping compaction */
    file_t c1 = mk("c1", 30000, 10000, 500, 100, 0, 44, 9);        /* A, 10 sectors */
    file_t g1 = mk("g1", 1000, 200, 100, 0, 1, 55, 0);             /* B, 1 sector */
    file_t c2 = mk("c2", 26000, 10000, 500, 100, 1, 66, 0);        /* B, 9 sectors */
    file_t g2 = mk("g2", 1000, 200, 100, 0, 0, 77, 3);             /* A, 1 sector */
    cut_everywhere("install c1 (model A, 10 sectors)", op_install, &(inst_arg_t){ &c1, 0 }, 0);
    cut_everywhere("install g1", op_install, &(inst_arg_t){ &g1, 0 }, 0);
    cut_everywhere("install c2 (model B, 9 sectors)", op_install, &(inst_arg_t){ &c2, 0 }, 0);
    cut_everywhere("install g2", op_install, &(inst_arg_t){ &g2, 0 }, 0);
    pex_xip_stats(&st);
    printf("filled: %u entries, %u KB free, largest %u KB\n", st.entries, st.free / 1024, st.largest / 1024);
    cut_everywhere("uninstall g1 (a hole of one sector)", op_uninstall, "g1", 0);
    cut_everywhere("uninstall b1", op_uninstall, "b1", 0);
    pex_xip_stats(&st);
    printf("fragmented: %u KB free, largest %u KB\n", st.free / 1024, st.largest / 1024);
    uint32_t want = st.largest + 4096;          /* one sector more than the largest hole */
    file_t d1 = mk("d1", want - 2000, 500, 300, 100, 0, 88, 4);
    CHECK(pex_install_mem(d1.p, d1.n, PEX_INSTALL_NO_COMPACT) == PEX_ENOSPC, "NO_COMPACT: refused (%s)", pex_errmsg());
    cut_everywhere("install d1 (needs compaction, overlapping moves)", op_install, &(inst_arg_t){ &d1, 0 }, 1);
    pex_xip_stats(&st);
    CHECK(pex_installed_find("d1", &in) >= 0 && st.moves == 0, "d1 installed");
    all_good("compacted");

    /* 5. compaction on its own, with loaded modules pinned */
    cut_everywhere("uninstall c1", op_uninstall, "c1", 0);
    cut_everywhere("compact (double cuts in the recovery)", op_compact, NULL, 1);
    pex_xip_stats(&st);
    CHECK(st.largest == st.free, "after compaction the free space is one run (%u vs %u)", st.largest, st.free);
    /* a loaded module is never moved */
    CHECK(pex_install_mem(c1.p, c1.n, 0) >= 0, "c1 again");
    pex_installed_t g2i;
    pex_installed_find("g2", &g2i);
    CHECK(pex_uninstall("a1") == PEX_OK, "uninstall a1");
    m = pex_load_installed("g2", NULL, &err);
    int mv = pex_xip_compact();
    CHECK(mv >= 1, "compact with g2 loaded moved others: %d", mv);
    pex_installed_find("g2", &in);
    CHECK(m && pex_info(m, &inf) == 0 && inf.code == in.code && in.offset == g2i.offset, "g2 not moved while loaded");
    if (m) { pex_unload(m); }
    all_good("pinned");

    /* 6. damage to the older index copy changes nothing; to both: an empty index */
    boot();
    set_t s1, s2;
    snap(&s1);
    uint32_t q0, q1;
    memcpy(&q0, s_flash + 4, 4);
    memcpy(&q1, s_flash + 4096 + 4, 4);
    uint32_t older = (int32_t)(q1 - q0) > 0 ? 0 : 4096;
    memset(s_flash + older + 100, 0, 8);
    boot();
    snap(&s2);
    CHECK(same(&s1, &s2) && s1.n > 0, "damage to the older copy changes nothing (%d entries)", s1.n);
    memset(s_flash + (4096 - older) + 100, 0, 8);
    boot();
    pex_xip_stats(&st);
    CHECK(st.entries == 0, "both copies damaged: an empty index (%u)", st.entries);
    all_good("damaged index");

    /* 7. a random workload with random cuts */
    memset(s_flash, 0xFF, sizeof s_flash);
    boot();
    file_t pool[8];
    for (int i = 0; i < 8; i++) {
        char nm[4] = { 'r', (char)('0' + i), 0, 0 };
        uint32_t sz = (2000 + rnd() % 30000) & ~3u;
        pool[i] = mk(nm, sz, (sz / 3) & ~3u, (200 + rnd() % 400) & ~3u, rnd() % 300, rnd() & 1, 100 + i, 3 + rnd() % 9);
    }
    int ok_ops = 0, cuts = 0, nospc = 0, other = 0;
    for (int it = 0; it < 3000; it++) {
        set_t b4;
        snap(&b4);
        int k = rnd() % 8;
        int cut = rnd() % 4 == 0;
        if (cut) { s_cut = s_ops + (long)(rnd() % 120); cuts++; }
        int rc;
        if (rnd() % 3 == 0) { rc = pex_uninstall(pool[k].name); } else { rc = pex_install_mem(pool[k].p, pool[k].n, 0); }
        if (rc >= 0) { ok_ops++; } else if (rc == PEX_ENOSPC) { nospc++; } else if (rc != PEX_ENOENT) { other++; }
        if (rc < 0 && rc != PEX_ENOSPC && rc != PEX_ENOENT && !cut && other < 5) { printf("  random: %d %s\n", rc, pex_errmsg()); }
        if (cut || rnd() % 10 == 0) { boot(); }
        s_cut = -1;
        s_dead = 0;
        if (it % 25 == 0) { all_good("random"); }
    }
    boot();
    all_good("random (end)");
    pex_xip_stats(&st);
    printf("random workload: 3000 operations (%d succeeded, %d no room, %d other/cut), %d power cuts; ends with %u "
           "entries, %u KB free, %u moves\n", ok_ops, nospc, other, cuts, st.entries, st.free / 1024, st.moves);

    /* 8. the index update borrows no pool scratch. A torn write restores s_ix. */
    memset(s_flash, 0xFF, sizeof s_flash);
    boot();
    file_t u1 = mk("u1", 2000, 200, 100, 0, 1, 201, 0);
    file_t u2 = mk("u2", 2000, 200, 100, 0, 1, 202, 0);
    CHECK(pex_install_mem(u1.p, u1.n, 0) >= 0, "u1 installed: %s", pex_errmsg());
    CHECK(pex_install_mem(u2.p, u2.n, 0) >= 0, "u2 installed: %s", pex_errmsg());
    void *fill[8] = { 0 };
    int nf = 0;
    for (int guard = 0; pex_scratch_avail() >= 3072 && nf < 8 && guard < 8; guard++) {
        size_t av = pex_scratch_avail();
        size_t want = av > 4096 ? (av - 2048) & ~(size_t)7u : 256;
        if (want < 8) { want = 8; }
        void *p = pex_alloc_host(want);
        CHECK(p != NULL, "filler of %zu (avail %zu)", want, av);
        if (!p) { break; }
        fill[nf++] = p;
    }
    size_t left = pex_scratch_avail();
    CHECK(left < 3072 && left >= sizeof(pex__hdr_t) + 8, "gap %zu is under an index copy and over the header", left);
    CHECK(pex_uninstall("u2") == PEX_OK, "uninstall with no index scratch: %s", pex_errmsg());
    CHECK(pex_installed_find("u2", NULL) == PEX_ENOENT, "u2 gone");
    CHECK(pex_install_mem(u2.p, u2.n, 0) >= 0, "install with no index scratch: %s", pex_errmsg());
    CHECK(pex_installed_find("u2", NULL) >= 0, "u2 back");
    for (int i = 0; i < nf; i++) { pex__free(fill[i]); }
    boot();
    CHECK(pex_installed_find("u1", NULL) >= 0 && pex_installed_find("u2", NULL) >= 0, "both survive a reboot");
    s_cut = s_ops;
    int cut_rc = pex_uninstall("u2");
    CHECK(cut_rc == PEX_EIO, "cut uninstall: %d %s", cut_rc, pex_errmsg());
    CHECK(pex_installed_find("u2", NULL) >= 0, "RAM index restored after the failed write");
    s_cut = -1;
    s_dead = 0;
    boot();
    CHECK(pex_installed_find("u2", NULL) >= 0, "flash index unchanged by the cut");
    CHECK(pex_uninstall("u2") == PEX_OK, "clean uninstall after the cut: %s", pex_errmsg());
    CHECK(pex_installed_find("u2", NULL) == PEX_ENOENT, "u2 removed");
    all_good("index without scratch");

    /* 9. suffix: no room refuses before a write; a failed commit rolls it back */
    memset(s_flash, 0xFF, sizeof s_flash);
    boot();
    file_t grow1 = mk("s1", 1500, 100, 200, 100, 0, 301, 4);
    file_t grow2 = mk("s2", 1500, 100, 400, 4000, 0, 302, 4);
    CHECK(pex_install_mem(grow1.p, grow1.n, 0) >= 0, "s1: %s", pex_errmsg());
    pex_xip_stats(&st);
    uint32_t saved_area = st.rw_area;
    int saved_n = (int)st.entries;
    long op0 = s_ops;
    CHECK(pex_install_mem(grow2.p, grow2.n, 0) >= 0, "s2 grows: %s", pex_errmsg());
    long delta = s_ops - op0;
    /* The last op is the index program. A torn program can still be a valid
     * index: the CRC covers the header and the entries, not the zero tail.
     * Cutting the erase before it cannot publish the new copy. */
    CHECK(delta >= 2, "s2 took %ld flash ops", delta);
    CHECK(pex_uninstall("s2") == PEX_OK, "drop s2: %s", pex_errmsg());
    pex_xip_stats(&st);
    CHECK(st.rw_area == saved_area, "suffix back to s1 (%u vs %u)", st.rw_area, saved_area);
    s_cut = s_ops + delta - 2;
    int grow_rc = pex_install_mem(grow2.p, grow2.n, 0);
    CHECK(grow_rc == PEX_EIO, "cut on the index erase: %d %s", grow_rc, pex_errmsg());
    pex_xip_stats(&st);
    CHECK(st.rw_area == saved_area && pex_installed_find("s2", NULL) == PEX_ENOENT,
          "failed commit restored the suffix (%u) and left s2 out", st.rw_area);
    s_cut = -1;
    s_dead = 0;
    boot();
    pex_xip_stats(&st);
    CHECK(st.rw_area == saved_area && st.entries == (uint32_t)saved_n, "flash index never took s2");
    void *fillb[128];
    int nf2 = 0;
    while (nf2 < 128 && pex__pool_gap() > 3000) {
        void *p = pex_alloc_host(2048);
        if (!p) { break; }
        fillb[nf2++] = p;
    }
    CHECK(pex__pool_gap() <= 3000 && pex_scratch_avail() > sizeof(pex__hdr_t),
          "gap %u still holds the install header", (unsigned)pex__pool_gap());
    file_t huge = mk("huge", 800, 0, 8, 20000, 0, 303, 0);
    int nrc = pex_install_mem(huge.p, huge.n, 0);
    CHECK(nrc == PEX_ENOMEM, "no room for RW: %d %s", nrc, pex_errmsg());
    CHECK(strstr(pex_errmsg(), "RW reservation") != NULL, "msg: %s", pex_errmsg());
    pex_xip_stats(&st);
    CHECK(st.rw_area == saved_area && st.entries == (uint32_t)saved_n && pex_installed_find("huge", NULL) == PEX_ENOENT,
          "ENOMEM wrote nothing (area %u entries %u)", st.rw_area, st.entries);
    for (int i = 0; i < nf2; i++) { pex__free(fillb[i]); }
    all_good("rw room");

    /* 10. an sd:/ install streams 4 KB slices and does not allocate the file */
    memset(s_flash, 0xFF, sizeof s_flash);
    boot();
    file_t card = mk("card", 9000, 2000, 300, 100, 0, 401, 6);
    CHECK(card.n > 4096, "fixture spans a second slice (%u)", card.n);
    s_fix = card.p;
    s_fix_n = card.n;
    s_pos = 0;
    s_pos_known = 0;
    s_seeks = s_reads = s_services = 0;
    s_max_read = 0;
    pex_mem_stats_t mb, ma_s;
    pex_pool_stats(&mb);
    pex_sd_cursor_t cur = { .file = 1, .size = card.n, .pos = 0, .pos_valid = 0 };
    pex_reader_t rd = { pex_sd_pread, &cur, card.n };
    int cr = pex_install(&rd, 0);
    pex_pool_stats(&ma_s);
    CHECK(cr >= 0, "streamed install: %d %s", cr, pex_errmsg());
    CHECK(pex_installed_find("card", &in) >= 0 && in.xip_ready, "card is ready");
    CHECK(s_reads > 1 && s_max_read <= 4096 && s_seeks >= 1 && s_services >= s_reads,
          "slices %d max %u seeks %d services %d", s_reads, s_max_read, s_seeks, s_services);
    uint32_t used = mb.pool_free - ma_s.pool_free;
    CHECK(used < card.n / 2, "pool drop %u is the RW suffix, not the %u-byte file", used, card.n);
    all_good("sd stream");

    printf("%d/%d checks passed\n", checks - fails, checks);
    return fails != 0;
}
