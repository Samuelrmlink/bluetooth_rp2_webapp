/*
 * src/net/eth_cfg.c: Ethernet addressing config. The owner's rules under
 * test: a missing, corrupt or unknown config is the DHCP CLIENT, never the
 * DHCP server; server mode only from an explicit {"mode":"server"}; and
 * link-local is added only after ETH_LL_AFTER_MS without a lease.
 *
 *   gcc -Isrc test/host/eth_cfg_test.c src/net/eth_cfg.c
 */
#include <stdio.h>
#include <string.h>

#include "net/eth_cfg.h"

static int checks, failed;
#define CHECK(c, msg) do { checks++; if (!(c)) { failed++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, msg); } } while (0)

static eth_mode_t loaded(const char *json) {
    eth_cfg_t c;
    memset(&c, 0xAA, sizeof c);           /* garbage in: load must not keep any */
    eth_cfg_load(json, &c);
    return c.mode;
}

int main(void) {
    const char *why = NULL;
    eth_cfg_t c;

    /* --- loading the file: anything wrong is the DHCP client ---------------- */
    CHECK(loaded(NULL) == ETH_MODE_DHCP, "no file -> dhcp");
    CHECK(loaded("") == ETH_MODE_DHCP, "empty file -> dhcp");
    CHECK(loaded("{garbage") == ETH_MODE_DHCP, "corrupt -> dhcp");
    CHECK(loaded("{\"mode\":\"Server\"}") == ETH_MODE_DHCP, "unknown mode (case) -> dhcp");
    CHECK(loaded("{\"mode\":\"dhcp-server\"}") == ETH_MODE_DHCP, "unknown mode -> dhcp");
    CHECK(loaded("{\"mode\":\"servers\"}") == ETH_MODE_DHCP, "near-miss mode -> dhcp");
    CHECK(loaded("{\"mode\":\"server\",\"mask\":\"255.0.255.0\"}") == ETH_MODE_DHCP, "invalid server config -> dhcp, not server");
    CHECK(loaded("{\"mode\":\"manual\"}") == ETH_MODE_DHCP, "incomplete manual -> dhcp");
    CHECK(loaded("{\"mode\":7}") == ETH_MODE_DHCP, "non-string mode -> dhcp");
    CHECK(loaded("{\"ip\":\"10.0.0.1\"}") == ETH_MODE_DHCP, "no mode -> dhcp");
    CHECK(loaded("{\"dhcp_router\":true}") == ETH_MODE_DHCP, "router flag alone does not imply server");
    CHECK(loaded("{\"mode\":\"server\"}") == ETH_MODE_SERVER, "an explicit, valid server config is honoured");

    /* --- the default ----------------------------------------------------------- */
    eth_cfg_default(&c);
    CHECK(c.mode == ETH_MODE_DHCP && !c.dhcp_router, "default: DHCP client, no router");

    /* --- server mode -------------------------------------------------------------- */
    CHECK(eth_cfg_parse("{\"mode\":\"server\"}", &c, &why), "server with defaults");
    CHECK(c.ip[0] == 192 && c.ip[1] == 168 && c.ip[2] == 9 && c.ip[3] == 1, "default 192.168.9.1");
    CHECK(c.mask[3] == 0 && c.mask[2] == 255, "default /24");
    CHECK(!c.dhcp_router, "no router option by default");
    CHECK(eth_cfg_parse("{\"mode\":\"server\",\"ip\":\"10.9.0.1\",\"mask\":\"255.255.0.0\",\"gw\":\"10.9.0.254\",\"dhcp_router\":true}", &c, &why),
          "server, editable");
    CHECK(c.ip[0] == 10 && c.dhcp_router && !(c.gw[0] | c.gw[1] | c.gw[2] | c.gw[3]), "gw dropped: e0 is no uplink in server mode");
    CHECK(!eth_cfg_parse("{\"mode\":\"server\",\"ip\":\"192.168.9.17\"}", &c, &why) && strstr(why, "lease range"),
          "server address inside the lease range refused");
    CHECK(!eth_cfg_parse("{\"mode\":\"server\",\"mask\":\"255.255.255.240\"}", &c, &why), "server subnet smaller than /24 refused");

    /* --- manual / dhcp -------------------------------------------------------------- */
    CHECK(eth_cfg_parse("{\"mode\":\"manual\",\"ip\":\"192.168.50.2\",\"mask\":\"255.255.255.0\",\"gw\":\"192.168.50.1\"}", &c, &why) &&
          c.mode == ETH_MODE_MANUAL, "manual");
    CHECK(!eth_cfg_parse("{\"mode\":\"manual\",\"ip\":\"192.168.50.2\",\"mask\":\"255.255.255.0\",\"gw\":\"10.0.0.1\"}", &c, &why),
          "manual gw off-subnet refused");
    eth_cfg_t keep = c;
    CHECK(!eth_cfg_parse("{\"mode\":\"bogus\"}", &c, &why) && memcmp(&keep, &c, sizeof c) == 0,
          "a refused config leaves the old one untouched");
    CHECK(eth_cfg_parse("{\"mode\":\"dhcp\"}", &c, &why) && c.mode == ETH_MODE_DHCP, "dhcp");
    CHECK(!eth_cfg_parse("{\"mode\":\"dhcp\",\"ip\":\"1.2.3\"}", &c, &why), "bad address field refused even in dhcp");

    /* --- round trip ------------------------------------------------------------------- */
    char js[200];
    eth_cfg_parse("{\"mode\":\"server\",\"ip\":\"192.168.9.1\",\"dhcp_router\":true}", &c, &why);
    eth_cfg_json(&c, js, sizeof js);
    eth_cfg_t back;
    eth_cfg_load(js, &back);
    CHECK(back.mode == ETH_MODE_SERVER && back.dhcp_router && back.ip[3] == 1, "server config survives save/load");
    eth_cfg_default(&c);
    eth_cfg_json(&c, js, sizeof js);
    eth_cfg_load(js, &back);
    CHECK(back.mode == ETH_MODE_DHCP, "dhcp survives save/load");

    /* --- link-local timing --------------------------------------------------------------- */
    CHECK(!eth_ll_due(ETH_MODE_DHCP, true, false, false, 1000, 1000 + ETH_LL_AFTER_MS - 1), "DHCP alone first");
    CHECK(eth_ll_due(ETH_MODE_DHCP, true, false, false, 1000, 1000 + ETH_LL_AFTER_MS), "then link-local");
    CHECK(!eth_ll_due(ETH_MODE_DHCP, true, true, false, 0, 100000), "not with a lease");
    CHECK(!eth_ll_due(ETH_MODE_DHCP, false, false, false, 0, 100000), "not without link");
    CHECK(!eth_ll_due(ETH_MODE_DHCP, true, false, true, 0, 100000), "not twice");
    CHECK(!eth_ll_due(ETH_MODE_MANUAL, true, false, false, 0, 100000), "never in manual");
    CHECK(!eth_ll_due(ETH_MODE_SERVER, true, false, false, 0, 100000), "never in server mode");
    CHECK(eth_ll_due(ETH_MODE_DHCP, true, false, false, 0xFFFFF000u, 0xFFFFF000u + ETH_LL_AFTER_MS), "across the 32-bit wrap");

    printf("%d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
