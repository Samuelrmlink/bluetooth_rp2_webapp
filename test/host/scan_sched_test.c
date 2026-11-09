/* The PEX scan scheduler (src/pex/pex_scan_sched.c) against a fake radio,
 * clock, and module table: priorities, one request per module, scans that
 * outlive a session, the quiet slot's policy (10-minute cap, one holder,
 * the 25 s guard), and unload. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "pex_rt.h"
#include "bt/le/central.h"
#include "pico/time.h"
#include "pex/pex_scan_sched.h"

static int failures, checks;
#define CHECK(c, msg) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, msg); } } while (0)

/* ---- the clock ---- */
static uint64_t g_us;
absolute_time_t get_absolute_time(void) { return g_us; }
static void at_ms(uint32_t ms) { g_us = (uint64_t)ms * 1000u; }

/* ---- the radio (central.c's scan calls) ---- */
static bool g_scanning, g_busy, g_refuse, g_active;
static uint32_t g_gen, g_starts, g_stops;
bool le_scan_start(const le_scan_filter_t *f, uint32_t duration_ms) {
    (void)duration_ms;
    if (g_refuse) { return false; }
    g_scanning = true;
    g_active = f->active;
    g_gen++;
    g_starts++;
    return true;
}
void le_scan_stop(void) { if (g_scanning) { g_scanning = false; g_stops++; } }
bool le_scanning(void) { return g_scanning; }
uint32_t le_scan_gen(void) { return g_gen; }
bool le_bringup_busy(void) { return g_busy; }
/* Someone else (the web page) starts a scan: a new session. */
static void external_scan(void) { g_scanning = true; g_gen++; }
/* That session ends (a timeout, scan.stop, WiFi). */
static void session_ends(void) { g_scanning = false; }

/* ---- modules ---- */
struct pex_mod { const char *name; };
static struct pex_mod m_iti = { "iti" }, m_anker = { "anker" }, m_other = { "other" };
static pex_mod_t *g_cur;
pex_mod_t *pex_self(void) { return g_cur; }
int pex_info(pex_mod_t *m, pex_info_t *out) {
    memset(out, 0, sizeof *out);
    out->name = ((struct pex_mod *)m)->name;
    return PEX_OK;
}
static void as(struct pex_mod *m) { g_cur = (pex_mod_t *)m; }
static void tick(void) { pex_scan_sched_apply(); }

static void reset_all(void) {
    /* Drop every request and slot the last test left. */
    pex_scan_sched_unload((pex_mod_t *)&m_iti);
    pex_scan_sched_unload((pex_mod_t *)&m_anker);
    pex_scan_sched_unload((pex_mod_t *)&m_other);
    g_us += 60u * 60u * 1000000u;       /* past any guard window */
    tick();
    g_scanning = g_busy = g_refuse = false;
    g_starts = g_stops = 0;
}

static void test_high(void) {
    reset_all();
    at_ms(1000000);
    as(&m_iti);
    CHECK(pex_bt_scan_state() == PEX_BT_SCAN_NONE, "no request: NONE");
    CHECK(pex_bt_scan(0, 1) == PEX_OK, "high scan accepted");
    CHECK(g_scanning && g_active, "high request starts an active scan at once");
    CHECK(pex_bt_scan_state() == PEX_BT_SCAN_ON, "state ON");
    as(&m_anker);
    pex_bt_scan(0, 1);
    as(&m_iti);
    pex_bt_scan_stop();
    CHECK(g_scanning, "one module's stop leaves another module's scan running");
    as(&m_anker);
    pex_bt_scan_stop();
    CHECK(!g_scanning, "the last request's stop ends the scan");
    /* A request outlives its session. */
    as(&m_iti);
    pex_bt_scan(0, 1);
    session_ends();
    tick();
    CHECK(g_scanning, "a request still wanting a scan restarts it after the session ends");
    /* A scan the web page started is not a module's to stop. */
    pex_bt_scan_stop();
    external_scan();
    as(&m_anker);
    pex_bt_scan(0, 1);
    pex_bt_scan_stop();
    CHECK(g_scanning, "a module's stop does not end a scan the web page started");
    session_ends();
    /* WiFi refusing the start: the next poll asks again. */
    pex_bt_scan(0, 1);
    g_refuse = true;
    session_ends();
    tick();
    CHECK(!g_scanning, "refused while WiFi holds the radio");
    g_refuse = false;
    tick();
    CHECK(g_scanning, "started on the next poll once WiFi lets go");
    pex_bt_scan_stop();
    /* A duration ends the request. */
    at_ms(2000000);
    pex_bt_scan(5000, 0);
    CHECK(g_scanning && !g_active, "passive timed request");
    at_ms(2004000); tick();
    CHECK(g_scanning, "still within its duration");
    at_ms(2005001); tick();
    CHECK(!g_scanning && pex_bt_scan_state() == PEX_BT_SCAN_NONE, "its duration over: request gone, scan stopped");
    /* Bring-up holds the radio: BLOCKED. */
    pex_bt_scan(0, 1);
    g_busy = true;
    CHECK(pex_bt_scan_state() == PEX_BT_SCAN_BLOCKED, "BLOCKED during a connect or discovery");
    g_busy = false;
    pex_bt_scan_stop();
}

static void test_low(void) {
    reset_all();
    at_ms(3000000);
    as(&m_anker);
    CHECK(pex_bt_scan_periodic(25000, 60000, 1) == PEX_OK, "periodic accepted");
    CHECK(g_scanning, "a new periodic request bursts at once");
    CHECK(pex_bt_scan_state() == PEX_BT_SCAN_ON, "ON during the burst");
    at_ms(3010000);
    pex_bt_scan_periodic(25000, 60000, 1);           /* the module's tick re-asserting */
    at_ms(3025001); tick();
    CHECK(!g_scanning, "the burst ends after 25 s (re-asserting kept the schedule)");
    CHECK(pex_bt_scan_state() == PEX_BT_SCAN_WAITING, "WAITING between bursts");
    at_ms(3059000); tick();
    CHECK(!g_scanning, "no scan before the period is up");
    at_ms(3060000); tick();
    CHECK(g_scanning, "the next burst a period after the last one started");
    pex_bt_scan_stop();
    CHECK(!g_scanning, "stop ends the burst");
    /* Clamps: a burst is at most 30 s, the period at least 10 s and burst + 5 s. */
    at_ms(4000000);
    pex_bt_scan_periodic(45000, 1000, 1);
    at_ms(4030001); tick();
    CHECK(!g_scanning, "burst clamped to 30 s");
    at_ms(4034000); tick();
    CHECK(!g_scanning, "period clamped to at least burst + 5 s");
    at_ms(4035000); tick();
    CHECK(g_scanning, "next burst at 35 s");
    /* One request per module: a high request replaces the periodic one. */
    pex_bt_scan(0, 1);
    at_ms(4100000); tick();
    CHECK(g_scanning, "the module's high request replaced its periodic one");
    pex_bt_scan_stop();
    CHECK(!g_scanning && pex_bt_scan_state() == PEX_BT_SCAN_NONE, "one stop drops the module's only request");
}

static void test_quiet(void) {
    reset_all();
    at_ms(5000000);
    as(&m_anker);
    pex_bt_scan_periodic(25000, 60000, 1);
    as(&m_iti);
    CHECK(pex_bt_quiet(0) == PEX_EINVAL, "a zero-length slot is refused");
    int id = pex_bt_quiet(300000);
    CHECK(id > 0, "quiet slot granted");
    CHECK(!g_scanning, "anker's burst is held off by iti's slot");
    as(&m_anker);
    CHECK(pex_bt_scan_state() == PEX_BT_SCAN_QUIET, "anker sees QUIET");
    CHECK(pex_bt_quiet(1000) == PEX_EBUSY, "a second module's slot is refused while one is held");
    CHECK(pex_bt_quiet_left(id) > 0, "anyone can ask what is left");
    CHECK(pex_bt_quiet_release(id) == PEX_ENOENT, "only the holder releases it");
    as(&m_iti);
    pex_bt_scan(0, 1);
    CHECK(g_scanning, "the holder's own scan runs during its slot");
    pex_bt_scan_stop();
    CHECK(!g_scanning, "and stops when the holder is done with it");
    /* Extend is capped at 10 minutes from the grant. */
    at_ms(5200000);                                  /* granted at 5000 s for 300 s: still live */
    int left = pex_bt_quiet_extend(id, 600000);
    CHECK(left == 400000, "extend is capped at 10 minutes from the grant");
    CHECK(pex_bt_quiet(600000) == id, "the holder asking again extends the same slot");
    at_ms(5599999); tick();
    as(&m_anker);
    CHECK(pex_bt_scan_state() == PEX_BT_SCAN_QUIET, "still quiet just before the cap");
    at_ms(5600000); tick();
    CHECK(g_scanning, "the slot ends at the cap and anker gets its burst at once");
    as(&m_iti);
    CHECK(pex_bt_quiet_left(id) == PEX_ENOENT, "the holder learns the slot ended");
    CHECK(pex_bt_quiet(60000) == PEX_EAGAIN, "no new slot during the 25 s guard");
    at_ms(5624999); tick();
    CHECK(g_scanning, "anker scans through the guard (at least 25 s)");
    at_ms(5625000); tick();
    CHECK(!g_scanning, "the guard burst is over");
    int id2 = pex_bt_quiet(60000);
    CHECK(id2 > 0 && id2 != id, "a new slot after the guard, with a new id");
    /* Releasing early hands the low-priority requests their burst at once. */
    at_ms(5630000);
    CHECK(pex_bt_quiet_release(id2) == PEX_OK, "the holder releases early");
    CHECK(g_scanning, "anker scans right after the release");
    CHECK(pex_bt_quiet_extend(id2, 1000) == PEX_ENOENT, "a released slot cannot be extended");
    /* Revoke (pex.radio_revoke) and unload. */
    at_ms(5700000); tick();
    int id3 = pex_bt_quiet(60000);
    CHECK(id3 > 0, "slot granted again");
    CHECK(pex_gw_bt_quiet_revoke() == PEX_OK, "revoked from the web page");
    CHECK(pex_bt_quiet_left(id3) == PEX_ENOENT, "the holder sees it revoked");
    CHECK(pex_gw_bt_quiet_revoke() == PEX_ENOENT, "nothing left to revoke");
    at_ms(5800000); tick();
    int id4 = pex_bt_quiet(60000);
    CHECK(id4 > 0, "slot granted");
    pex_bt_scan(0, 1);
    pex_scan_sched_unload((pex_mod_t *)&m_iti);
    CHECK(pex_bt_quiet_left(id4) == PEX_ENOENT, "unloading the holder ends its slot");
    as(&m_anker);
    CHECK(pex_bt_scan_state() != PEX_BT_SCAN_QUIET, "anker is no longer held off");
    pex_bt_scan_stop();
    /* The schedule as JSON. */
    as(&m_other);
    pex_bt_scan_periodic(10000, 60000, 0);
    char buf[900];
    int n = pex_gw_bt_sched_json(buf, sizeof buf);
    CHECK(n > 0 && strstr(buf, "\"module\":\"other\"") && strstr(buf, "\"prio\":\"low\""), "pex.radio lists the request");
    CHECK(pex_gw_bt_sched_json(buf, 20) < 0, "a short buffer is refused, not overrun");
    pex_bt_scan_stop();
}

/* The guard is owed to other modules' scans: with nobody else waiting, the
 * same module's next job gets a slot at once. */
static void test_guard_owed(void) {
    reset_all();
    at_ms(7000000);
    as(&m_iti);
    int id = pex_bt_quiet(60000);
    CHECK(id > 0, "slot granted with nobody else scanning");
    CHECK(pex_bt_quiet_release(id) == PEX_OK, "released");
    int id2 = pex_bt_quiet(60000);
    CHECK(id2 > 0, "no guard when no other module wants a scan");
    /* the holder's own scan request does not create a guard either */
    pex_bt_scan(0, 1);
    pex_bt_quiet_release(id2);
    CHECK(pex_bt_quiet(60000) > 0, "the holder's own request is not owed a guard");
    pex_bt_scan_stop();
    pex_scan_sched_unload((pex_mod_t *)&m_iti);
    /* anker waiting: guard holds; anker giving up its request ends it early */
    at_ms(7100000); tick();
    as(&m_anker);
    pex_bt_scan_periodic(25000, 60000, 1);
    as(&m_iti);
    id = pex_bt_quiet(60000);
    pex_bt_quiet_release(id);
    CHECK(pex_bt_quiet(60000) == PEX_EAGAIN, "guard while anker is owed its burst");
    as(&m_anker);
    pex_bt_scan_stop();
    as(&m_iti);
    CHECK(pex_bt_quiet(60000) > 0, "guard ends once anker no longer wants a scan");
}

int main(void) {
    test_guard_owed();
    test_high();
    test_low();
    test_quiet();
    printf("scan_sched: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
