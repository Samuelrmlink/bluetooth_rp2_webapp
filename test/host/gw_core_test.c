/*
 * src/net/gw_core.c: gateway LAN config, link-local beside a lease,
 * advertisements, the 6 s drop, and which iface NAT may use.
 *
 *   gcc -Isrc test/host/gw_core_test.c src/net/gw_core.c
 */
#include <stdio.h>
#include <string.h>

#include "net/gw_core.h"

static int checks, failed;
#define CHECK(c, msg) do { checks++; if (!(c)) { failed++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, msg); } } while (0)

static void load(const char *json, bool w5500, char *iface, size_t cap) {
    memset(iface, 0x5a, cap);
    gw_cfg_load(json, w5500, iface, cap);
}

int main(void) {
    char iface[8];
    const char *why = "untouched";

    load(NULL, true, iface, sizeof iface);
    CHECK(strcmp(iface, "e0") == 0, "missing cfg/gw.json with W5500 -> e0");
    load(NULL, false, iface, sizeof iface);
    CHECK(strcmp(iface, "none") == 0, "missing cfg/gw.json without W5500 -> none");
    load("", true, iface, sizeof iface);
    CHECK(strcmp(iface, "e0") == 0, "empty file -> e0");
    load("{", true, iface, sizeof iface);
    CHECK(strcmp(iface, "e0") == 0, "corrupt file -> e0");
    load("{\"iface\":\"none\"}", true, iface, sizeof iface);
    CHECK(strcmp(iface, "none") == 0, "stored none stays none");
    load("{\"iface\":\"none\"}", false, iface, sizeof iface);
    CHECK(strcmp(iface, "none") == 0, "stored none stays none without W5500");
    load("{\"iface\":\"nope\"}", true, iface, sizeof iface);
    CHECK(strcmp(iface, "e0") == 0, "unknown iface falls back on load");
    load("{\"iface\":\"e0\"}", false, iface, sizeof iface);
    CHECK(strcmp(iface, "e0") == 0, "explicit e0 is kept");

    CHECK(!gw_cfg_parse("{\"iface\":\"nope\"}", iface, sizeof iface, &why),
          "unknown iface rejected");
    CHECK(why && strstr(why, "iface"), "strict parse names the field");
    CHECK(gw_cfg_parse("{\"iface\":\"w0\"}", iface, sizeof iface, &why) &&
          strcmp(iface, "w0") == 0, "w0 is a carrier");
    CHECK(!gw_cfg_parse("{\"iface\":\"el\"}", iface, sizeof iface, &why),
          "companion is not a carrier");
    CHECK(!gw_cfg_parse("{\"iface\":\"wg\"}", iface, sizeof iface, &why),
          "tunnel is not a carrier");

    CHECK(gw_ll_kept(true, true), "link-local kept across a lease when the feature is on");
    CHECK(gw_ll_kept(true, false), "link-local kept with no lease when the feature is on");
    CHECK(!gw_ll_kept(false, true), "a lease replaces link-local when the feature is off");
    CHECK(gw_ll_kept(false, false), "no lease still uses link-local when the feature is off");

    CHECK(!gw_advertise(true, true), "the DHCP router on the gateway LAN is not advertised");
    CHECK(!gw_advertise(false, false), "a connection with no gateway is not advertised");
    CHECK(gw_advertise(true, false), "Wi-Fi with a gateway is advertised");

    char inst[80], txt_if[32], txt_m[32], host[64], ifc[4];
    CHECK(gw_instance_name("lab", "w0", inst, sizeof inst) && strcmp(inst, "lab-w0") == 0,
          "instance is hostname-iface");
    gw_ad_txt("w0", 12, txt_if, sizeof txt_if, txt_m, sizeof txt_m);
    CHECK(strcmp(txt_if, "iface=w0") == 0 && strcmp(txt_m, "metric=12") == 0,
          "Wi-Fi instance TXT carries iface and upl_metric");
    CHECK(gw_split_instance("bluetooth-rp2-webapp-w0", host, sizeof host, ifc, sizeof ifc) &&
          strcmp(host, "bluetooth-rp2-webapp") == 0 && strcmp(ifc, "w0") == 0,
          "split on the last hyphen");
    CHECK(!gw_instance_name("lab", "w0", inst, 4), "instance that does not fit is refused");

    uint8_t id[2] = { 0xab, 0x0c };
    char hn[40];
    gw_default_hostname("RP2", id, 2, hn, sizeof hn);
    CHECK(strcmp(hn, "RP2-ab0c") == 0, "RP2 serial formula");
    gw_default_hostname("ESP32C5", id, 2, hn, sizeof hn);
    CHECK(strcmp(hn, "ESP32C5-ab0c") == 0, "C5 serial formula");
    CHECK(gw_hostname_parse("{\"name\":\"Lab-1\"}", hn, sizeof hn, &why) &&
          strcmp(hn, "Lab-1") == 0, "stored hostname");
    CHECK(!gw_hostname_parse("{}", hn, sizeof hn, &why), "missing name is not the formula");
    CHECK(!gw_hostname_parse("{\"name\":\"-no\"}", hn, sizeof hn, &why), "label cannot start with a hyphen");

    gw_table_t t;
    gw_table_init(&t);
    CHECK(gw_peer_note(&t, "lab", "w0", 12, "169.254.1.2", 0), "note wifi");
    CHECK(gw_peer_note(&t, "lab", "b0", 40, "169.254.1.2", 0), "note nap on the same board");
    CHECK(gw_peer_expire(&t, 5999) == 0, "silence under 6 s keeps both");
    char best_if[4];
    int32_t metric = -1;
    CHECK(gw_peer_best(&t, 5999, host, sizeof host, best_if, sizeof best_if, &metric, hn, sizeof hn) &&
          strcmp(host, "lab") == 0 && strcmp(best_if, "w0") == 0 && metric == 12 &&
          strcmp(hn, "169.254.1.2") == 0, "best connection is the lower metric");

    gw_table_init(&t);
    CHECK(gw_peer_note(&t, "lab", "w0", 12, "169.254.1.2", 0), "wifi again");
    CHECK(gw_peer_note(&t, "lab", "b0", 40, "169.254.1.9", 5000), "nap refreshed later");
    CHECK(gw_peer_expire(&t, 6000) == 1, "6 s of silence drops that one instance");
    CHECK(gw_peer_best(&t, 6000, NULL, 0, best_if, sizeof best_if, &metric, hn, sizeof hn) &&
          strcmp(best_if, "b0") == 0 && metric == 40, "the other connection stays");
    CHECK(gw_peer_touch(&t, "lab", "b0", 6000), "a fresh advertisement refreshes one connection");
    CHECK(!gw_peer_touch(&t, "lab", "w0", 6000), "a dropped connection is not refreshed into existence");

    CHECK(!gw_nat_out_ok("e0", "e0"), "NAT is not sent back onto the gateway LAN");
    CHECK(!gw_nat_out_ok("el", "e0"), "NAT is not sent out the link-local companion");
    CHECK(!gw_nat_out_ok("wg", "e0"), "NAT is not sent out WireGuard");
    CHECK(!gw_nat_out_ok("", "e0"), "NAT needs an egress iface");
    CHECK(!gw_nat_out_ok(NULL, "e0"), "NAT needs an egress iface");
    CHECK(gw_nat_out_ok("w0", "e0"), "Wi-Fi is a masquerade egress");
    CHECK(gw_nat_out_ok("us", "e0"), "USB-NCM is a masquerade egress when it is the out iface");
    CHECK(!gw_nat_out_ok("b0", "b0"), "the carrier itself is never the egress");

    printf("%d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
