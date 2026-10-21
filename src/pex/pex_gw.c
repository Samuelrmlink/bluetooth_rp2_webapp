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
#endif

#if CONFIG_PEX_ALLOW_UNSIGNED
#define ALLOW_UNSIGNED 1
#else
#define ALLOW_UNSIGNED 0
#endif

#define AUTOSTART_PATH "cfg/pex_autostart.json"
#define FAULTS_PATH "var/pex_faults.json"
#define AUTOSTART_DELAY_MS 3000u
#define FAULTS_MAX 1024

/* ---- the record that survives a reset (SCRATCH_X, after crumb_rec_t) ------
 * 0x20080100..0x20080184 is the crumb record; stackmark paints from
 * 0x20080200. This 64-byte record sits in the gap. */
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
static uint32_t s_boot_ms;
static bool s_autostart_done;

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

static void record_prev_fault(void);

bool pex_gw_ensure(void) {
    if (s_up) { return true; }
    boot_record();
    size_t want = (size_t)CONFIG_PEX_POOL_KB * 1024u, keep = (size_t)CONFIG_PEX_HEAP_RESERVE_KB * 1024u;
    if (s_failed) { return false; }
    size_t need = 0;
    s_pool = take_pool(want, keep, &need);
    if (!s_pool) {
        printf("pex: not starting: no %u KB block in the C heap (%u KB free in all, %u KB must stay free)\n",
               6u, (unsigned)(heap_room() / 1024), (unsigned)(keep / 1024));
        return false;
    }
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
        .max_stack = 2048,
    };
    if (pex_init(&cfg) != PEX_OK) { free(s_pool); s_pool = NULL; s_failed = true; return false; }
    printf("pex: pool %u KB from the C heap (%u KB left free, %u KB of it at the top)\n", (unsigned)(need / 1024),
           (unsigned)(heap_room() / 1024), (unsigned)(heap_top_gap() / 1024));
    pex_register_group(&pex_group_sys);
    pex_register_group(&pex_group_board);
    pex_register_group(&pex_group_bus);
    pex_register_group(&pex_group_fs);
#if CONFIG_NET_HTTP_CLIENT
    pex_register_group(&pex_group_net);
#endif
#if CONFIG_BT_LE
    pex_register_group(&pex_group_bt);
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

/* "lfs:/a/b", "/a/b", "a/b" -> "a/b"; sd:/ is not a load source in this version. */
static const char *lfs_rel(const char *path, int *err) {
    if (strncmp(path, "sd:", 3) == 0) {
        pex_seterr(PEX_ENOTSUP, "loading from sd:/ is the next version: copy the file to lfs:/pex/");
        *err = PEX_ENOTSUP;
        return NULL;
    }
    if (strncmp(path, "lfs:", 4) == 0) { path += 4; }
    while (*path == '/') { path++; }
    return path;
}

pex_mod_t *pex_gw_load_path(const char *path, const char *as, int *err) {
    int e = 0;
    if (!err) { err = &e; }
    if (!pex_gw_ensure()) { *err = PEX_ENOMEM; return NULL; }
    const char *rel = lfs_rel(path, err);
    if (!rel) { return NULL; }
    size_t size = 0;
    if (!fs_stat(rel, &size)) { *err = PEX_ENOENT; return NULL; }
    pex_arena_t *a = pex_scratch_begin(fs_file_mem());
    void *mem = a ? pex_scratch_alloc(a, fs_file_mem()) : NULL;
    if (!mem) { if (a) { pex_scratch_end(a); } *err = PEX_ENOMEM; return NULL; }
    lfsrd_t r = { fs_fopen(rel, FS_O_READ, mem) };
    if (r.h < 0) { pex_scratch_end(a); *err = PEX_EIO; return NULL; }
    pex_reader_t rd = { lfs_read_at, &r, (uint32_t)size };
    pex_load_opts_t o = { .name = as };
    crumb(CRUMB_LOOP_PEX);
    pex_mod_t *m = pex_load(&rd, &o, err);
    fs_fclose(r.h);
    pex_scratch_end(a);
    return m;
}

int pex_gw_find(const char *name, char *path, size_t cap) {
    size_t sz;
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
    /* append to var/pex_faults.json: {"faults":[{"name","build","why","offset"}...]} */
    char *buf = malloc(FAULTS_MAX + 160);
    if (!buf) { return; }
    if (!cfg_file_read(FAULTS_PATH, buf, FAULTS_MAX) || pex_json_len(buf, "faults") < 0) {
        strcpy(buf, "{\"faults\":[]}");
    }
    size_t n = strlen(buf);
    char *end = strrchr(buf, ']');
    if (end && n < FAULTS_MAX - 10) {
        char item[150];
        snprintf(item, sizeof item, "%s{\"name\":\"%s\",\"build\":\"%s\",\"why\":\"%s\",\"offset\":%lu}",
                 pex_json_len(buf, "faults") > 0 ? "," : "", s_prev.fault ? s_prev.name : "?", bid,
                 s_prev.fault ? "fault" : "hang", (unsigned long)s_prev.offset);
        memmove(end + strlen(item), end, strlen(end) + 1);
        memcpy(end, item, strlen(item));
        fs_mkdir("var");
        cfg_file_write(FAULTS_PATH, buf);
    }
    free(buf);
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

/* ---- autostart: cfg/pex_autostart.json ----------------------------------------
 * {"modules":[{"name":"app","path":"pex/app.m33.pex","start":true,"cfg":{...}}, ...]}
 * "path" defaults to pex/<name>.<arch>.pex, "start" to true (if it is an app). */
static void autostart(void) {
    size_t sz;
    if (!fs_stat(AUTOSTART_PATH, &sz) || sz == 0) { return; }
    if (sz > 4096) { printf("pex: %s is over 4 KB, ignored\n", AUTOSTART_PATH); return; }
    if (!pex_gw_ensure()) { return; }
    pex_arena_t *a = pex_scratch_begin(sz + 1 + 1024 + 256);
    char *buf = a ? pex_scratch_alloc(a, sz + 1) : NULL;
    char *cfgbuf = a ? pex_scratch_alloc(a, 1024) : NULL;
    char *path = a ? pex_scratch_alloc(a, 128) : NULL;
    if (!buf || !cfgbuf || !path) { if (a) { pex_scratch_end(a); } return; }
    if (!cfg_file_read(AUTOSTART_PATH, buf, sz + 1)) { pex_scratch_end(a); return; }
    int n = pex_json_len(buf, "modules");
    if (n < 0) { printf("pex: %s: no \"modules\" array\n", AUTOSTART_PATH); }
    for (int i = 0; i < n; i++) {
        char key[40], name[24];
        snprintf(key, sizeof key, "modules.%d.name", i);
        if (!pex_json_str(buf, key, name, sizeof name)) { continue; }
        snprintf(key, sizeof key, "modules.%d.path", i);
        if (!pex_json_str(buf, key, path, 128)) { pex_gw_find(name, path, 128); }
        uint8_t bid[16];
        int ign = 0;
        const char *rel = lfs_rel(path, &ign);
        if (!rel || !file_build_id(rel, bid)) {
            printf("pex: autostart %s: %s not found\n", name, path);
            continue;
        }
        if (faulted_before(bid)) {
            printf("pex: autostart %s: SKIPPED -- this build faulted before (var/pex_faults.json; pex.faults_clear)\n", name);
            api_emit("pex.autostart_skipped", "{\"reason\":\"faulted before\"}");
            continue;
        }
        int err;
        pex_mod_t *m = pex_gw_load_path(path, name, &err);
        if (!m) {
            printf("pex: autostart %s: %s\n", name, pex_errmsg());
            continue;
        }
        int start = 1;
        snprintf(key, sizeof key, "modules.%d.start", i);
        pex_json_bool(buf, key, &start);
        pex_info_t in;
        pex_info(m, &in);
        if (start && (in.flags & PEX_F_APP)) {
            snprintf(key, sizeof key, "modules.%d.cfg", i);
            size_t cl = 0;
            const char *c = pex_json_find(buf, key, &cl);
            const char *arg = NULL;
            if (c && *c == '"') {
                pex_json_str(buf, key, cfgbuf, 1024);
                arg = cfgbuf;
            } else if (c && cl < 1024) {
                memcpy(cfgbuf, c, cl);
                cfgbuf[cl] = 0;
                arg = cfgbuf;
            }
            if (pex_start(m, arg) != PEX_OK) { printf("pex: autostart %s: %s\n", name, pex_errmsg()); }
        }
    }
    pex_scratch_end(a);
}

void pex_gw_poll(void) {
    if (!s_boot_ms) {
        boot_record();
        s_boot_ms = to_ms_since_boot(get_absolute_time());
        if (!s_boot_ms) { s_boot_ms = 1; }
    }
    if (!s_autostart_done && to_ms_since_boot(get_absolute_time()) - s_boot_ms > AUTOSTART_DELAY_MS) {
        s_autostart_done = true;
        record_prev_fault();
        autostart();
    }
    if (!s_up) { return; }
    pex_poll();
    pex_gw_fs_poll();
#if CONFIG_NET_HTTP_CLIENT
    pex_gw_net_poll();
#endif
#if CONFIG_BT_LE
    pex_gw_bt_poll();
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
    char pf[200];
    prev_fault_json(pf, sizeof pf);
    n += (size_t)snprintf(b + n, cap - n, "{\"runtime\":\"%s\",\"api\":\"%d.%d\",\"arch\":\"%s\",\"up\":%s,\"policy\":\"%s\","
                          "\"pool\":{\"size\":%lu,\"free\":%lu,\"largest\":%lu,\"configured_kb\":%d},\"c_heap_free\":%lu,\"c_heap_top\":%lu,\"prev_fault\":%s,\"modules\":",
                          PEX_RT_VERSION, PEX_API_MAJOR, PEX_API_MINOR, pex_gw_arch(), s_up ? "true" : "false",
                          (s_up ? pex_get_policy() : (ALLOW_UNSIGNED ? 1u : 0u)) & PEX_POLICY_ALLOW_UNSIGNED
                              ? "UNSIGNED ALLOWED (development)" : "signed-only",
                          (unsigned long)ps.pool_size, (unsigned long)ps.pool_free, (unsigned long)ps.pool_largest,
                          CONFIG_PEX_POOL_KB, (unsigned long)heap_room(), (unsigned long)heap_top_gap(), pf);
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
    param_str(params, "name", nm, sizeof nm);
    param_str(params, "as", as, sizeof as);
    if (!param_str(params, "path", path, sizeof path)) {
        if (!nm[0]) { api_reply_err(reply, ctx, id, "bad_request", "need \"name\" or \"path\""); return; }
        if (!pex_gw_find(nm, path, sizeof path)) {
            api_reply_err(reply, ctx, id, "not_found", "no lfs:/pex/<name>.<arch>.pex");
            return;
        }
    }
    int err = 0;
    pex_mod_t *m = pex_gw_load_path(path, as[0] ? as : (nm[0] ? nm : NULL), &err);
    if (!m) {
        if (err == PEX_ENOENT) { api_reply_err(reply, ctx, id, "not_found", path); return; }
        if (err == PEX_ENOMEM && !s_up) { api_reply_err(reply, ctx, id, "no_memory", "the PEX pool could not be taken from the C heap"); return; }
        reply_err_pex(reply, ctx, id, err);
        return;
    }
    char *b = api_scratch();
    pex_info_json(m, b, api_scratch_size());
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
    int rc = pex_unload(m);
    if (rc != PEX_OK) { reply_err_pex(reply, ctx, id, rc); return; }
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
        n += snprintf(b + n, api_scratch_size() - (size_t)n, "{\"name\":\"%s\",\"offset\":%lu,\"pc\":%lu,\"in_host\":%s}",
                      f->name, (unsigned long)f->offset, (unsigned long)f->pc, f->in_host ? "true" : "false");
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
    { "pex.ops", op_ops },       { NULL, NULL }
};
static const char *const pex_topics[] = { "pex.*", NULL };

static bool pex_mod_init(void) {
    boot_record();      /* nothing else: the pool is taken on first use */
    return true;
}

const op_module_t op_module_pex = { .ns = "pex", .ops = pex_ops_tbl, .topics = pex_topics, .init = pex_mod_init };
