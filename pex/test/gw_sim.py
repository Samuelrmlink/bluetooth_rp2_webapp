#!/usr/bin/env python3
"""PEX on the real gateway firmware, in mcusim -- the deploy image, the way it
will be deployed, then everything pex_remote_test.py does, then the fault
paths that must never be tried on the real board.

  pex/test/gw_sim.py --deploy <TBYB build> --old <slot-A build> --mods <module build>
                     [--release FILE.smp.bin] [--mcusim PATH] [--work DIR]
                     [--scenarios ota,suite,quiet,fault,hang]

The board is laid out as the real gateway is since 2026-10-21: partition
table with LittleFS 1776 KB + PEX 256 KB, the old firmware (feature/lfs-
migrate, 1.195) in slot A, slot B erased, a LittleFS holding cfg/ (Wi-Fi for
mcusim's AP, a throwaway WireGuard key), an SD card (exFAT), the CYW43 with
mcusim's virtual network and an iti-otap BLE peer.

  ota    smpmgr uploads the signed release into slot B, tests it, resets: the
         TBYB image must boot from slot B, then 'state-write --confirm' buys
         it. Checks LittleFS/SD/Wi-Fi/WireGuard came up and sys.info has fs.
  quiet  nothing PEX ran at boot (no pool taken, no module, no pex log).
  suite  pex/tools/pex_remote_test.py with --sd, --http-url (a server on the
         PC) and --bt (the iti-otap peer).
  fault  cfg/pex_autostart.json starts crashapp: the fault is attributed, the
         board reboots into the SAME image, the next boot skips the module.
  hang   the same with hangapp: a watchdog reset, the module named next boot.
  xip    power cuts (SIGKILL of the simulator: every flash operation is already
         in the flash file) during installs and during an overlapping
         compaction: after each restart the index is the old or the new one
         and every installed module verifies and runs in place; a resumed move.
  budget the Lua arena after boot, with the Pi Switch driver (iti_ble_lua)
         compiled and running, with a PEX launcher script, after a VM restart
         (reported: what the config sizes are chosen from).
  --via R   first OTA release R (the board's current one, 1.212) into slot B,
            then the new one into slot A -- the order the real board takes.
            NOTE (2026-10-21): in mcusim the second flash-update boot (slot B
            -> slot A) comes up without the ROM's partition table ("no A/B
            partition table ... boot type 4", the image cannot be bought) --
            with 1.212 as the new image just the same, so it is the simulator
            (the known watchdog-reset gap, docs/pex-format.md 15); the real
            gateway has taken B -> A updates (1.163 -> 1.180).
Nothing leaves 127.0.0.1. Exit status 0 when everything passed.
"""
import argparse, base64, http.server, random, json, os, re, shutil, signal, socketserver, subprocess, sys, threading, time
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
PEX = os.path.dirname(HERE)
ROOT = os.path.dirname(PEX)
PC_IP = "192.168.8.16"              # the PC on the USB network (the firmware's DHCP hands it out)
HTTP_PORT = int(os.environ.get("GWSIM_HTTP_PORT", "18391"))
FWD = 18782
# the board's forwards listen here: a loopback address of our own (SMP is udp 1337, fixed);
# GWSIM_BIND / GWSIM_HTTP_PORT let two runs share the PC
BIND = os.environ.get("GWSIM_BIND", "127.0.0.77")
OTAP = "49646561-7346-6c6f-6174-40882af10000"
FS_OFF, FS_BLOCKS = 0x200000, 444

RESULTS = []


def check(name, ok, extra=""):
    RESULTS.append((name, bool(ok), str(extra)))
    print(("PASS " if ok else "FAIL ") + name + (f"  [{str(extra)[:150]}]" if extra else ""), flush=True)
    return ok


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


class H(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def do_GET(self):
        if self.path == "/small.json":
            b = b'{"hello":"pex"}'
        elif self.path == "/big":
            b = bytes(i * 7 % 251 for i in range(40000))
        else:
            self.send_response(404)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        self.send_response(200)
        self.send_header("Content-Length", str(len(b)))
        self.end_headers()
        self.wfile.write(b)


class TS(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True


TOML = """
[project]
name = "btgw-pex"
board = "pico2_w"
[boot]
bootrom = "A2"
[flash]
partition_table = "{pt}"
images = [ {{ file = "{wifi}", partition = "Wi-Fi Firmware" }}, {{ file = "{lfs}", at = 0x10200000 }} ]
persist = "{flash}"
[firmware]
image = "{old}"
partition = "Main A"
[[device]]
type = "cyw43439"
name = "cyw43"
[device.network]
kind = "sim"
dns_upstream = "none"
backend = {{ kind = "mock", service = [] }}
[[device.ble_peer]]
profile = "iti-otap"
addr = "c0:11:22:33:44:55"
random = true
name = "FE_1234"
model = "CRT"
rssi = -50
[[device]]
type = "sdcard"
mode = "sdio"
clk = 2
cmd = 3
d0 = 4
detect = 9
image = "{card}"
writeback = true
[chip]
usb_host = {{ kind = "net", forward = ["{fwd}:80", "udp:1337:1337"], net = {{ nat = true, bind = "{bind}" }} }}
[run]
time = "{time}"
rtt = "stdio"
"""


class Sim:
    def __init__(self, exe, toml, log):
        self.lines = []
        self.log = open(log, "a")
        self.log.write(f"\n===== mcusim started {time.strftime('%H:%M:%S')} =====\n")
        self.p = subprocess.Popen([exe, "-p", toml], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                                  errors="replace")
        self.t = threading.Thread(target=self._read, daemon=True)
        self.t.start()

    def _read(self):
        for line in self.p.stdout:
            self.lines.append(line.rstrip("\n"))
            self.log.write(line)
            self.log.flush()

    def wait(self, pattern, timeout, start=0):
        rx = re.compile(pattern)
        t0 = time.time()
        i = start
        while time.time() - t0 < timeout:
            while i < len(self.lines):
                if rx.search(self.lines[i]):
                    return i
                i += 1
            if self.p.poll() is not None:
                return -1
            time.sleep(0.1)
        return -1

    def kill(self):
        """A power cut: SIGKILL (mcusim writes every flash program/erase through to the file at once)."""
        if self.p.poll() is None:
            self.p.kill()
            self.p.wait()
        self.t.join(5)
        self.log.flush()

    def stop(self):
        if self.p.poll() is None:
            self.p.send_signal(signal.SIGTERM)
            try:
                self.p.wait(20)
            except subprocess.TimeoutExpired:
                self.p.kill()
                self.p.wait()
        self.t.join(5)
        self.log.close()


def op(name, params=None, timeout=20):
    body = json.dumps({"op": name, "params": params or {}}).encode()
    req = urllib.request.Request(f"http://{BIND}:{FWD}/api/op", data=body, method="POST")
    try:
        j = json.load(urllib.request.urlopen(req, timeout=timeout))
    except Exception as e:
        return False, str(e)
    return j.get("ok", False), j.get("result") if j.get("ok") else j.get("error")


def put(path, data):
    req = urllib.request.Request(f"http://{BIND}:{FWD}/api/fs/{path}", data=data, method="PUT")
    return urllib.request.urlopen(req, timeout=30).status == 200


def wait_http(timeout):
    t0 = time.time()
    while time.time() - t0 < timeout:
        try:
            urllib.request.urlopen(f"http://{BIND}:{FWD}/api/fs/", timeout=4).read()
            return True
        except Exception:
            time.sleep(1)
    return False


def smp(*args, timeout=600):
    return run(["smpmgr", "--ip", BIND, "--mtu", "1200", "--timeout", "30", *args], timeout=timeout)


def ota(s, rel, slot, label):
    """smpmgr upload into the other slot, test it, reset: the TBYB image must boot
    from that slot; 'state-write --confirm' buys it. Returns the boot's first line."""
    r = smp("image", "state-read")
    before = {int(sl): hx.lower() for sl, hx in re.findall(r"slot=(\d+).*?HashBytes\(\s*'([0-9A-Fa-f]{64})'", r.stdout, re.S)}
    for attempt in range(3):     # an SMP upload can stall mid-way (seen over WireGuard too): retry
        t0 = time.time()
        r = smp("image", "upload", "--format", "any", rel, timeout=1800)
        if r.returncode == 0:
            break
        print(f"  (upload attempt {attempt + 1} failed after {time.time() - t0:.0f} s, retrying)", flush=True)
    check(f"[{label}] smpmgr image upload into slot {slot}", r.returncode == 0,
          f"{time.time() - t0:.0f} s, attempt {attempt + 1} {r.stdout[-120:]} {r.stderr[-120:]}")
    r = smp("image", "state-read")
    slots = {int(sl): hx.lower() for sl, hx in re.findall(r"slot=(\d+).*?HashBytes\(\s*'([0-9A-Fa-f]{64})'", r.stdout, re.S)}
    check(f"[{label}] state-read lists both slots", 0 in slots and 1 in slots, r.stdout[-300:])
    idx = 0 if slot == "A" else 1
    new_hash = slots.get(idx, "")
    check(f"[{label}] the upload landed in slot {slot}", new_hash and new_hash != before.get(idx), f"{before.get(idx)} -> {new_hash}")
    r = smp("image", "state-write", new_hash) if new_hash else None
    check(f"[{label}] state-write <hash> (test slot {slot})", r is not None and r.returncode == 0, r and (r.stdout + r.stderr)[-200:])
    mark = len(s.lines)
    smp("os", "reset", timeout=60)
    i = s.wait(r"^ota: slot A .* booted %d" % idx, 300, mark)
    check(f"[{label}] the TBYB image boots from slot {slot}", i >= 0, s.lines[i] if i >= 0 else "")
    i = s.wait(r"running a try-before-you-buy image \(slot %s\)" % slot, 120, mark)
    check(f"[{label}] ...as a try-before-you-buy image", i >= 0)
    check(f"[{label}] ...and initialises", s.wait(r"^main: init done", 300, mark) >= 0)
    check(f"[{label}] HTTP up on the new image", wait_http(180))
    r = smp("image", "state-write", "--confirm")
    i = s.wait(r"^ota: explicit_buy -> 0", 60, mark)
    check(f"[{label}] state-write --confirm buys it", r.returncode == 0 and i >= 0, (r.stdout + r.stderr)[-200:])
    return mark


def boot(exe, toml, log, label, timeout=300):
    s = Sim(exe, toml, log)
    ok = s.wait(r"^main: init done", timeout) >= 0 and wait_http(180)
    check(label, ok)
    return s


def put_mods(a):
    for model, d in (("pic", "pex"), ("reloc", "pex/a")):
        for n in ("big", "hello", "counter"):
            put(f"{d}/{n}.m33.pex", open(os.path.join(a.mods, "pexmods", model, f"{n}.m33.pex"), "rb").read())


def installed():
    ok, r = op("pex.installed")
    return {m["name"]: m for m in r.get("modules", [])} if ok else None


def consistent(label, expect_any):
    """After a power cut: the index is one of the expected sets, and everything in it verifies and runs."""
    inst = installed()
    names = set(inst or {})
    good = inst is not None and any(names == e for e in expect_any)
    bad = []
    for n in names:
        ok, v = op("pex.verify", {"name": n})
        if not ok:
            bad.append(f"{n}: verify {v}")
            continue
        ok, r = op("pex.load", {"name": n})
        if not ok:
            bad.append(f"{n}: load {r}")
            continue
        if n == "big":
            _, c = op("pex.call", {"name": "big", "fn": "big_check"})
            if c.get("result") != 0 or not r.get("xip"):
                bad.append(f"big: check {c} xip {r.get('xip')}")
        op("pex.unload", {"name": n})
    check(label, good and not bad, f"installed {sorted(names)} {bad}")
    return names


def xip_power_cuts(a, exe, toml, log, s):
    """Power cuts (SIGKILL of the simulator) during installs and during an
    overlapping compaction; after each restart the index is the old or the new
    one and every installed module verifies and runs in place."""
    put_mods(a)
    for n in list(installed() or {}):
        op("pex.unload", {"name": n})
        op("pex.uninstall", {"name": n})
    ok, r = op("pex.install", {"name": "hello"})
    t0 = time.time()
    ok2, r2 = op("pex.install", {"name": "big"}, timeout=120)
    t_inst = time.time() - t0
    check("xip: clean install of big (simulated time, wall clock)", ok and ok2, f"{t_inst:.1f} s wall {r2}")
    op("pex.uninstall", {"name": "big"})
    resumed = 0
    for i, frac in enumerate((0.1, 0.35, 0.6, 0.85, 1.1)):
        th = threading.Thread(target=op, args=("pex.install", {"name": "big"}, 120), daemon=True)
        th.start()
        time.sleep(max(0.05, frac * t_inst))
        s.kill()
        s = boot(exe, toml, log, f"xip: power cut {i + 1} during install ({frac:.0%}): reboots")
        consistent(f"xip: power cut {i + 1} during install: old or new index, all verify/run", [{"hello"}, {"hello", "big"}])
        put_mods(a)
        op("pex.uninstall", {"name": "big"})
    # an overlapping compaction: big (11 sectors) slides down over itself by one sector
    t_comp = None
    for i, frac in enumerate((None, 0.15, 0.4, 0.65, 0.9)):
        for n in ("hello", "big", "counter"):
            op("pex.uninstall", {"name": n})
        op("pex.install", {"name": "hello"})
        op("pex.install", {"name": "big"}, timeout=120)
        op("pex.install", {"name": "counter"})
        op("pex.uninstall", {"name": "hello"})
        if frac is None:
            t0 = time.time()
            ok, c = op("pex.compact", timeout=180)
            t_comp = time.time() - t0
            check("xip: clean compaction (big slides over itself)", ok and c.get("moved", 0) >= 1, f"{t_comp:.1f} s wall {c}")
            consistent("xip: after the clean compaction", [{"big", "counter"}])
            continue
        mark = len(s.lines)
        th = threading.Thread(target=op, args=("pex.compact", None, 180), daemon=True)
        th.start()
        time.sleep(max(0.05, frac * t_comp))
        s.kill()
        s = boot(exe, toml, log, f"xip: power cut {i} during compaction ({frac:.0%}): reboots")
        mark = len(s.lines)
        consistent(f"xip: power cut {i} during compaction: both still verify/run", [{"big", "counter"}])
        if any("resuming the move" in l for l in s.lines[mark:]):
            resumed += 1
        put_mods(a)
    check("xip: an interrupted move was resumed after a power cut", resumed >= 1, f"{resumed} of 4")
    for n in ("hello", "big", "counter"):
        op("pex.uninstall", {"name": n})
    return s


def lua_budget(a, s):
    """What Lua needs as a launcher: the arena after boot, with the Pi Switch
    driver (iti_ble_lua) compiled and running, with a PEX launcher script,
    and after a VM restart (bytecode from the cache). Reported, not judged:
    the sizes in the configs come from these numbers."""
    def heap(label):
        ok, h = op("script.heap", timeout=30)
        ok2, st = op("script.stats")
        ar = (h or {}).get("arena", {}) if ok else {}
        rt = (st or {}).get("runtime", {}) if ok2 else {}
        row = {"live": ar.get("used"), "peak": rt.get("peak"), "largest_free": ar.get("largest_free"),
               "capacity": ar.get("bytes"), "fails": rt.get("alloc_fails")}
        check("lua budget: " + label, ok and ok2, json.dumps(row))
        return row
    rows = {"boot": heap("after boot (VM, bindings; no script)")}
    files = []
    for root, _, fs in os.walk(a.pi_switch):
        for f in fs:
            if f.endswith(".lua"):
                rel = os.path.relpath(os.path.join(root, f), a.pi_switch)
                put("lua/" + rel, open(os.path.join(root, f), "rb").read())
                files.append(rel)
    time.sleep(15)                       # the bytecode cache compiles changed modules on the next ticks
    rows["compiled"] = heap(f"Pi Switch driver uploaded and compiled into the cache ({len(files)} files)")
    ok, r = op("script.start", {"name": "pi_switch"})
    time.sleep(25)
    rows["pi_switch"] = heap("Pi Switch driver running (scanning)" + ("" if ok else f" -- start failed {r}"))
    put("pex/app.m33.pex", open(os.path.join(a.mods, "pexmods", "pic", "app.m33.pex"), "rb").read())
    put("lua/pexlaunch.lua", b'pex.load("app")\npex.start("app", { ticks = 100000, period = 500 })\n'
                             b'while true do sys.sleep(1000) end\n')
    ok, r = op("script.start", {"name": "pexlaunch"})
    time.sleep(10)
    rows["launcher"] = heap("+ a PEX launcher script (app started, script kept alive)" + ("" if ok else f" -- {r}"))
    ok, r = op("script.vm_restart", timeout=30)
    time.sleep(10)
    op("script.start", {"name": "pi_switch"})
    op("script.start", {"name": "pexlaunch"})
    time.sleep(25)
    rows["restarted"] = heap("after a VM restart, both started again (bytecode from the cache)")
    for n in ("pexlaunch", "pi_switch"):
        op("script.stop", {"name": n})
    op("pex.stop", {"name": "app"})
    op("pex.unload", {"name": "app"})
    json.dump(rows, open(os.path.join(a.work, "lua_budget.json"), "w"), indent=1)
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--deploy", required=True, help="the TBYB build (btgw.elf, btgw.bin, wifi blob, picotool)")
    ap.add_argument("--old", required=True, help="the slot A build (feature/lfs-migrate, non-TBYB)")
    ap.add_argument("--mods", required=True, help="the module build (-DPEX_TEST_GW=ON -DPEX_TEST_SIGN=ON)")
    ap.add_argument("--release", help="the signed release (default: deploy/btgw-*.smp.bin)")
    ap.add_argument("--mcusim", default=os.path.expanduser("~/Work/mcusim-agents/blegatt/target/release/mcusim"))
    ap.add_argument("--work", default=os.path.expanduser("~/Work/pexwork/gwsim"))
    ap.add_argument("--scenarios", default="ota,quiet,suite,xip,fault,hang")
    ap.add_argument("--pi-switch", default=os.path.expanduser("~/Work/iti_ble_lua/lua"),
                    help="the Pi Switch driver's lua/ folder, for the 'budget' scenario")
    ap.add_argument("--via", help="first OTA this release (the board's current one) into slot B, then ours into slot A")
    ap.add_argument("--direct", help="development: boot this (non-TBYB) build's btgw.elf from slot A instead of "
                                     "the old firmware, and skip 'ota'")
    a = ap.parse_args()
    for k in ("deploy", "old", "mods", "release", "mcusim", "direct", "via", "pi_switch"):
        if getattr(a, k):
            setattr(a, k, os.path.abspath(getattr(a, k)))
    sc = set(a.scenarios.split(","))
    w = os.path.abspath(a.work)
    shutil.rmtree(w, ignore_errors=True)
    os.makedirs(w)
    exe = os.path.join(w, "pexsim-gw")             # a renamed copy: something here kills 'mcusim'
    shutil.copy2(a.mcusim, exe)
    rel = a.release or next((os.path.join(a.deploy, f) for f in sorted(os.listdir(a.deploy)) if f.endswith(".smp.bin")), None)
    if not rel:
        sys.exit("no release .smp.bin in the deploy build (tools/ota_release.py)")

    # ---- the board
    lt = os.path.join(w, "lfs_tool")
    r = run(["gcc", "-O2", "-DLFS_NAME_MAX=64", f"-I{ROOT}/src/fs/littlefs", "-o", lt, f"{ROOT}/test/sim/lfs_tool.c",
             f"{ROOT}/src/fs/littlefs/lfs.c", f"{ROOT}/src/fs/littlefs/lfs_util.c"])
    if r.returncode:
        sys.exit(r.stderr)
    pt = os.path.join(w, "pt.bin")
    r = run([os.path.join(a.deploy, "_deps/picotool/picotool"), "partition", "create", os.path.join(ROOT, "partitions.json"), pt])
    if r.returncode:
        sys.exit(r.stderr)
    tree = os.path.join(w, "tree")
    os.makedirs(os.path.join(tree, "cfg"))
    json.dump({"mode": "sta", "nets": [{"ssid": "mcusim", "psk": "mcusim123"}]}, open(os.path.join(tree, "cfg/wifi.json"), "w"))
    rnd = random.Random(1234)            # a throwaway key, as test/sim/lfs_migrate.py does
    json.dump({"private_key": base64.b64encode(rnd.randbytes(32)).decode(),
               "peer_public_key": base64.b64encode(rnd.randbytes(32)).decode(),
               "address": "10.99.0.2", "netmask": "255.255.255.0", "endpoint": "10.42.0.1", "endpoint_port": 51820},
              open(os.path.join(tree, "cfg/wireguard.json"), "w"))
    lfs = os.path.join(w, "lfs.img")
    r = run([lt, "mk", lfs, str(FS_BLOCKS), tree])
    if r.returncode:
        sys.exit(r.stderr)
    card = os.path.join(w, "card.img")
    with open(card, "wb") as f:
        f.truncate(64 << 20)
    r = run(["mkfs.exfat", "-L", "GW", card])
    if r.returncode:
        sys.exit("mkfs.exfat: " + r.stderr)
    files = os.path.join(w, "remote")
    r = run([sys.executable, os.path.join(HERE, "remote", "mkremote.py"), "--mods", os.path.join(a.mods, "pexmods"),
             "--objs", a.mods, "--out", files])
    if r.returncode:
        sys.exit(r.stdout + r.stderr)
    flash = os.path.join(w, "board.flash")
    toml = os.path.join(w, "board.toml")
    open(toml, "w").write(TOML.format(pt=pt, wifi=os.path.join(a.deploy, "btgw_wifi_firmware.bin"), lfs=lfs, flash=flash,
                                      old=os.path.join(a.direct or a.old, "btgw.elf"), card=card, fwd=FWD, bind=BIND,
                                      time="3000s"))
    if a.direct:
        sc.discard("ota")
    srv = TS(("127.0.0.1", HTTP_PORT), H)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    log = os.path.join(w, "board.log")
    s = Sim(exe, toml, log)
    try:
        # ---- the old firmware first, as on the real board
        check("slot A boots" + (" (direct)" if a.direct else " (the old firmware)"), s.wait(r"^main: init done", 300) >= 0)
        check("HTTP over USB-NCM", wait_http(180))
        boot1 = 0 if a.direct else len(s.lines)

        if "ota" in sc:
            time.sleep(20)               # let Wi-Fi/WireGuard/BT finish coming up first
            if a.via:                    # the board as it is now: the previous release in slot B
                ota(s, a.via, "B", "via " + os.path.basename(a.via))
                time.sleep(20)
            boot1 = ota(s, rel, "A" if a.via else "B", os.path.basename(rel))
        ok, info = op("sys.info")
        fsj = info.get("fs", {}) if ok else {}
        check("sys.info has the fs object (1776 KB + PEX partition)", fsj.get("kb") == 1776 and fsj.get("pex", {}).get("size") == 262144,
              json.dumps(fsj))
        check("LittleFS mounted (1776 KiB)", any(re.search(r"^fs: mounted 1776KiB", l) for l in s.lines[boot1:]),
              next((l for l in s.lines[boot1:] if l.startswith("fs: mounted")), "no 'fs: mounted'"))
        st = {}
        for _ in range(60):
            try:
                st = json.load(urllib.request.urlopen(f"http://{BIND}:{FWD}/api/wifi", timeout=10))
            except Exception:
                st = {}
            if st.get("ip") not in (None, "", "0.0.0.0"):
                break
            time.sleep(2)
        check("Wi-Fi joined mcusim's AP", st.get("ip") not in (None, "", "0.0.0.0"), st.get("ip"))
        check("WireGuard started from cfg/", s.wait(r"^wg: ", 60, boot1) >= 0,
              next((l for l in s.lines[boot1:] if l.startswith("wg: ")), ""))
        sd = {}
        for _ in range(30):
            try:
                sd = json.load(urllib.request.urlopen(f"http://{BIND}:{FWD}/api/sd", timeout=10))
            except Exception:
                sd = {}
            if sd.get("state") == "ok" and sd.get("mounted"):
                break
            time.sleep(2)
        check("SD card mounted", sd.get("state") == "ok" and sd.get("mounted"), json.dumps(sd)[:150])

        if "quiet" in sc:
            ok, lst = op("pex.list")
            pexlines = [l for l in s.lines[boot1:] if l.startswith("pex:") or l.startswith("[pex")]
            check("nothing PEX ran at boot (no pool, no module, no log)",
                  ok and not lst.get("up") and not lst.get("modules") and not pexlines,
                  f"up={lst.get('up') if ok else '?'} lines={pexlines[:2]}")

        if "suite" in sc:
            r = run([sys.executable, os.path.join(PEX, "tools", "pex_remote_test.py"), "--base", f"http://{BIND}:{FWD}",
                     "--files", files, "--sd", "--http-url", f"http://{PC_IP}:{HTTP_PORT}/small.json",
                     "--http-file-url", f"http://{PC_IP}:{HTTP_PORT}/big", "--bt", OTAP, "--allow-reboot"], timeout=2400)
            open(os.path.join(w, "remote_test.log"), "w").write(r.stdout + r.stderr)
            for line in r.stdout.splitlines():
                m = re.match(r"\s+(PASS|FAIL|skip) (.{34}) ?(.*)", line)
                if m and m.group(1) != "skip":
                    check("remote: " + m.group(2).strip(), m.group(1) == "PASS", m.group(3))
            check("pex_remote_test.py exit status 0", r.returncode == 0, r.stdout.strip().splitlines()[-1] if r.stdout else r.stderr[-200:])

        if "budget" in sc:
            lua_budget(a, s)

        if "xip" in sc:
            s = xip_power_cuts(a, exe, toml, log, s)

        for name, mod in (("fault", "crashapp"), ("hang", "hangapp")):
            if name not in sc:
                continue
            put(f"pex/{mod}.m33.pex", open(os.path.join(a.mods, "pexmods", "gw-faults", f"{mod}.m33.pex"), "rb").read())
            put("cfg/pex_autostart.json", json.dumps({"modules": [{"name": mod, "cfg": {"delay": 1500}}]}).encode())
            ok, _ = op("pex.faults_clear")
            mark = len(s.lines)
            op("sys.reboot")
            i = s.wait(r"^main: init done", 300, mark)
            check(f"{name}: reboots", i >= 0)
            if name == "fault":
                j = s.wait(r"^PEX FAULT: module crashapp build [0-9a-f]{32} at offset", 120, mark)
                check("fault: attributed (module, build id, offset)", j >= 0, s.lines[j] if j >= 0 else "")
            else:
                j = s.wait(r"hanging now", 120, mark)
                check("hang: the module hangs the loop", j >= 0)
            k = s.wait(r"^main: REBOOTED BY WATCHDOG|^main: previous boot stopped", 120, max(j, mark))
            check(f"{name}: the watchdog reboots the board", k >= 0, s.lines[k] if k >= 0 else "")
            # The OTA group's slot line is not printed after a watchdog reset in
            # mcusim ("no A/B partition table ... boot type 0", after any
            # reboot, not PEX-related), so "the same image" is checked as the
            # image with PEX in it answering -- the old firmware has no pex.*.
            b = s.wait(r"^main: init done", 300, max(k, mark))
            okb = b >= 0 and wait_http(120) and op("pex.list")[0]
            check(f"{name}: into the same image (the PEX firmware answers pex.list)", okb)
            why = "FAULTED in module crashapp" if name == "fault" else "was reset while running code"
            p = s.wait(r"^pex: the previous boot " + why, 120, max(b, mark))
            check(f"{name}: the next boot names it", p >= 0, s.lines[p] if p >= 0 else "")
            q = s.wait(r"^pex: autostart %s: SKIPPED" % mod, 60, max(p, mark))
            check(f"{name}: and does not autostart it again", q >= 0)
            check(f"{name}: board reachable afterwards", wait_http(120))
            ok, f = op("pex.faults")
            check(f"{name}: pex.faults reports it", ok and (f.get("skip") or {}).get("faults"), json.dumps(f)[:150])
            ok, lst = op("pex.list")
            check(f"{name}: the module is not running", ok and not any(m["name"] == mod for m in lst.get("modules", [])))
            # leave the board clean for the next scenario
            urllib.request.urlopen(urllib.request.Request(f"http://{BIND}:{FWD}/api/fs/cfg/pex_autostart.json",
                                                          method="DELETE"), timeout=10).read()
            op("pex.faults_clear")
    finally:
        s.stop()
        srv.shutdown()
    n = sum(1 for _, ok, _ in RESULTS if ok)
    print(f"\n{n}/{len(RESULTS)} passed   (board log: {log})")
    return 0 if n == len(RESULTS) else 1


if __name__ == "__main__":
    sys.exit(main())
