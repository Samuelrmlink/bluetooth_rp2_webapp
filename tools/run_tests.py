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
    """The negotiation keystream lives in the Anker PEX module. The C parity
    test keeps a fixture copy so it can prove the generalised GHASH still
    matches the tags the Anker-specific original produced. Two copies can
    drift, and a drift here would be invisible until a charger stopped
    answering, so assert they are identical.

    The former Lua profile is gone. A guard that quietly stops matching
    anything is worse than no guard, so a missing file fails rather than skips."""
    import pathlib
    fix = pathlib.Path(ROOT, "test/host/neg_fixture.h")
    wire = pathlib.Path(ROOT, "pex/modules/drivers/anker_wire.c")
    if not fix.exists() or not wire.exists():
        return False, "fixture or anker_wire.c missing"
    ftxt, wtxt = fix.read_text(), wire.read_text()

    def c_hex(txt, name):
        m = re.search(re.escape(name) + r"\s*\[[^\]]*\]\s*=\s*\{(.*?)\};", txt, re.S)
        if not m:
            return None
        return "".join(b.lower() for b in re.findall(r"0x([0-9a-fA-F]{2})", m.group(1)))

    for fname, wname in (("NEG_KEYSTREAM", "KEYSTREAM"), ("NEG_GHASH_H", "GHASH_H"),
                         ("NEG_EJ0", "EJ0"), ("ANKER_GCM_AAD", "AAD")):
        a, b = c_hex(ftxt, fname), c_hex(wtxt, wname)
        if not a or not b:
            return False, f"could not read {fname} / {wname}"
        if a != b:
            return False, f"{fname} != anker_wire.c {wname}"
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
      never from cyw43_wait.c (the radio's wait hook pumps USB only), and
      nothing outside src/sd calls FatFs, the card driver or the store;
    - the SPI transport ends every transaction with spibus_release_trailing
      (the card drives MISO until a clock with CS high) and never releases
      the bus any other way;
    - its pins are reserved in the pin broker (SDIO lines and card detect);
    - no carlk3 / ZuluSCSI SDIO source in this MIT tree (GPL-3.0; it may
      only ever be fetched into a test build, test/sdio_diff)."""
    w = open(os.path.join(ROOT, "src/net/cyw43_wait.c")).read()
    if re.search(r"\bsd_(poll|core|card|api)|sdw_|sdio_", w):
        return False, "cyw43_wait.c reaches SD code"
    # ... and the main context (HTTP, ops, main.c) only posts jobs: no FatFs,
    # card driver or store call outside src/sd, where it could run long.
    rc, mc = sh("grep -rnE '\\b(f_(open|close|read|write|lseek|sync|mount|mkfs|getfree|opendir|readdir|"
                "closedir|stat|unlink|rename|mkdir|expand)|sd_card_(init|read|write|sync)|ls_[a-z_]+)\\(' "
                "src --include=*.c | grep -v '^src/sd/'")
    if mc.strip():
        return False, f"card or FatFs call outside the worker: {mc.splitlines()[0][:70]}"
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

def t_wifi_radio_window():
    """A lost station can rejoin while Bluetooth holds links -- by a bounded
    radio window, never by joining under BLE.

    begin_join() must still refuse while le_radio_busy() (a join under BLE
    wedges the driver); the window must actually free the radio
    (le_release_radio), hold new BLE scans/connects off while open
    (wifi_bt_hold), be bounded (WIFI_WIN_MAX_MS) and close on success and on
    loss. Without it a gateway that keeps the C1000 connected could never
    rejoin a different AP (2026-10-15)."""
    src = open(os.path.join(ROOT, "src/net/wifi.c")).read()
    body = _fn_body(src, "begin_join")
    if body is None or "le_radio_busy()" not in body:
        return False, "begin_join() no longer refuses while Bluetooth has the radio"
    if "le_release_radio(" not in (_fn_body(src, "win_open") or ""):
        return False, "win_open() does not release the radio"
    i = src.find("bool wifi_bt_hold(void)")
    if i < 0 or "win_start" not in src[i:src.find("\n}\n", i)]:
        return False, "wifi_bt_hold() does not hold Bluetooth off during a radio window"
    if "WIFI_WIN_MAX_MS" not in src or "win_close(" not in (_fn_body(src, "on_up") or ""):
        return False, "the radio window is unbounded or never closes on success"
    if "win_close(" not in (_fn_body(src, "on_lost") or ""):
        return False, "the radio window does not close when the join fails"
    if "no interface up; re-enabling the station" not in src:
        return False, "the no-interface safety net is gone: scans can fail forever with the AP down and the station parked"
    # coex "live" (2026-10-15): joins with links open, but a BLE scan still
    # blocks; it is a saved, opt-in setting -- window stays the default.
    if "le_scanning()" not in body:
        return False, "begin_join() no longer waits out a BLE scan in coex live"
    cfg = open(os.path.join(ROOT, "src/config/config.c")).read()
    if not re.search(r'static\s+char\s+coex\[\d+\]\s*=\s*"window"', cfg):
        return False, "coex live became the default without a decision"
    i = cfg.find("static bool persist(void)")
    if i < 0 or '\\"coex\\"' not in cfg[i:cfg.find("\n}\n", i)]:
        return False, "the coex setting is no longer saved in cfg/wifi.json"
    if "cfg_coex()" not in src:
        return False, "wifi.c does not read the saved coex setting"
    c = open(os.path.join(ROOT, "src/bt/le/central.c")).read()
    if "int le_release_radio(" not in c:
        return False, "central.c has no le_release_radio()"
    return True, "joins never run under BLE; a bounded window frees the radio and holds BLE off"


def t_bond_slots():
    """Bonds live in any of le_device_db_max_count() slots, with holes after
    a removal. Walking 0..le_device_db_count() (the number of VALID entries)
    listed an empty slot as 00:00:00:00:00:00 type 254 and hid the real bond
    above it, so it could not be removed either (2026-10-15)."""
    src = open(os.path.join(ROOT, "src/bt/le/bond.c")).read()
    if re.search(r"=\s*le_device_db_count\(\)", src):
        return False, "bond.c bounds a slot walk by le_device_db_count()"
    if src.count("le_device_db_max_count()") < 2 or "bond_addr_real(" not in src:
        return False, "bond list/remove do not walk every slot and skip empty ones"
    if "BD_ADDR_TYPE_UNKNOWN" not in src:
        return False, "empty bond slots are not rejected"
    return True, "every bond slot walked, empty ones skipped"

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
    taken before the module runs (lua_pcall) and can reuse src_buf, and the FNV-1a constants match
    the 32-bit standard the browser side implements."""
    c = open(os.path.join(ROOT, "src/lua/lrt.c")).read()
    o = open(os.path.join(ROOT, "src/lua/lops.c")).read()
    if '"script.loaded"' not in o:
        return False, "script.loaded is not in the op table"
    if "s->src_hash = hash" not in c or "module_set_src(name, tier, hash)" not in c:
        return False, "a load path no longer records what it compiled"
    req = c[c.index("static int l_require"):]
    if req.index("module_identity(name, &tier, &hash, &stored)") > req.index("lua_pcall(Ls, 0, 1, 0)"):
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
    """The Anker product tables live in the PEX module.

    The former Lua loader did require('profiles.anker.models.' .. product_code).
    That tree is gone. A1761 and A2687 have to stay named in the C tables, and
    only A2687 is marked verified. The byte checks are host_test.c."""
    import pathlib
    h = pathlib.Path(ROOT, "pex/modules/drivers/anker_models.h").read_text()
    c = pathlib.Path(ROOT, "pex/modules/drivers/anker_models.c").read_text()
    for ident in ("ANKER_MODEL_A1761", "ANKER_MODEL_A2687"):
        if ident not in h:
            return False, f"{ident} missing from anker_models.h"
    for code in ('"A1761"', '"A2687"'):
        if code not in c:
            return False, f"{code} missing from anker_models.c"
    if "SET_A1761" not in c or "SET_A2687" not in c:
        return False, "setter tables missing"
    if "return id == ANKER_MODEL_A2687" not in c:
        return False, "verified is not A2687-only"
    return True, "A1761 and A2687 tables present; A2687 is the verified model"

def t_anker_driver_host():
    """Compile and run the pure Anker and Pinecil driver checks. These are
    the vectors that used to load the Lua profile."""
    binp = os.path.join(os.environ.get("TMPDIR", "/tmp"), "btgw_anker_host")
    srcs = " ".join([
        "pex/modules/drivers/host_test.c",
        "pex/modules/drivers/anker_adv.c",
        "pex/modules/drivers/anker_wire.c",
        "pex/modules/drivers/anker_models.c",
        "pex/modules/drivers/pinecil_live.c",
        "src/codec/anker_tlv.c",
        "src/crypto/ghash.c",
    ])
    rc, out = sh(f"gcc -O1 -Wall -Wno-unused-function "
                 f"-Ipex/modules/drivers -Isrc -Isrc/codec -Isrc/crypto "
                 f"-o {binp} {srcs} -lm")
    if rc:
        errs = [l for l in out.splitlines() if "error:" in l]
        return False, errs[0][:120] if errs else "host_test build failed"
    rc, out = sh(binp)
    if rc:
        bad = next((l for l in out.splitlines() if l.startswith("FAIL") or l.endswith("failed")), "")
        return False, bad[:120] or "host_test failed"
    return True, (out.strip().splitlines() or ["ok"])[-1][:80]

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
    rc, out = sh(f"gcc -O1 -Wall -Wextra -Isrc -DLHEAP_KB=64 -DLHEAP_POOL_KB=40 -o {binp} "
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

def t_lua_json_decode():
    """json.decode against the real vendored Lua: every value type, json.null,
    escapes and surrogate pairs, integer/float, bounded nesting (it recurses
    on core0's 4 KB stack), malformed input of every kind -- each read from a
    buffer that ends on a PROT_NONE page, so a decoder that looks one byte past
    the input faults -- and round trips through the encoder."""
    binp = os.path.join(os.environ.get("TMPDIR", "/tmp"), "btgw_json_decode")
    rc, out = sh(f"gcc -O1 -w -DMAKE_LIB=1 -Isrc -Ithird_party/lua -o {binp} "
                 f"test/host/json_decode_test.c src/lua/ljson.c src/api/json_escape.c "
                 f"third_party/lua/onelua.c -lm")
    if rc:
        errs = [l for l in out.splitlines() if "error:" in l]
        return False, errs[0][:100] if errs else "json_decode harness build failed"
    rc, out = sh(binp)
    if rc != 0:
        first = next((l.strip() for l in out.splitlines() if "FAIL" in l), "")
        return False, first[:100] or f"json_decode_test exited {rc}"
    return True, out.strip().splitlines()[-1]

def t_fs_batch():
    """Path lists for a chosen tar or a batch delete: JSON paths (escapes,
    the 32-path cap, a path that does not fit), which roots cover a path,
    and a pump that stops when the connection has closed."""
    return _host_c("fs_batch", ["test/host/fs_batch_test.c", "src/fs/fs_batch.c"],
                   "btgw_fs_batch", defs="")

def t_lfile_path():
    """The Lua file module's volume-qualified paths: lfs:/ and sd:/, slash
    normalisation, each volume's character set, the 64/128 limits, '.'/'..',
    empty segments and an embedded NUL."""
    return _host_c("lfile_path", ["test/host/lfile_path_test.c", "src/lua/lfile_path.c"],
                   "btgw_lfile_path", defs="")

def t_httpc_parse():
    """The HTTP client's response parser (Content-Length, chunked with
    extensions and trailers, read-until-close, 1xx, HEAD/204, every error),
    each response fed whole, byte by byte and split at every offset; and URL
    parsing / redirect Location resolution."""
    return _host_c("httpc_parse", ["test/host/httpc_parse_test.c", "src/net/httpc_parse.c"],
                   "btgw_httpc_parse", defs="")

def t_tls_mem():
    """The firmware's mbedTLS configuration against a local `openssl s_server`
    (127.0.0.1 only), set up as httpc.c does (restartable ECC, the roots held
    only while the server's chain is verified), with ALL SEVEN embedded roots
    loaded -- so a root that does not parse fails here, not on a board (GSR1
    did, until SHA-1 was enabled). Chains: Google's (P-256 leaf <- WE2-like
    P-256 intermediate signed ecdsa-with-SHA384 <- a P-384 root, and that root
    cross-signed by an RSA root); RSA-4096/SHA-384 root -> RSA-2048
    intermediate -> ECDSA or RSA leaf; untrusted refused; insecure accepted.
    Three sessions per case in one process, and the heap must be back where it
    started after each: what the Lua arena must have free, and that nothing
    leaks between sessions."""
    import tempfile, signal, socket
    if sh("command -v openssl")[0]:
        return True, "skipped: no openssl"
    m = os.path.expanduser("~/.pico-sdk/sdk/2.3.0/lib/mbedtls")
    binp = os.path.join(os.environ.get("TMPDIR", "/tmp"), "btgw_tls_mem")
    rc, out = sh(f"gcc -O1 -w -DMBEDTLS_CONFIG_FILE='\"{ROOT}/include/mbedtls_config.h\"' "
                 f"-I{m}/include -I{m}/library -o {binp} test/host/tls_mem_test.c {m}/library/*.c")
    if rc:
        return False, "tls harness build failed"
    d = tempfile.mkdtemp(prefix="btgw_tls_")
    o = lambda c: sh(c, cwd=d)
    try:
        real = []
        for f in sorted(os.listdir(os.path.join(ROOT, "third_party/ca_roots"))):
            if f.endswith(".pem"):
                o(f"openssl x509 -in {ROOT}/third_party/ca_roots/{f} -outform der -out real_{f[:-4]}.der")
                real.append(f"real_{f[:-4]}.der")
        o("printf 'basicConstraints=critical,CA:TRUE\nkeyUsage=critical,keyCertSign,cRLSign\n' > root.ext")
        o("printf 'basicConstraints=critical,CA:TRUE,pathlen:0\nkeyUsage=critical,keyCertSign,digitalSignature\n' > ca.ext")
        o("printf 'subjectAltName=DNS:localhost\nextendedKeyUsage=serverAuth\n' > leaf.ext")
        # RSA-shaped
        o("openssl req -x509 -newkey rsa:4096 -sha384 -days 3650 -nodes -keyout r.key -out r.pem "
          "-subj '/CN=Test Root R1' -addext basicConstraints=critical,CA:TRUE -addext keyUsage=critical,keyCertSign")
        o("openssl req -newkey rsa:2048 -nodes -keyout i.key -out i.csr -subj '/CN=Test WR2'")
        o("openssl x509 -req -in i.csr -CA r.pem -CAkey r.key -CAcreateserial -days 900 -sha256 -extfile ca.ext -out i.pem")
        o("openssl req -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -keyout le.key -out le.csr -subj /CN=localhost")
        o("openssl x509 -req -in le.csr -CA i.pem -CAkey i.key -CAcreateserial -days 90 -sha256 -extfile leaf.ext -out le.pem")
        o("openssl req -newkey rsa:2048 -nodes -keyout lr.key -out lr.csr -subj /CN=localhost")
        o("openssl x509 -req -in lr.csr -CA i.pem -CAkey i.key -CAcreateserial -days 90 -sha256 -extfile leaf.ext -out lr.pem")
        # Google-shaped: P-384 root, cross-signed by an RSA root; P-256 intermediate (SHA-384); P-256 leaf
        o("openssl ecparam -name secp384r1 -genkey -noout -out r4.key")
        o("openssl req -x509 -new -key r4.key -sha384 -days 3650 -subj '/O=Test Trust LLC/CN=Test Root R4' "
          "-addext basicConstraints=critical,CA:TRUE -addext keyUsage=critical,keyCertSign,cRLSign -out r4.pem")
        o("openssl req -x509 -newkey rsa:2048 -nodes -keyout gs.key -sha256 -days 3650 -subj '/CN=Test GlobalSign' "
          "-addext basicConstraints=critical,CA:TRUE -addext keyUsage=critical,keyCertSign -out gs.pem")
        o("openssl req -new -key r4.key -subj '/O=Test Trust LLC/CN=Test Root R4' -out r4.csr")
        o("openssl x509 -req -in r4.csr -CA gs.pem -CAkey gs.key -CAcreateserial -days 1000 -sha256 -extfile root.ext -out r4x.pem")
        o("openssl ecparam -name prime256v1 -genkey -noout -out we.key")
        o("openssl req -new -key we.key -subj '/CN=Test WE2' -out we.csr")
        o("openssl x509 -req -in we.csr -CA r4.pem -CAkey r4.key -CAcreateserial -days 900 -sha384 -extfile ca.ext -out we.pem")
        o("openssl x509 -req -in le.csr -CA we.pem -CAkey we.key -CAcreateserial -days 90 -sha256 -extfile leaf.ext -out lg.pem")
        o("cat we.pem r4x.pem > gchain.pem")
        o("openssl req -x509 -newkey rsa:2048 -nodes -keyout x.key -out x.pem -subj /CN=Other -days 30")
        for n in ("r", "r4", "gs", "x"):
            o(f"openssl x509 -in {n}.pem -outform der -out {n}.der")
        o("head -c 200000 /dev/urandom > big.bin")
        R = ",".join(real)
        cases = [("Google-shaped ECDSA chain (+RSA cross-sign)", "lg.pem", "le.key", "gchain.pem", f"r4.der,{R}", "", 0),
                 ("the same via the RSA cross-sign only", "lg.pem", "le.key", "gchain.pem", f"gs.der,{R}", "", 0),
                 ("RSA chain, ECDSA leaf", "le.pem", "le.key", "i.pem", f"r.der,{R}", "", 0),
                 ("RSA chain, RSA leaf", "lr.pem", "lr.key", "i.pem", f"r.der,{R}", "", 0),
                 ("untrusted root refused", "le.pem", "le.key", "i.pem", f"x.der,{R}", "", 1),
                 ("untrusted root, insecure", "le.pem", "le.key", "i.pem", "x.der", "insecure", 0)]
        peaks = []
        for what, cert, key, chain, root, extra, want in cases:
            s0 = socket.socket(); s0.bind(("127.0.0.1", 0)); port = s0.getsockname()[1]; s0.close()
            args = ["openssl", "s_server", "-accept", f"127.0.0.1:{port}", "-cert", cert, "-key", key,
                    "-WWW", "-tls1_2", "-quiet"] + (["-cert_chain", chain] if chain else [])
            srv = subprocess.Popen(args, cwd=d, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            try:
                time.sleep(0.6)
                p = subprocess.run([binp, str(port), root, "big.bin"] + ([extra] if extra else []),
                                   cwd=d, capture_output=True, text=True, timeout=120, env=dict(os.environ, SESSIONS="3"))
            finally:
                srv.send_signal(signal.SIGTERM); srv.wait()
            if (p.returncode != 0) != bool(want):
                return False, f"{what}: {(p.stdout.strip().splitlines() or ['?'])[-1][:70]}"
            left = re.findall(r"(\d+) B left after free", p.stdout)
            if len(left) != 3 or any(int(x) for x in left):
                return False, f"{what}: heap not back to baseline after each session ({left})"
            peaks += [int(x) for x in re.findall(r"(\d+) B handshake peak", p.stdout)]
        return True, f"6 cases x 3 sessions, heap back to 0 each time; peak {max(peaks)} B (64-bit host)"
    finally:
        shutil.rmtree(d, ignore_errors=True)

def t_lfile_sd():
    """The Lua file module's SD side end to end on a host: lfile.c's yields
    and continuations -> sd_jobs -> the storage worker -> FatFs -> SDIO -> a
    simulated card. Every call and error code, writes and listings larger
    than the job buffer, four handles, <close>, a script stopped mid-job,
    the card pulled under an open handle, SD from a handler refused."""
    binp = os.path.join(os.environ.get("TMPDIR", "/tmp"), "btgw_lfile_sd")
    srcs = [s for s in SD_STACK_SRCS] + ["src/lua/lfile.c", "src/lua/lfile_path.c", "src/lua/lheap.c",
                                          "third_party/lua/onelua.c"]
    rc, out = sh(f"gcc -O1 -g -w -no-pie -DSPIBUS_HOST -DCONFIG_SD=1 -DCONFIG_SD_DETECT=22 -DMAKE_LIB=1 "
                 f"-DLHEAP_KB=64 -DFS_FLASH_OFF=0 -Isrc -Isrc/fs -Ithird_party/fatfs -Ithird_party/lua "
                 f"-Itest/host -Itest/host/stub -o {binp} test/host/lfile_sd_test.c {' '.join(srcs)} -lm")
    if rc:
        errs = [l for l in out.splitlines() if "error" in l]
        return False, errs[0][:100] if errs else "lfile_sd build failed"
    rc, out = sh(binp)
    m = re.search(r"(\d+) checks, (\d+) failed", out)
    if not m or rc:
        first = next((l for l in out.splitlines() if l.startswith("FAIL")), out.strip()[-90:])
        return False, first[:100]
    return True, f"{m.group(1)} checks"

def t_lhttp_sd():
    """http.start{to_file="sd:/..."} on a host: lhttp.c's sink and SD state
    machine against the real jobs/worker/FatFs/card, a fake transport playing
    the server at the pace room() allows. Downloads byte for byte, a 404 not
    written, a failed transfer leaving no partial file, a stopped script."""
    binp = os.path.join(os.environ.get("TMPDIR", "/tmp"), "btgw_lhttp_sd")
    srcs = [s for s in SD_STACK_SRCS] + ["src/lua/lhttp.c", "src/lua/lfile.c", "src/lua/lfile_path.c",
                                          "src/lua/lheap.c", "src/lua/lwait.c", "third_party/lua/onelua.c"]
    rc, out = sh(f"gcc -O1 -g -w -no-pie -DSPIBUS_HOST -DCONFIG_SD=1 -DCONFIG_SD_DETECT=22 "
                 f"-DCONFIG_NET_HTTP_CLIENT=1 -DMAKE_LIB=1 -DLHEAP_KB=96 -DFS_FLASH_OFF=0 -Isrc -Isrc/fs "
                 f"-Ithird_party/fatfs -Ithird_party/lua -Itest/host -Itest/host/stub -o {binp} "
                 f"test/host/lhttp_sd_test.c {' '.join(srcs)} -lm")
    if rc:
        errs = [l for l in out.splitlines() if "error" in l]
        return False, errs[0][:100] if errs else "lhttp_sd build failed"
    rc, out = sh(binp)
    m = re.search(r"(\d+) checks, (\d+) failed", out)
    if not m or rc:
        first = next((l for l in out.splitlines() if l.startswith("FAIL")), out.strip()[-90:])
        return False, first[:100]
    return True, f"{m.group(1)} checks"

def t_ca_roots():
    """The HTTP client's embedded roots match the pinned SHA-256 fingerprints
    and src/net/ca_roots.h is what the PEMs generate."""
    rc, out = sh("python3 tools/gen_ca_roots.py --check")
    return rc == 0, out.strip().splitlines()[-1] if out.strip() else ""

def t_lua_proto():
    """Firmware codec and crypto bindings, on a host, against the vendored Lua.
    The Anker profile checks moved to pex/modules/drivers/host_test.c."""
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

def t_cl_sess():
    """Classic session rules with no radio: two owners, PIN list, security, filters."""
    return _host_c("cl_sess", ["test/host/cl_sess_test.c", "src/bt/classic/cl_sess.c"],
                   "btgw_cl_sess", defs="")

def t_classic_contract():
    """The Classic host API stays generic and is the group the requirements named.

    No PIN, Serial Port UUID, or channel is compiled into the driver. The
    PEX group is id 14 and can be tested with pex_avail. Opening a channel
    does not send printer traffic."""
    import json
    api = json.load(open(os.path.join(ROOT, "pex/host_api.json")))
    groups = [g for g in api["groups"] if g.get("name") == "classic"]
    if len(groups) != 1 or groups[0].get("id") != 14:
        return False, "pex/host_api.json needs one classic group, id 14"
    names = {f["name"] for f in groups[0].get("functions", [])}
    for need in ("pex_bt_classic_open", "pex_bt_classic_send", "pex_bt_classic_forget",
                 "pex_bt_classic_devices", "pex_bt_classic_sdp"):
        if need not in names:
            return False, f"classic group is missing {need}"
    banned = ("BATTERY?", '"0000"', '"1234"', "0x1101")
    for rel in ("src/bt/classic/cl_sess.c", "src/bt/classic/spp.c", "src/pex/pex_gw_classic.c"):
        text = open(os.path.join(ROOT, rel)).read()
        for s in banned:
            if s in text:
                return False, f"{rel} compiles in {s}"
    spp = open(os.path.join(ROOT, "src/bt/classic/spp.c")).read()
    if "classic_send" not in spp or "op_classic_disconnect" not in spp:
        return False, "classic.send / classic.disconnect are not on the same core"
    sd = open(os.path.join(ROOT, "configs/pico2w_sd_classic_defconfig")).read()
    if "CONFIG_BT_CLASSIC=y" not in sd or "CONFIG_NET_WIFI=y" not in sd or "CONFIG_NET_WIREGUARD=y" not in sd:
        return False, "the SD Classic image drops Classic, Wi-Fi, or WireGuard"
    if "# CONFIG_NET_HTTP_TLS is not set" not in sd:
        return False, "SD + Classic keeps the TLS pool, which misses the 8 KiB heap floor"
    if "# CONFIG_BT_CLASSIC_NAP is not set" not in sd:
        return False, "this Classic image turns NAP on"
    return True, "group 14, no built-in PIN, UUID, channel, or BATTERY?; SD image keeps Wi-Fi and WireGuard"

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

def t_spibus_share():
    """Sharing an SPI bus (docs/spi-bus-sharing.md): the bus layer's
    fairness (a refused device is in line, the releaser cannot reclaim
    ahead of it, interrupt wants, TTL), then a format-shaped SD workload
    with a W5500-like and an MCP2515-like device on the same bus -- every
    request granted within the bound, every job done, no CS overlap, no MISO
    fight, the card byte-exact."""
    if "spibus_contended" not in open(os.path.join(ROOT, "src/sd/sd_spi.c")).read():
        return True, "skipped: src/sd/sd_spi.c lacks the fairness points (docs/spi-bus-sharing.md, Gaps)"
    return _host_c("spibus_share", ["test/host/spibus_share_test.c"] + SD_SIM_SRCS, "btgw_spibus_share",
                   defs="-DSPIBUS_HOST -DCONFIG_SD_DETECT=22")

SD_STACK_SRCS = ["src/sd/sd_card.c", "src/sd/sd_sdio.c", "src/sd/sd_spi.c", "src/sd/sd_crc.c",
                 "src/sd/sd_store.c", "src/sd/sd_api.c", "src/sd/sd_core.c", "src/sd/sd_diskio.c",
                 "src/sd/sd_jobs.c",
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
    # -finstrument-functions on the firmware sources only: the worker's work
    # meter (test/host/sd_worker_uc.c) counts their calls between yields.
    rc, out = sh(f"gcc -O1 -g -Wall -Wextra -no-pie -finstrument-functions "
                 f"-finstrument-functions-exclude-file-list=test/host,/usr/ "
                 f"-DSPIBUS_HOST -DCONFIG_SD_DETECT=22 -DFS_FLASH_OFF=0 -Isrc -Ithird_party/fatfs "
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
    (pico2w_sd) and the SPI one on SPI0 (pico2w_sdspi) for real."""
    import json as _j, signal, urllib.request
    mc = os.path.expanduser("~/Work/mcusim/target/release/mcusim")
    rom = os.path.expanduser("~/Work/mcusim/third_party/bootrom/rp2350-A2/bootrom-combined.bin")
    if not os.path.exists(mc):
        return True, "skipped: no mcusim"
    cfg = os.path.join(os.environ.get("TMPDIR", "/tmp"), "btgw_sd_present.toml")
    with open(cfg, "w") as f:
        f.write('chip = "rp2350"\n[[device]]\ntype = "button"\nname = "sd_detect"\npin = 9\n'
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


def t_pinecil_bt_sim():
    """test/sim/pinecil_bt.py: the Pinecil PEX module against a mock iron
    on the blegatt GATT backend. Skips when that mcusim, or a development
    gateway image, is not on this machine. Does not touch the board."""
    def fn():
        mc = os.path.expanduser("~/Work/mcusim-agents/blegatt/target/release/mcusim")
        if not os.path.exists(mc):
            return True, "skipped: no blegatt mcusim"
        b = os.environ.get("BTGW_BUILD", "/tmp/btgw_sdcl_1m")
        if not os.path.exists(os.path.join(b, "btgw.elf")):
            return True, "skipped: no gateway image (set BTGW_BUILD)"
        rc, out = sh("python3 test/sim/pinecil_bt.py")
        m = re.search(r"(\d+) checks, (\d+) failed", out)
        if rc == 77:
            return True, "skipped"
        if not m:
            return False, (out.strip().splitlines() or ["no output"])[-1][:90]
        first = next((l for l in out.splitlines() if l.startswith("FAIL")), "")
        return rc == 0, f"{m.group(1)} checks" + (f", {m.group(2)} failed: {first[:60]}" if rc else "")
    return fn


def t_lua_net_fs_sim(radio):
    """test/sim/lua_net_fs.py: the Lua file/json/http/sys APIs and POST
    /api/op on the real firmware in mcusim (USB-NCM, local servers only), or
    with --radio the Wi-Fi uplink (SNTP, DNS, HTTP against mcusim's offline
    virtual AP) and a BLE advertiser (uuids128, age_ms)."""
    def fn():
        elf = f"{BUILD_PREFIX}pico2w/btgw.elf"
        if not os.path.exists(os.path.expanduser("~/Work/mcusim/target/release/mcusim")):
            return True, "skipped: no mcusim"
        if not os.path.exists(elf):
            return False, "pico2w not built (run with --tier 1)"
        rel = os.path.relpath(os.path.dirname(elf), ROOT)
        rc, out = sh(f"python3 test/sim/lua_net_fs.py {'--radio ' if radio else ''}{rel}")
        m = re.search(r"(\d+) checks, (\d+) failed", out)
        if rc == 77:
            return True, "skipped"
        if not m:
            return False, (out.strip().splitlines() or ["no output"])[-1][:90]
        first = next((l for l in out.splitlines() if l.startswith("FAIL")), "")
        return rc == 0, f"{m.group(1)} checks" + (f", {m.group(2)} failed: {first[:60]}" if rc else "")
    return fn


def _fs_h_defines():
    """The flash-layout numbers in src/fs/fs.h, evaluated (they are written
    as `(2u * 1024u * 1024u)` and in terms of each other)."""
    src = open(os.path.join(ROOT, "src/fs/fs.h")).read()
    defs = {}
    for name, expr in re.findall(r"^#define\s+(FS_[A-Z_]+)\s+(\(.*\))\s*$", src, re.M):
        e = re.sub(r"(\d+)u\b", r"\1", expr)
        for k, v in defs.items():
            e = re.sub(r"\b%s\b" % k, str(v), e)
        if re.fullmatch(r"[\d\s()*+\-/]+", e):
            defs[name] = eval(e)
    return defs


def t_lfs_migrate_sim():
    """test/sim/lfs_migrate.py: the pre-PEX LittleFS migration in mcusim
    (happy path, no card, full card, write errors, power cuts, TBYB). Needs a
    build of a firmware from before the PEX partition: BTGW_OLD_BUILD=<dir>
    (e.g. feature/lua-net-fs~4 = 1.180, pico2w_sd, -DBTGW_TBYB=OFF). The new
    image must be built with CONFIG_FS_MIGRATE=y; the Kconfig default is n."""
    old = os.environ.get("BTGW_OLD_BUILD")
    if not os.path.exists(os.path.expanduser("~/Work/mcusim/target/release/mcusim")):
        return True, "skipped: no mcusim"
    if not old or not os.path.exists(os.path.join(old, "btgw.elf")):
        return True, "skipped: set BTGW_OLD_BUILD to a pre-PEX firmware build"
    new = f"{BUILD_PREFIX}pico2w_sd"
    if not os.path.exists(os.path.join(new, "btgw.elf")):
        return False, "pico2w_sd not built (run with --tier 1)"
    rc, out = sh(f"python3 test/sim/lfs_migrate.py --old {old} --new {new} "
                 f"--work {BUILD_PREFIX}lfsmig --scenarios happy,nocard,full,writefail,powercut")
    m = re.search(r"(\d+) checks, (\d+) failed", out)
    if not m:
        return False, (out.strip().splitlines() or ["no output"])[-1][:90]
    first = next((l for l in out.splitlines() if l.startswith("FAIL")), "")
    return rc == 0, f"{m.group(1)} checks" + (f", {m.group(2)} failed: {first[5:65]}" if rc else "")


def t_pex_lua_unload():
    """A Lua-held module can be unloaded, and a later call is an error."""
    d = f"{BUILD_PREFIX}pexlua"
    os.makedirs(d, exist_ok=True)
    names = ("lapi.c lauxlib.c lbaselib.c lcode.c lcorolib.c lctype.c ldebug.c "
             "ldo.c ldump.c lfunc.c lgc.c llex.c lmathlib.c lmem.c lobject.c "
             "lopcodes.c lparser.c lstate.c lstring.c lstrlib.c ltable.c "
             "ltablib.c ltm.c lundump.c lutf8lib.c lvm.c lzio.c").split()
    lua = " ".join(f"{ROOT}/third_party/lua/{s}" for s in names)
    rc, out = sh(
        f"rm -f *.o pex_lua_unload && "
        f"python3 {ROOT}/pex/tools/gen_host_api.py header {ROOT}/pex/host_api.json pex_api.h && "
        f"gcc -O1 -w -I{ROOT}/third_party/lua -c {lua} && "
        f"gcc -O1 -Wall -Wextra -Werror -std=c11 "
        f"-I{ROOT}/pex/lua -I{ROOT}/pex/runtime -I{ROOT}/pex/sdk/include "
        f"-I{ROOT}/third_party/lua -I. "
        f"{ROOT}/pex/lua/pexlua.c {ROOT}/pex/test/unit/pex_lua_unload.c *.o -lm "
        f"-o pex_lua_unload && ./pex_lua_unload",
        cwd=d)
    if rc:
        lines = [l for l in out.splitlines() if l.strip()]
        return False, (lines[-1] if lines else "no output")[:90]
    last = [l for l in out.splitlines() if "checks" in l]
    return True, (last[-1] if last else "ok")[:90]


def t_pex_host():
    """pex/: the host API source is consistent and the portable runtime parts
    (json group, CRC-32, SHA-256) pass on the build host."""
    rc, out = sh("python3 pex/tools/gen_host_api.py check pex/host_api.json")
    if rc:
        return False, out.strip()[-90:]
    d = f"{BUILD_PREFIX}pexunit"
    os.makedirs(d, exist_ok=True)
    rc, out = sh(f"python3 pex/tools/gen_host_api.py header pex/host_api.json {d}/pex_api.h && "
                 f"gcc -O1 -Wall -Wextra -Werror -Ipex/runtime -Ipex/sdk/include -I{d} pex/test/unit/pex_unit.c "
                 f"pex/runtime/groups/pex_json.c pex/runtime/pex_hash.c -o {d}/pex_unit && {d}/pex_unit")
    if rc:
        return False, out.strip().splitlines()[-1][:90] if out.strip() else ""
    # XIP: install/uninstall/compaction over a simulated flash, a power cut at every operation
    groups = ("core", "log", "mem", "sched", "pex", "json")
    tables = " && ".join(f"python3 pex/tools/gen_host_api.py table pex/host_api.json {g} {d}/pex_host_{g}.c" for g in groups)
    rt = " ".join(f"pex/runtime/{f}" for f in ("pex_core.c", "pex_load.c", "pex_mem.c", "pex_sched.c", "pex_hash.c",
                                               "pex_xip.c", "groups/pex_json.c"))
    rc, out2 = sh(f"{tables} && gcc -O1 -Wall -Wextra -DPEX_FORCE_TARGET=2 -DPEX_FORCE_FEATURES=5 -DPEX_SD_READ_TEST "
                  f"-Ipex/runtime -Ipex/sdk/include -Isrc/pex -I{d} pex/test/unit/pex_xip_unit.c {rt} "
                  f"src/pex/pex_sd_read.c "
                  + " ".join(f"{d}/pex_host_{g}.c" for g in groups) + f" -o {d}/pex_xip_unit && {d}/pex_xip_unit")
    last = [l for l in out2.strip().splitlines() if "checks passed" in l]
    return rc == 0, (out.strip().splitlines()[-1][:40] + "; xip " + (last[-1] if last else out2.strip()[-60:]))[:90]


def t_pex_sim():
    """pex/test/run_sim.py: the runtime alone (no Lua) on RP2350 and RP2040 in
    mcusim, both code models, plus pexlink's refusals."""
    if not os.path.exists(os.path.expanduser("~/Work/mcusim/target/release/mcusim")):
        return True, "skipped: no mcusim"
    rc, out = sh(f"python3 pex/test/run_sim.py --work {BUILD_PREFIX}pexsim")
    m = re.search(r"(\d+)/(\d+) passed", out)
    return rc == 0, m.group(0) if m else (out.strip().splitlines() or ["no output"])[-1][:90]


def t_ota_partition_layout():
    """partitions.json, fs.h and the linker script agree on the flash map.

    The ROM boots from the table, fs.c reads at FS_FLASH_OFF, and the linker
    caps the image at one slot; three copies of the same numbers, so they are
    checked against each other rather than trusted. LittleFS is FS_SIZE, PEX
    follows it, and the two together end 16 KB before the end of the 4 MB
    part (BTstack's NVM, unpartitioned). The bench volume is exactly 300
    blocks, so that size must not count as a legacy filesystem."""
    import json
    pt = json.load(open(os.path.join(ROOT, "partitions.json")))["partitions"]
    by = {p["name"]: p for p in pt}
    if "PEX" not in by:
        return False, "partitions.json has no PEX partition"
    a, b, w, fs, pex = by["Main A"], by["Main B"], by["Wi-Fi Firmware"], by["LittleFS"], by["PEX"]
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
    d = _fs_h_defines()
    for k in ("FS_FLASH_OFF", "FS_BT_TAIL", "FS_REGION_SIZE", "FS_PEX_SIZE", "FS_SIZE", "FS_PEX_OFF"):
        if k not in d:
            return False, f"fs.h: cannot read {k}"
    if _kb(fs["start"]) != d["FS_FLASH_OFF"] or d["FS_FLASH_OFF"] != 2368 * 1024:
        return False, "LittleFS partition start != FS_FLASH_OFF (2368 KiB)"
    if d["FS_SIZE"] != 1200 * 1024 or d["FS_SIZE"] // 4096 != 300:
        return False, "LittleFS must be 1200 KiB, 300 blocks, the volume on the board"
    if _kb(fs["size"]) != d["FS_SIZE"]:
        return False, f"LittleFS partition size {_kb(fs['size'])} != FS_SIZE {d['FS_SIZE']}"
    if _kb(pex["start"]) != d["FS_PEX_OFF"] or _kb(pex["size"]) != d["FS_PEX_SIZE"]:
        return False, "PEX partition != FS_PEX_OFF / FS_PEX_SIZE"
    if pex["id"].lower() in (fs["id"].lower(), w["id"].lower()) or pex.get("families") != ["data"]:
        return False, "PEX partition needs its own id and the data family"
    if d["FS_SIZE"] + d["FS_PEX_SIZE"] != d["FS_REGION_SIZE"] or \
            d["FS_FLASH_OFF"] + d["FS_REGION_SIZE"] + d["FS_BT_TAIL"] != 4 * 1024 * 1024:
        return False, "LittleFS + PEX must end 16 KB (BTstack NVM) before the end of flash"
    if d["FS_SIZE"] % 4096 or d["FS_PEX_SIZE"] % 4096:
        return False, "LittleFS / PEX not sector aligned"
    ld = open(os.path.join(ROOT, "linker/pico_flash_region.ld")).read()
    m = re.search(r"LENGTH\s*=\s*(\d+)\s*\*\s*1024", ld)
    if not m or int(m.group(1)) * 1024 != asz:
        return False, "linker FLASH LENGTH != slot size"
    fsc = open(os.path.join(ROOT, "src/fs/fs.c")).read()
    if "FS_READ_BASE" not in fsc:
        return False, "fs.c reads flash via XIP_BASE: faults past the translated slot"
    if "mount_as_found" not in fsc:
        return False, "fs.c formats on a geometry mismatch: the pre-PEX filesystem would be wiped"
    if "g_legacy = g_blocks > FS_SIZE / FS_BLOCK" not in fsc:
        return False, "a volume of exactly FS_SIZE blocks must not be treated as legacy"
    if w0 != a0 + 2 * asz:
        return False, "Wi-Fi firmware does not start where the two slots end"
    pages = d["FS_PEX_SIZE"] // 4096
    if pages != 128 or pages > 0x7FF:
        return False, "PEX window is not 128 pages, which is what the QMI size field holds"
    return True, (f"A/B {asz // 1024} KB @ {a0 // 1024} KB, WiFi {wsz // 1024} KB, LittleFS "
                  f"{d['FS_SIZE'] // 1024} KB @ {d['FS_FLASH_OFF'] // 1024} KB, "
                  f"PEX {d['FS_PEX_SIZE'] // 1024} KB @ {d['FS_PEX_OFF'] // 1024} KB")


def t_fs_layout():
    """The mover's addresses abut, a started move never formats 0x200000, and
    the picobin parser accepts partitions-next.json only.

    partitions.json, fs.h and the linker length stay on the 864 KiB / 2 MiB
    map. This image mounts the old volume until the SD state says otherwise,
    so those four copies are not switched here (t_ota_partition_layout)."""
    import tempfile
    fsc = open(os.path.join(ROOT, "src/fs/fs.c")).read()
    if "g_freeze_old" not in fsc or "not formatting (layout mover decides)" not in fsc:
        return False, "fs.c is missing the layout mount guard"
    hdr = open(os.path.join(ROOT, "src/fs/fs.h")).read()
    if "FS_FLASH_OFF (2u * 1024u * 1024u)" not in hdr:
        return False, "FS_FLASH_OFF is no longer 2 MiB"
    ld = open(os.path.join(ROOT, "linker/pico_flash_region.ld")).read()
    if "LENGTH = 864 * 1024" not in ld:
        return False, "linker FLASH length is no longer 864 KiB"
    picotool = os.environ.get("PICOTOOL") or \
        "/home/samuel/Work/bluetooth_rp2_webapp/build-bnep/_deps/picotool/picotool"
    if not os.access(picotool, os.X_OK):
        found = shutil.which("picotool")
        picotool = found if found and os.access(found, os.X_OK) else None
    if not picotool:
        return False, "picotool 2.3.0 not found"
    d = tempfile.mkdtemp(prefix="btgw_layout_")
    try:
        newb = os.path.join(d, "next.bin")
        oldb = os.path.join(d, "old.bin")
        rc, out = sh(f"{picotool} partition create partitions-next.json {newb}")
        if rc:
            return False, (out.strip().splitlines() or ["picotool next"])[-1][:90]
        rc, out = sh(f"{picotool} partition create partitions.json {oldb}")
        if rc:
            return False, (out.strip().splitlines() or ["picotool old"])[-1][:90]
        host = os.path.join(d, "host")
        rc, out = sh("gcc -std=c11 -Wall -Wextra -Werror -Isrc/fs "
                     f"tools/fs_layout_host.c src/fs/fs_layout_decide.c -o {host} && {host} {newb} {oldb}")
        if rc:
            return False, (out.strip().splitlines() or ["host"])[-1][:90]
        last = out.strip().splitlines()[-1]
        return True, last
    finally:
        shutil.rmtree(d, ignore_errors=True)


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
    step(0, "Classic sessions: owners, PIN, security, filters", t_cl_sess)
    step(0, "Classic host API stays generic", t_classic_contract)
    step(0, "protocol binding stays a thin wrapper", t_proto_binding_is_thin)
    step(0, "device registry stays transport-neutral", t_registry_transport_neutral)
    step(0, "frozen protocol vectors", t_parity)
    step(0, "neg constants agree with the Anker wire module", t_neg_constants_agree)
    step(0, "protocol Lua on a host", t_lua_proto)
    step(0, "the Lua arena, invariants and fuzz", t_lua_heap)
    step(0, "gzip inflate matches zlib, rejects corrupt input", t_inflate)
    step(0, "flash_safe_execute is never nested (interrupts stay on)", t_no_nested_flash_safe)
    step(0, "the wait/wake contract and its races", t_lua_wait)
    step(0, "the Lua->JSON encoder stays inside its buffer", t_lua_json)
    step(0, "json.decode: types, escapes, depth, malformed, bounds", t_lua_json_decode)
    step(0, "file module paths: volumes, charsets, limits", t_lfile_path)
    step(0, "batch tar and delete path lists", t_fs_batch)
    step(0, "file module on the SD card: binding -> jobs -> worker -> FatFs", t_lfile_sd)
    step(0, "HTTP client: response parser, URLs, redirects", t_httpc_parse)
    step(0, "http to_file onto the SD card: stage, write, close, remove", t_lhttp_sd)
    step(0, "HTTPS roots match their pinned fingerprints", t_ca_roots)
    step(0, "TLS config: real handshakes on 127.0.0.1, session memory", t_tls_mem)
    step(0, "wireguard is never the default route", t_wireguard_is_never_default)
    step(0, "the SoftAP is independent of the station", t_wifi_ap_is_independent)
    step(0, "WiFi rejoins under Bluetooth only through a radio window", t_wifi_radio_window)
    step(0, "bonds: every slot listed, empty slots skipped", t_bond_slots)
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
    step(0, "layout mover map, decide, and next partition table", t_fs_layout)
    step(0, "OTA keys: public in tree, private out", t_ota_keys)
    step(0, "SMP writes flash from the loop only", t_ota_flash_from_loop_only)
    step(0, "anker model tables name A1761 and A2687", t_anker_model_schemas)
    step(0, "anker and pinecil driver host checks", t_anker_driver_host)
    step(0, "SPI bus: claims, chains, completes from the loop", t_spibus)
    step(0, "SD CRCs (CRC7, CRC16, 4-line CRC16 as one CRC64)", t_sd_crc)
    step(0, "SD card on SDIO and on SPI vs a simulated card", t_sd_card)
    step(0, "SPI bus shared fairly: a format beside a W5500 and an MCP2515", t_spibus_share)
    step(0, "SD stack (worker, API, FatFs) on both buses + fsck", t_sd_stack)
    step(0, "SD rules: worker-only, trailing-clock releases, pins, no GPL", t_sd_guards)
    step(0, "W5500 MACRAW state machine vs a simulated chip", t_w5500)
    step(0, "SPI only through the bus layer", t_spi_only_through_spibus)
    step(0, "the cyw43 wait hook stays USB-only", t_cyw43_wait_stays_usb_only)
    step(0, "W5500 pins are reserved", t_w5500_pins_reserved)
    step(0, "PEX: host API source + portable runtime parts (json, CRC, SHA-256)", t_pex_host)
    step(0, "PEX: Lua unload notifies, detaches, and errors on use after", t_pex_lua_unload)

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
        step(1, "build pico2w_sdspi (pico2w_eth + SD card on SPI0, same pins)", build("pico2w_sdspi"))
        step(1, "SD off means none of its code is linked", t_sd_symbols)

    if a.sim:
        step(3, "SDIO differential test in mcusim (ours vs carlk3)", t_sdio_diff)
        step(3, "SD builds in mcusim: identify/fail/retry, slices, /api/sd", t_sd_gateway_sim)
        step(3, "Lua file/json/http/sys + /api/op in mcusim (USB-NCM)", t_lua_net_fs_sim(False))
        step(3, "Lua over Wi-Fi + BLE in mcusim: SNTP, DNS, uuids128", t_lua_net_fs_sim(True))
        step(3, "Pinecil PEX against a mock iron (blegatt GATT)", t_pinecil_bt_sim())
        step(3, "pre-PEX LittleFS migration in mcusim (SD copy, power cuts)", t_lfs_migrate_sim)
        step(3, "PEX runtime without Lua in mcusim: RP2350 + RP2040, both code models", t_pex_sim)

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
