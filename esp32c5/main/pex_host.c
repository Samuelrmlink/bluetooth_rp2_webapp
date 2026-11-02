#include "pex_host.h"

#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"
#include "esp_cache.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "psa/crypto.h"

#include "ble_pex.h"
#include "fs_lfs.h"
#include "ota/ota_pubkey.h"
#include "pex_rt.h"
#include "wifi_sta.h"

extern const pex_group_t pex_group_bt;

static const char *TAG = "pex";
/* DRAM, and executable only because CONFIG_ESP_SYSTEM_MEMPROT is off.
 * The IDRAM split would refuse instruction fetch from this pool. */
static uint8_t pool[32 * 1024] __attribute__((aligned(8)));
static const esp_partition_t *pex_part;
static const void *pex_map;
static esp_partition_mmap_handle_t pex_mmap;
static SemaphoreHandle_t mu;
static bool up;

int pex_json_str(const char *json, const char *path, char *out, size_t cap);
int pex_json_bool(const char *json, const char *path, int *out);
int pex_json_len(const char *json, const char *path);
int pex_json_int(const char *json, const char *path, int64_t *out);

uint32_t pex_port_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
uint64_t pex_port_us(void) { return (uint64_t)esp_timer_get_time(); }
void pex_port_log(int level, const char *line) {
    int l = level >= 3 ? ESP_LOG_ERROR : level == 2 ? ESP_LOG_WARN : ESP_LOG_INFO;
    ESP_LOG_LEVEL(l, "pex", "%s", line);
}
void pex_port_code_sync(void *p, uint32_t n) {
    if (p && n) {
        esp_cache_msync(p, n, ESP_CACHE_MSYNC_FLAG_TYPE_DATA | ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
        esp_cache_msync(p, n, ESP_CACHE_MSYNC_FLAG_TYPE_INST | ESP_CACHE_MSYNC_FLAG_INVALIDATE | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    }
    asm volatile("fence.i" ::: "memory");
}
int pex_port_stack_bounds(uintptr_t *lo, uintptr_t *hi) { (void)lo; (void)hi; return 0; }
void pex_port_slice_arm(uint32_t ms) { (void)ms; }
void pex_port_slice_disarm(void) {}

static int fl_erase(void *ctx, uint32_t off) {
    (void)ctx;
    return esp_partition_erase_range(pex_part, off, 4096) == ESP_OK ? 0 : -1;
}
static int fl_prog(void *ctx, uint32_t off, const void *src, uint32_t n) {
    (void)ctx;
    return esp_partition_write(pex_part, off, src, n) == ESP_OK ? 0 : -1;
}

static int pex_verify(uint16_t alg, uint16_t key_id, const uint8_t digest[32], const uint8_t *sig, uint32_t sig_len) {
    (void)key_id;
    if (alg != 1 || !sig || sig_len != 64) return PEX_ESIG;
    uint8_t raw[65] = { 0x04 };
    memcpy(raw + 1, OTA_PUBKEY, 64);
    if (psa_crypto_init() != PSA_SUCCESS) return PEX_ESIG;
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_VERIFY_HASH);
    psa_set_key_algorithm(&attr, PSA_ALG_ECDSA(PSA_ALG_SHA_256));
    psa_set_key_type(&attr, PSA_KEY_TYPE_ECC_PUBLIC_KEY(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&attr, 256);
    mbedtls_svc_key_id_t key = 0;
    if (psa_import_key(&attr, raw, sizeof raw, &key) != PSA_SUCCESS) return PEX_ESIG;
    psa_status_t st = psa_verify_hash(key, PSA_ALG_ECDSA(PSA_ALG_SHA_256), digest, 32, sig, 64);
    psa_destroy_key(key);
    return st == PSA_SUCCESS ? 0 : PEX_ESIG;
}

void pex_host_lock(void) { if (mu) xSemaphoreTakeRecursive(mu, portMAX_DELAY); }
void pex_host_unlock(void) { if (mu) xSemaphoreGiveRecursive(mu); }

void pex_host_init(void) {
    mu = xSemaphoreCreateRecursiveMutex();
    pex_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "pex");
    static pex_flash_t flash;
    const pex_flash_t *fp = NULL;
    if (pex_part && esp_partition_mmap(pex_part, 0, pex_part->size, ESP_PARTITION_MMAP_DATA, &pex_map, &pex_mmap) == ESP_OK) {
        flash.size = pex_part->size;
        flash.exec_base = (uintptr_t)pex_map;
        flash.read_base = (uintptr_t)pex_map;
        flash.erase = fl_erase;
        flash.prog = fl_prog;
        fp = &flash;
        ESP_LOGI(TAG, "partition %u bytes", (unsigned)pex_part->size);
    } else {
        ESP_LOGW(TAG, "no PEX partition; install is unavailable");
    }
    pex_config_t c = {
        .pool = pool, .pool_size = sizeof pool,
        .policy = PEX_POLICY_ALLOW_UNSIGNED,
        .verify = pex_verify, .host_name = "esp32c5", .flash = fp,
        .on_unload = ble_pex_unload,
    };
    if (pex_init(&c) != PEX_OK) {
        ESP_LOGE(TAG, "pex_init failed: %s", pex_errmsg());
        return;
    }
    if (pex_register_group(&pex_group_bt) != PEX_OK) {
        ESP_LOGE(TAG, "bt group: %s", pex_errmsg());
    }
    ble_pex_start();
    up = true;
}

/* ---- launch.json ---------------------------------------------------------- */
#define LAUNCH_MAX 8
typedef struct { char name[24]; bool enabled; bool started; } lent_t;
static lent_t lent[LAUNCH_MAX];
static int ln;
static char lwhen[16] = "wifi";
static bool lloaded, lfallback, lnow, ldone;
static const char *lskip;

static void load_launch(void) {
    lloaded = true;
    ln = 0;
    lfallback = false;
    snprintf(lwhen, sizeof lwhen, "wifi");
    char buf[1536];
    size_t got = 0;
    const char *doc = NULL;
    if (fs_read_file("cfg/launch.json", buf, sizeof buf - 1, &got) == 0 && got > 0) {
        buf[got] = 0;
        doc = buf;
        char when[16];
        if (pex_json_str(buf, "when", when, sizeof when)) snprintf(lwhen, sizeof lwhen, "%s", when);
        int n = pex_json_len(buf, "entries");
        for (int i = 0; i < n && ln < LAUNCH_MAX; i++) {
            char key[40], kind[8], name[24];
            snprintf(key, sizeof key, "entries.%d.kind", i);
            if (!pex_json_str(buf, key, kind, sizeof kind) || strcmp(kind, "pex")) {
                if (pex_json_str(buf, key, kind, sizeof kind) && !strcmp(kind, "lua"))
                    ESP_LOGW(TAG, "launch: lua is not in this image");
                continue;
            }
            snprintf(key, sizeof key, "entries.%d.name", i);
            if (!pex_json_str(buf, key, name, sizeof name) || !name[0]) continue;
            int en = 1;
            snprintf(key, sizeof key, "entries.%d.enabled", i);
            pex_json_bool(buf, key, &en);
            snprintf(lent[ln].name, sizeof lent[ln].name, "%s", name);
            lent[ln].enabled = en != 0;
            lent[ln].started = false;
            ln++;
        }
        (void)doc;
        return;
    }
    got = 0;
    if (fs_read_file("cfg/pex_autostart.json", buf, sizeof buf - 1, &got) == 0 && got > 0) {
        buf[got] = 0;
        lfallback = true;
        int n = pex_json_len(buf, "modules");
        for (int i = 0; i < n && ln < LAUNCH_MAX; i++) {
            char key[40], name[24];
            snprintf(key, sizeof key, "modules.%d.name", i);
            if (!pex_json_str(buf, key, name, sizeof name) || !name[0]) continue;
            int start = 1;
            snprintf(key, sizeof key, "modules.%d.start", i);
            pex_json_bool(buf, key, &start);
            snprintf(lent[ln].name, sizeof lent[ln].name, "%s", name);
            lent[ln].enabled = start != 0;
            ln++;
        }
    }
}

static int load_installed_or_file(const char *name);

static void launch_step(void) {
    if (!up) return;
#if CONFIG_C5_BLE
    /* The role console is the BLE user on this image. A launched module
     * would take the same controller. */
    lskip = "role";
    ldone = true;
    return;
#endif
    if (!lloaded) load_launch();
    if (ldone || lskip) return;
    if (!lnow) {
        if (strcmp(lwhen, "wifi") == 0) {
            if (!wifi_has_addr()) return;
        } else {
            lskip = "when";
            ESP_LOGW(TAG, "launch when \"%s\" is not available on this image", lwhen);
            return;
        }
    }
    for (int i = 0; i < ln; i++) {
        if (!lent[i].enabled || lent[i].started) continue;
        int rc = load_installed_or_file(lent[i].name);
        lent[i].started = true;
        if (rc != PEX_OK) ESP_LOGW(TAG, "launch %s: %s (%s)", lent[i].name, pex_strerror(rc), pex_errmsg());
        else ESP_LOGI(TAG, "launch started %s", lent[i].name);
        return;
    }
    ldone = true;
}

void pex_host_poll(void) {
    if (!up) return;
    pex_host_lock();
    pex_poll();
    ble_pex_poll();
    launch_step();
    pex_host_unlock();
}

void launch_now(void) { lnow = true; }

int launch_status_json(char *dst, size_t cap) {
    if (!lloaded) load_launch();
    const char *phase = lskip ? "skipped" : ldone ? "done" : "waiting";
    size_t n = (size_t)snprintf(dst, cap,
        "{\"when\":\"%s\",\"source\":\"%s\",\"phase\":\"%s\",\"reason\":\"%s\","
        "\"safe_mode\":false,\"watchdog\":false,\"gate\":%s,\"now\":%s,\"index\":%d,\"count\":%d,\"entries\":[",
        lwhen, lfallback ? "fallback" : "launch", phase, lskip ? lskip : "",
        (lnow || wifi_has_addr()) ? "true" : "false", lnow ? "true" : "false", ldone ? ln : 0, ln);
    for (int i = 0; i < ln && n + 80 < cap; i++) {
        n += (size_t)snprintf(dst + n, cap - n, "%s{\"kind\":\"pex\",\"name\":\"%s\",\"enabled\":%s}",
                              i ? "," : "", lent[i].name, lent[i].enabled ? "true" : "false");
    }
    snprintf(dst + n, cap - n, "]}");
    return 0;
}

int launch_set_json(const char *frame, char *dst, size_t cap, const char **err) {
    char when[16];
    if (!pex_json_str(frame, "params.when", when, sizeof when) ||
        (strcmp(when, "wifi") && strcmp(when, "wireguard") && strcmp(when, "meshtastic"))) {
        *err = "when is \"wifi\", \"wireguard\" or \"meshtastic\"";
        return -1;
    }
    int nent = pex_json_len(frame, "params.entries");
    if (nent < 0 || nent > LAUNCH_MAX) { *err = "need {\"when\",\"entries\":[...]}"; return -1; }
    char body[1024];
    size_t o = (size_t)snprintf(body, sizeof body, "{\"when\":\"%s\",\"entries\":[", when);
    for (int i = 0; i < nent; i++) {
        char key[48], kind[8], name[24];
        snprintf(key, sizeof key, "params.entries.%d.kind", i);
        if (!pex_json_str(frame, key, kind, sizeof kind) || strcmp(kind, "pex")) {
            *err = "this image starts PEX apps only";
            return -1;
        }
        snprintf(key, sizeof key, "params.entries.%d.name", i);
        if (!pex_json_str(frame, key, name, sizeof name) || !name[0]) { *err = "need a name"; return -1; }
        int en = 1;
        snprintf(key, sizeof key, "params.entries.%d.enabled", i);
        pex_json_bool(frame, key, &en);
        int w = snprintf(body + o, sizeof body - o, "%s{\"kind\":\"%s\",\"name\":\"%s\",\"enabled\":%s}",
                         i ? "," : "", kind, name, en ? "true" : "false");
        if (w < 0 || o + (size_t)w >= sizeof body - 3) { *err = "list too long"; return -1; }
        o += (size_t)w;
    }
    snprintf(body + o, sizeof body - o, "]}");
    if (fs_write_file("cfg/launch.json", body, strlen(body))) { *err = "write failed"; return -1; }
    snprintf(dst, cap, "{\"stored\":true,\"next_boot\":true,\"file\":%s}", body);
    return 0;
}

static int load_from_path(const char *path, const char *name, bool start, int *err);

static int file_read(void *ctx, uint32_t off, void *dst, uint32_t n) {
    return fs_pread(*(fs_rd_t **)ctx, off, dst, n);
}

/* Installed copy first, then pex/<name>.c5.pex. Starts it. */
static int load_installed_or_file(const char *name) {
    int err = 0;
    pex_load_opts_t opt = { .name = name };
    pex_mod_t *m = pex_load_installed(name, &opt, &err);
    if (!m) {
        char path[64];
        snprintf(path, sizeof path, "pex/%s.c5.pex", name);
        if (load_from_path(path, name, true, &err)) return err ? err : PEX_EIO;
        return PEX_OK;
    }
    return pex_start(m, NULL);
}

static int load_from_path(const char *path, const char *name, bool start, int *err) {
    uint32_t sz = 0;
    fs_rd_t *rd = fs_open_read(path, &sz);
    if (!rd) { *err = PEX_ENOENT; pex_seterr(PEX_ENOENT, "%s not found", path); return -1; }
    fs_rd_t *slot = rd;
    pex_reader_t reader = { file_read, &slot, sz };
    pex_load_opts_t opt = { .name = name && name[0] ? name : NULL };
    pex_mod_t *m = pex_load(&reader, &opt, err);
    fs_close_read(rd);
    if (!m) return -1;
    if (start) {
        int rc = pex_start(m, NULL);
        if (rc != PEX_OK) { *err = rc; return -1; }
    }
    return 0;
}

static int op_load(const char *frame, char *dst, size_t cap, const char **err) {
    char name[24] = "", path[96] = "";
    int ram = 0;
    pex_json_str(frame, "params.name", name, sizeof name);
    pex_json_str(frame, "params.path", path, sizeof path);
    pex_json_bool(frame, "params.ram", &ram);
    int lerr = 0;
    if (!path[0]) {
        if (!name[0]) { *err = "need \"name\" or \"path\""; return -1; }
        if (!ram) {
            pex_load_opts_t opt = { .name = name };
            pex_mod_t *m = pex_load_installed(name, &opt, &lerr);
            if (m) {
                snprintf(dst, cap, "{\"loaded\":true,\"name\":\"%s\"}", pex_name(m));
                return 0;
            }
        }
        snprintf(path, sizeof path, "pex/%s.c5.pex", name);
    }
    if (load_from_path(path, name, false, &lerr)) {
        *err = pex_errmsg();
        return -1;
    }
    snprintf(dst, cap, "{\"loaded\":true,\"name\":\"%s\"}", name[0] ? name : path);
    return 0;
}

int pex_host_op(const char *op, const char *frame, char *dst, size_t cap, const char **err) {
    if (!up) { *err = "pex is not up"; return -1; }
    pex_host_lock();
    int rc = 0;
    if (!strcmp(op, "pex.list")) {
        pex_mem_stats_t ps = { 0 };
        pex_pool_stats(&ps);
        pex_xip_stats_t st;
        char xs[200] = "null";
        if (pex_xip_stats(&st) == PEX_OK) {
            snprintf(xs, sizeof xs, "{\"size\":%lu,\"free\":%lu,\"largest\":%lu,\"modules\":%lu,\"rw_area\":%lu,\"rw_room\":%lu,\"flash_ops\":0}",
                     (unsigned long)st.size, (unsigned long)st.free, (unsigned long)st.largest,
                     (unsigned long)st.entries, (unsigned long)st.rw_area, (unsigned long)st.rw_room);
        }
        int n = snprintf(dst, cap,
            "{\"runtime\":\"%s\",\"api\":\"%d.%d\",\"arch\":\"c5\",\"up\":true,\"policy\":\"%s\","
            "\"pool\":{\"size\":%lu,\"free\":%lu,\"largest\":%lu,\"configured_kb\":32,\"static\":true},"
            "\"xip\":%s,\"c_heap_free\":0,\"c_heap_top\":0,\"prev_fault\":null,\"modules\":",
            PEX_RT_VERSION, PEX_API_MAJOR, PEX_API_MINOR,
            (pex_get_policy() & PEX_POLICY_ALLOW_UNSIGNED) ? "UNSIGNED ALLOWED (development)" : "signed-only",
            (unsigned long)ps.pool_size, (unsigned long)ps.pool_free, (unsigned long)ps.pool_largest, xs);
        int k = pex_list_json(dst + n, cap - (size_t)n - 2);
        if (k < 0) { *err = "list too large"; rc = -1; }
        else snprintf(dst + n + k, cap - (size_t)(n + k), "}");
    } else if (!strcmp(op, "pex.installed")) {
        if (pex_installed_json(dst, cap) < 0) { *err = "installed list too large"; rc = -1; }
    } else if (!strcmp(op, "pex.files")) {
        if (fs_list_json("pex", dst, cap) < 0) snprintf(dst, cap, "{\"dir\":\"pex\",\"files\":[]}");
    } else if (!strcmp(op, "pex.info") || !strcmp(op, "pex.stop") || !strcmp(op, "pex.unload") ||
               !strcmp(op, "pex.start") || !strcmp(op, "pex.run")) {
        char name[24];
        if (!pex_json_str(frame, "params.name", name, sizeof name)) { *err = "need \"name\""; rc = -1; }
        else {
            pex_mod_t *m = pex_find(name);
            if (!m) { *err = "not loaded"; rc = -1; }
            else if (!strcmp(op, "pex.info")) {
                if (pex_info_json(m, dst, cap) < 0) { *err = "info too large"; rc = -1; }
            } else if (!strcmp(op, "pex.stop")) {
                int e = pex_stop(m);
                if (e) { *err = pex_errmsg(); rc = -1; } else snprintf(dst, cap, "{\"stopped\":true}");
            } else if (!strcmp(op, "pex.unload")) {
                int e = pex_unload(m);
                if (e) { *err = pex_errmsg(); rc = -1; } else snprintf(dst, cap, "{\"unloaded\":true}");
            } else {
                int e = pex_start(m, NULL);
                if (e) { *err = pex_errmsg(); rc = -1; } else snprintf(dst, cap, "{\"started\":true}");
            }
        }
    } else if (!strcmp(op, "pex.load")) {
        rc = op_load(frame, dst, cap, err);
    } else if (!strcmp(op, "pex.install")) {
        char name[24] = "", path[96] = "";
        pex_json_str(frame, "params.name", name, sizeof name);
        pex_json_str(frame, "params.path", path, sizeof path);
        if (!path[0] && name[0]) snprintf(path, sizeof path, "pex/%s.c5.pex", name);
        uint32_t sz = 0;
        fs_rd_t *rd = path[0] ? fs_open_read(path, &sz) : NULL;
        if (!rd) { *err = "not found"; rc = -1; }
        else {
            fs_rd_t *slot = rd;
            pex_reader_t reader = { file_read, &slot, sz };
            int slotn = pex_install(&reader, 0);
            fs_close_read(rd);
            if (slotn < 0) { *err = pex_errmsg(); rc = -1; }
            else snprintf(dst, cap, "{\"installed\":true,\"slot\":%d}", slotn);
        }
    } else if (!strcmp(op, "pex.uninstall") || !strcmp(op, "pex.verify")) {
        char name[24];
        if (!pex_json_str(frame, "params.name", name, sizeof name)) { *err = "need \"name\""; rc = -1; }
        else if (!strcmp(op, "pex.uninstall")) {
            int e = pex_uninstall(name);
            if (e) { *err = pex_errmsg(); rc = -1; } else snprintf(dst, cap, "{\"uninstalled\":true}");
        } else {
            int e = pex_xip_verify(name);
            if (e) { *err = pex_errmsg(); rc = -1; } else snprintf(dst, cap, "{\"ok\":true}");
        }
    } else if (!strcmp(op, "pex.compact")) {
        int e = pex_xip_compact();
        if (e < 0) { *err = pex_errmsg(); rc = -1; }
        else snprintf(dst, cap, "{\"moved\":%d}", e);
    } else if (!strcmp(op, "pex.call")) {
        char name[24], fn[40];
        if (!pex_json_str(frame, "params.name", name, sizeof name) || !pex_json_str(frame, "params.fn", fn, sizeof fn)) {
            *err = "need \"name\" and \"fn\""; rc = -1;
        } else {
            pex_mod_t *m = pex_find(name);
            uint32_t a = m ? pex_sym(m, fn) : 0;
            if (!a) { *err = "not found"; rc = -1; }
            else {
                int64_t v[4] = { 0 };
                for (int i = 0; i < 4; i++) {
                    char key[24];
                    snprintf(key, sizeof key, "params.args.%d", i);
                    pex_json_int(frame, key, &v[i]);
                }
                int32_t r = (int32_t)(uint32_t)pex_call(m, a, (uint32_t)v[0], (uint32_t)v[1], (uint32_t)v[2], (uint32_t)v[3]);
                snprintf(dst, cap, "{\"result\":%ld}", (long)r);
            }
        }
    } else if (!strcmp(op, "pex.faults")) {
        const pex_fault_t *f = pex_last_fault();
        if (f && f->valid) {
            snprintf(dst, cap, "{\"prev_boot\":null,\"last\":{\"name\":\"%s\",\"offset\":%lu,\"pc\":%lu,\"in_host\":%s,\"hang\":%s},\"skip\":null}",
                     f->name, (unsigned long)f->offset, (unsigned long)f->pc,
                     f->in_host ? "true" : "false", f->hang ? "true" : "false");
        } else {
            snprintf(dst, cap, "{\"prev_boot\":null,\"last\":null,\"skip\":null}");
        }
    } else if (!strcmp(op, "pex.autostart")) {
        char buf[800];
        size_t got = 0;
        if (fs_read_file("cfg/pex_autostart.json", buf, sizeof buf - 1, &got) != 0) snprintf(dst, cap, "{\"modules\":[]}");
        else { buf[got] = 0; snprintf(dst, cap, "%s", buf); }
    } else {
        *err = "not supported on this image";
        rc = -1;
    }
    pex_host_unlock();
    return rc;
}
