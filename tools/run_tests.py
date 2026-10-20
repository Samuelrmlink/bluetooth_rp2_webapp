#!/usr/bin/env python3
"""Tiered test runner. Tiers 0-2 need no board; 3-4 do.

  tools/run_tests.py            # tier 0 only, the every-save default
  tools/run_tests.py --tier 2   # everything that needs no hardware
  tools/run_tests.py --tier 3   # adds the board

The tiering exists so the common case stays fast: a suite that is slow does not
get run, and a suite that does not get run is not a regression guard.
"""
import argparse, os, shutil, subprocess, sys, re, time
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FAILS = []

def step(tier, name, fn):
    print(f"[tier {tier}] {name} ... ", end="", flush=True)
    try:
        ok, detail = fn()
    except Exception as e:
        ok, detail = False, str(e)
    print("OK" + (f"  ({detail})" if detail else "") if ok else f"FAIL  {detail}")
    if not ok:
        FAILS.append(name)

ANSI = re.compile(r"\x1b\[[0-9;]*m")

def sh(cmd, cwd=ROOT, env_extra=""):
    # node --test colourises its summary, which quietly defeats any regex over
    # it, so strip escapes here rather than at every call site.
    p = subprocess.run(env_extra + cmd, shell=True, cwd=cwd, capture_output=True, text=True)
    return p.returncode, ANSI.sub("", p.stdout + p.stderr)

# --- tier 0 -------------------------------------------------------------------
def t_unit():
    rc, out = sh('node --test "tests/unit/**/*.test.js"', cwd=os.path.join(ROOT, "web"))
    m = re.search(r"^. pass (\d+)", out, re.M)
    f = re.search(r"^. fail (\d+)", out, re.M)
    npass = int(m.group(1)) if m else 0
    nfail = int(f.group(1)) if f else -1
    return (rc == 0 and nfail == 0), f"{npass} passed"

def _grep(pattern, paths):
    """True when the pattern is ABSENT. Note the exit status is grep's own --
    piping into head would make it head's, which is always 0, and the check
    would then pass or fail at random."""
    rc, out = sh(f"grep -rnE {pattern!r} {' '.join(paths)} 2>/dev/null")
    hits = [l for l in out.splitlines() if l.strip()]
    return (len(hits) == 0), (hits[0][:90] if hits else "")

def t_no_device_in_core():
    # src/lua/lbind_proto.c is excluded here and checked more strictly below.
    # It is the bridge to src/codec + src/crypto, so it cannot avoid naming
    # them in an #include; a blanket exemption would be too generous, so it
    # gets its own tighter rule instead.
    rc, out = sh("grep -rnE 'anker|8c8500|0x0840' "
                 "src/bt/ src/ops/ src/api/ src/lua/ src/io/ 2>/dev/null")
    hits = [l for l in out.splitlines()
            if l.strip() and not l.startswith("src/lua/lbind_proto.c:")]
    return (len(hits) == 0), (hits[0][:90] if hits else "")

def t_proto_binding_is_thin():
    """The protocol binding may NAME the codec it wraps; it may not know the
    protocol. Anything Anker-ish outside an #include line is device knowledge
    that belongs in lua/."""
    rc, out = sh("grep -nE 'anker|8c8500|0x0840' src/lua/lbind_proto.c 2>/dev/null")
    hits = [l for l in out.splitlines()
            if l.strip() and "#include" not in l and "ANKER_REASM_MAX" not in l
            and "anker_tlv_item_t" not in l and "anker_tlv_spec_t" not in l
            and "anker_enc_t" not in l and "anker_frame_t" not in l
            and "anker_reasm_t" not in l and "ANKER_ENC_" not in l
            and "ANKER_TLV_" not in l and "CONFIG_PROTO_ANKER" not in l
            # Calls INTO the codec are the whole job of a wrapper. kdf and
            # derive_account_key joined the list with the C1000; they are
            # entry points, not protocol knowledge living in the binding.
            and not re.search(
                r"\banker_(frame|tlv|reasm|pubkey|ecdh|aes|kdf|derive_account_key)\w*\(", l)]
    return (len(hits) == 0), (hits[0][:90] if hits else "")

def t_neg_constants_agree():
    """The negotiation keystream lives in the Lua profile; the C parity test
    keeps a fixture copy so it can prove the generalised GHASH still matches
    the tags the Anker-specific original produced. Two copies can drift, and a
    drift here would be invisible until a charger stopped answering, so assert
    they are identical.

    Repointed at lua/profiles/anker/cipher.lua when the profile was split into
    modules. A guard that quietly stops matching anything is worse than no
    guard, so the missing-file case below fails rather than skips."""
    import pathlib
    fix = pathlib.Path(ROOT, "test/host/neg_fixture.h")
    prof = pathlib.Path(ROOT, "lua/profiles/anker/cipher.lua")
    if not fix.exists() or not prof.exists():
        return False, "fixture or profile missing"
    ftxt, ptxt = fix.read_text(), prof.read_text()

    def c_hex(name):
        m = re.search(re.escape(name) + r"\s*\[[^\]]*\]\s*=\s*\{(.*?)\};", ftxt, re.S)
        if not m:
            return None
        return "".join(b.lower() for b in re.findall(r"0x([0-9a-fA-F]{2})", m.group(1)))

    def lua_hex(name):
        # The value may be several quoted chunks joined with '..'.
        m = re.search(r"\b" + re.escape(name) + r"\s*=\s*bytes\.unhex\((.*?)\)\n", ptxt, re.S)
        if not m:
            return None
        return "".join(re.findall(r"'([0-9a-fA-F]*)'", m.group(1))).lower()

    for cname, lname in (("NEG_KEYSTREAM", "KEYSTREAM"), ("NEG_GHASH_H", "GHASH_H"),
                         ("NEG_EJ0", "EJ0"), ("ANKER_GCM_AAD", "AAD")):
        a, b = c_hex(cname), lua_hex(lname)
        if not a or not b:
            return False, f"could not read {cname} / {lname}"
        if a != b:
            return False, f"{cname} != the profile's {lname}"
    return True, "keystream, H, E(J0), AAD"

def t_wireguard_is_never_default():
    """The tunnel must never become lwIP's default route.

    This is the whole safety property of the feature: WireGuard is an
    ADDITIONAL way in, and a remote-access path that can strand the device
    when it misbehaves is worse than no remote access. A single
    netif_set_default() on the tunnel netif would undo that, quietly, and
    the symptom would be a device that is unreachable exactly when the
    tunnel is broken -- which is when you need to reach it.

    The firmware also reports is_default_route in wg.status so a monitor can
    assert it at runtime; this catches it at build time."""
    rc, out = sh("grep -n 'netif_set_default' src/net/wg.c 2>/dev/null")
    hits = [l for l in out.splitlines() if l.strip() and "DELIBERATELY NOT" not in l]
    if hits:
        return False, f"wg.c calls netif_set_default: {hits[0][:70]}"
    # And the status must keep reporting it, so the runtime check cannot be
    # dropped without this failing too.
    rc, out = sh("grep -c 'is_default_route' src/net/wg.c 2>/dev/null")
    if not out.strip() or int(out.strip() or 0) < 1:
        return False, "wg.status no longer reports is_default_route"
    return True, "tunnel is additive, and says so at runtime"

def _fn_body(src, name):
    """The text of C function `name` in `src`, from its signature to the first
    line that is exactly '}'. Enough for a grep guard; not a parser."""
    i = src.find(f"static void {name}(")
    if i < 0: i = src.find(f"void {name}(")
    if i < 0: return None
    j = src.find("\n}\n", i)
    return src[i:j] if j > 0 else src[i:]

def t_sd_guards():
    """The SD card's standing rules, as greps:
    - its code runs only in the storage worker, resumed from the main loop:
      never from cyw43_wait.c (the radio's wait hook pumps USB only);
    - the SPI transport ends every transaction with spibus_release_trailing
      (the card drives MISO until a clock with CS high) and never releases
      the bus any other way;
    - its pins are reserved in the pin broker (SDIO lines and card detect);
    - no carlk3 / ZuluSCSI SDIO source in this MIT tree (GPL-3.0; it may
      only ever be fetched into a test build, test/sdio_diff)."""
    w = open(os.path.join(ROOT, "src/net/cyw43_wait.c")).read()
    if re.search(r"\bsd_(poll|core|card|api)|sdw_|sdio_", w):
        return False, "cyw43_wait.c reaches SD code"
    spi = open(os.path.join(ROOT, "src/sd/sd_spi.c")).read()
    if "spibus_release_trailing(" not in spi or re.search(r"spibus_release\(", spi):
        return False, "sd_spi.c releases the bus without the trailing clock"
    hw = open(os.path.join(ROOT, "src/sd/sdio_hw_pico.c")).read()
    if hw.count("reserve(") < 4 or "pins_reserve(" not in hw:
        return False, "sdio_hw_pico.c does not reserve CLK/CMD/D0-D3"
    port = open(os.path.join(ROOT, "src/sd/sd_port_pico.c")).read()
    if "pins_reserve(" not in port:
        return False, "card detect is not reserved"
    rc, pd = sh("grep -rn 'gpio_pull_down\\|gpio_set_pulls([^,]*, *[a-z]*, *true' src/sd 2>/dev/null")
    if pd.strip():
        return False, f"SD code enables a pull-down (RP2350-E9 on A2): {pd.split(chr(10))[0][:60]}"
    rc, out = sh("grep -rliE 'zuluscsi|rabbit hole computing|carlk3|no-OS-FatFS' src third_party 2>/dev/null")
    if out.strip():
        return False, f"GPL SDIO source in the tree: {out.split()[0]}"
    return True, "worker-only, trailing-clock releases, pins reserved, no GPL SDIO code"


def t_wifi_ap_is_independent():
    """The SoftAP is a parallel service, not a state the station has to leave.

    The two used to be exclusive by construction: start_ap() disabled the
    station and begin_join() stopped the AP. Removing that is the whole
    point of mode `both` and of the WireGuard-gated `auto`, and either call
    creeping back would silently restore the exclusivity while everything
    still compiled. The default-route rule is guarded here too: WireGuard's
    ANY-bound pcb follows netif_default, and the SDK points it at whichever
    interface came up LAST, so wifi.c must only ever point it at the station."""
    src = open(os.path.join(ROOT, "src/net/wifi.c")).read()
    for fn, bad in (("start_ap", "cyw43_arch_disable_sta_mode"),
                    ("begin_join", "stop_ap("),
                    ("stop_ap", "cyw43_arch_disable_sta_mode")):
        body = _fn_body(src, fn)
        if body is None:
            return False, f"wifi.c no longer has {fn}()"
        if bad in body:
            return False, f"{fn}() calls {bad}: the AP and station are exclusive again"
    for tok in ("ST_AP", "pending_ap", "have_other_route"):
        if re.search(r"\b" + tok + r"\b", src):
            return False, f"wifi.c still has {tok}"
    # The default route is uplink.c's decision now (Ethernet can carry it),
    # but with WiFi alone it must come out exactly as before: the station
    # whenever it is enabled, never the AP. wifi.c re-asserts it through
    # uplink_reselect() wherever the driver moves it, and the fallback that
    # uplink.c uses when nothing is measured up is the enabled station.
    for line in src.splitlines():
        s = line.strip()
        if s.startswith(("*", "/*", "//")):
            continue                      # prose about the SDK, not a call
        if "netif_set_default(" in line:
            return False, f"wifi.c sets the default route itself: {s[:60]}"
    body = _fn_body(src, "fix_default_route")
    if body is None or "uplink_reselect()" not in body:
        return False, "fix_default_route() no longer hands the decision to uplink_reselect()"
    fb = src[src.find("struct netif *wifi_fallback_netif(void)"):]
    if not fb or "CYW43_ITF_STA" not in fb[:300] or "sta_netif()" not in fb[:300]:
        return False, "wifi_fallback_netif() is not the enabled station"
    up = open(os.path.join(ROOT, "src/net/uplink.c")).read()
    if "wifi_fallback_netif()" not in up:
        return False, "uplink.c does not fall back to the station"
    return True, "AP and station decoupled; the route falls back to the station, never the AP"

def t_only_uplink_sets_default():
    """src/net/uplink.c is the ONLY caller of netif_set_default(). Two owners
    of the default route is how the tunnel once left through the AP: each
    sets it for good reasons of its own, and the last one wins silently."""
    rc, out = sh("grep -rn --include=*.c --include=*.h 'netif_set_default(' src/ 2>/dev/null")
    bad = []
    for l in out.splitlines():
        if not l.strip():
            continue
        path, _, code = l.split(":", 2)
        c = code.strip()
        if c.startswith(("*", "/*", "//")) or "DELIBERATELY NOT" in c or "NOT netif_set_default" in c:
            continue
        if path != "src/net/uplink.c":
            bad.append(l)
    if bad:
        return False, bad[0][:90]
    return True, "uplink.c only"

def t_uplink_policy():
    """Uplink metrics and selection from scripted ping results: hysteresis
    (30% better for 20 s), immediate switch on failure, failover, fixed, no
    flapping when every probe fails, gateway loss."""
    return _host_c("uplink", ["test/host/uplink_test.c", "src/net/uplink_core.c"], "btgw_uplink", defs="")

def t_wg_tx_netif_local_change():
    """The WireGuard egress LOCAL CHANGE is in place and recorded: every outer
    send goes through wg_udp_send(), and VENDORED.txt says so."""
    w = open(os.path.join(ROOT, "third_party/wireguard-lwip/src/wireguardif.c")).read()
    code = re.sub(r"//[^\n]*", "", w)
    sends = re.findall(r"\budp_sendto\s*\(", code)
    if len(sends) != 1:
        return False, f"{len(sends)} bare udp_sendto() in wireguardif.c (only wg_udp_send's fallback may remain)"
    if "udp_sendto_if_src(" not in w or "wireguardif_send_keepalive_now" not in w:
        return False, "the tx_netif / keepalive-now change is missing"
    v = open(os.path.join(ROOT, "third_party/wireguard-lwip/VENDORED.txt")).read()
    if "wireguardif_set_tx_netif" not in v:
        return False, "VENDORED.txt does not record the change"
    return True, "egress via tx_netif, keepalive on switch, recorded"

def t_mdns_records():
    """mDNS record groups: the table, the wire encoding (A/PTR/SRV/TXT,
    cache-flush, TTLs, additionals), answering incl. compression pointers,
    QU and legacy unicast, announce/goodbye, limits -- under ASan/UBSan
    where the compiler has them."""
    rc, _ = sh("echo 'int main(){}' | gcc -fsanitize=address,undefined -x c - -o /dev/null 2>/dev/null")
    san = "-fsanitize=address,undefined" if rc == 0 else ""
    return _host_c("mdns", ["test/host/mdns_test.c", "src/net/mdns_records.c"], "btgw_mdns", defs=san)

def t_lwip_mdns_vendored():
    """third_party/lwip-mdns is lwIP 2.2.1's responder with ONE local change:
    mdns_out.c and mdns_domain.c byte-identical to the SDK's, and mdns.c
    different only by the hook (every differing line is inside a LOCAL
    CHANGE block). A drifted copy would silently fork lwIP."""
    import difflib
    sdk = os.path.expanduser("~/.pico-sdk/sdk/2.3.0/lib/lwip/src/apps/mdns")
    if not os.path.isdir(sdk):
        return True, "skipped (no SDK checkout to compare against)"
    for f in ("mdns_out.c", "mdns_domain.c"):
        if open(os.path.join(sdk, f), "rb").read() != open(os.path.join(ROOT, "third_party/lwip-mdns", f), "rb").read():
            return False, f"{f} differs from lwIP 2.2.1"
    a = open(os.path.join(sdk, "mdns.c")).read().splitlines()
    b = open(os.path.join(ROOT, "third_party/lwip-mdns/mdns.c")).read().splitlines()
    added = [l for l in difflib.unified_diff(a, b, lineterm="", n=0)
             if l.startswith(("+", "-")) and not l.startswith(("+++", "---"))]
    if any(l.startswith("-") for l in added):
        return False, "mdns.c removes upstream lines"
    allowed = ("btgw", "LOCAL CHANGE", "/*", "*", "*/", "if (packet.questions)", "}", "")
    for l in added:
        t = l[1:].strip()
        if not (t.startswith(allowed[:-1]) or t in allowed or "btgw_mdns_query_hook" in t
                or t.startswith(("u16_t port", "void btgw"))):
            return False, f"unrecorded change: {t[:60]}"
    if "btgw_mdns_query_hook" not in open(os.path.join(ROOT, "third_party/lwip-mdns/VENDORED.txt")).read():
        return False, "VENDORED.txt does not describe the hook"
    return True, f"2 files identical, mdns.c +{len(added)} lines (the hook)"

def t_lua_net_released():
    """What a script declares on the network goes when it stops: mDNS services
    (they used to leak for the life of the boot) and record groups, per slot
    and on a VM restart."""
    c = open(os.path.join(ROOT, "src/lua/lbind.c")).read()
    i = c.find("static void lbind_release_net(int slot) {")
    rel = c[i:c.find("\n}\n", i)] if i >= 0 else ""
    if "mdns_release_owner(slot)" not in rel:
        return False, "lbind_release_net does not release mDNS"
    ops = c[c.index("int lbind_release_ops(int slot)"):]
    if "lbind_release_net(slot)" not in ops[:ops.index("\n}\n")]:
        return False, "lbind_release_ops does not call lbind_release_net"
    reset = _fn_body(c, "lbind_reset")
    if not reset or "mdns_release_all_owned()" not in reset:
        return False, "lbind_reset does not release every script's mDNS"
    if "mdns_service_add_owned(lrt_current_slot()" not in c:
        return False, "mdns.service does not record its owner"
    return True, "services + groups released per script and on VM restart"

def t_fwd_core():
    """Forwarding decisions: UDP session table (cap, 60 s idle expiry), target
    health / fail fast with the 5 s re-test, TCP backpressure."""
    return _host_c("fwd", ["test/host/fwd_test.c", "src/net/fwd_core.c"], "btgw_fwd", defs="")

def t_fwd_released_with_script():
    """Forwarding rules belong to their script: released per slot and on a VM
    restart, like mDNS declarations."""
    c = open(os.path.join(ROOT, "src/lua/lbind.c")).read()
    i = c.find("static void lbind_release_net(int slot) {")
    rel = c[i:c.find("\n}\n", i)] if i >= 0 else ""
    if "fwd_release_owner(slot)" not in rel:
        return False, "lbind_release_net does not release forwarding rules"
    reset = _fn_body(c, "lbind_reset")
    if not reset or "fwd_release_all_owned()" not in reset:
        return False, "lbind_reset does not release forwarding rules"
    if "fwd_add(lrt_current_slot()" not in c:
        return False, "net.forward does not record its owner"
    return True, "per script and on VM restart"

def t_sys_timeout_pool_covers_mdns():
    """MEMP_NUM_SYS_TIMEOUT must cover the mDNS responder's per-netif timers on
    every netif it can be bound to. At LWIP_NUM_SYS_TIMEOUT_INTERNAL + 8 the
    pool ran dry when a service was re-added while another announced, and
    sys_timeout() panics on an empty pool (HardFault, watchdog reboot)."""
    o = open(os.path.join(ROOT, "include/lwipopts.h")).read()
    m = re.search(r"#define\s+MDNS_TIMEOUT_NETIFS\s+(\d+)", o)
    k = re.search(r"#define\s+MDNS_TIMEOUTS_PER_NETIF\s+(\d+)", o)
    if not m or not k or "MDNS_TIMEOUT_NETIFS * MDNS_TIMEOUTS_PER_NETIF" not in o:
        return False, "MEMP_NUM_SYS_TIMEOUT no longer reserves mDNS's per-netif timers"
    c = open(os.path.join(ROOT, "src/net/mdns.c")).read()
    nn = re.search(r"#define\s+MDNS_NETIFS\s+(\d+)", c)
    if not nn or int(m.group(1)) < int(nn.group(1)):
        return False, "fewer netifs reserved for than mdns.c can bind"
    if int(k.group(1)) < 6:
        return False, "lwIP mDNS uses up to 6 timeouts per netif"
    return True, f"{m.group(1)} netifs x {k.group(1)} mDNS timeouts reserved"

def t_eth_cfg():
    """Ethernet addressing config: a missing/corrupt/unknown cfg is the DHCP
    client, server mode only from an explicit {"mode":"server"}, link-local
    only after the DHCP-alone window."""
    rc, _ = sh("echo 'int main(){}' | gcc -fsanitize=address,undefined -x c - -o /dev/null 2>/dev/null")
    san = "-fsanitize=address,undefined" if rc == 0 else ""
    return _host_c("eth_cfg", ["test/host/eth_cfg_test.c", "src/net/eth_cfg.c"], "btgw_eth_cfg", defs=san)

def t_eth_server_mode_only_explicit():
    """The owner's rule: DHCP-server mode on e0 is NEVER entered on its own.
    The only code that may produce ETH_MODE_SERVER is eth_cfg_parse() (reached
    from the eth.config op and from loading cfg/eth.json); nothing else may
    assign it -- not a DHCP failure path, the uplink logic, Lua or a Kconfig
    default -- and the boot-time load must fall back to the default."""
    hits = []
    for root, _, files in os.walk(os.path.join(ROOT, "src")):
        for fn in files:
            if not fn.endswith((".c", ".h")):
                continue
            path = os.path.join(root, fn)
            for i, line in enumerate(open(path), 1):
                code = line.split("//")[0]
                # an assignment (or initialiser), not a comparison
                if re.search(r"(?<![=!<>])=\s*ETH_MODE_SERVER\b", code):
                    hits.append(f"{os.path.relpath(path, ROOT)}:{i}")
    rel = [h for h in hits if not h.startswith("src/net/eth_cfg.c:")]
    if rel:
        return False, f"ETH_MODE_SERVER assigned outside eth_cfg.c: {rel[0]}"
    c = open(os.path.join(ROOT, "src/net/eth_cfg.c")).read()
    body = c[c.index("bool eth_cfg_parse("):c.index("void eth_cfg_load(")]
    if len(hits) != 1 or "ETH_MODE_SERVER" not in body or 'strcmp(mode, "server") == 0' not in body:
        return False, "server mode must be set exactly once, in eth_cfg_parse, on an explicit \"server\""
    load = c[c.index("void eth_cfg_load("):]
    if "eth_cfg_default(out)" not in load[:load.index("\n}\n")]:
        return False, "eth_cfg_load no longer falls back to the default"
    if "eth_cfg_load(" not in open(os.path.join(ROOT, "src/net/eth_w5500.c")).read():
        return False, "eth_w5500.c does not load its config through eth_cfg_load"
    for f in ("src/lua/lbind.c", "src/net/uplink.c", "src/net/wifi.c"):
        if "ETH_MODE_SERVER" in open(os.path.join(ROOT, f)).read():
            return False, f"{f} refers to ETH_MODE_SERVER"
    d = open(os.path.join(ROOT, "src/net/dhcpserver.c")).read()
    if "if (!d->no_router)" not in d:
        return False, "dhcpserver.c always sends the router option"
    for f in ("src/net/usb_ncm.c", "src/net/wifi.c"):
        if "no_router" in open(os.path.join(ROOT, f)).read():
            return False, f"{f} changed its DHCP server's router option"
    return True, "only eth_cfg_parse sets it; bad config -> DHCP client; NCM/AP offers unchanged"

def t_wifi_mode_both_everywhere():
    """A mode that the config accepts must also be offered by the UI and named
    in the API's error text, or it exists in one place and is undiscoverable
    from the others -- exactly how `off` sat advertised-but-rejected before."""
    cfg = open(os.path.join(ROOT, "src/config/config.c")).read()
    if 'strcmp(m, "both") == 0' not in cfg:
        return False, "config.c valid_mode() does not accept both"
    ops = open(os.path.join(ROOT, "src/net/wifi_ops.c")).read()
    if "auto, sta, ap, both, off" not in ops:
        return False, "wifi_ops.c error text does not list both"
    # The WiFi UI is a Settings section of the one-page app now (it was
    # web/wifi/index.html); the mode select is built there.
    ui = open(os.path.join(ROOT, "web/js/settings/wifi.js")).read()
    if 'value="both"' not in ui:
        return False, "web/js/settings/wifi.js offers no both option"
    return True, "both: config, API text and UI agree"

def t_script_loaded_reports_arena():
    """The web editor's "loaded" and "loaded, modified since" markers rest on
    script.loaded: each unit's compiled tier + FNV-1a hash, and the hash of its
    file now. Check the op is registered, the load paths record what they
    compiled (scripts in load_tier, modules in require), a module's hash is
    taken before lua_call can reuse src_buf, and the FNV-1a constants match
    the 32-bit standard the browser side implements."""
    c = open(os.path.join(ROOT, "src/lua/lrt.c")).read()
    o = open(os.path.join(ROOT, "src/lua/lops.c")).read()
    if '"script.loaded"' not in o:
        return False, "script.loaded is not in the op table"
    if "s->src_hash = hash" not in c or "module_set_src(name, tier, hash)" not in c:
        return False, "a load path no longer records what it compiled"
    req = c[c.index("static int l_require"):]
    if req.index("module_identity(name, &tier, &hash, &stored)") > req.index("lua_call(Ls, 0, 1)"):
        return False, "module hash taken after lua_call -- src_buf may hold another module by then"
    if "2166136261u" not in c or "16777619u" not in c:
        return False, "fnv1a constants changed; the web editor hashes with the standard ones"
    if '\\"truncated\\"' not in c:
        return False, "script.loaded must report truncation rather than overflow the frame"
    return True, "registered, recorded on both load paths, hashed before running"


def t_no_nested_flash_safe():
    """SDK 2.3.0's pico_bootrom helpers (rom_explicit_buy, rom_flash_op ...)
    run themselves under flash_safe_execute(). Calling one from INSIDE our
    own flash_safe_execute() nests the safe zone, whose single saved
    interrupt state per core is then overwritten with "disabled": PRIMASK
    stays set forever and USB-NCM dies on the spot (fw 1.95 confirm). So no
    callback handed to flash_safe_execute may call a rom_* helper."""
    import re
    bad = []
    for root, _, files in os.walk(os.path.join(ROOT, "src")):
        for fn in files:
            if not fn.endswith(".c"):
                continue
            c = open(os.path.join(root, fn)).read()
            for cb in set(re.findall(r"flash_safe_execute\(\s*(\w+)", c)):
                m = re.search(r"\b(?:static\s+)?void\s+" + cb + r"\s*\([^)]*\)\s*\{", c)
                if not m:
                    continue
                depth, i = 0, m.end() - 1
                while True:
                    depth += {"{": 1, "}": -1}.get(c[i], 0)
                    i += 1
                    if depth == 0:
                        break
                body = c[m.end():i]
                if re.search(r"\brom_(?!get_boot_info|func_lookup)\w+\s*\(", body):
                    bad.append(f"{fn}:{cb}")
    if bad:
        return False, "rom_* helper inside flash_safe_execute: " + ", ".join(bad)
    return True, "no rom_* helper runs inside our own flash_safe_execute"

def t_wg_confirmed_exists():
    """wifi.c's AP policy keys on wg_is_confirmed(); if it were removed or its
    stub dropped, the AP would be permanently on (or the build would fail only
    with WireGuard enabled). Both header branches must declare it and the
    status must report it, so a monitor can see what the policy sees."""
    h = open(os.path.join(ROOT, "src/net/wg.h")).read()
    if h.count("wg_is_confirmed(void)") < 2:
        return False, "wg.h must declare wg_is_confirmed in both #if branches"
    c = open(os.path.join(ROOT, "src/net/wg.c")).read()
    if "bool wg_is_confirmed(void)" not in c:
        return False, "wg.c does not define wg_is_confirmed"
    if '\\"confirmed\\"' not in c:
        return False, "wg.status does not report confirmed"
    w = open(os.path.join(ROOT, "src/net/wifi.c")).read()
    if "wg_is_confirmed()" not in w:
        return False, "wifi.c's AP policy no longer consults wg_is_confirmed"
    return True, "declared, defined, reported, and consulted"

def t_dhcp_binds_specific_ip():
    """Two DHCP servers share :67 only because each binds its own address;
    lwIP refuses a second ANY bind. Bound as ANY the SoftAP silently had no
    DHCP at all. The dead LWIP_SO_REUSE option must stay gone too, or the
    next reader will believe it makes ANY binds shareable -- it never did."""
    d = open(os.path.join(ROOT, "src/net/dhcpserver.c")).read()
    if "udp_bind(d->udp, IP4_ADDR_ANY" in d:
        return False, "dhcpserver.c binds ANY:67 again"
    if "udp_bind(d->udp, ip, PORT_DHCP_SERVER)" not in d:
        return False, "dhcpserver.c does not bind the netif's own address"
    o = open(os.path.join(ROOT, "include/lwipopts.h")).read()
    if re.search(r"^\s*#define\s+LWIP_SO_REUSE\b", o, re.M):
        return False, "lwipopts.h defines LWIP_SO_REUSE, which is not an lwIP option"
    return True, "per-interface :67 binds; no dead reuse option"

def t_anker_model_schemas():
    """Every per-model schema must satisfy the contract the loader assumes.

    The loader does require('profiles.anker.models.' .. product_code), so a
    file whose name and product_code disagree is a schema that never loads --
    silently, because a missing model is a legitimate state that just falls
    back to raw hex. That is the failure this catches.

    It also insists on an explicit `verified` flag. Schemas mined from the
    decompiled app cannot be checked against hardware we do not have, and a
    confident-looking wrong field name is worse than a missing one, so saying
    which is which is not optional."""
    import pathlib
    d = pathlib.Path(ROOT, "lua/profiles/anker/models")
    if not d.is_dir():
        return False, "lua/profiles/anker/models is missing"
    files = sorted(d.glob("*.lua"))
    if not files:
        return False, "no model schemas at all"
    unverified = []
    for f in files:
        src = f.read_text()
        m = re.search(r"product_code\s*=\s*'([A-Z0-9]+)'", src)
        if not m:
            return False, f"{f.name} declares no product_code"
        if m.group(1) != f.stem:
            return False, (f"{f.name} declares product_code '{m.group(1)}' -- "
                           f"the loader would look for {m.group(1)}.lua")
        if not re.search(r"\bfamily\s*=\s*'(pps|scp)'", src):
            return False, f"{f.name} has no family = 'pps' or 'scp'"
        if not re.search(r"\bfields\s*=", src):
            return False, f"{f.name} has no fields table"
        vm = re.search(r"\bverified\s*=\s*(true|false)", src)
        if not vm:
            return False, f"{f.name} does not say whether it is verified"
        if vm.group(1) == "false":
            unverified.append(f.stem)
    detail = f"{len(files)} schema(s)"
    if unverified:
        detail += f", {len(unverified)} unverified ({', '.join(sorted(unverified))})"
    return True, detail

def t_inflate():
    """The board's gzip inflater (src/util/inflate.c) against zlib: Python
    writes streams at every level and with every optional header field, over
    random data and the repo's own Lua and JS, plus truncated and corrupted
    copies; the C side must reproduce each byte for byte, fed in odd-sized
    pieces, and reject every corrupt one without crashing."""
    tmp = os.path.join(os.environ.get("TMPDIR", "/tmp"), "btgw_gz")
    binp = os.path.join(os.environ.get("TMPDIR", "/tmp"), "btgw_inflate")
    rc, out = sh(f"python3 test/host/mkgz.py {tmp}")
    if rc:
        return False, (out.strip().splitlines() or ["mkgz failed"])[-1][:90]
    rc, out = sh(f"gcc -O1 -Wall -Wextra -Isrc -o {binp} test/host/inflate_test.c src/util/inflate.c")
    if rc:
        errs = [l for l in out.splitlines() if "error:" in l]
        return False, errs[0][:100] if errs else "inflate harness build failed"
    rc, out = sh(f"{binp} {tmp}")
    last = (out.strip().splitlines() or ["no output"])[-1]
    if rc != 0:
        first = next((l for l in out.splitlines() if l.startswith("FAIL")), last)
        return False, first[:100]
    return True, last

def t_lua_wait():
    """The wait/wake contract. These are the races -- a disconnect arriving
    while the coroutine is runnable rather than blocked, a frame from a
    previous connection arriving after a reconnect, an event that never comes
    -- and none of them is reachable from a hardware test with any
    reliability. The state machine is pure logic over an injected clock for
    exactly that reason."""
    binp = os.path.join(os.environ.get("TMPDIR", "/tmp"), "btgw_wait")
    rc, out = sh(f"gcc -O1 -Wall -Wextra -Isrc -o {binp} "
                 f"test/host/wait_test.c src/lua/lwait.c")
    if rc:
        errs = [l for l in out.splitlines() if "error:" in l]
        return False, errs[0][:100] if errs else "wait harness build failed"
    rc, out = sh(binp)
    if rc != 0:
        first = next((l.strip() for l in out.splitlines() if "FAIL" in l), "")
        return False, first[:100] or f"wait_test exited {rc}"
    return True, "delivered / timeout / link_lost / cancelled, and the races"

def t_lua_heap():
    """The Lua arena, on a host, with every structural invariant asserted after
    EVERY operation. This test exists because two attempts at the allocator's
    in-place realloc shipped to hardware and had to be reverted; each is
    reproduced as a deliberate defect in the test's own comments, and each is
    caught by a named assertion rather than by a wrapped byte counter.

    A small arena is used so the fuzz reaches exhaustion and the refused-grow
    paths actually run."""
    binp = os.path.join(os.environ.get("TMPDIR", "/tmp"), "btgw_heap")
    rc, out = sh(f"gcc -O1 -Wall -Wextra -Isrc -DLHEAP_KB=64 -o {binp} "
                 f"test/host/heap_test.c src/lua/lheap.c")
    if rc:
        errs = [l for l in out.splitlines() if "error:" in l]
        return False, errs[0][:100] if errs else "heap harness build failed"
    rc, out = sh(binp)
    if rc != 0:
        first = next((l.strip() for l in out.splitlines() if "FAIL" in l), "")
        return False, first[:100] or f"heap_test exited {rc}"
    peak = re.search(r"growth: final (\d+) B, peak (\d+) B", out)
    detail = "invariants hold after every op"
    if peak:
        detail += f"; doubling to {peak.group(1)} B peaked at {peak.group(2)} B"
    return True, detail

def t_lua_json():
    """The Lua -> JSON encoder, on a host, with the output buffer backed onto a
    guard page.

    This exists because the encoder shipped with an out-of-bounds write: every
    append accumulated what snprintf WOULD have written, so a table that
    outgrew the buffer pushed the cursor past the end, and the unconditional
    closing brace was then written out of bounds through a wrapped size_t
    length. An ordinary Anker telemetry report reached it.

    Asserting on the output text would not catch that -- the visible symptom is
    only truncated JSON -- so the buffer ends flush against a PROT_NONE page
    and an overrun is a SIGSEGV. Restoring the old idiom makes this test crash,
    which is how it was checked."""
    binp = os.path.join(os.environ.get("TMPDIR", "/tmp"), "btgw_json")
    rc, out = sh(f"gcc -O1 -w -DMAKE_LIB=1 -Isrc -Ithird_party/lua -o {binp} "
                 f"test/host/json_test.c src/lua/ljson.c src/api/json_escape.c "
                 f"third_party/lua/onelua.c -lm")
    if rc:
        errs = [l for l in out.splitlines() if "error:" in l]
        return False, errs[0][:100] if errs else "json harness build failed"
    rc, out = sh(binp)
    if rc != 0:
        if rc >= 128 or rc < 0:
            return False, f"the encoder wrote past its buffer (signal {abs(rc) & 0x7f})"
        first = next((l.strip() for l in out.splitlines() if "FAIL" in l), "")
        return False, first[:100] or f"json_test exited {rc}"
    return True, "bounded under a guard page; truncation always reported"

def t_lua_proto():
    """The profile's Lua, on a host, against the REAL C bindings and the REAL
    vendored Lua -- no board, no flash cycle, no charger. This is the dividend
    of putting the device-specific half in Lua: the frame construction and the
    telemetry decode, which are the parts most likely to be wrong, are pure
    functions of bytes and can be checked in milliseconds."""
    uecc = os.path.expanduser("~/.pico-sdk/sdk/2.3.0/lib/btstack/3rd-party/micro-ecc")
    binp = os.path.join(os.environ.get("TMPDIR", "/tmp"), "btgw_luaproto")
    rc, out = sh(f"gcc -O1 -w -DMAKE_LIB=1 "
                 f"-Isrc -Itest/host -Ithird_party/tiny-AES-c -Ithird_party/lua -I{uecc} "
                 f"-o {binp} test/host/luaproto_main.c src/lua/lbind_proto.c "
                 f"src/codec/anker_frame.c src/codec/anker_tlv.c "
                 f"src/crypto/ghash.c src/crypto/anker_crypto.c src/crypto/sha256.c "
                 f"third_party/tiny-AES-c/aes.c {uecc}/uECC.c "
                 f"third_party/lua/onelua.c -lm")
    if rc:
        errs = [l for l in out.splitlines() if "error:" in l]
        return False, errs[0][:100] if errs else "harness build failed"
    rc, out = sh(f"{binp} test/host/proto_test.lua")
    m = re.search(r"(\d+) checks, (\d+) failed", out)
    if not m:
        return False, (out.strip().splitlines() or ["no output"])[-1][:90]
    nchk, nfail = int(m.group(1)), int(m.group(2))
    if nfail:
        first = next((l for l in out.splitlines() if l.startswith("FAIL")), "")
        return False, f"{nfail} of {nchk} failed: {first[:70]}"
    return rc == 0, f"{nchk} checks"

def t_parity():
    """The frozen protocol vectors. Two of these assertions are regression
    locks -- the lenient PKCS7 unpad and GCM without tag verification -- and a
    'fix' to either stops the device talking."""
    uecc = os.path.expanduser("~/.pico-sdk/sdk/2.3.0/lib/btstack/3rd-party/micro-ecc")
    binp = "/tmp/btgw_parity"
    rc, out = sh(f"gcc -O1 -Wall -Wextra -Wno-unused-parameter "
                 f"-Isrc -Itest/host -Ithird_party/tiny-AES-c -I{uecc} -o {binp} "
                 f"test/host/parity_main.c src/codec/anker_frame.c src/codec/anker_tlv.c "
                 f"src/crypto/ghash.c src/crypto/anker_crypto.c src/crypto/sha256.c "
                 f"third_party/tiny-AES-c/aes.c {uecc}/uECC.c -lm")
    if rc:
        errs = [l for l in out.splitlines() if "error:" in l]
        return False, errs[0][:100] if errs else "parity build failed"
    rc, out = sh(binp)
    npass = len(re.findall(r"^OK", out, re.M))
    nfail = len(re.findall(r"^FAIL", out, re.M))
    nskip = len(re.findall(r"^SKIP", out, re.M))
    detail = f"{npass} passed"
    if nskip:
        detail += f", {nskip} skipped (C1000-only / no ref workspace)"
    if nfail:
        first = next((l for l in out.splitlines() if l.startswith("FAIL")), "")
        return False, f"{nfail} failed: {first[:70]}"
    return rc == 0, detail

def t_registry_transport_neutral():
    return _grep(r"\ble_[a-z]|gatt_|att_", ["src/bt/registry.c"])

def t_webapp_e2e():
    """The one-page app driven end to end against dev/devserver.py's mock
    device: windows and keys, the editor's four states with vim and undo, and
    the device-plugin lifecycle with a write confirmed by telemetry. Skipped
    where Playwright is not installed (cd web && npm install)."""
    if not os.path.exists(os.path.join(ROOT, "web/node_modules/.bin/playwright")):
        return True, "skipped (Playwright not installed: cd web && npm install)"
    rc, out = sh("npx playwright test --reporter=line", cwd=os.path.join(ROOT, "web"))
    m = re.search(r"(\d+) passed", out)
    if rc or not m:
        bad = re.search(r"(\d+) failed", out)
        return False, (f"{bad.group(1)} failed" if bad else (out.strip().splitlines() or ["no output"])[-1][:90])
    return True, f"{m.group(1)} passed"

def t_webapp_dom():
    """Render the real page in a real browser. Every model test passed while
    the app opened to an empty workspace nothing could be selected in, because
    a model test cannot see a wrong argument at the one call site that builds
    the layout, or a render method nobody called. Both of those shipped."""
    rc, out = sh("python3 tools/webtest.py")
    if "SKIP:" in out:
        return True, "skipped (no chromium)"
    m = re.search(r"(\d+)/(\d+) DOM checks passed", out)
    if not m:
        return False, (out.strip().splitlines() or ["no output"])[-1][:90]
    good, total = int(m.group(1)), int(m.group(2))
    if rc or good != total:
        first = next((l for l in out.splitlines() if l.startswith("FAIL")), "")
        return False, f"{total - good} of {total} failed: {first[:70]}"
    return True, f"{good} checks in chromium"

def _host_c(name, srcs, binname, defs="-DSPIBUS_HOST"):
    """Build and run one host C test that prints 'N checks, M failed'."""
    binp = os.path.join(os.environ.get("TMPDIR", "/tmp"), binname)
    rc, out = sh(f"gcc -O1 -Wall -Wextra {defs} -Isrc -Itest/host -o {binp} {' '.join(srcs)}")
    if rc:
        errs = [l for l in out.splitlines() if "error:" in l]
        return False, errs[0][:100] if errs else f"{name} harness build failed"
    rc, out = sh(binp)
    m = re.search(r"(\d+) checks, (\d+) failed", out)
    if not m:
        return False, (out.strip().splitlines() or ["no output"])[-1][:90]
    if rc or int(m.group(2)):
        first = next((l for l in out.splitlines() if l.startswith("FAIL")), "")
        return False, f"{m.group(2)} of {m.group(1)} failed: {first[:70]}"
    return True, f"{m.group(1)} checks"

def t_spibus():
    """The SPI bus layer against a fake DMA: exclusive claims, CS held across
    a chained transfer, completion only from the loop, clock per device."""
    return _host_c("spibus", ["test/host/spibus_test.c", "src/io/spibus.c",
                              "test/host/fake_spi.c"], "btgw_spibus")

def t_sd_crc():
    """SD CRC7/CRC16 against the spec's examples and bit-serial references,
    and the claim the SDIO driver rests on: one CRC64 over a 4-bit block is
    the four per-line CRC16s, interleaved in wire order."""
    return _host_c("sd_crc", ["test/host/sd_crc_test.c", "src/sd/sd_crc.c"], "btgw_sd_crc", defs="")

SD_SIM_SRCS = ["src/sd/sd_card.c", "src/sd/sd_sdio.c", "src/sd/sd_spi.c", "src/sd/sd_crc.c",
               "src/io/spibus.c", "test/host/fake_spi.c", "test/host/sdcard_sim.c",
               "test/host/sdio_hw_sim.c", "test/host/sd_port_host.c"]

def t_sd_card():
    """Both SD buses -- SDIO (SD mode) and SPI on the shared bus -- through
    sd_card.c against one simulated card: identification (SDHC, SDSC, v1,
    slow power-up), 1-64 block reads/writes at the edges, CRC/write errors
    retried, long busy waited out (and the W5500 getting the bus meanwhile),
    a dead command, a card pulled mid-read and reinserted, and never a MISO
    conflict on the shared bus."""
    return _host_c("sd_card", ["test/host/sd_card_test.c"] + SD_SIM_SRCS, "btgw_sd_card",
                   defs="-DSPIBUS_HOST -DCONFIG_SD_DETECT=22")

SD_STACK_SRCS = ["src/sd/sd_card.c", "src/sd/sd_sdio.c", "src/sd/sd_spi.c", "src/sd/sd_crc.c",
                 "src/sd/sd_store.c", "src/sd/sd_api.c", "src/sd/sd_core.c", "src/sd/sd_diskio.c",
                 "third_party/fatfs/ff.c", "third_party/fatfs/ffunicode.c", "src/io/spibus.c",
                 "test/host/fake_spi.c", "test/host/sdcard_sim.c", "test/host/sdio_hw_sim.c",
                 "test/host/sd_port_host.c", "test/host/sd_worker_uc.c"]

def t_sd_stack():
    """The whole SD stack on both buses: worker -> /api/sd transfers -> store
    -> FatFs -> card driver -> simulated card. Format FAT32 and exFAT, upload,
    download, list, mkdir, rename, delete, the error map, bench, card pulled
    and reinserted. Then the card images go through fsck.fat / fsck.exfat --
    Linux's own checkers, independent of FatFs."""
    import tempfile
    d = tempfile.mkdtemp(prefix="btgw_sdimg_")
    try:
        return _sd_stack_in(d)
    finally:
        shutil.rmtree(d, ignore_errors=True)    # four ~300 MB partitions per run


def _sd_stack_in(d):
    binp = os.path.join(os.environ.get("TMPDIR", "/tmp"), "btgw_sd_stack")
    rc, out = sh(f"gcc -O1 -Wall -Wextra -DSPIBUS_HOST -DCONFIG_SD_DETECT=22 -Isrc -Ithird_party/fatfs "
                 f"-Itest/host -o {binp} test/host/sd_stack_test.c {' '.join(SD_STACK_SRCS)}")
    if rc:
        errs = [l for l in out.splitlines() if "error:" in l]
        return False, errs[0][:100] if errs else "sd_stack build failed"
    rc, out = sh(f"SD_IMG_DIR={d} {binp}")
    m = re.search(r"(\d+) checks, (\d+) failed", out)
    if not m or rc or int(m.group(2)):
        first = next((l for l in out.splitlines() if l.startswith("FAIL")), out.strip()[-90:])
        return False, first[:100]
    imgs = re.findall(r"IMAGE (\S+)", out)
    fscks = 0
    for img in imgs:
        tool = "fsck.exfat -n" if "exfat" in img else "fsck.fat -n"
        if sh(f"command -v {tool.split()[0]}")[0]:
            continue
        rc2, o2 = sh(f"{tool} {img}")
        if rc2:
            return False, f"{os.path.basename(img)}: {tool.split()[0]} says: {o2.strip().splitlines()[-1][:60]}"
        fscks += 1
    return True, f"{m.group(1)} checks, {fscks}/{len(imgs)} images fsck-clean"

def t_w5500():
    """The W5500 MACRAW state machine against a simulated chip: ring
    wrap-around, back-to-back frames, chained buffers, ring full, one SEND at
    a time, link, resync on a corrupt length, and a bus held by another
    device."""
    return _host_c("w5500", ["test/host/w5500_test.c", "src/net/w5500.c", "src/io/spibus.c",
                             "test/host/fake_spi.c"], "btgw_w5500")

def t_spi_only_through_spibus():
    """Every SPI access goes through src/io/spibus*.c, which claims the bus.
    A direct spi_write/spi_read/spi_init anywhere else would bypass the
    semaphore and could interleave with the W5500's DMA mid-frame."""
    rc, out = sh(r"grep -rnE '\bspi_(write|read|init|set_baudrate|set_format|get_hw)\w*\s*\(' src/ 2>/dev/null")
    hits = [l for l in out.splitlines() if l.strip() and not l.startswith("src/io/spibus")]
    return (len(hits) == 0), (hits[0][:90] if hits else "spibus.c / spibus_pico.c only")

def t_cyw43_wait_stays_usb_only():
    """cyw43_wait.c pumps USB from inside the radio driver's waits. Nothing
    that touches lwIP may run there -- that includes the SPI completions and
    the Ethernet state machine, which deliver frames into lwIP."""
    src = open(os.path.join(ROOT, "src/net/cyw43_wait.c")).read()
    for bad in ("spibus_", "eth_", "w5500", "uplink_", "fwd_"):
        if bad in src:
            return False, f"cyw43_wait.c calls {bad}*"
    return True, "no spibus/eth/uplink/fwd in the radio's wait hook"

def t_w5500_pins_reserved():
    """The W5500's pins must be SYSTEM-reserved in the pin broker, or a
    script's gpio.set on GP13 would yank CS in the middle of a DMA frame.
    Follow the chain: eth passes the Kconfig pins to spibus/w5500, spibus
    reserves sck/mosi/miso and each device's CS, w5500 reserves INT and RST,
    and the Pico port turns that into pins_reserve()."""
    e = open(os.path.join(ROOT, "src/net/eth_w5500.c")).read()
    for k in ("CONFIG_ETH_W5500_SCK", "CONFIG_ETH_W5500_MOSI", "CONFIG_ETH_W5500_MISO",
              "CONFIG_ETH_W5500_CS", "CONFIG_ETH_W5500_INT", "CONFIG_ETH_W5500_RST"):
        if k not in e:
            return False, f"eth_w5500.c does not use {k}"
    b = open(os.path.join(ROOT, "src/io/spibus.c")).read()
    if b.count("spibus_port_reserve_pin(") < 4:
        return False, "spibus.c no longer reserves sck/mosi/miso and the CS pin"
    w = open(os.path.join(ROOT, "src/net/w5500.c")).read()
    if 'spibus_port_reserve_pin(int_pin' not in w or 'spibus_port_reserve_pin(rst_pin' not in w:
        return False, "w5500.c no longer reserves INT and RST"
    pc = open(os.path.join(ROOT, "src/io/spibus_pico.c")).read()
    if "pins_reserve(" not in pc:
        return False, "spibus_pico.c does not reserve through the pin broker"
    return True, "SCK/MOSI/MISO/CS/INT/RST all SYSTEM-owned"

# --- tier 1 -------------------------------------------------------------------
# BTGW_BUILD_PREFIX lets two checkouts (worktrees) run tier 1 at once without
# building into, and wiping, each other's directories.
BUILD_PREFIX = os.environ.get("BTGW_BUILD_PREFIX", "/tmp/btgw_")

def build(cfg):
    def fn():
        bdir = f"{BUILD_PREFIX}{cfg}"
        env = ('PICO_SDK_PATH=$HOME/.pico-sdk/sdk/2.3.0 '
               'PICO_TOOLCHAIN_PATH=$HOME/.pico-sdk/toolchain/15_2_Rel1 ')
        sh(f"rm -rf {bdir}")
        rc, out = sh(f"{env} cmake -S . -B {bdir} -G Ninja "
                     f"-DAPP_DEFCONFIG={ROOT}/configs/{cfg}_defconfig")
        if rc: return False, out.strip().splitlines()[-1] if out.strip() else "configure failed"
        rc, out = sh(f"{env} cmake --build {bdir}")
        if rc:
            errs = [l for l in out.splitlines() if "error:" in l]
            return False, errs[0][:100] if errs else "build failed"
        rc, sz = sh(f"{os.path.expanduser('~/.pico-sdk/toolchain/15_2_Rel1')}/bin/arm-none-eabi-size {bdir}/btgw.elf")
        last = sz.strip().splitlines()[-1].split()
        return True, f"{last[0]} text / {last[2]} bss"
    return fn

def t_w5500_symbols():
    """CONFIG_NET_ETH_W5500=n must carry none of it: no w5500_/eth_ symbol in
    the builds that leave it off, and all of it in the one that turns it on."""
    nm = os.path.expanduser("~/.pico-sdk/toolchain/15_2_Rel1/bin/arm-none-eabi-nm")
    pat = re.compile(r"\b(w5500_\w+|eth_(init|poll|status_json|configure)|op_module_eth)\b")
    for cfg in ("pico2w", "pico2w_minimal", "pico2w_noanker"):
        elf = f"{BUILD_PREFIX}{cfg}/btgw.elf"
        if not os.path.exists(elf):
            return False, f"{cfg} was not built"
        rc, out = sh(f"{nm} {elf}")
        hit = next((l for l in out.splitlines() if pat.search(l)), None)
        if hit:
            return False, f"{cfg} links {hit.split()[-1]}"
    rc, out = sh(f"{nm} {BUILD_PREFIX}pico2w_eth/btgw.elf")
    if not pat.search(out):
        return False, "pico2w_eth has no W5500 code at all"
    return True, "absent from pico2w/minimal/noanker, present in pico2w_eth"


def t_sd_symbols():
    """CONFIG_SD=n must carry none of it (no FatFs, no sd_ symbols, no /api/sd
    routes) in the four builds that leave it off; the SDIO build has the PIO
    driver and not the SPI one, and the SPI build the reverse."""
    nm = os.path.expanduser("~/.pico-sdk/toolchain/15_2_Rel1/bin/arm-none-eabi-nm")
    pat = re.compile(r"\b(f_mount|f_mkfs|sd_card_init|sd_api_status_json|sd_routes|op_module_sd|sdio_hw_init)\b")
    for cfg in ("pico2w", "pico2w_minimal", "pico2w_noanker", "pico2w_eth"):
        elf = f"{BUILD_PREFIX}{cfg}/btgw.elf"
        if not os.path.exists(elf):
            return False, f"{cfg} was not built"
        rc, out = sh(f"{nm} {elf}")
        hit = next((l for l in out.splitlines() if pat.search(l)), None)
        if hit:
            return False, f"{cfg} links {hit.split()[-1]}"
    rc, sdio = sh(f"{nm} {BUILD_PREFIX}pico2w_sd/btgw.elf")
    rc, spi = sh(f"{nm} {BUILD_PREFIX}pico2w_sdspi/btgw.elf")
    if "sdio_hw_init" not in sdio or "f_mkfs" not in sdio or "sd_bus_spi" in sdio:
        return False, "pico2w_sd: not the SDIO driver alone"
    if "sd_bus_spi" not in spi or "sdio_hw_init" in spi:
        return False, "pico2w_sdspi: not the SPI driver alone"
    return True, "absent from the four non-SD builds; SDIO and SPI builds each carry their own bus"


def t_sdio_diff():
    """test/sdio_diff in mcusim: our SDIO driver and carlk3's run the same
    script on the same simulated card; tools/sdio_diff.py compares them.
    Builds the four ELFs first (carlk3 fetched into the build dir only).
    Skipped, not failed, while mcusim has no SD card model."""
    bdir = os.path.join(ROOT, "build-sdio-diff")
    env = ('PICO_SDK_PATH=$HOME/.pico-sdk/sdk/2.3.0 '
           'PICO_TOOLCHAIN_PATH=$HOME/.pico-sdk/toolchain/15_2_Rel1 ')
    rc, out = sh(f"{env} cmake -S test/sdio_diff -B {bdir} -G Ninja -Wno-dev && {env} cmake --build {bdir}")
    if rc:
        return False, (out.strip().splitlines() or ["build failed"])[-1][:90]
    rc, out = sh(f"python3 tools/sdio_diff.py --build {bdir}")
    last = (out.strip().splitlines() or [""])[-1]
    if rc == 77:
        return True, "skipped: " + last.replace("SKIP: ", "")[:80]
    return rc == 0, last[:90]


def t_sd_gateway_sim():
    """The SD builds themselves in mcusim, over the simulated USB network:
    with card detect held low and no card on the bus (mcusim has no SD model
    yet) the worker must identify, fail and retry without a slice over 10 ms,
    report state "error", and keep /api/sd answering. Runs the PIO driver
    (pico2w_sd) and the SPI one on the shared bus (pico2w_sdspi) for real."""
    import json as _j, signal, urllib.request
    mc = os.path.expanduser("~/Work/mcusim/target/release/mcusim")
    rom = os.path.expanduser("~/Work/mcusim/third_party/bootrom/rp2350-A2/bootrom-combined.bin")
    if not os.path.exists(mc):
        return True, "skipped: no mcusim"
    cfg = os.path.join(os.environ.get("TMPDIR", "/tmp"), "btgw_sd_present.toml")
    with open(cfg, "w") as f:
        f.write('chip = "rp2350"\n[[device]]\ntype = "button"\nname = "sd_detect"\npin = 22\n'
                'released = "low"\nstdin = false\n')
    notes = []
    for cfgname in ("pico2w_sd", "pico2w_sdspi"):
        elf = f"{BUILD_PREFIX}{cfgname}/btgw.elf"
        if not os.path.exists(elf):
            return False, f"{cfgname} not built (run with --tier 1)"
        p = subprocess.Popen([mc, "--bootrom", rom, "-c", cfg, "--set", "boot=direct", "--rtt", "stdio",
                              "--usb-host", "net", "--usb-net-forward", "18081:80", "-t", "120s", elf],
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            st = None
            t0 = time.time()
            while time.time() - t0 < 120:
                try:
                    st = _j.load(urllib.request.urlopen("http://127.0.0.1:18081/api/sd", timeout=5))
                    if st["worker"]["card_inits"] >= 3:
                        break
                except Exception:
                    pass
                time.sleep(2)
            if not st:
                return False, f"{cfgname}: /api/sd never answered"
            w = st["worker"]
            if st["state"] != "error" or w["slices_over_10ms"] or w["card_inits"] < 3:
                return False, (f"{cfgname}: state {st['state']}, inits {w['card_inits']}, "
                               f"slices over 10 ms {w['slices_over_10ms']} (max {w['slice_max_us']} us)")
            notes.append(f"{cfgname} {st['bus']['type']} max slice {w['slice_max_us']} us")
        finally:
            p.send_signal(signal.SIGTERM)
            try:
                p.wait(10)
            except Exception:
                p.kill()
    return True, "; ".join(notes)


def _kb(v):
    """picotool sizes: "864K", "2048K", "64K"."""
    v = str(v).strip().upper()
    return int(v[:-1]) * 1024 if v.endswith("K") else int(v)


def t_ota_partition_layout():
    """partitions.json, fs.h and the linker script agree on the flash map.

    The ROM boots from the table, fs.c reads at FS_FLASH_OFF, and the linker
    caps the image at one slot; three copies of the same numbers, so they are
    checked against each other rather than trusted."""
    import json
    pt = json.load(open(os.path.join(ROOT, "partitions.json")))["partitions"]
    by = {p["name"]: p for p in pt}
    a, b, w, fs = by["Main A"], by["Main B"], by["Wi-Fi Firmware"], by["LittleFS"]
    a0, asz, bsz = _kb(a["start"]), _kb(a["size"]), _kb(b["size"])
    if asz != bsz or asz % 4096 or a0 % 4096:
        return False, "A/B slots must be equal and sector aligned"
    if b.get("link") != ["a", 0]:
        return False, "Main B is not linked to Main A"
    w0, wsz = _kb(w["start"]), _kb(w["size"])
    if a0 + 2 * asz > w0:
        return False, "slots overlap the WiFi firmware partition"
    if w["id"].lower() != "0x776966696669726d":
        return False, "WiFi firmware partition id is not the SDK's"
    if w0 + wsz > _kb(fs["start"]):
        return False, "WiFi partition overlaps LittleFS"
    fsh = open(os.path.join(ROOT, "src/fs/fs.h")).read()
    if "#define FS_FLASH_OFF (2u * 1024u * 1024u)" not in fsh or _kb(fs["start"]) != 2 * 1024 * 1024:
        return False, "LittleFS partition start != FS_FLASH_OFF"
    if _kb(fs["size"]) != 2 * 1024 * 1024 - 16 * 1024:
        return False, "LittleFS partition size != FS_SIZE (2 MiB - BT NVM tail)"
    ld = open(os.path.join(ROOT, "linker/pico_flash_region.ld")).read()
    m = re.search(r"LENGTH\s*=\s*(\d+)\s*\*\s*1024", ld)
    if not m or int(m.group(1)) * 1024 != asz:
        return False, "linker FLASH LENGTH != slot size"
    if "FS_READ_BASE" not in open(os.path.join(ROOT, "src/fs/fs.c")).read():
        return False, "fs.c reads flash via XIP_BASE: faults past the translated slot"
    return True, f"A/B {asz // 1024} KB @ {a0 // 1024} KB, WiFi {wsz // 1024} KB, LittleFS untouched"


def t_ota_keys():
    """Only the public key is in the tree; nothing tracked holds private material."""
    hdr = open(os.path.join(ROOT, "src/ota/ota_pubkey.h")).read()
    n = len(re.findall(r"0x[0-9a-fA-F]{2}", hdr))
    if n != 64:
        return False, f"ota_pubkey.h has {n} bytes, want 64"
    gi = open(os.path.join(ROOT, ".gitignore")).read()
    if "*.pem" not in gi or "keys/" not in gi:
        return False, ".gitignore lacks *.pem / keys/"
    import subprocess
    files = subprocess.run(["git", "ls-files"], cwd=ROOT, capture_output=True, text=True).stdout.split()
    for f in files:
        try:
            data = open(os.path.join(ROOT, f), "rb").read()
        except OSError:
            continue
        if b"PRIVATE " + b"KEY-----" in data:   # split so this file is not a hit
            return False, f"{f} contains private key material"
    return True, "64-byte public key; no PEM tracked"


def t_ota_flash_from_loop_only():
    """SMP handlers write flash, so the UDP receive callback must only copy.

    The HTTP PUT path writes flash inside an lwIP callback and http.c records
    what that costs; the OTA path was designed not to, and one call slipping
    into on_rx would silently undo that."""
    src = open(os.path.join(ROOT, "src/ota/smp_udp.c")).read()
    body = _fn_body(src, "on_rx")
    if body is None:
        return False, "smp_udp.c has no on_rx()"
    for bad in ("smp_process", "fs_", "rom_flash", "ota_slot", "flash_"):
        if bad in body:
            return False, f"on_rx() calls {bad}: flash work in the lwIP callback"
    return True, "on_rx copies; ota_tick processes"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tier", type=int, default=0)
    ap.add_argument("--sim", action="store_true",
                    help="also the mcusim differential test: our SDIO driver vs carlk3's (test/sdio_diff)")
    a = ap.parse_args()

    step(0, "webapp + model unit tests", t_unit)
    step(0, "script.loaded reports what the arena holds", t_script_loaded_reports_arena)
    step(0, "no device-specific code in the core", t_no_device_in_core)
    step(0, "protocol binding stays a thin wrapper", t_proto_binding_is_thin)
    step(0, "device registry stays transport-neutral", t_registry_transport_neutral)
    step(0, "frozen protocol vectors", t_parity)
    step(0, "neg constants agree with the Lua profile", t_neg_constants_agree)
    step(0, "protocol Lua on a host", t_lua_proto)
    step(0, "the Lua arena, invariants and fuzz", t_lua_heap)
    step(0, "gzip inflate matches zlib, rejects corrupt input", t_inflate)
    step(0, "flash_safe_execute is never nested (interrupts stay on)", t_no_nested_flash_safe)
    step(0, "the wait/wake contract and its races", t_lua_wait)
    step(0, "the Lua->JSON encoder stays inside its buffer", t_lua_json)
    step(0, "wireguard is never the default route", t_wireguard_is_never_default)
    step(0, "the SoftAP is independent of the station", t_wifi_ap_is_independent)
    step(0, "wifi mode both exists everywhere", t_wifi_mode_both_everywhere)
    step(0, "only uplink.c sets the default route", t_only_uplink_sets_default)
    step(0, "uplink metrics and policy from scripted pings", t_uplink_policy)
    step(0, "WireGuard egress LOCAL CHANGE in place and recorded", t_wg_tx_netif_local_change)
    step(0, "mDNS record groups: encoding and answering", t_mdns_records)
    step(0, "vendored lwIP mDNS carries only the hook", t_lwip_mdns_vendored)
    step(0, "a script's mDNS declarations go when it stops", t_lua_net_released)
    step(0, "the lwIP timeout pool covers mDNS on every netif", t_sys_timeout_pool_covers_mdns)
    step(0, "Ethernet config: bad -> DHCP client, link-local timing", t_eth_cfg)
    step(0, "Ethernet DHCP-server mode is only ever explicit", t_eth_server_mode_only_explicit)
    step(0, "forwarding: sessions, fail fast, backpressure", t_fwd_core)
    step(0, "a script's forwarding rules go when it stops", t_fwd_released_with_script)
    step(0, "wg_is_confirmed is declared, reported and consulted", t_wg_confirmed_exists)
    step(0, "DHCP servers bind their own address", t_dhcp_binds_specific_ip)
    step(0, "OTA flash layout is consistent", t_ota_partition_layout)
    step(0, "OTA keys: public in tree, private out", t_ota_keys)
    step(0, "SMP writes flash from the loop only", t_ota_flash_from_loop_only)
    step(0, "anker model schemas match their filenames", t_anker_model_schemas)
    step(0, "SPI bus: claims, chains, completes from the loop", t_spibus)
    step(0, "SD CRCs (CRC7, CRC16, 4-line CRC16 as one CRC64)", t_sd_crc)
    step(0, "SD card on SDIO and on SPI vs a simulated card", t_sd_card)
    step(0, "SD stack (worker, API, FatFs) on both buses + fsck", t_sd_stack)
    step(0, "SD rules: worker-only, trailing-clock releases, pins, no GPL", t_sd_guards)
    step(0, "W5500 MACRAW state machine vs a simulated chip", t_w5500)
    step(0, "SPI only through the bus layer", t_spi_only_through_spibus)
    step(0, "the cyw43 wait hook stays USB-only", t_cyw43_wait_stays_usb_only)
    step(0, "W5500 pins are reserved", t_w5500_pins_reserved)

    if a.tier >= 1:
        # Both configs, because a compile gate that is never exercised rots:
        # a module that only ever builds with its neighbours enabled is a
        # module that will not build when you need it off.
        step(1, "build pico2w (everything)", build("pico2w"))
        step(1, "build pico2w_minimal (no wifi/lua/io/anker)", build("pico2w_minimal"))
        # The combination neither of the other two reaches: Lua present, the
        # device codec absent. Without it the #if in lbind.c that skips
        # lbind_proto_open is never once compiled out.
        step(1, "build pico2w_noanker (lua, no device codec)", build("pico2w_noanker"))
        # The W5500 is opt-in (default n, like WiFi): this is the only build
        # that compiles it, so it is the only thing keeping it compiling.
        step(1, "build pico2w_eth (pico2w + W5500 Ethernet)", build("pico2w_eth"))
        step(1, "W5500 off means none of its code is linked", t_w5500_symbols)
        # The SD card is opt-in and no board has the slot yet: these two
        # builds are what keep both buses compiling.
        step(1, "build pico2w_sd (pico2w_eth + SD card on SDIO)", build("pico2w_sd"))
        step(1, "build pico2w_sdspi (pico2w_eth + SD card on SPI, shared bus)", build("pico2w_sdspi"))
        step(1, "SD off means none of its code is linked", t_sd_symbols)

    if a.sim:
        step(3, "SDIO differential test in mcusim (ours vs carlk3)", t_sdio_diff)
        step(3, "SD builds in mcusim: identify/fail/retry, slices, /api/sd", t_sd_gateway_sim)

    if a.tier >= 2:
        step(2, "webapp renders and panes can be selected", t_webapp_dom)
        step(2, "webapp end to end (Playwright, mock device)", t_webapp_e2e)

    print()
    if FAILS:
        print("FAILED: " + ", ".join(FAILS))
        sys.exit(1)
    print("all checks passed")

if __name__ == "__main__":
    main()
