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
Nothing leaves 127.0.0.1. Exit status 0 when everything passed.
"""
import argparse, base64, http.server, random, json, os, re, shutil, signal, socketserver, subprocess, sys, threading, time
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
PEX = os.path.dirname(HERE)
ROOT = os.path.dirname(PEX)
PC_IP = "192.168.8.16"              # the PC on the USB network (the firmware's DHCP hands it out)
HTTP_PORT, FWD = 18091, 18782
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
usb_host = {{ kind = "net", forward = ["{fwd}:80", "udp:1337:1337"], net = {{ nat = true }} }}
[run]
time = "{time}"
rtt = "stdio"
"""


class Sim:
    def __init__(self, exe, toml, log):
        self.lines = []
        self.log = open(log, "a")
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
    req = urllib.request.Request(f"http://127.0.0.1:{FWD}/api/op", data=body, method="POST")
    try:
        j = json.load(urllib.request.urlopen(req, timeout=timeout))
    except Exception as e:
        return False, str(e)
    return j.get("ok", False), j.get("result") if j.get("ok") else j.get("error")


def put(path, data):
    req = urllib.request.Request(f"http://127.0.0.1:{FWD}/api/fs/{path}", data=data, method="PUT")
    return urllib.request.urlopen(req, timeout=30).status == 200


def wait_http(timeout):
    t0 = time.time()
    while time.time() - t0 < timeout:
        try:
            urllib.request.urlopen(f"http://127.0.0.1:{FWD}/api/fs/", timeout=4).read()
            return True
        except Exception:
            time.sleep(1)
    return False


def smp(*args, timeout=600):
    return run(["smpmgr", "--ip", "127.0.0.1", "--mtu", "1200", "--timeout", "30", *args], timeout=timeout)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--deploy", required=True, help="the TBYB build (btgw.elf, btgw.bin, wifi blob, picotool)")
    ap.add_argument("--old", required=True, help="the slot A build (feature/lfs-migrate, non-TBYB)")
    ap.add_argument("--mods", required=True, help="the module build (-DPEX_TEST_GW=ON -DPEX_TEST_SIGN=ON)")
    ap.add_argument("--release", help="the signed release (default: deploy/btgw-*.smp.bin)")
    ap.add_argument("--mcusim", default=os.path.expanduser("~/Work/mcusim-agents/blegatt/target/release/mcusim"))
    ap.add_argument("--work", default=os.path.expanduser("~/Work/pexwork/gwsim"))
    ap.add_argument("--scenarios", default="ota,quiet,suite,fault,hang")
    a = ap.parse_args()
    for k in ("deploy", "old", "mods", "release", "mcusim"):
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
                                      old=os.path.join(a.old, "btgw.elf"), card=card, fwd=FWD, time="3000s"))
    srv = TS(("127.0.0.1", HTTP_PORT), H)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    log = os.path.join(w, "board.log")
    s = Sim(exe, toml, log)
    try:
        # ---- the old firmware first, as on the real board
        check("old firmware (slot A) boots", s.wait(r"^main: init done", 300) >= 0)
        check("HTTP over USB-NCM", wait_http(180))
        boot1 = len(s.lines)

        if "ota" in sc:
            t0 = time.time()
            r = smp("image", "upload", "--format", "any", rel, timeout=1800)
            check("smpmgr image upload into slot B", r.returncode == 0, f"{time.time() - t0:.0f} s {r.stdout[-200:]} {r.stderr[-200:]}")
            r = smp("image", "state-read")
            slots = {int(sl): hx.lower() for sl, hx in
                     re.findall(r"slot=(\d+).*?HashBytes\(\s*'([0-9A-Fa-f]{64})'", r.stdout, re.S)}
            check("state-read lists both slots", 0 in slots and 1 in slots, r.stdout[-300:])
            new_hash = slots.get(1, "")
            r = smp("image", "state-write", new_hash) if new_hash else None
            check("state-write <hash> (test slot B)", r is not None and r.returncode == 0, r and (r.stdout + r.stderr)[-200:])
            mark = len(s.lines)
            smp("os", "reset", timeout=60)
            i = s.wait(r"^ota: slot A .* booted 1", 300, mark)
            check("the TBYB image boots from slot B", i >= 0, s.lines[i] if i >= 0 else "")
            i = s.wait(r"running a try-before-you-buy image \(slot B\)", 120, mark)
            check("...as a try-before-you-buy image", i >= 0)
            check("...and initialises", s.wait(r"^main: init done", 300, mark) >= 0)
            check("HTTP up on the new image", wait_http(180))
            r = smp("image", "state-write", "--confirm")
            i = s.wait(r"^ota: explicit_buy -> 0", 60, mark)
            check("state-write --confirm buys it", r.returncode == 0 and i >= 0, (r.stdout + r.stderr)[-200:])
            boot1 = mark
        ok, info = op("sys.info")
        fsj = info.get("fs", {}) if ok else {}
        check("sys.info has the fs object (1776 KB + PEX partition)", fsj.get("kb") == 1776 and fsj.get("pex", {}).get("size") == 262144,
              json.dumps(fsj))
        check("LittleFS mounted (1776 KiB)", any(re.search(r"^fs: mounted 1776KiB", l) for l in s.lines[boot1:]),
              next((l for l in s.lines[boot1:] if l.startswith("fs: mounted")), "no 'fs: mounted'"))
        st = {}
        for _ in range(60):
            try:
                st = json.load(urllib.request.urlopen(f"http://127.0.0.1:{FWD}/api/wifi", timeout=10))
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
                sd = json.load(urllib.request.urlopen(f"http://127.0.0.1:{FWD}/api/sd", timeout=10))
            except Exception:
                sd = {}
            if str(sd.get("card", sd.get("state", ""))) == "ok":
                break
            time.sleep(2)
        check("SD card mounted", str(sd.get("card", sd.get("state", ""))) == "ok", json.dumps(sd)[:150])

        if "quiet" in sc:
            ok, lst = op("pex.list")
            pexlines = [l for l in s.lines[boot1:] if l.startswith("pex:") or l.startswith("[pex")]
            check("nothing PEX ran at boot (no pool, no module, no log)",
                  ok and not lst.get("up") and not lst.get("modules") and not pexlines,
                  f"up={lst.get('up') if ok else '?'} lines={pexlines[:2]}")

        if "suite" in sc:
            r = run([sys.executable, os.path.join(PEX, "tools", "pex_remote_test.py"), "--base", f"http://127.0.0.1:{FWD}",
                     "--files", files, "--sd", "--http-url", f"http://{PC_IP}:{HTTP_PORT}/small.json",
                     "--http-file-url", f"http://{PC_IP}:{HTTP_PORT}/big", "--bt", OTAP], timeout=1800)
            open(os.path.join(w, "remote_test.log"), "w").write(r.stdout + r.stderr)
            for line in r.stdout.splitlines():
                m = re.match(r"\s+(PASS|FAIL|skip) (.{34}) ?(.*)", line)
                if m and m.group(1) != "skip":
                    check("remote: " + m.group(2).strip(), m.group(1) == "PASS", m.group(3))
            check("pex_remote_test.py exit status 0", r.returncode == 0, r.stdout.strip().splitlines()[-1] if r.stdout else r.stderr[-200:])

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
            b = s.wait(r"^ota: slot A .* booted 1", 120, max(k, mark))
            check(f"{name}: into the same image (slot B)", b >= 0, s.lines[b] if b >= 0 else "")
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
            urllib.request.urlopen(urllib.request.Request(f"http://127.0.0.1:{FWD}/api/fs/cfg/pex_autostart.json",
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
