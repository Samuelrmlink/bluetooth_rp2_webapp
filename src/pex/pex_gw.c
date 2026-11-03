/* The PEX runtime in the gateway: see pex_gw.h. */
#include "pex/pex_gw.h"

#include <malloc.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "autoconf.h"
#include "api/ops.h"
#include "config/config.h"
#include "fs/fs.h"
#include "pico/stdlib.h"
#include "util/crumb.h"
#if CONFIG_OTA
#include "ota/ota_pubkey.h"
#endif

int pex_sig_uecc_verify(uint16_t alg, uint16_t key_id, const uint8_t digest[32], const uint8_t *sig, uint32_t len);
void pex_sig_uecc_set_keys(const uint8_t (*keys)[64], uint32_t n);
#if CONFIG_PEX_LUA
extern const pex_group_t pex_group_lua;
int pexlua_before_unload(pex_mod_t *m);
#endif

#if CONFIG_PEX_ALLOW_UNSIGNED
#define ALLOW_UNSIGNED 1
#else
#define ALLOW_UNSIGNED 0
#endif

/* A .config generated before this option still gets the recovery. 0 in
 * Kconfig is an explicit "watchdog only". */
#ifndef CONFIG_PEX_SLICE_MS
#define CONFIG_PEX_SLICE_MS 500
#endif

#if CONFIG_PEX_POOL_KB > 0
#define POOL_KB_CONF CONFIG_PEX_POOL_KB
#else
#define POOL_KB_CONF CONFIG_PEX_HEAP_POOL_KB
#endif

#define AUTOSTART_PATH "cfg/pex_autostart.json"
#define FAULTS_PATH "var/pex_faults.json"
#define FAULTS_MAX 1024

/* ---- the record that survives a reset (SCRATCH_X, after crumb_rec_t) ------
 * 0x20080100..0x20080184 is the crumb record. This 64-byte record sits in
 * the gap under the file-watch paths at 0x20080200. stackmark paints from
 * 0x20080E00, above those paths. */
typedef struct {
    uint32_t magic;
    uint32_t trace;              /* first build-id word of the module executing (0 = none) */
    uint32_t fault;              /* 1: a HardFault was attributed to a module */
    char name[24];
    uint8_t build_id[16];
    uint32_t offset, pc;
    uint32_t in_host;
} pex_boot_rec_t;
#define PEX_REC ((volatile pex_boot_rec_t *)0x20080188u)
#define PEX_REC_MAGIC 0x50455852u
_Static_assert(0x20080188u + sizeof(pex_boot_rec_t) <= 0x20080200u, "PEX record overlaps the stack paint");

static pex_boot_rec_t s_prev;       /* the previous boot's record */
static bool s_prev_valid, s_prev_logged;
static bool s_up, s_failed;
static uint8_t *s_pool;
static uint32_t s_pool_size;
#if CONFIG_PEX_POOL_KB > 0
/* The pool (spec 7.2, Q10): static, apart from the Lua arena and the C heap,
 * so a Lua VM restart never touches it. Taken from the arena's budget
 * (CONFIG_LUA_HEAP_KB) now that native modules do the work. */
static uint8_t s_static_pool[CONFIG_PEX_POOL_KB * 1024u] __attribute__((aligned(8)));
#endif
static uint32_t s_boot_ms;

static void boot_record(void) {
    static bool done;
    if (done) { return; }
    done = true;
    volatile pex_boot_rec_t *r = PEX_REC;
    if (r->magic == PEX_REC_MAGIC && (r->fault || r->trace)) {
        memcpy(&s_prev, (const void *)r, sizeof s_prev);
        s_prev_valid = true;
    }
    memset((void *)r, 0, sizeof *r);
    r->magic = PEX_REC_MAGIC;
}

const char *pex_gw_arch(void) {
#if PICO_RP2040
    return "m0p";
#else
    return "m33";
#endif
}

/* The gateway's log: printf (RTT), like everything else. */
void pex_port_log(int level, const char *line) {
    (void)level;
    printf("%s\n", line);
}

static void on_unload(pex_mod_t *m) {
    pex_gw_bus_unload(m);
#if CONFIG_SD || 1
    pex_gw_fs_unload(m);
#endif
#if CONFIG_NET_HTTP_CLIENT
    pex_gw_net_unload(m);
#endif
#if CONFIG_BT_LE
    pex_gw_bt_unload(m);
#endif
#if CONFIG_BT_CLASSIC
    pex_gw_classic_unload(m);
#endif
    pex_gw_pio_unload(m);
}

/* Append one record to var/pex_faults.json. Autostart skips that build.
 * Static on purpose: this also runs from a slice abort, which may have
 * landed inside malloc, and taking the heap lock again would wedge the loop
 * the slice just saved. */
static void faults_append(const char *name, const char *bid, const char *why, unsigned long offset) {
    static char buf[FAULTS_MAX + 160];
    if (!cfg_file_read(FAULTS_PATH, buf, FAULTS_MAX) || pex_json_len(buf, "faults") < 0) {
        strcpy(buf, "{\"faults\":[]}");
    }
    size_t n = strlen(buf);
    char *end = strrchr(buf, ']');
    if (end && n < FAULTS_MAX - 10) {
        char item[160];
        snprintf(item, sizeof item, "%s{\"name\":\"%s\",\"build\":\"%s\",\"why\":\"%s\",\"offset\":%lu}",
                 pex_json_len(buf, "faults") > 0 ? "," : "", name, bid, why, offset);
        memmove(end + strlen(item), end, strlen(end) + 1);
        memcpy(end, item, strlen(item));
        fs_mkdir("var");
        cfg_file_write(FAULTS_PATH, buf);
    }
}

/* The slice stopped this module. Release what it registered (same as unload,
 * and idempotent there) and remember the build so the next boot does not
 * autostart it. The module stays loaded, faulted, until pex.unload. */
static void on_slice(pex_mod_t *m) {
    on_unload(m);
    pex_info_t in;
    if (pex_info(m, &in) != PEX_OK || !in.build_id) { return; }
    char bid[33];
    for (int i = 0; i < 16; i++) { snprintf(bid + 2 * i, 3, "%02x", in.build_id[i]); }
    const pex_fault_t *f = pex_last_fault();
    unsigned long off = (f && strcmp(f->name, in.name) == 0) ? f->offset : 0;
    faults_append(in.name, bid, "hang", off);
}

/* C heap headroom, as fs_migrate.c measures it: what is left between the
 * heap's top and the stack, plus the free chunks inside the heap. A block
 * needs CONTIGUOUS room, which the sum overstates -- see take_pool(). */
static size_t heap_top_gap(void) {
    extern char __StackLimit;
    char *brk = sbrk(0);
    return (brk != (char *)-1 && brk < &__StackLimit) ? (size_t)(&__StackLimit - brk) : 0;
}
static size_t heap_room(void) {
    struct mallinfo mi = mallinfo();
    return heap_top_gap() + (size_t)mi.fordblks;
}

/* pico_malloc's malloc PANICS when it fails ("Out of memory": found in mcusim,
 * with the SD card and BLE up the free space was there but not in one piece).
 * __real_malloc is newlib's own, behind pico_malloc's --wrap: it returns NULL.
 * Try the configured size, then smaller, never below 6 KB, always leaving the
 * reserve free afterwards. */
#if CONFIG_PEX_POOL_KB == 0
extern void *__real_malloc(size_t n);
static uint8_t *take_pool(size_t want, size_t keep, size_t *got) {
    for (size_t n = want & ~(size_t)1023; n >= 6 * 1024u; n -= 2048u) {
        if (heap_room() < n + keep) { continue; }
        uint8_t *p = __real_malloc(n);
        if (!p) { continue; }
        if (heap_room() < keep) { free(p); continue; }     /* it came from the top and took the reserve */
        *got = n;
        return p;
    }
    return NULL;
}
#endif

static void record_prev_fault(void);

bool pex_gw_ensure(void) {
    if (s_up) { return true; }
    boot_record();
    if (s_failed) { return false; }
    size_t need = 0;
#if CONFIG_PEX_POOL_KB > 0
    s_pool = s_static_pool;
    need = sizeof s_static_pool;
#else
    size_t want = (size_t)CONFIG_PEX_HEAP_POOL_KB * 1024u, keep = (size_t)CONFIG_PEX_HEAP_RESERVE_KB * 1024u;
    s_pool = take_pool(want, keep, &need);
    if (!s_pool) {
        printf("pex: not starting: no %u KB block in the C heap (%u KB free in all, %u KB must stay free)\n",
               6u, (unsigned)(heap_room() / 1024), (unsigned)(keep / 1024));
        return false;
    }
#endif
    s_pool_size = (uint32_t)need;
#if CONFIG_OTA
    pex_sig_uecc_set_keys(&OTA_PUBKEY, 1);
#endif
    pex_config_t cfg = {
        .pool = s_pool, .pool_size = (uint32_t)need,
        .policy = ALLOW_UNSIGNED ? PEX_POLICY_ALLOW_UNSIGNED : 0,
#if CONFIG_OTA
        .verify = pex_sig_uecc_verify,
#endif
        .host_name = "btgw",
        .trace = &PEX_REC->trace,
        .on_unload = on_unload,
        .on_slice = on_slice,
        .slice_ms = CONFIG_PEX_SLICE_MS,
        .max_stack = 2048,
        .flash = pex_gw_flash(),
    };
    if (pex_init(&cfg) != PEX_OK) {
#if CONFIG_PEX_POOL_KB == 0
        free(s_pool);
#endif
        s_pool = NULL;
        s_failed = true;
        return false;
    }
#if CONFIG_PEX_POOL_KB > 0
    printf("pex: pool %u KB (static, apart from the Lua arena)\n", (unsigned)(need / 1024));
#else
    printf("pex: pool %u KB from the C heap (%u KB left free, %u KB of it at the top)\n", (unsigned)(need / 1024),
           (unsigned)(heap_room() / 1024), (unsigned)(heap_top_gap() / 1024));
#endif
    pex_register_group(&pex_group_sys);
    pex_register_group(&pex_group_board);
    pex_register_group(&pex_group_pio);
    pex_register_group(&pex_group_bus);
    pex_register_group(&pex_group_fs);
#if CONFIG_NET_HTTP_CLIENT
    pex_register_group(&pex_group_net);
#endif
#if CONFIG_BT_LE
    pex_register_group(&pex_group_bt);
#endif
#if CONFIG_BT_CLASSIC
    pex_register_group(&pex_group_classic);
#endif
#if CONFIG_PROTO_ANKER_CRYPTO
    pex_register_group(&pex_group_codec);
#endif
#if CONFIG_PEX_LUA
    pex_register_group(&pex_group_lua);
#endif
    s_up = true;
    return true;
}

/* ---- files ------------------------------------------------------------------ */
typedef struct { int h; } lfsrd_t;
static int lfs_read_at(void *ctx, uint32_t off, void *dst, uint32_t n) {
    lfsrd_t *r = ctx;
    if (fs_fseek(r->h, off) < 0) { return -1; }
    uint32_t done = 0;
    while (done < n) {
        int k = fs_fread(r->h, (uint8_t *)dst + done, n - done);
        if (k <= 0) { break; }
        done += (uint32_t)k;
    }
    return (int)done;
}

/* "lfs:/a/b", "/a/b", "a/b" -> "a/b"; sd:/ is read asynchronously (pex_gw_sd_begin). */
static const char *lfs_rel(const char *path, int *err) {
    if (strncmp(path, "sd:", 3) == 0) {
        pex_seterr(PEX_ENOTSUP, "sd:/ is read asynchronously: the pex.load/pex.install ops take it (or copy the file to lfs:/pex/)");
        *err = PEX_ENOTSUP;
        return NULL;
    }
    if (strncmp(path, "lfs:", 4) == 0) { path += 4; }
    while (*path == '/') { path++; }
    return path;
}

/* Open lfs file `rel` and load it into RAM (out) or install it into flash. */
int pex_gw_with_file(const char *rel, const char *as, pex_mod_t **out, bool install) {
    size_t size = 0;
    if (!fs_stat(rel, &size)) { pex_seterr(PEX_ENOENT, "lfs:/%s not found", rel); return PEX_ENOENT; }
    pex_arena_t *a = pex_scratch_begin(fs_file_mem());
    void *mem = a ? pex_scratch_alloc(a, fs_file_mem()) : NULL;
    if (!mem) { if (a) { pex_scratch_end(a); } pex_seterr(PEX_ENOMEM, "PEX pool full"); return PEX_ENOMEM; }
    lfsrd_t r = { fs_fopen(rel, FS_O_READ, mem) };
    if (r.h < 0) { pex_scratch_end(a); pex_seterr(PEX_EIO, "cannot open lfs:/%s", rel); return PEX_EIO; }
    pex_reader_t rd = { lfs_read_at, &r, (uint32_t)size };
    crumb(CRUMB_LOOP_PEX);
    int rc;
    if (install) {
        rc = pex_install(&rd, 0);
    } else {
        pex_load_opts_t o = { .name = as };
        *out = pex_load(&rd, &o, &rc);
    }
    fs_fclose(r.h);
    pex_scratch_end(a);
    return rc;
}

pex_mod_t *pex_gw_load_path(const char *path, const char *as, int *err) {
    int e = 0;
    if (!err) { err = &e; }
    if (!pex_gw_ensure()) { *err = PEX_ENOMEM; return NULL; }
    if (strncmp(path, "xip:", 4) == 0) {        /* an installed module, run in place */
        pex_load_opts_t o = { .name = as };
        crumb(CRUMB_LOOP_PEX);
        return pex_load_installed(path + 4, &o, err);
    }
    const char *rel = lfs_rel(path, err);
    if (!rel) { return NULL; }
    pex_mod_t *m = NULL;
    *err = pex_gw_with_file(rel, as, &m, false);
    return m;
}

/* The installed copy first (xip:<name>), else lfs:/pex/<name>.<arch>.pex. */
int pex_gw_find(const char *name, char *path, size_t cap) {
    size_t sz;
    if (pex_gw_ensure() && pex_installed_find(name, NULL) >= 0) {
        snprintf(path, cap, "xip:%s", name);
        return 1;
    }
    snprintf(path, cap, "pex/%s.%s.pex", name, pex_gw_arch());
    return fs_stat(path, &sz) ? 1 : 0;
}

/* ---- faults: previous boot, and the list autostart skips ------------------------ */
static void record_prev_fault(void) {
    if (!s_prev_valid || s_prev_logged) { return; }
    s_prev_logged = true;
    char bid[33];
    for (int i = 0; i < 16; i++) { snprintf(bid + 2 * i, 3, "%02x", s_prev.build_id[i]); }
    if (!s_prev.fault) {     /* reset while module code ran, no fault: a hang (watchdog) */
        snprintf(bid, sizeof bid, "%08lx", (unsigned long)s_prev.trace);
    }
    printf("pex: the previous boot %s in module %s (build %s, offset 0x%lx): autostart will skip it\n",
           s_prev.fault ? "FAULTED" : "was reset while running code", s_prev.fault ? s_prev.name : "?", bid,
           (unsigned long)s_prev.offset);
    faults_append(s_prev.fault ? s_prev.name : "?", bid, s_prev.fault ? "fault" : "hang",
                  (unsigned long)s_prev.offset);
}

/* Is this build id (hex) in var/pex_faults.json? */
static bool faulted_before(const uint8_t *build_id) {
    char *buf = malloc(FAULTS_MAX);
    if (!buf) { return false; }
    bool hit = false;
    if (cfg_file_read(FAULTS_PATH, buf, FAULTS_MAX)) {
        char bid[33], tr[9], path[24], got[40];
        for (int i = 0; i < 16; i++) { snprintf(bid + 2 * i, 3, "%02x", build_id[i]); }
        snprintf(tr, sizeof tr, "%02x%02x%02x%02x", build_id[3], build_id[2], build_id[1], build_id[0]);
        int n = pex_json_len(buf, "faults");
        for (int i = 0; i < n && !hit; i++) {
            snprintf(path, sizeof path, "faults.%d.build", i);
            if (pex_json_str(buf, path, got, sizeof got) && (strcmp(got, bid) == 0 || strcmp(got, tr) == 0)) { hit = true; }
        }
    }
    free(buf);
    return hit;
}

static bool file_build_id(const char *rel, uint8_t *bid) {
    size_t sz;
    if (!fs_stat(rel, &sz) || sz < PEX_HEADER_SIZE) { return false; }
    int h = fs_begin_read(rel, &sz);
    if (h < 0) { return false; }
    uint8_t hdr[PEX_HEADER_SIZE];
    int n = fs_read(h, hdr, sizeof hdr);
    fs_end_read(h);
    if (n != (int)sizeof hdr) { return false; }
    memcpy(bid, hdr + 76, 16);
    return true;
}

/* One app, for src/launch.c. The old all-at-once pass lived here. */
int pex_gw_launch_one(const char *name, const char *path_opt, const char *cfg_opt) {
    if (!name || !name[0]) return 2;
    if (!pex_gw_ensure()) {
        printf("pex: launch %s: the pool could not be taken\n", name);
        return -1;
    }
    pex_mod_t *m = pex_find(name);
    if (m) {
        pex_info_t in;
        pex_info(m, &in);
        if (in.state == PEX_ST_RUNNING || in.tasks > 0) {
            printf("pex: launch %s: already running\n", name);
            return 1;
        }
        if (!(in.flags & PEX_F_APP)) {
            printf("pex: launch %s: not an app\n", name);
            return 2;
        }
        if (pex_start(m, (cfg_opt && cfg_opt[0]) ? cfg_opt : NULL) != PEX_OK) {
            printf("pex: launch %s: %s\n", name, pex_errmsg());
            return -1;
        }
        printf("pex: launch %s\n", name);
        return 0;
    }
    char pathbuf[128];
    const char *path = (path_opt && path_opt[0]) ? path_opt : NULL;
    if (!path) {
        if (!pex_gw_find(name, pathbuf, sizeof pathbuf)) {
            printf("pex: launch %s: not found\n", name);
            return 2;
        }
        path = pathbuf;
    }
    uint8_t bid[16];
    int ign = 0;
    pex_installed_t xin;
    bool xip = strncmp(path, "xip:", 4) == 0 && pex_installed_find(path + 4, &xin) >= 0;
    if (xip) { memcpy(bid, xin.build_id, 16); }
    const char *rel = xip ? path : lfs_rel(path, &ign);
    if (!xip && (!rel || !file_build_id(rel, bid))) {
        printf("pex: launch %s: %s not found\n", name, path);
        return 2;
    }
    if (faulted_before(bid)) {
        printf("pex: launch %s: skipped, this build faulted before (var/pex_faults.json; pex.faults_clear)\n", name);
        api_emit("pex.autostart_skipped", "{\"reason\":\"faulted before\"}");
        return 2;
    }
    int err = 0;
    m = pex_gw_load_path(path, name, &err);
    if (!m) {
        printf("pex: launch %s: %s\n", name, pex_errmsg());
        return -1;
    }
    pex_info_t in;
    pex_info(m, &in);
    if (!(in.flags & PEX_F_APP)) {
        printf("pex: launch %s: not an app\n", name);
        return 2;
    }
    if (pex_start(m, (cfg_opt && cfg_opt[0]) ? cfg_opt : NULL) != PEX_OK) {
        printf("pex: launch %s: %s\n", name, pex_errmsg());
        return -1;
    }
    printf("pex: launch %s\n", name);
    return 0;
}

void pex_gw_record_boot_fault(void) {
    if (s_boot_ms) return;
    boot_record();
    s_boot_ms = to_ms_since_boot(get_absolute_time());
    if (!s_boot_ms) { s_boot_ms = 1; }
    record_prev_fault();
}

void pex_gw_poll(void) {
    pex_gw_record_boot_fault();
    if (!s_up) {
#if CONFIG_BT_LE
        /* The BLE queue is not part of the module pool. A host listener
         * (the role console) registers before any module has started it. */
        pex_gw_bt_poll();
#endif
        return;
    }
    pex_poll();
    pex_gw_pio_poll();
    pex_gw_fs_poll();
    pex_gw_sd_poll();
#if CONFIG_NET_HTTP_CLIENT
    pex_gw_net_poll();
#endif
#if CONFIG_BT_LE
    pex_gw_bt_poll();
#endif
#if CONFIG_BT_CLASSIC
    pex_gw_classic_poll();
#endif
}

/* ---- HardFault: attribute, persist, then the normal path (watchdog reboot) ----- */
void crumb_fault_hook(const uint32_t *frame) {
    if (!s_up) { return; }
    pex_fault_t f;
    if (!pex_fault_attribute(frame, &f)) { return; }
    volatile pex_boot_rec_t *r = PEX_REC;
    r->fault = 1;
    for (unsigned i = 0; i < sizeof f.name; i++) { r->name[i] = f.name[i]; }
    for (unsigned i = 0; i < 16; i++) { r->build_id[i] = f.build_id[i]; }
    r->offset = f.offset;
    r->pc = f.pc;
    r->in_host = f.in_host;
}

/* ---- ops ------------------------------------------------------------------------- */
static bool param_str(const char *params, const char *key, char *out, size_t cap) {
    return pex_json_str(params, key, out, cap) && out[0];
}

static void reply_err_pex(api_reply_fn reply, void *ctx, long id, int rc) {
    api_reply_err(reply, ctx, id, pex_strerror(rc), pex_errmsg()[0] && strcmp(pex_errmsg(), "ok") ? pex_errmsg() : pex_strerror(rc));
}

static pex_mod_t *named(const char *params, api_reply_fn reply, void *ctx, long id) {
    char nm[24];
    if (!param_str(params, "name", nm, sizeof nm)) { api_reply_err(reply, ctx, id, "bad_request", "need \"name\""); return NULL; }
    pex_mod_t *m = s_up ? pex_find(nm) : NULL;
    if (!m) { api_reply_err(reply, ctx, id, "not_loaded", nm); }
    return m;
}

static void prev_fault_json(char *dst, size_t cap) {
    if (!s_prev_valid) { snprintf(dst, cap, "null"); return; }
    char bid[33];
    for (int i = 0; i < 16; i++) { snprintf(bid + 2 * i, 3, "%02x", s_prev.build_id[i]); }
    snprintf(dst, cap, "{\"why\":\"%s\",\"name\":\"%s\",\"build\":\"%s\",\"offset\":%lu,\"pc\":%lu,\"trace\":%lu}",
             s_prev.fault ? "fault" : "hang", s_prev.fault ? s_prev.name : "", s_prev.fault ? bid : "",
             (unsigned long)s_prev.offset, (unsigned long)s_prev.pc, (unsigned long)s_prev.trace);
}

static void op_list(const char *params, api_reply_fn reply, void *ctx, long id) {
    (void)params;
    char *b = api_scratch();
    size_t cap = api_scratch_size(), n = 0;
    pex_mem_stats_t ps = { 0 };
    if (s_up) { pex_pool_stats(&ps); }
    char pf[200], xs[220] = "null";
    prev_fault_json(pf, sizeof pf);
    pex_xip_stats_t st;
    if (s_up && pex_xip_stats(&st) == PEX_OK) {
        snprintf(xs, sizeof xs, "{\"size\":%lu,\"free\":%lu,\"largest\":%lu,\"modules\":%lu,\"rw_area\":%lu,\"rw_room\":%lu,\"flash_ops\":%lu}",
                 (unsigned long)st.size, (unsigned long)st.free, (unsigned long)st.largest, (unsigned long)st.entries,
                 (unsigned long)st.rw_area, (unsigned long)st.rw_room, (unsigned long)pex_gw_xip_flash_ops());
    }
    n += (size_t)snprintf(b + n, cap - n, "{\"runtime\":\"%s\",\"api\":\"%d.%d\",\"arch\":\"%s\",\"up\":%s,\"policy\":\"%s\","
                          "\"pool\":{\"size\":%lu,\"free\":%lu,\"largest\":%lu,\"configured_kb\":%d,\"static\":%s},\"xip\":%s,\"c_heap_free\":%lu,\"c_heap_top\":%lu,\"prev_fault\":%s,\"modules\":",
                          PEX_RT_VERSION, PEX_API_MAJOR, PEX_API_MINOR, pex_gw_arch(), s_up ? "true" : "false",
                          (s_up ? pex_get_policy() : (ALLOW_UNSIGNED ? 1u : 0u)) & PEX_POLICY_ALLOW_UNSIGNED
                              ? "UNSIGNED ALLOWED (development)" : "signed-only",
                          (unsigned long)ps.pool_size, (unsigned long)ps.pool_free, (unsigned long)ps.pool_largest,
                          POOL_KB_CONF, CONFIG_PEX_POOL_KB > 0 ? "true" : "false", xs, (unsigned long)heap_room(),
                          (unsigned long)heap_top_gap(), pf);
    if (s_up) {
        int k = pex_list_json(b + n, cap - n - 2);
        n += k > 0 ? (size_t)k : 0;
    } else {
        n += (size_t)snprintf(b + n, cap - n, "[]");
    }
    snprintf(b + n, cap - n, "}");
    api_reply_ok(reply, ctx, id, b);
}

static void op_info(const char *params, api_reply_fn reply, void *ctx, long id) {
    pex_mod_t *m = named(params, reply, ctx, id);
    if (!m) { return; }
    char *b = api_scratch();
    if (pex_info_json(m, b, api_scratch_size()) < 0) { api_reply_err(reply, ctx, id, "too_large", "info"); return; }
    api_reply_ok(reply, ctx, id, b);
}

static void op_load(const char *params, api_reply_fn reply, void *ctx, long id) {
    char nm[24] = "", path[128] = "", as[24] = "";
    int ram = 0;
    param_str(params, "name", nm, sizeof nm);
    param_str(params, "as", as, sizeof as);
    pex_json_bool(params, "ram", &ram);
    if (!param_str(params, "path", path, sizeof path)) {
        if (!nm[0]) { api_reply_err(reply, ctx, id, "bad_request", "need \"name\" or \"path\""); return; }
        if (ram) {
            size_t sz;
            snprintf(path, sizeof path, "pex/%s.%s.pex", nm, pex_gw_arch());
            if (!fs_stat(path, &sz)) { api_reply_err(reply, ctx, id, "not_found", path); return; }
        } else if (!pex_gw_find(nm, path, sizeof path)) {
            api_reply_err(reply, ctx, id, "not_found", "not installed, and no lfs:/pex/<name>.<arch>.pex");
            return;
        }
    }
    if (strncmp(path, "sd:", 3) == 0) {
        int rc = pex_gw_sd_begin(path, as[0] ? as : (nm[0] ? nm : NULL), false);
        if (rc != PEX_EPENDING) { reply_err_pex(reply, ctx, id, rc); return; }
        api_reply_ok(reply, ctx, id, "{\"pending\":true,\"event\":\"pex.sd_done\"}");
        return;
    }
    int err = 0;
    pex_mod_t *m = pex_gw_load_path(path, as[0] ? as : (nm[0] ? nm : NULL), &err);
    if (!m) {
        if (err == PEX_ENOENT) { api_reply_err(reply, ctx, id, "not_found", path); return; }
        if (err == PEX_ENOMEM && !s_up) { api_reply_err(reply, ctx, id, "no_memory", "the PEX pool could not be taken"); return; }
        reply_err_pex(reply, ctx, id, err);
        return;
    }
    char *b = api_scratch();
    pex_info_json(m, b, api_scratch_size());
    api_reply_ok(reply, ctx, id, b);
}

/* ---- XIP: the PEX flash partition ------------------------------------------------------ */
static bool xip_up(api_reply_fn reply, void *ctx, long id) {
    if (!pex_gw_ensure()) { api_reply_err(reply, ctx, id, "no_memory", "the PEX runtime could not start"); return false; }
    pex_xip_stats_t st;
    if (pex_xip_stats(&st) != PEX_OK) { api_reply_err(reply, ctx, id, "not_supported", "this build has no PEX flash partition"); return false; }
    return true;
}

static void reply_installed(api_reply_fn reply, void *ctx, long id, int slot) {
    pex_installed_t in;
    char *b = api_scratch();
    if (pex_installed(slot, &in) != PEX_OK) { api_reply_ok(reply, ctx, id, "{}"); return; }
    char bid[33];
    for (int i = 0; i < 16; i++) { snprintf(bid + 2 * i, 3, "%02x", in.build_id[i]); }
    pex_xip_stats_t st;
    pex_xip_stats(&st);
    snprintf(b, api_scratch_size(), "{\"name\":\"%s\",\"version\":\"%lu.%lu.%lu\",\"build_id\":\"%s\",\"model\":\"%s\","
             "\"offset\":%lu,\"size\":%lu,\"signed\":%s,\"xip_ready\":%s,\"free\":%lu,\"largest\":%lu,\"moves\":%lu}",
             in.name, (unsigned long)(in.version >> 24), (unsigned long)((in.version >> 16) & 0xFF),
             (unsigned long)(in.version & 0xFFFF), bid, (in.flags & PEX_F_PIC) ? "pic" : "reloc", (unsigned long)in.offset,
             (unsigned long)in.file_size, in.signed_ ? "true" : "false", in.xip_ready ? "true" : "false",
             (unsigned long)st.free, (unsigned long)st.largest, (unsigned long)st.moves);
    api_reply_ok(reply, ctx, id, b);
}

static void op_install(const char *params, api_reply_fn reply, void *ctx, long id) {
    char nm[24] = "", path[128] = "";
    param_str(params, "name", nm, sizeof nm);
    if (!param_str(params, "path", path, sizeof path)) {
        if (!nm[0]) { api_reply_err(reply, ctx, id, "bad_request", "need \"name\" or \"path\""); return; }
        snprintf(path, sizeof path, "%s", nm);
    }
    if (!xip_up(reply, ctx, id)) { return; }
    if (strncmp(path, "sd:", 3) == 0) {
        int rc = pex_gw_sd_begin(path, NULL, true);
        if (rc != PEX_EPENDING) { reply_err_pex(reply, ctx, id, rc); return; }
        api_reply_ok(reply, ctx, id, "{\"pending\":true,\"event\":\"pex.sd_done\"}");
        return;
    }
    int slot = pex_gw_install_path(path);
    if (slot < 0) {
        if (slot == PEX_ENOENT) { api_reply_err(reply, ctx, id, "not_found", pex_errmsg()); return; }
        reply_err_pex(reply, ctx, id, slot);
        return;
    }
    reply_installed(reply, ctx, id, slot);
}

static void op_installed(const char *params, api_reply_fn reply, void *ctx, long id) {
    (void)params;
    if (!xip_up(reply, ctx, id)) { return; }
    char *b = api_scratch();
    if (pex_installed_json(b, api_scratch_size()) < 0) { api_reply_err(reply, ctx, id, "too_large", "installed"); return; }
    api_reply_ok(reply, ctx, id, b);
}

static void op_uninstall(const char *params, api_reply_fn reply, void *ctx, long id) {
    char nm[24];
    if (!param_str(params, "name", nm, sizeof nm)) { api_reply_err(reply, ctx, id, "bad_request", "need \"name\""); return; }
    if (!xip_up(reply, ctx, id)) { return; }
    int rc = pex_uninstall(nm);
    if (rc == PEX_ENOENT) { api_reply_err(reply, ctx, id, "not_found", nm); return; }
    if (rc != PEX_OK) { reply_err_pex(reply, ctx, id, rc); return; }
    api_reply_ok(reply, ctx, id, "{\"uninstalled\":true}");
}

static void op_compact(const char *params, api_reply_fn reply, void *ctx, long id) {
    (void)params;
    if (!xip_up(reply, ctx, id)) { return; }
    int rc = pex_xip_compact();
    if (rc < 0) { reply_err_pex(reply, ctx, id, rc); return; }
    pex_xip_stats_t st;
    pex_xip_stats(&st);
    char b[96];
    snprintf(b, sizeof b, "{\"moved\":%d,\"free\":%lu,\"largest\":%lu}", rc, (unsigned long)st.free, (unsigned long)st.largest);
    api_reply_ok(reply, ctx, id, b);
}

static void op_verify(const char *params, api_reply_fn reply, void *ctx, long id) {
    char nm[24];
    if (!param_str(params, "name", nm, sizeof nm)) { api_reply_err(reply, ctx, id, "bad_request", "need \"name\""); return; }
    if (!xip_up(reply, ctx, id)) { return; }
    uint64_t t0 = time_us_64();
    int rc = pex_xip_verify(nm);
    if (rc == PEX_ENOENT) { api_reply_err(reply, ctx, id, "not_found", nm); return; }
    if (rc != PEX_OK) { reply_err_pex(reply, ctx, id, rc); return; }
    char b[64];
    snprintf(b, sizeof b, "{\"verified\":true,\"ms\":%lu}", (unsigned long)((time_us_64() - t0) / 1000));
    api_reply_ok(reply, ctx, id, b);
}

static void op_start(const char *params, api_reply_fn reply, void *ctx, long id) {
    pex_mod_t *m = named(params, reply, ctx, id);
    if (!m) { return; }
    char *cfg = api_scratch();
    const char *arg = NULL;
    size_t cl = 0;
    const char *c = pex_json_find(params, "cfg", &cl);
    if (c && *c == '"') {
        pex_json_str(params, "cfg", cfg, api_scratch_size());
        arg = cfg;
    } else if (c && cl < api_scratch_size()) {
        memcpy(cfg, c, cl);
        cfg[cl] = 0;
        arg = cfg;
    }
    int rc = pex_start(m, arg);
    if (rc != PEX_OK) { reply_err_pex(reply, ctx, id, rc); return; }
    api_reply_ok(reply, ctx, id, "{\"started\":true}");
}

static void op_stop(const char *params, api_reply_fn reply, void *ctx, long id) {
    pex_mod_t *m = named(params, reply, ctx, id);
    if (!m) { return; }
    int rc = pex_stop(m);
    if (rc != PEX_OK) { reply_err_pex(reply, ctx, id, rc); return; }
    api_reply_ok(reply, ctx, id, "{\"stopped\":true}");
}

static void op_unload(const char *params, api_reply_fn reply, void *ctx, long id) {
    pex_mod_t *m = named(params, reply, ctx, id);
    if (!m) { return; }
    /* Lua may hold the only ref. Tell it, drop that ref, then unload.
     * A module that is still running or on a call stack is left held, and
     * pex_unload reports that. */
#if CONFIG_PEX_LUA
    int detached = pexlua_before_unload(m);
#else
    int detached = 0;
#endif
    int rc = pex_unload(m);
    if (rc != PEX_OK) {
        if (detached) {
            printf("pexlua: Lua was detached from %s but unload failed: %s\n", pex_name(m), pex_errmsg());
        }
        reply_err_pex(reply, ctx, id, rc);
        return;
    }
    api_reply_ok(reply, ctx, id, "{\"unloaded\":true}");
}

static void op_call(const char *params, api_reply_fn reply, void *ctx, long id) {
    pex_mod_t *m = named(params, reply, ctx, id);
    if (!m) { return; }
    char fn[40];
    if (!param_str(params, "fn", fn, sizeof fn)) { api_reply_err(reply, ctx, id, "bad_request", "need \"fn\""); return; }
    uint32_t a = pex_sym(m, fn);
    if (!a) { api_reply_err(reply, ctx, id, "not_found", fn); return; }
    int64_t v[4] = { 0 };
    char key[12];
    for (int i = 0; i < 4; i++) {
        snprintf(key, sizeof key, "args.%d", i);
        pex_json_int(params, key, &v[i]);
    }
    int32_t r = (int32_t)(uint32_t)pex_call(m, a, (uint32_t)v[0], (uint32_t)v[1], (uint32_t)v[2], (uint32_t)v[3]);
    char b[48];
    snprintf(b, sizeof b, "{\"result\":%ld}", (long)r);
    api_reply_ok(reply, ctx, id, b);
}

static void op_files(const char *params, api_reply_fn reply, void *ctx, long id) {
    (void)params;
    char *b = api_scratch();
    if (fs_list_json("pex", b, api_scratch_size()) < 0) { snprintf(b, api_scratch_size(), "{\"dir\":\"pex\",\"files\":[]}"); }
    api_reply_ok(reply, ctx, id, b);
}

static void op_autostart(const char *params, api_reply_fn reply, void *ctx, long id) {
    /* {"set": {...}} writes the file (validated); no params reads it */
    size_t sl = 0;
    const char *set = pex_json_find(params, "set", &sl);
    char *b = api_scratch();
    if (set) {
        if (*set != '{' || sl >= api_scratch_size()) { api_reply_err(reply, ctx, id, "bad_request", "\"set\" must be an object"); return; }
        memcpy(b, set, sl);
        b[sl] = 0;
        if (pex_json_len(b, "modules") < 0) { api_reply_err(reply, ctx, id, "bad_request", "need {\"modules\":[...]}"); return; }
        if (!cfg_file_write(AUTOSTART_PATH, b)) { api_reply_err(reply, ctx, id, "busy", "write failed, retry"); return; }
        api_reply_ok(reply, ctx, id, b);
        return;
    }
    if (!cfg_file_read(AUTOSTART_PATH, b, api_scratch_size())) { strcpy(b, "{\"modules\":[]}"); }
    api_reply_ok(reply, ctx, id, b);
}

static void op_faults(const char *params, api_reply_fn reply, void *ctx, long id) {
    (void)params;
    char *b = api_scratch();
    char pf[200];
    prev_fault_json(pf, sizeof pf);
    int n = snprintf(b, api_scratch_size(), "{\"prev_boot\":%s,\"last\":", pf);
    const pex_fault_t *f = s_up ? pex_last_fault() : NULL;
    if (f) {
        n += snprintf(b + n, api_scratch_size() - (size_t)n,
                      "{\"name\":\"%s\",\"offset\":%lu,\"pc\":%lu,\"in_host\":%s,\"hang\":%s}",
                      f->name, (unsigned long)f->offset, (unsigned long)f->pc, f->in_host ? "true" : "false",
                      f->hang ? "true" : "false");
    } else {
        n += snprintf(b + n, api_scratch_size() - (size_t)n, "null");
    }
    n += snprintf(b + n, api_scratch_size() - (size_t)n, ",\"skip\":");
    if (!cfg_file_read(FAULTS_PATH, b + n, api_scratch_size() - (size_t)n - 2)) { strcpy(b + n, "null"); }
    strcat(b, "}");
    api_reply_ok(reply, ctx, id, b);
}

static void op_faults_clear(const char *params, api_reply_fn reply, void *ctx, long id) {
    (void)params;
    fs_delete(FAULTS_PATH);
    api_reply_ok(reply, ctx, id, "{\"cleared\":true}");
}

static void op_ops(const char *params, api_reply_fn reply, void *ctx, long id) {
    (void)params;
    char *b = api_scratch();
    pex_gw_bus_ops_json(b, api_scratch_size());
    api_reply_ok(reply, ctx, id, b);
}

static const op_def_t pex_ops_tbl[] = {
    { "pex.list", op_list },     { "pex.info", op_info },     { "pex.load", op_load },
    { "pex.start", op_start },   { "pex.run", op_start },     { "pex.stop", op_stop },
    { "pex.unload", op_unload }, { "pex.call", op_call },     { "pex.files", op_files },
    { "pex.autostart", op_autostart }, { "pex.faults", op_faults }, { "pex.faults_clear", op_faults_clear },
    { "pex.ops", op_ops },       { "pex.install", op_install }, { "pex.installed", op_installed },
    { "pex.uninstall", op_uninstall }, { "pex.compact", op_compact }, { "pex.verify", op_verify },
    { NULL, NULL }
};
static const char *const pex_topics[] = { "pex.*", NULL };

static bool pex_mod_init(void) {
    boot_record();      /* nothing else: the pool is taken on first use */
    return true;
}

const op_module_t op_module_pex = { .ns = "pex", .ops = pex_ops_tbl, .topics = pex_topics, .init = pex_mod_init };
