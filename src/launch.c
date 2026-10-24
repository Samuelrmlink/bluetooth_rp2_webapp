/* Boot launcher. See launch.h. */
#include "launch.h"

#include <stdio.h>
#include <string.h>

#include "autoconf.h"
#include "api/api.h"
#include "api/ops.h"
#include "config/config.h"
#include "fs/fs.h"
#include "hardware/watchdog.h"
#include "pico/stdlib.h"

#if CONFIG_PEX
#include "pex/pex_gw.h"
#endif
#if CONFIG_LUA_ENABLE
#include "lua/lrt.h"
#endif
#if CONFIG_NET_WIFI
#include "net/wifi.h"
#endif
#if CONFIG_NET_WIREGUARD
#include "net/wg.h"
#endif
#if CONFIG_NET_UPLINK
#include "net/uplink.h"
#endif
#if CONFIG_BT_LE
#include "bt/le/central.h"
#endif

#define LAUNCH_PATH     "cfg/launch.json"
#define LAUNCH_ARM      0x4C41554Eu   /* 'LAUN' — scratch[2], free of crumb and the SDK */
#define LAUNCH_MAX      16
#define LAUNCH_GAP_MS   1000u   /* long enough for the entry to issue its first connect */
#define LAUNCH_SETTLE_MS 2000u  /* idle must hold; Anker's other slot connects inside this */
#define LAUNCH_QUIET_MS 60000u  /* no attempt at all: a missing device, or not a radio program */
#define LAUNCH_BUSY_MS  75000u  /* connect 15s + discovery 15s + handshake 45s */
#define LAUNCH_DISARM_MS 1000u

/* phase: 0 waiting on the gate, 1 gap between entries, 2 finished, 3 skipped */
enum { PH_WAIT = 0, PH_GAP, PH_DONE, PH_SKIP };

static bool s_armed;
static bool s_safe;
static const char *s_skip;          /* "watchdog" | "safe" | NULL */
static bool s_skip_emitted;
static bool s_now;                  /* launch.now: this boot, leave the file alone */
static bool s_loaded;
static bool s_fallback;
static char s_when[16];
static uint32_t s_loop_ms;
static uint32_t s_started_ms;       /* when the entry we are waiting on was started; 0 = none */
static uint32_t s_busy_ms;          /* first moment that entry had a connect in flight */
static uint32_t s_idle_ms;          /* when that attempt went idle */
static int s_phase;
static int s_i, s_n;

typedef struct {
    char kind;                      /* 'p' PEX, 'l' Lua */
    char name[33];
    bool enabled;
    int src_index;                  /* modules[] index in the old PEX file, or -1 */
} entry_t;

static entry_t s_ent[LAUNCH_MAX];

/* One file buffer. The loop is cooperative, so the list load, the fallback
 * re-read at start, and launch.set share it. Replies go in api_scratch,
 * which already exists. A second 4 KB copy of each file does not fit in RAM. */
static char s_file[4096];

static uint32_t now_ms(void) { return to_ms_since_boot(get_absolute_time()); }

static bool known_when(const char *w) {
    return strcmp(w, "wifi") == 0 || strcmp(w, "wireguard") == 0 || strcmp(w, "meshtastic") == 0;
}

static bool name_ok(const char *s) {
    size_t n = strlen(s);
    if (n < 1 || n > 32) return false;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}

static bool have_name(const char *name) {
    for (int i = 0; i < s_n; i++) {
        if (strcmp(s_ent[i].name, name) == 0) return true;
    }
    return false;
}

static bool add_entry(char kind, const char *name, bool enabled, int src_index) {
    if (!name || !name[0] || s_n >= LAUNCH_MAX) return false;
    if (have_name(name)) {
        printf("launch: %s is already in the list\n", name);
        return false;
    }
    entry_t *e = &s_ent[s_n++];
    memset(e, 0, sizeof *e);
    e->kind = kind;
    snprintf(e->name, sizeof e->name, "%s", name);
    e->enabled = enabled;
    e->src_index = src_index;
    return true;
}

#if CONFIG_PEX
static void load_launch_file(char *buf) {
    char when[16] = "";
    if (!pex_json_str(buf, "when", when, sizeof when) || !known_when(when)) {
        snprintf(s_when, sizeof s_when, "%s", when[0] ? when : "wifi");
        if (!known_when(s_when)) {
            printf("launch: %s when \"%s\" is not wifi, wireguard or meshtastic\n", LAUNCH_PATH, s_when);
        }
    } else {
        snprintf(s_when, sizeof s_when, "%s", when);
    }
    int n = pex_json_len(buf, "entries");
    for (int i = 0; i < n && s_n < LAUNCH_MAX; i++) {
        char key[40], kind[8], name[40];
        snprintf(key, sizeof key, "entries.%d.kind", i);
        if (!pex_json_str(buf, key, kind, sizeof kind)) continue;
        snprintf(key, sizeof key, "entries.%d.name", i);
        if (!pex_json_str(buf, key, name, sizeof name) || !name[0]) continue;
        int en = 1;
        snprintf(key, sizeof key, "entries.%d.enabled", i);
        pex_json_bool(buf, key, &en);
        char k = 0;
        if (strcmp(kind, "pex") == 0) k = 'p';
        else if (strcmp(kind, "lua") == 0) k = 'l';
        else continue;
        add_entry(k, name, en != 0, -1);
    }
}

static void load_fallback(void) {
    s_fallback = true;
    snprintf(s_when, sizeof s_when, "wifi");
    size_t sz = 0;
    if (fs_stat("cfg/pex_autostart.json", &sz) && sz > 0 && sz < sizeof s_file &&
        cfg_file_read("cfg/pex_autostart.json", s_file, sizeof s_file)) {
        int n = pex_json_len(s_file, "modules");
        for (int i = 0; i < n && s_n < LAUNCH_MAX; i++) {
            char key[40], name[40];
            snprintf(key, sizeof key, "modules.%d.name", i);
            if (!pex_json_str(s_file, key, name, sizeof name) || !name[0]) continue;
            int start = 1;
            snprintf(key, sizeof key, "modules.%d.start", i);
            pex_json_bool(s_file, key, &start);
            add_entry('p', name, start != 0, i);
        }
    }
    sz = 0;
    if (fs_stat("cfg/autostart.json", &sz) && sz > 0 && sz < sizeof s_file &&
        cfg_file_read("cfg/autostart.json", s_file, sizeof s_file)) {
        int n = pex_json_len(s_file, "scripts");
        for (int i = 0; i < n && s_n < LAUNCH_MAX; i++) {
            char key[24], name[40];
            snprintf(key, sizeof key, "scripts.%d", i);
            if (!pex_json_str(s_file, key, name, sizeof name) || !name[0]) continue;
            add_entry('l', name, true, -1);
        }
    }
}

/* A JSON string, unescaped over itself. Output is never longer than input. */
static void unescape_inplace(char *s) {
    char *o = s;
    const char *p = s + 1;
    while (*p && *p != '"') {
        if (*p != '\\') { *o++ = *p++; continue; }
        p++;
        switch (*p) {
        case 'n': *o++ = '\n'; break;
        case 't': *o++ = '\t'; break;
        case 'r': *o++ = '\r'; break;
        case 'b': *o++ = '\b'; break;
        case 'f': *o++ = '\f'; break;
        case 'u': {
            unsigned cp = 0;
            for (int k = 1; k <= 4 && p[k]; k++) {
                char c = p[k];
                cp = cp * 16 + (unsigned)(c <= '9' ? c - '0' : (c | 32) - 'a' + 10);
            }
            p += 4;
            if (cp < 0x80) { *o++ = (char)cp; }
            else if (cp < 0x800) { *o++ = (char)(0xC0 | cp >> 6); *o++ = (char)(0x80 | (cp & 63)); }
            else { *o++ = (char)(0xE0 | cp >> 12); *o++ = (char)(0x80 | ((cp >> 6) & 63)); *o++ = (char)(0x80 | (cp & 63)); }
            break;
        }
        default: *o++ = *p; break;
        }
        if (*p) p++;
    }
    *o = 0;
}

/* Path and cfg from one row of cfg/pex_autostart.json. cfg points into s_file
 * and is valid until the next user of that buffer. NULL when the row has none. */
static void fallback_pex_args(int index, char *path, size_t pcap, const char **cfg) {
    path[0] = 0;
    *cfg = NULL;
    size_t sz = 0;
    if (!fs_stat("cfg/pex_autostart.json", &sz) || sz == 0 || sz >= sizeof s_file) return;
    if (!cfg_file_read("cfg/pex_autostart.json", s_file, sizeof s_file)) return;
    char key[40];
    snprintf(key, sizeof key, "modules.%d.path", index);
    pex_json_str(s_file, key, path, pcap);
    snprintf(key, sizeof key, "modules.%d.cfg", index);
    size_t cl = 0;
    const char *c = pex_json_find(s_file, key, &cl);
    if (!c || cl == 0 || cl + 1 > sizeof s_file) return;
    if (*c != '"' && *c != '{' && *c != '[') return;
    char *dst = s_file + sizeof s_file - cl - 1;
    memmove(dst, c, cl);
    dst[cl] = 0;
    if (*dst == '"') unescape_inplace(dst);
    *cfg = dst;
}
#endif

static void load_list(void) {
    s_loaded = true;
    s_n = 0;
    s_fallback = false;
    snprintf(s_when, sizeof s_when, "wifi");
#if CONFIG_PEX
    size_t sz = 0;
    if (fs_stat(LAUNCH_PATH, &sz) && sz > 0) {
        if (sz >= sizeof s_file || !cfg_file_read(LAUNCH_PATH, s_file, sizeof s_file) ||
            pex_json_len(s_file, "entries") < 0) {
            printf("launch: %s is unreadable, nothing starts from it\n", LAUNCH_PATH);
            return;
        }
        load_launch_file(s_file);
        return;
    }
    load_fallback();
#else
    printf("launch: this firmware has no PEX, so it cannot read the list\n");
#endif
}

/* wifi_bt_hold_base() is the grace window only. wifi_bt_hold() also adds the
 * WireGuard extension, and that calls back here, so the wifi gate uses the
 * base. */
static bool wifi_gate(void) {
#if CONFIG_NET_WIFI
    return !wifi_bt_hold_base();
#else
    return true;
#endif
}

static bool gate_open(void) {
    if (s_now) return true;
    if (strcmp(s_when, "wifi") == 0) return wifi_gate();
    if (strcmp(s_when, "wireguard") == 0) {
#if CONFIG_NET_WIREGUARD && CONFIG_NET_UPLINK
        return wg_is_confirmed() && uplink_ping_recent();
#else
        return false;
#endif
    }
    return false;     /* meshtastic, and any other value: held */
}

bool launch_radio_hold(void) {
    if (s_skip || s_now || !s_loaded) return false;
    if (strcmp(s_when, "wireguard") != 0) return false;
    return !gate_open();
}

bool launch_safe_mode(void) { return s_safe; }

void launch_boot(void) {
    s_phase = PH_WAIT;
    snprintf(s_when, sizeof s_when, "wifi");
    if (boot_after_watchdog) {
        watchdog_hw->scratch[2] = 0;
        s_skip = "watchdog";
        s_phase = PH_SKIP;
        printf("launch: watchdog boot, starting nothing\n");
        return;
    }
    if (watchdog_hw->scratch[2] == LAUNCH_ARM) {
        watchdog_hw->scratch[2] = 0;
        s_safe = true;
        s_skip = "safe";
        s_phase = PH_SKIP;
        printf("launch: safe mode (reset again before the arm cleared), starting nothing\n");
        return;
    }
    watchdog_hw->scratch[2] = LAUNCH_ARM;
    s_armed = true;
}

static void disarm(void) {
    if (!s_armed) return;
    uint32_t now = now_ms();
    if (!s_loop_ms) {
        s_loop_ms = now ? now : 1;
        return;
    }
    if ((int32_t)(now - s_loop_ms) >= (int32_t)LAUNCH_DISARM_MS) {
        watchdog_hw->scratch[2] = 0;
        s_armed = false;
    }
}

static void emit_skip(void) {
    if (s_skip_emitted || !s_skip) return;
    s_skip_emitted = true;
    char b[48];
    snprintf(b, sizeof b, "{\"reason\":\"%s\"}", s_skip);
    api_emit("launch.skipped", b);
}

static void start_lua(const char *name) {
#if CONFIG_LUA_ENABLE
    if (lrt_script_running(name)) {
        printf("launch: lua %s already running\n", name);
        return;
    }
    const char *err = NULL;
    if (lrt_start(name, &err)) {
        printf("launch: lua %s\n", name);
        return;
    }
    printf("launch: lua %s failed: %s\n", name, err ? err : "?");
    char msg[200];
    size_t k = (size_t)snprintf(msg, sizeof msg, "{\"script\":\"%s\",\"autostart\":true,\"error\":\"", name);
    k += api_json_escape(msg + k, sizeof msg - k - 4, err ? err : "?");
    snprintf(msg + k, sizeof msg - k, "\"}");
    api_emit("script.error", msg);
#else
    (void)name;
    printf("launch: lua is not in this firmware\n");
#endif
}

static void start_pex(const entry_t *e) {
#if CONFIG_PEX
    char path[128];
    const char *cfg = NULL;
    path[0] = 0;
    if (s_fallback && e->src_index >= 0) fallback_pex_args(e->src_index, path, sizeof path, &cfg);
    pex_gw_record_boot_fault();
    pex_gw_launch_one(e->name, path[0] ? path : NULL, cfg);
#else
    (void)e;
    printf("launch: pex is not in this firmware\n");
#endif
}

static void start_one(const entry_t *e) {
    if (e->kind == 'l') start_lua(e->name);
    else start_pex(e);
}

/* The entry in s_ent[s_i - 1] has been started. The next one may follow when
 * this returns true.
 *
 * le_connect already refuses a second gap_connect while one is outstanding
 * or a link is still being discovered. It does not refuse during the
 * application handshake: the same driver's other slot connects in that
 * window, and a refusal there would be counted as a failed attempt. So the
 * wait lives here. Once an attempt has been seen, the next entry waits until
 * discovery and the negotiation hold have both been idle for a couple of
 * seconds. That covers the gap between one slot releasing and the next slot
 * connecting. An entry that never connects, and one whose link never
 * finishes, both give up the wait. */
static bool entry_settled(uint32_t now) {
#if !CONFIG_BT_LE
    return (int32_t)(now - s_started_ms) >= (int32_t)LAUNCH_GAP_MS;
#else
    bool busy = le_bringup_busy() || le_negotiation_pending();
    const char *who = (s_i > 0) ? s_ent[s_i - 1].name : "?";
    if (busy) {
        if (!s_busy_ms) { s_busy_ms = now ? now : 1; }
        s_idle_ms = 0;
        if ((int32_t)(now - s_busy_ms) >= (int32_t)LAUNCH_BUSY_MS) {
            printf("launch: %s still bringing a link up; continuing\n", who);
            return true;
        }
        return false;
    }
    if (s_busy_ms) {
        if (!s_idle_ms) { s_idle_ms = now ? now : 1; }
        return (int32_t)(now - s_idle_ms) >= (int32_t)LAUNCH_SETTLE_MS &&
               (int32_t)(now - s_started_ms) >= (int32_t)LAUNCH_GAP_MS;
    }
    if ((int32_t)(now - s_started_ms) >= (int32_t)LAUNCH_QUIET_MS) {
        printf("launch: %s made no connection attempt; continuing\n", who);
        return true;
    }
    return false;
#endif
}

static void step(void) {
    if (s_phase == PH_DONE || s_phase == PH_SKIP) return;
    uint32_t now = now_ms();
    if (s_phase == PH_WAIT) {
        if (!gate_open()) return;
        s_phase = PH_GAP;
        s_started_ms = 0;                  /* the first entry starts on this pass */
    }
    if (s_phase == PH_GAP && s_started_ms && !entry_settled(now)) return;
    while (s_i < s_n && !s_ent[s_i].enabled) s_i++;
    if (s_i >= s_n) {
        s_phase = PH_DONE;
        printf("launch: list finished (%d entries)\n", s_n);
        return;
    }
    start_one(&s_ent[s_i]);
    s_i++;
    s_started_ms = now_ms();
    if (!s_started_ms) { s_started_ms = 1; }
    s_busy_ms = 0;
    s_idle_ms = 0;
    s_phase = s_i >= s_n ? PH_DONE : PH_GAP;
    if (s_phase == PH_DONE) printf("launch: list finished (%d entries)\n", s_n);
}

void launch_poll(void) {
    disarm();
    if (!s_loaded) load_list();
    if (s_skip) {
        emit_skip();
        return;
    }
    step();
}

static const char *phase_name(void) {
    if (s_skip) return "skipped";
    if (s_phase == PH_DONE) return "done";
    if (s_phase == PH_GAP) return "running";
    return "waiting";
}

size_t launch_status_json(char *dst, size_t cap) {
    if (!s_loaded) load_list();
    char when[24], reason[24];
    api_json_escape(when, sizeof when, s_when);
    api_json_escape(reason, sizeof reason, s_skip ? s_skip : "");
    size_t n = (size_t)snprintf(dst, cap,
        "{\"when\":\"%s\",\"source\":\"%s\",\"phase\":\"%s\",\"reason\":\"%s\","
        "\"safe_mode\":%s,\"watchdog\":%s,\"gate\":%s,\"now\":%s,\"index\":%d,\"count\":%d,\"entries\":[",
        when, s_fallback ? "fallback" : "launch", phase_name(), reason,
        s_safe ? "true" : "false",
        boot_after_watchdog ? "true" : "false",
        (!s_skip && gate_open()) ? "true" : "false",
        s_now ? "true" : "false",
        s_i, s_n);
    if (n >= cap) return cap ? cap - 1 : 0;
    for (int i = 0; i < s_n && n + 80 < cap; i++) {
        char nm[80];
        api_json_escape(nm, sizeof nm, s_ent[i].name);
        int w = snprintf(dst + n, cap - n,
            "%s{\"kind\":\"%s\",\"name\":\"%s\",\"enabled\":%s}",
            i ? "," : "", s_ent[i].kind == 'l' ? "lua" : "pex", nm,
            s_ent[i].enabled ? "true" : "false");
        if (w < 0 || n + (size_t)w >= cap) return cap - 1;
        n += (size_t)w;
    }
    int tail = snprintf(dst + n, cap - n, "]}");
    if (tail < 0 || n + (size_t)tail >= cap) return cap - 1;
    return n + (size_t)tail;
}

/* ---- ops ---------------------------------------------------------------------- */

static void op_status(const char *params, api_reply_fn reply, void *ctx, long id) {
    (void)params;
    char *b = api_scratch();
    launch_status_json(b, api_scratch_size());
    api_reply_ok(reply, ctx, id, b);
}

static void op_now(const char *params, api_reply_fn reply, void *ctx, long id) {
    (void)params;
    if (s_skip) {
        char b[64];
        snprintf(b, sizeof b, "{\"started\":false,\"reason\":\"%s\"}", s_skip);
        api_reply_ok(reply, ctx, id, b);
        return;
    }
    if (s_phase == PH_DONE) {
        api_reply_ok(reply, ctx, id, "{\"started\":false,\"reason\":\"done\"}");
        return;
    }
    s_now = true;
    api_reply_ok(reply, ctx, id, "{\"started\":true}");
}

#if CONFIG_PEX
static bool write_launch(const char *when, api_reply_fn reply, void *ctx, long id, const char *params) {
    int n = pex_json_len(params, "entries");
    if (n < 0) {
        api_reply_err(reply, ctx, id, "bad_request", "need {\"when\",\"entries\":[...]}");
        return false;
    }
    if (n > LAUNCH_MAX) {
        api_reply_err(reply, ctx, id, "bad_request", "at most 16 entries");
        return false;
    }
    /* The reply is this body plus a short prefix, and it has to fit in the
     * API scratch (WS_TX_MAX - 256). 16 short names are about 1.3 KB. */
    size_t o = (size_t)snprintf(s_file, 2048, "{\"when\":\"%s\",\"entries\":[", when);
    static char seen[LAUNCH_MAX][33];
    int ns = 0;
    for (int i = 0; i < n; i++) {
        char key[40], kind[8], name[40];
        snprintf(key, sizeof key, "entries.%d.kind", i);
        if (!pex_json_str(params, key, kind, sizeof kind) ||
            (strcmp(kind, "pex") != 0 && strcmp(kind, "lua") != 0)) {
            api_reply_err(reply, ctx, id, "bad_request", "kind is \"pex\" or \"lua\"");
            return false;
        }
        snprintf(key, sizeof key, "entries.%d.name", i);
        if (!pex_json_str(params, key, name, sizeof name) || !name_ok(name)) {
            api_reply_err(reply, ctx, id, "bad_request", "name must be letters, digits, underscore or dash");
            return false;
        }
        for (int k = 0; k < ns; k++) {
            if (strcmp(seen[k], name) == 0) {
                api_reply_err(reply, ctx, id, "bad_request", "each name once");
                return false;
            }
        }
        snprintf(seen[ns], sizeof seen[ns], "%s", name);
        ns++;
        int en = 1;
        snprintf(key, sizeof key, "entries.%d.enabled", i);
        pex_json_bool(params, key, &en);
        int w = snprintf(s_file + o, 2048 - o, "%s{\"kind\":\"%s\",\"name\":\"%s\",\"enabled\":%s}",
                         i ? "," : "", kind, name, en ? "true" : "false");
        if (w < 0 || o + (size_t)w >= 2048 - 3) {
            api_reply_err(reply, ctx, id, "bad_request", "list too long");
            return false;
        }
        o += (size_t)w;
    }
    snprintf(s_file + o, 2048 - o, "]}");
    if (!cfg_file_write(LAUNCH_PATH, s_file)) {
        api_reply_err(reply, ctx, id, "busy", "write failed, retry");
        return false;
    }
    char *out = api_scratch();
    snprintf(out, api_scratch_size(), "{\"stored\":true,\"next_boot\":true,\"file\":%s}", s_file);
    api_reply_ok(reply, ctx, id, out);
    return true;
}
#endif

static void op_set(const char *params, api_reply_fn reply, void *ctx, long id) {
#if CONFIG_PEX
    char when[16];
    if (!pex_json_str(params, "when", when, sizeof when) || !known_when(when)) {
        api_reply_err(reply, ctx, id, "bad_request", "when is \"wifi\", \"wireguard\" or \"meshtastic\"");
        return;
    }
    write_launch(when, reply, ctx, id, params);
#else
    (void)params;
    api_reply_err(reply, ctx, id, "unavailable", "this firmware has no PEX");
#endif
}

static const op_def_t launch_ops[] = {
    { "launch.status", op_status },
    { "launch.set", op_set },
    { "launch.now", op_now },
    { NULL, NULL }
};

static const char *const launch_topics[] = { "launch.skipped", NULL };

const op_module_t op_module_launch = {
    .ns = "launch", .ops = launch_ops, .topics = launch_topics, .init = NULL
};
