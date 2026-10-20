/*
 * The Lua `file` module's SD side, end to end on a host: src/lua/lfile.c
 * (the binding, its yields and continuations) -> src/sd/sd_jobs.c -> the
 * storage worker (ucontext) -> sd_store -> FatFs -> the SDIO driver -> a
 * simulated card. A Lua script runs as a coroutine the way lrt.c runs one:
 * every yield is a main-loop pass, in which the worker gets its turn.
 *
 * Checked: open/read/write/seek/tell/size/close, stat/list/mkdir/remove/
 * rename and their error codes, writes larger than the job buffer, a listing
 * larger than it, four handles at once, <close>, a script stopped while a
 * job is in flight (the job finishes alone, nothing leaks), the card pulled
 * under an open handle, and that LittleFS-only paths do not yield.
 *
 * The LittleFS calls lfile.c makes are stubbed (fs.c needs flash); this test
 * is about the card.
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

/* --- what lfile.c needs from the rest of the firmware ---------------------- */
static uint64_t s_now_us;
absolute_time_t get_absolute_time(void) { return s_now_us; }
static int s_slot = -1;
int lrt_current_slot(void) { return s_slot; }
void lrt_yield_soon(void) {}
/* LittleFS: absent here. */
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

static void worker_yield(void) { sdw_yield(); }

static void pass(void) {
    sdsim_advance_us(1000);
    s_now_us += 1000;
    sdw_service();
}

/* Run `src` as a script would run: a coroutine, resumed once a pass, with
 * lfile's per-script state at slot 0. Returns the number of yields, or -1
 * when it raised (the message printed). Stops after max_passes, leaving the
 * coroutine suspended, when stop_early is set. */
static int run(lua_State *L, const char *src, int max_passes, bool stop_early, lua_State **out_co) {
    lua_State *co = lua_newthread(L);
    if (luaL_loadstring(co, src) != LUA_OK) { printf("load: %s\n", lua_tostring(co, -1)); return -1; }
    int yields = 0;
    for (int i = 0; i < max_passes; i++) {
        int nres = 0;
        s_slot = 0;
        int rc = lua_resume(co, L, 0, &nres);
        s_slot = -1;
        if (rc == LUA_OK) { lua_pop(L, 1); return yields; }
        if (rc != LUA_YIELD) { printf("script error: %s\n", lua_tostring(co, -1)); lua_pop(L, 1); return -1; }
        lua_pop(co, nres);
        yields++;
        pass();
        if (stop_early && i == max_passes - 1) { if (out_co) { *out_co = co; } return yields; }
    }
    lua_pop(L, 1);
    return yields;
}

/* The script reports through a global table R = { {name, ok, extra}, ... }. */
static void collect(lua_State *L) {
    lua_getglobal(L, "R");
    lua_Integer n = luaL_len(L, -1);
    for (lua_Integer i = 1; i <= n; i++) {
        lua_rawgeti(L, -1, i);
        lua_getfield(L, -1, "name");
        lua_getfield(L, -2, "ok");
        lua_getfield(L, -3, "extra");
        char msg[200];
        snprintf(msg, sizeof msg, "%s %s", lua_tostring(L, -3), lua_tostring(L, -1) ? lua_tostring(L, -1) : "");
        CHECK(lua_toboolean(L, -2), msg);
        lua_pop(L, 4);
    }
    lua_pop(L, 1);
    lua_newtable(L);
    lua_setglobal(L, "R");
}

static const char *SCRIPT =
    "local function ok(n, c, x) R[#R+1] = { name = n, ok = c and true or false, extra = x ~= nil and tostring(x) or nil } end\n"
    "local m = file.media()\n"
    "ok('media', m.sd.state == 'ok' and m.sd.fs == 'fat32' and m.sd.total > 0, m.sd.state)\n"
    "local h, e = file.open('sd:/fw/v1/img.bin', 'w')\n"
    "ok('open w makes parents', h, e)\n"
    "local big = {}\n"
    "for i = 1, 10000 do big[i] = string.char(i % 251) end\n"
    "big = table.concat(big)\n"
    "ok('write 10000 (three job buffers)', h:write(big))\n"
    "ok('size/tell after write', h:size() == 10000 and h:tell() == 10000, h:size())\n"
    "ok('close', h:close())\n"
    "ok('close again is fine', h:close())\n"
    "h = file.open('sd:/fw/v1/img.bin', 'r')\n"
    "ok('open r', h)\n"
    "ok('size on open', h:size() == 10000)\n"
    "local parts = {}\n"
    "while true do local c = h:read(4096); if not c or c == '' then break end parts[#parts+1] = c end\n"
    "ok('read back', table.concat(parts) == big, #table.concat(parts))\n"
    "ok('seek', h:seek(5000) == 5000 and h:tell() == 5000)\n"
    "ok('read after seek', h:read(3) == big:sub(5001, 5003))\n"
    "ok('read clamps to 4096', #h:read(100000) == 4096)\n"
    "h:close()\n"
    "do local a = file.open('sd:/fw/v1/img.bin', 'a'); a:write('XYZ'); ok('append', a:size() == 10003); a:close() end\n"
    "local st = file.stat('sd:/fw/v1/img.bin')\n"
    "ok('stat file', st and st.size == 10003 and not st.dir)\n"
    "ok('stat dir', file.stat('sd:/fw').dir)\n"
    "ok('stat root', file.stat('sd:/').dir)\n"
    "ok('stat missing', select(2, file.stat('sd:/nope')) == 'enoent')\n"
    "ok('open missing', select(2, file.open('sd:/nope.bin', 'r')) == 'enoent')\n"
    "ok('badpath', select(2, file.open('sd:/a:b', 'r')) == 'badpath')\n"
    "ok('mkdir -p', file.mkdir('sd:/x/y/z'))\n"
    "ok('mkdir existing', file.mkdir('sd:/x/y/z'))\n"
    "for i = 1, 150 do local w = file.open(string.format('sd:/x/many_a_rather_long_name_%03d.dat', i), 'w'); w:write('x'); w:close() end\n"
    "local l = file.list('sd:/x')\n"
    "ok('list beyond one buffer', l and #l == 151, l and #l)\n"
    "local seen = 0; for _, e in ipairs(l) do if e.name == 'y' and e.dir then seen = seen + 1 end; if e.size == 1 then seen = seen + 1 end end\n"
    "ok('list entries', seen == 151, seen)\n"
    "ok('remove notempty', select(2, file.remove('sd:/x/y')) == 'notempty')\n"
    "ok('remove dir', file.remove('sd:/x/y/z') and file.remove('sd:/x/y'))\n"
    "ok('rename eexist', select(2, file.rename('sd:/x/many_a_rather_long_name_001.dat', 'sd:/x/many_a_rather_long_name_002.dat')) == 'eexist')\n"
    "ok('rename', file.rename('sd:/x/many_a_rather_long_name_001.dat', 'sd:/moved/one.dat') and file.stat('sd:/moved/one.dat').size == 1)\n"
    "ok('rename across volumes', select(2, file.rename('sd:/moved/one.dat', 'lfs:/one.dat')) == 'badpath')\n"
    "local hs = {}\n"
    "for i = 1, 4 do hs[i] = file.open('sd:/fw/v1/img.bin', 'r') end\n"
    "ok('four handles', hs[4] ~= nil)\n"
    "ok('a fifth: nohandle', select(2, file.open('sd:/fw/v1/img.bin', 'r')) == 'nohandle')\n"
    "for i = 1, 4 do hs[i]:close() end\n"
    "do local c <close> = file.open('sd:/fw/v1/img.bin', 'r'); ok('<close> handle reads', #c:read(10) == 10) end\n"
    "ok('after <close>, a handle is free again', file.open('sd:/fw/v1/img.bin', 'r') ~= nil)\n"
    "collectgarbage(); collectgarbage()\n";

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
    lua_State *L = luaL_newstate();
    luaL_openlibs(L);
    lfile_open(L);
    lua_newtable(L);
    lua_setglobal(L, "R");

    int y = run(L, SCRIPT, 200000, false, NULL);
    CHECK(y > 0, "the script ran (and yielded for the card)");
    collect(L);
    lfile_release(0);
    for (int i = 0; i < 50; i++) { pass(); }

    /* From a handler (not a coroutine): SD calls refuse, LittleFS calls do
     * not need to yield. */
    {
        int rc = luaL_dostring(L, "return file.open('sd:/fw/v1/img.bin', 'r')");
        CHECK(rc != LUA_OK && strstr(lua_tostring(L, -1), "script's own code"), "SD from a handler raises");
        lua_settop(L, 0);
        rc = luaL_dostring(L, "local h, e = file.open('lfs:/x.bin', 'r'); return e");
        CHECK(rc == LUA_OK && strcmp(lua_tostring(L, -1), "enoent") == 0, "LittleFS from a handler answers");
        lua_settop(L, 0);
    }

    /* A script stopped mid-job: the job finishes alone, and every handle and
     * the borrowed ring come back. */
    {
        lua_State *co = NULL;
        run(L, "local h = file.open('sd:/fw/v1/img.bin', 'r'); while true do h:read(4096); h:seek(0) end", 7, true, &co);
        lfile_release(0);                    /* what teardown does */
        for (int i = 0; i < 100; i++) { pass(); }
        CHECK(sd_xfer_active() == 0, "no transfer ring held after the stop");
        int y2 = run(L,
            "local hs = {} for i = 1, 4 do hs[i] = file.open('sd:/fw/v1/img.bin', 'r') end\n"
            "R[#R+1] = { name = 'all four handles free after a stop', ok = hs[4] ~= nil }\n"
            "for i = 1, 4 do if hs[i] then hs[i]:close() end end", 100000, false, NULL);
        CHECK(y2 > 0, "second script ran");
        collect(L);
        lfile_release(0);
    }

    /* The card pulled under an open handle: eio, not someone else's file. */
    {
        lua_State *co = NULL;
        run(L, "H = file.open('sd:/fw/v1/img.bin', 'r')", 1000, false, &co);
        sdsim_pull(&card);
        for (int i = 0; i < 50; i++) { pass(); }
        sdsim_insert(&card);
        for (int i = 0; i < 5000 && strcmp(sd_card_state(), "ok") != 0; i++) { pass(); }
        run(L, "local c, e = H:read(10); R[#R+1] = { name = 'a handle from before the pull is eio', ok = c == nil and e == 'eio', extra = e }\n"
               "R[#R+1] = { name = 'the card works after it', ok = file.stat('sd:/fw/v1/img.bin') ~= nil }", 100000, false, NULL);
        collect(L);
        lfile_release(0);
    }

    lfile_reset();
    lua_close(L);
    sdsim_free(&card);
    printf("lfile sd: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
