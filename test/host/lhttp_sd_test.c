/*
 * http.start{to_file = "sd:/..."} on a host: src/lua/lhttp.c's sink and its
 * SD state machine (open, stage, write, close, remove on failure) against
 * the real sd_jobs / storage worker / FatFs / simulated card. The transport
 * is a fake httpc (below): the test plays the server, feeding the body only
 * as fast as the sink's room() allows -- the backpressure the real client
 * applies through TCP's window.
 *
 * Checked: a 2xx body lands in the file byte for byte (sizes that do and do
 * not fill the 4 KiB stage); the result has no body; a 404 is not written
 * and comes back in memory; a transfer that fails part way leaves no partial
 * file; a script stopped mid-download frees everything; the file module
 * reads the download back.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "fake_spi.h"
#include "fs/fs.h"
#include "lua/lfile.h"
#include "lua/lheap.h"
#include "lua/lhttp.h"
#include "lua/lwait.h"
#include "net/httpc.h"
#include "pico/time.h"
#include "sd/sd_api.h"
#include "sd/sd_card.h"
#include "sd/sd_core.h"
#include "sd/sd_jobs.h"
#include "sd/sd_store.h"
#include "sd/sd_worker.h"
#include "sdcard_sim.h"

static int checks, failed;
#define CHECK(c, msg) do { checks++; if (!(c)) { failed++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, msg); } } while (0)

extern void (*sd_host_yield_hook)(void);
extern void (*sd_host_maybe_yield_hook)(uint32_t budget_us);
extern sdsim_t *sd_host_detect_card;
static sdsim_t card;

/* --- the firmware around the module ---------------------------------------- */
static uint64_t s_now_us;
absolute_time_t get_absolute_time(void) { return s_now_us; }
static int s_slot = -1;
int lrt_current_slot(void) { return s_slot; }
void lrt_yield_soon(void) {}
/* The wait: the test's scheduler resumes the script every pass, so a wait is
 * just "come back next pass"; DELIVERED makes await look at the result. */
lwait_reason_t lrt_block_current(uint32_t token, uint8_t link, uint32_t timeout_ms) {
    (void)token; (void)link; (void)timeout_ms;
    return LWAIT_PENDING;
}
lwait_reason_t lrt_wake_reason(void) { return LWAIT_DELIVERED; }
bool fs_stat(const char *name, size_t *size) { (void)name; (void)size; return false; }
bool fs_is_dir(const char *path) { (void)path; return false; }
size_t fs_file_mem(void) { return 16; }
int fs_fopen(const char *name, int mode, void *mem) { (void)name; (void)mode; (void)mem; return -2; }
int fs_fread(int h, void *b, size_t n) { (void)h; (void)b; (void)n; return -9; }
int fs_fwrite(int h, const void *b, size_t n) { (void)h; (void)b; (void)n; return -9; }
int fs_fseek(int h, uint32_t p) { (void)h; (void)p; return -9; }
int32_t fs_ftell(int h) { (void)h; return -9; }
int32_t fs_fsize(int h) { (void)h; return -9; }
int fs_fclose(int h) { (void)h; return -9; }
bool fs_usage(uint32_t *u, uint32_t *t) { *u = 0; *t = 0; return true; }
int fs_walk(const char *d, fs_walk_fn cb, void *c) { (void)d; (void)cb; (void)c; return -1; }
int fs_mkdir(const char *p) { (void)p; return -1; }
int fs_delete(const char *p) { (void)p; return -1; }
int fs_rename(const char *a, const char *b) { (void)a; (void)b; return -1; }

/* --- a fake transport: the test is the server -------------------------------- */
static struct {
    bool used, cancelled, finished;
    int id;
    httpc_sink_t sink;
    void *user;
    char url[200];
} s_fake;
static int s_next_id = 0x10;

void httpc_set_alloc(const httpc_alloc_fns *fns) { (void)fns; }
int httpc_start(const httpc_opts_t *o, const httpc_sink_t *sink, void *user, httpc_err_t *err) {
    *err = HTTPC_OK;
    if (s_fake.used) { *err = HTTPC_ENOMEM; return -1; }
    memset(&s_fake, 0, sizeof s_fake);
    s_fake.used = true;
    s_fake.id = s_next_id;
    s_next_id += 0x10;
    s_fake.sink = *sink;
    s_fake.user = user;
    snprintf(s_fake.url, sizeof s_fake.url, "%s", o->url);
    return s_fake.id;
}
void httpc_cancel(int id) { if (s_fake.used && s_fake.id == id) { s_fake.cancelled = true; s_fake.used = false; } }
bool httpc_info(int id, httpc_state_t *st, uint64_t *bytes, int64_t *total, int *status) {
    if (!s_fake.used || s_fake.id != id) { return false; }
    if (st) { *st = s_fake.finished ? HTTPC_DONE : HTTPC_RECV; }
    if (bytes) { *bytes = 0; }
    if (total) { *total = -1; }
    if (status) { *status = 0; }
    return true;
}
void httpc_tick(void) {}
void httpc_reset(void) { s_fake.used = false; }
void httpc_stats(httpc_stats_t *out) { memset(out, 0, sizeof *out); }
const char *httpc_err_name(httpc_err_t e) { return e == HTTPC_OK ? "ok" : e == HTTPC_EIO ? "io" : "other"; }
const char *httpc_state_name(httpc_state_t s) { (void)s; return "recv"; }

static void worker_yield(void) { sdw_yield(); }

static void pass(void) {
    sdsim_advance_us(1000);
    s_now_us += 1000;
    sdw_service();
    lhttp_tick();            /* lrt_tick does this on the device */
}

/* Serve `n` bytes of the pattern as the response body, honouring room();
 * fail after `fail_at` bytes if it is set. Returns passes taken. */
static uint8_t pat(size_t i) { return (uint8_t)(i * 7 + (i >> 9)); }

static lua_State *s_co;
static int resume(lua_State *L) {
    if (!s_co) { return LUA_OK; }
    int nres = 0;
    s_slot = 0;
    int rc = lua_resume(s_co, L, 0, &nres);
    s_slot = -1;
    if (rc == LUA_YIELD) { lua_pop(s_co, nres); return rc; }
    if (rc != LUA_OK) { printf("script error: %s\n", lua_tostring(s_co, -1)); }
    s_co = NULL;
    return rc;
}

static void start(lua_State *L, const char *src) {
    s_co = lua_newthread(L);
    luaL_ref(L, LUA_REGISTRYINDEX);            /* keep it alive */
    if (luaL_loadstring(s_co, src) != LUA_OK) { printf("load: %s\n", lua_tostring(s_co, -1)); s_co = NULL; return; }
    resume(L);
}

static void serve(lua_State *L, int status, size_t n, size_t fail_at) {
    /* The request must exist (the script started it on its first resume). */
    CHECK(s_fake.used, "the script started a request");
    if (!s_fake.used) { return; }
    s_fake.sink.on_status(s_fake.user, status);
    s_fake.sink.on_header(s_fake.user, "content-type", "application/octet-stream");
    size_t sent = 0;
    uint8_t buf[1500];
    unsigned rnd = 12345;
    for (int guard = 0; guard < 200000 && sent < n; guard++) {
        size_t room = s_fake.sink.room(s_fake.user);
        rnd = rnd * 1103515245u + 12345u;
        size_t want = 1 + (rnd >> 16) % sizeof buf;
        if (want > n - sent) { want = n - sent; }
        if (want > room) { want = room; }
        if (fail_at && sent + want > fail_at) { want = fail_at - sent; }
        for (size_t k = 0; k < want; k++) { buf[k] = pat(sent + k); }
        if (want) { s_fake.sink.on_body(s_fake.user, buf, want); sent += want; }
        if (fail_at && sent >= fail_at) {
            s_fake.finished = true;
            s_fake.sink.on_done(s_fake.user, HTTPC_EIO, "connection reset");
            break;
        }
        pass();
        resume(L);
    }
    if (!fail_at) {
        s_fake.finished = true;
        s_fake.sink.on_done(s_fake.user, HTTPC_OK, NULL);
    }
    for (int i = 0; i < 20000 && s_co; i++) { pass(); resume(L); }
    CHECK(!s_co, "the script finished");
}

static void results(lua_State *L) {
    lua_getglobal(L, "R");
    lua_Integer n = luaL_len(L, -1);
    for (lua_Integer i = 1; i <= n; i++) {
        lua_rawgeti(L, -1, i);
        lua_getfield(L, -1, "name");
        lua_getfield(L, -2, "ok");
        lua_getfield(L, -3, "extra");
        char msg[220];
        snprintf(msg, sizeof msg, "%s %s", lua_tostring(L, -3), lua_tostring(L, -1) ? lua_tostring(L, -1) : "");
        CHECK(lua_toboolean(L, -2), msg);
        lua_pop(L, 4);
    }
    lua_pop(L, 1);
    lua_newtable(L);
    lua_setglobal(L, "R");
}

static const char *PRELUDE =
    "function ok(n, c, x) R[#R+1] = { name = n, ok = c and true or false, extra = x ~= nil and tostring(x) or nil } end\n"
    "function pat(i) return (i * 7 + (i >> 9)) & 0xFF end\n"
    "function get(t) local id, e = http.start(t); if not id then return nil, e end\n"
    "  while true do local r, w = http.await(id, 1000); if r or w ~= 'timeout' then return r, w end end end\n"
    "function check_file(path, n)\n"
    "  local h = file.open(path, 'r'); if not h then return false end\n"
    "  local i = 0\n"
    "  while true do local c = h:read(4096); if not c or c == '' then break end\n"
    "    for k = 1, #c do if c:byte(k) ~= pat(i) then h:close(); return false, i end; i = i + 1 end end\n"
    "  h:close(); return i == n, i end\n";

static void download(lua_State *L, size_t n) {
    char src[800];
    snprintf(src, sizeof src,
             "local r = get{ url = 'http://server/fw.bin', to_file = 'sd:/dl/fw.bin' }\n"
             "ok('to_file %zu: 200', r and r.status == 200, r and (r.error or r.status))\n"
             "ok('to_file %zu: no body in the result', r and r.body == nil)\n"
             "ok('to_file %zu: bytes', r and r.bytes == %zu, r and r.bytes)\n"
             "ok('to_file %zu: header', r and r.headers['content-type'] == 'application/octet-stream')\n"
             "local st = file.stat('sd:/dl/fw.bin')\n"
             "ok('to_file %zu: size on the card', st and st.size == %zu, st and st.size)\n"
             "ok('to_file %zu: content', check_file('sd:/dl/fw.bin', %zu))\n",
             n, n, n, n, n, n, n, n, n);
    start(L, src);
    serve(L, 200, n, 0);
    results(L);
}

int main(void) {
    fake_reset();
    fake_autocomplete(true);
    sd_host_yield_hook = worker_yield;
    sd_host_maybe_yield_hook = sdw_maybe_yield;
    sd_init();
    sdsim_init(&card, 300 * 2048, true);
    sd_host_detect_card = &card;
    sdsim_attach_sdio(&card);
    sd_card_use(sd_bus_sdio());
    for (int i = 0; i < 2600; i++) { pass(); }
    struct sd_reply r;
    CHECK(sd_format_start("fat32", 0, &r), "format starts");
    for (int i = 0; i < 100000; i++) {
        struct sd_format_status f;
        sd_format_get(&f);
        if (f.state == SDF_DONE || f.state == SDF_ERROR) { break; }
        pass();
    }
    for (int i = 0; i < 5000 && strcmp(sd_card_state(), "ok") != 0; i++) { pass(); }
    CHECK(strcmp(sd_card_state(), "ok") == 0, "card mounted");

    lheap_init();
    lwait_reset();
    lua_State *L = luaL_newstate();
    luaL_openlibs(L);
    lfile_open(L);
    lhttp_open(L);
    lua_newtable(L);
    lua_setglobal(L, "R");
    luaL_dostring(L, PRELUDE);

    download(L, 4096);           /* exactly one stage */
    download(L, 100000);         /* many, and a partial last one */
    download(L, 1);
    CHECK(sd_xfer_active() == 0, "no transfer ring held between downloads");

    /* A 404: not written, the body comes back in memory. */
    start(L, "local r = get{ url = 'http://server/missing', to_file = 'sd:/dl/missing.bin' }\n"
             "ok('404: status', r and r.status == 404, r and (r.error or r.status))\n"
             "ok('404: body in memory', r and r.body and #r.body == 300, r and r.body and #r.body)\n"
             "ok('404: no file', file.stat('sd:/dl/missing.bin') == nil)\n");
    serve(L, 404, 300, 0);
    results(L);

    /* A transfer that fails part way: an error, and no half file. */
    start(L, "local r = get{ url = 'http://server/broken', to_file = 'sd:/dl/broken.bin' }\n"
             "ok('failure: error io', r and r.error == 'io', r and (r.error or r.status))\n"
             "ok('failure: no partial file', file.stat('sd:/dl/broken.bin') == nil)\n");
    serve(L, 200, 50000, 20000);
    results(L);

    /* A script stopped mid-download: everything goes. */
    start(L, "local id = http.start{ url = 'http://server/fw2.bin', to_file = 'sd:/dl/fw2.bin' }\n"
             "while true do http.await(id, 1000) end\n");
    s_fake.sink.on_status(s_fake.user, 200);
    for (int i = 0; i < 200; i++) {
        uint8_t b[512];
        size_t room = s_fake.sink.room(s_fake.user);
        size_t n = room < sizeof b ? room : sizeof b;
        if (n) { memset(b, 0xAB, n); s_fake.sink.on_body(s_fake.user, b, n); }
        pass();
        resume(L);
    }
    lhttp_release(0);
    lfile_release(0);
    s_co = NULL;
    for (int i = 0; i < 200; i++) { pass(); }
    CHECK(s_fake.cancelled, "the stopped script's request was cancelled");
    CHECK(sd_xfer_active() == 0, "and the ring is back");
    /* All four SD handles are free again. */
    start(L, "local hs = {} for i = 1, 4 do hs[i] = file.open('sd:/dl/fw.bin', 'r') end\n"
             "ok('handles free after a stopped download', hs[4] ~= nil)\n"
             "for i = 1, 4 do if hs[i] then hs[i]:close() end end\n");
    for (int i = 0; i < 20000 && s_co; i++) { pass(); resume(L); }
    results(L);

    lhttp_reset();
    lfile_reset();
    lua_close(L);
    sdsim_free(&card);
    printf("lhttp sd: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
