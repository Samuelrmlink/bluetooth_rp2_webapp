#!/usr/bin/env python3
"""Pinecil PEX against a mock iron, in mcusim's blegatt GATT backend.

The iron on the bench can be off. This boots a development gateway image
(unsigned PEX allowed) with one generic ``ble_peer`` that serves the
Pinecil characteristics, PUTs ``pinecil.m33.pex`` onto the simulated
LittleFS, and drives ``pex.load`` / ``pex.start``.

Two hunts, one boot each:

  name  The peer is named Pinecil and advertises no live-service UUID.
  uuid  The name does not start with Pinecil. The live service UUID is
        advertised, which is the scanner manifest's second rule.

Each hunt checks ``profile.pinecil.telemetry`` against the 56-byte live
value, the ``pinecil.set_temp`` clamp, and that the settings characteristic
holds the u16 LE write. The stored bytes are read back by a short Lua
script after the module has disconnected, so the check is the peer's
value. A ``set_temp`` issued before the link is up must not change it.

Nothing here talks to anything but 127.0.0.1. The board is not contacted.

  test/sim/pinecil_bt.py [--build DIR] [--pex FILE] [--only name|uuid]

``--build`` defaults to ``$BTGW_BUILD`` or ``/tmp/btgw_sdcl_1m`` (btgw.elf
and btgw_wifi_firmware.bin). The blegatt mcusim is copied to another name
before it is started: a process whose name is mcusim is killed on this
machine. Exit 77 when the simulator or the firmware image is missing.
"""
import argparse, json, os, shutil, signal, subprocess, sys, tempfile, time, urllib.error, urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BLEGATT = os.path.expanduser("~/Work/mcusim-agents/blegatt")
MCUSIM = os.path.join(BLEGATT, "target/release/mcusim")
BOOTROM = os.path.expanduser("~/Work/mcusim/third_party/bootrom")
SVC_LIVE = "d85ef000-168e-4a71-aa55-33e27f9bc533"
SVC_BULK = "9eae1000-9d0d-48c5-aa55-33e27f9bc533"
CHR_LIVE = "9eae1001-9d0d-48c5-aa55-33e27f9bc533"
CHR_BUILD = "9eae1003-9d0d-48c5-aa55-33e27f9bc533"
CHR_SERIAL = "9eae1004-9d0d-48c5-aa55-33e27f9bc533"
SVC_SET = "f6d80000-5a10-4eba-aa55-33e27f9bc533"
CHR_SETPT = "f6d70000-5a10-4eba-aa55-33e27f9bc533"

# 14 x u32 LE, the order pinecil_decode reads. Tenths are the raw units.
TIP_C, SETPOINT_C = 320, 300
INPUT_TENTHS, HANDLE_TENTHS = 200, 255
POWER_PCT, POWER_SOURCE = 40, 3
TIP_TENTHS, UPTIME_TENTHS, MOVE_TENTHS = 80, 125, 15
MAX_TIP_C, TIP_UV, HALL_UT, MODE, WATTS_TENTHS = 450, 1234, 0, 1, 185
BUILD = "v2.23"
SERIAL = bytes([0x01, 0x02, 0x03, 0x04, 0x05, 0x06])
SETPT_INITIAL = (300).to_bytes(2, "little")  # same degrees as the live setpoint

HUNTS = (
    {"id": "name", "addr": "c0:11:22:33:44:71", "name": "Pinecil", "uuids": ()},
    {"id": "uuid", "addr": "c0:11:22:33:44:72", "name": "BenchIron", "uuids": (SVC_LIVE,)},
)

sys.path.insert(0, os.path.join(BLEGATT, "tools"))
import btgw_ws  # noqa: E402


def live_hex():
    vals = (TIP_C, SETPOINT_C, INPUT_TENTHS, HANDLE_TENTHS, POWER_PCT, POWER_SOURCE,
            TIP_TENTHS, UPTIME_TENTHS, MOVE_TENTHS, MAX_TIP_C, TIP_UV, HALL_UT, MODE, WATTS_TENTHS)
    raw = b"".join(v.to_bytes(4, "little") for v in vals)
    if len(raw) != 56:
        raise SystemExit("live characteristic is not 56 bytes")
    return raw.hex()


def peer_toml(hunt):
    lines = ["[[device.ble_peer]]", 'profile = "generic"',
             'addr = "%s"' % hunt["addr"], 'name = "%s"' % hunt["name"], "rssi = -55"]
    if hunt["uuids"]:
        lines.append("uuids = [%s]" % ", ".join('"%s"' % u for u in hunt["uuids"]))
    chars = (
        (SVC_BULK, (
            (CHR_LIVE, '["read"]', 'value = "%s"' % live_hex()),
            (CHR_BUILD, '["read"]', 'text = "%s"' % BUILD),
            (CHR_SERIAL, '["read"]', 'value = "%s"' % SERIAL.hex()),
        )),
        (SVC_SET, (
            (CHR_SETPT, '["read", "write"]', 'value = "%s"' % SETPT_INITIAL.hex()),
        )),
    )
    for svc, chs in chars:
        lines += ["[[device.ble_peer.service]]", 'uuid = "%s"' % svc]
        for uuid, props, value in chs:
            lines += ["[[device.ble_peer.service.char]]", 'uuid = "%s"' % uuid,
                      "props = %s" % props, value]
    return "\n".join(lines) + "\n"


def project_toml(build, hunt, port):
    elf = os.path.join(build, "btgw.elf")
    blob = os.path.join(build, "btgw_wifi_firmware.bin")
    pt = os.path.join(ROOT, "partitions.json")
    body = """[project]
name = "btgw-pinecil"
board = "pico2_w"
[boot]
bootrom = "A2"
reboot = "flash_update:Main A"
[flash]
partition_table = %s
images = [ { file = %s, partition = "Wi-Fi Firmware" } ]
[firmware]
image = %s
partition = "Main A"
[[device]]
type = "cyw43439"
name = "cyw43"
log = true
[device.network]
kind = "sim"
dns_upstream = "none"
dns_records = { "*" = "10.42.0.1" }
backend = { kind = "mock" }
%s[[device]]
type = "sdcard"
name = "sd"
mode = "sdio"
detect = 9
clk = 2
cmd = 3
d0 = 4
size = "64M"
[run]
rtt = "stdio"
stats = true
usb_host = "net"
usb_net_forward = ["%d:80"]
""" % (json.dumps(pt), json.dumps(blob), json.dumps(elf), peer_toml(hunt), port)
    return body


READBACK = r"""
local ADDR = "@@ADDR@@"
local RUN = "@@RUN@@"
local SVC = "f6d80000-5a10-4eba-aa55-33e27f9bc533"
local CHR = "f6d70000-5a10-4eba-aa55-33e27f9bc533"

local function hex(s)
  local t = {}
  for i = 1, #s do t[i] = string.format("%02x", s:byte(i)) end
  return table.concat(t)
end

local function finish(ok, extra)
  pcall(function() bt.disconnect(ADDR) end)
  pcall(function() bt.scan(false) end)
  local body = json.encode({ ok = ok and true or false, run = RUN, hex = extra })
  local h = file.open("lfs:/pinecil_readback.json", "w")
  if h then h:write(body) h:close() end
  log.info("pinecil-readback " .. body)
end

bt.scan(true)
local dev
for _ = 1, 150 do
  for _, d in ipairs(bt.devices() or {}) do
    if d.addr == ADDR then dev = d end
  end
  if dev then break end
  sys.sleep(100)
end
if not dev then finish(false, "not-seen") return end
if not bt.connect(ADDR) then finish(false, "connect-refused") return end
local path
for _ = 1, 150 do
  local found = bt.find(ADDR, SVC, CHR)
  if found then path = found break end
  sys.sleep(100)
end
if not path then finish(false, "no-char") return end
bt.notify_flush(ADDR)
if not bt.read(ADDR, path) then finish(false, "read-refused") return end
local deadline = sys.ms() + 5000
local value
while sys.ms() < deadline do
  local left = deadline - sys.ms()
  if left <= 0 then break end
  local n, why = bt.await(ADDR, left)
  if n and n.source == "read" and n.value then value = n.value break end
  if not n and why ~= "timeout" then finish(false, "await-" .. tostring(why)) return end
end
if not value then finish(false, "timeout") return end
finish(true, hex(value))
"""


class Checks:
    def __init__(self):
        self.n = 0
        self.fails = 0

    def check(self, name, cond, extra=""):
        self.n += 1
        tag = "ok   " if cond else "FAIL "
        if not cond:
            self.fails += 1
        line = tag + name
        if extra:
            line += "  [" + extra + "]"
        print(line, flush=True)
        return cond


def http(method, url, body=None, timeout=60):
    req = urllib.request.Request(url, data=body, method=method)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status, r.read()
    except urllib.error.HTTPError as e:
        return e.code, e.read()


def ensure_pex(path):
    if path and os.path.exists(path):
        return path
    bdir = "/tmp/pex-drivers-rel"
    env = dict(os.environ)
    env.setdefault("PICO_TOOLCHAIN_PATH", os.path.expanduser("~/.pico-sdk/toolchain/15_2_Rel1"))
    env.setdefault("PICO_SDK_PATH", os.path.expanduser("~/.pico-sdk/sdk/2.3.0"))
    src = os.path.join(ROOT, "pex/modules/drivers")
    subprocess.run(["cmake", "-S", src, "-B", bdir, "-G", "Ninja"], check=True, env=env)
    subprocess.run(["cmake", "--build", bdir, "--target", "pex_pinecil_reloc"], check=True, env=env)
    out = os.path.join(bdir, "pex-drivers/pinecil.m33.pex")
    if not os.path.exists(out):
        raise SystemExit("pinecil.m33.pex was not produced")
    return out


def renamed_mcusim():
    dst = "/tmp/pexsim-pinecil-%d" % os.getpid()
    shutil.copy2(MCUSIM, dst)
    os.chmod(dst, 0o755)
    return dst


def tail(path, n=40):
    try:
        lines = open(path, errors="replace").read().splitlines()
    except OSError:
        return ""
    return "\n".join(lines[-n:])


def wait_http(base, proc, log_path, seconds):
    end = time.time() + seconds
    while time.time() < end:
        if proc.poll() is not None:
            return False
        try:
            st, _ = http("GET", base + "/api/fs", timeout=5)
            if st == 200:
                return True
        except Exception:
            pass
        time.sleep(1)
    return False


def note_event(box, j):
    evt = j.get("evt")
    data = j.get("data") if isinstance(j.get("data"), dict) else {}
    if evt == "profile.pinecil.telemetry" and box["tel"] is None:
        box["tel"] = data
    if evt == "log":
        msg = data.get("message", "")
        if "pinecil" in msg.lower():
            print("[%s] %s" % (data.get("level", "?"), msg), flush=True)
    if evt and str(evt).startswith("script"):
        print("[%s] %s" % (evt, json.dumps(data)), flush=True)


def wait_tel(ws, box, seconds):
    end = time.time() + seconds
    while box["tel"] is None and time.time() < end:
        m = ws.recv(timeout=1)
        if not m:
            continue
        try:
            note_event(box, json.loads(m))
        except ValueError:
            pass
    return box["tel"]


def call_ok(checks, ws, name, op, params, timeout=30, on_event=None):
    try:
        rep = ws.call(op, params, timeout=timeout, on_event=on_event)
    except Exception as e:
        checks.check(name, False, str(e))
        return None
    ok = bool(rep.get("ok"))
    extra = "" if ok else json.dumps(rep)[:180]
    checks.check(name, ok, extra)
    return rep if ok else None


def set_temp(ws, celsius, on_event, tries=6):
    last = None
    for _ in range(tries):
        last = ws.call("pinecil.set_temp", {"celsius": celsius}, timeout=20, on_event=on_event)
        res = last.get("result") if isinstance(last.get("result"), dict) else {}
        if last.get("ok") and res.get("ok"):
            return last
        time.sleep(0.4)
    return last


def read_setpoint(checks, ws, base, addr, expect_hex, label):
    run = "%s-%s" % (label, int(time.time()))
    name = "pinecil_rb_%s" % label.replace("-", "_")
    body = READBACK.replace("@@ADDR@@", addr.upper()).replace("@@RUN@@", run).encode()
    st, raw = http("PUT", base + "/api/fs/lua/%s.lua" % name, body)
    if not checks.check("%s: upload readback" % label, st == 200, "HTTP %s %s" % (st, raw[:80])):
        return
    try:
        ws.call("script.stop", {"name": name}, timeout=10)
    except Exception:
        pass
    rep = call_ok(checks, ws, "%s: start readback" % label, "script.start", {"name": name}, timeout=20)
    if not rep:
        return
    end = time.time() + 40
    got = None
    while time.time() < end:
        m = ws.recv(timeout=1)
        if m:
            try:
                j = json.loads(m)
            except ValueError:
                j = {}
            if j.get("evt") == "log" and run in (j.get("data") or {}).get("message", ""):
                try:
                    got = json.loads(j["data"]["message"].split(" ", 1)[1])
                except (ValueError, IndexError, KeyError):
                    pass
        if got and got.get("run") == run:
            break
        try:
            st, raw = http("GET", base + "/api/fs/pinecil_readback.json", timeout=5)
            if st == 200:
                doc = json.loads(raw.decode())
                if doc.get("run") == run:
                    got = doc
                    break
        except Exception:
            pass
    if not got:
        checks.check("%s: setpoint %s" % (label, expect_hex), False, "no readback")
        return
    checks.check("%s: setpoint %s" % (label, expect_hex),
                 got.get("ok") is True and got.get("hex") == expect_hex, json.dumps(got))


def run_hunt(build, pex, hunt, port):
    print("\n== hunt %s (%s) ==" % (hunt["id"], hunt["name"]), flush=True)
    checks = Checks()
    work = tempfile.mkdtemp(prefix="pinecil_bt_")
    toml = os.path.join(work, "project.toml")
    log_path = os.path.join(work, "rtt.log")
    open(toml, "w").write(project_toml(build, hunt, port))
    log = open(log_path, "w")
    env = dict(os.environ)
    env.setdefault("MCUSIM_BOOTROM_DIR", BOOTROM)
    proc = subprocess.Popen([renamed_mcusim(), "-p", toml, "-t", "900s"],
                            stdout=log, stderr=subprocess.STDOUT, env=env)
    base = "http://127.0.0.1:%d" % port
    ws = None
    try:
        if not wait_http(base, proc, log_path, 180):
            log.flush()
            checks.check("HTTP API", False, "mcusim exit %s; %s" % (proc.poll(), tail(log_path).replace("\n", " | ")[:300]))
            return checks
        checks.check("HTTP API", True, base)
        ws = btgw_ws.WS("127.0.0.1", port, timeout=20)
        wifi = call_ok(checks, ws, "wifi.connect", "wifi.connect", {"ssid": "mcusim", "psk": "mcusim123"}, timeout=60)
        if not wifi:
            return checks
        ready = None
        for _ in range(120):
            if proc.poll() is not None:
                break
            st = ws.call("bt.state", timeout=15)
            ready = (st.get("result") or {}).get("ready")
            if ready:
                break
            time.sleep(0.5)
        if not checks.check("bt.state ready", bool(ready), json.dumps(ready)):
            print(tail(log_path), flush=True)
            return checks

        listed = call_ok(checks, ws, "pex.list", "pex.list", {}, timeout=30)
        if listed:
            checks.check("unsigned PEX allowed", "UNSIGNED ALLOWED" in json.dumps(listed.get("result")))

        for folder in ("pex", "lua"):
            st, raw = http("PUT", base + "/api/fs/%s/" % folder, b"")
            checks.check("mkdir %s" % folder, st == 200, "HTTP %s %s" % (st, raw[:60]))
        blob = open(pex, "rb").read()
        st, raw = http("PUT", base + "/api/fs/pex/pinecil.m33.pex", blob, timeout=60)
        if not checks.check("PUT pinecil.m33.pex", st == 200, "HTTP %s %s bytes %s" % (st, len(blob), raw[:60])):
            return checks

        loaded = call_ok(checks, ws, "pex.load pinecil", "pex.load", {"name": "pinecil"}, timeout=60)
        if not loaded:
            print(tail(log_path), flush=True)
            return checks

        early = ws.call("pinecil.set_temp", {"celsius": 200}, timeout=20)
        res = early.get("result") if isinstance(early.get("result"), dict) else {}
        checks.check("set_temp before the link is refused",
                     early.get("ok") is True and res.get("ok") is False and res.get("celsius") == 200,
                     json.dumps(early)[:180])
        read_setpoint(checks, ws, base, hunt["addr"], SETPT_INITIAL.hex(), "before-start")
        time.sleep(1.5)

        box = {"tel": None}
        started = call_ok(checks, ws, "pex.start", "pex.start",
                          {"name": "pinecil", "cfg": {"poll_ms": 8000}}, timeout=30,
                          on_event=lambda j: note_event(box, j))
        if not started:
            print(tail(log_path), flush=True)
            return checks
        tel = wait_tel(ws, box, 90)
        if not checks.check("profile.pinecil.telemetry", tel is not None, tail(log_path).replace("\n", " | ")[-240:]):
            return checks
        expect = {
            "addr": hunt["addr"].upper(),
            "build": BUILD,
            "serial": SERIAL.hex().upper(),
            "tip_c": TIP_C,
            "setpoint_c": SETPOINT_C,
            "input_v": INPUT_TENTHS / 10.0,
            "handle_c": HANDLE_TENTHS / 10.0,
            "power_pct": POWER_PCT,
            "power_source": "PD",
            "tip_ohms": TIP_TENTHS / 10.0,
            "uptime_s": UPTIME_TENTHS / 10.0,
            "last_move_s": MOVE_TENTHS / 10.0,
            "max_tip_c": MAX_TIP_C,
            "tip_uv": TIP_UV,
            "hall_uT": HALL_UT,
            "mode": "soldering",
            "watts": WATTS_TENTHS / 10.0,
        }
        for key, exp in expect.items():
            got = tel.get(key)
            if isinstance(exp, str):
                ok = got == exp
            else:
                try:
                    ok = abs(float(got) - float(exp)) < 1e-6
                except (TypeError, ValueError):
                    ok = False
            checks.check("telemetry.%s" % key, ok, "got %r" % (got,))

        def on_event(j):
            note_event(box, j)

        for celsius, clamped in ((9, 10), (321, 321), (500, MAX_TIP_C)):
            rep = set_temp(ws, celsius, on_event)
            res = rep.get("result") if isinstance(rep, dict) and isinstance(rep.get("result"), dict) else {}
            checks.check("set_temp %s -> %s" % (celsius, clamped),
                         bool(rep and rep.get("ok") and res.get("ok") and res.get("celsius") == clamped),
                         json.dumps(rep)[:180] if rep else "no reply")
            # The op returns when the host accepts the write. The peer stores
            # the bytes when the ATT response completes, so the next write
            # waits until that round trip has had time to finish.
            time.sleep(1.0)
        stopped = call_ok(checks, ws, "pex.stop", "pex.stop", {"name": "pinecil"}, timeout=20)
        if stopped:
            time.sleep(1.5)
            read_setpoint(checks, ws, base, hunt["addr"], (MAX_TIP_C).to_bytes(2, "little").hex(), "after-write")
        return checks
    except Exception as e:
        log.flush()
        checks.check("hunt %s finished" % hunt["id"], False, "%s: %s" % (type(e).__name__, e))
        print(tail(log_path), flush=True)
        return checks
    finally:
        if ws:
            try:
                ws.s.close()
            except Exception:
                pass
        if proc.poll() is None:
            proc.send_signal(signal.SIGTERM)
            try:
                proc.wait(10)
            except subprocess.TimeoutExpired:
                proc.kill()
        log.close()
        print("RTT log:", log_path, flush=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build", default=os.environ.get("BTGW_BUILD", "/tmp/btgw_sdcl_1m"))
    ap.add_argument("--pex", default=os.environ.get("PINECIL_PEX", ""))
    ap.add_argument("--only", choices=("name", "uuid"), default=None)
    ap.add_argument("--port", type=int, default=18087)
    a = ap.parse_args()
    if not os.path.exists(MCUSIM):
        print("SKIP: no blegatt mcusim at %s" % MCUSIM)
        return 77
    a.build = os.path.abspath(os.path.expanduser(a.build))
    elf = os.path.join(a.build, "btgw.elf")
    blob = os.path.join(a.build, "btgw_wifi_firmware.bin")
    if not os.path.exists(elf) or not os.path.exists(blob):
        print("SKIP: %s has no btgw.elf and btgw_wifi_firmware.bin" % a.build)
        return 77
    if not os.path.exists(os.path.join(ROOT, "partitions.json")):
        print("SKIP: partitions.json missing")
        return 77
    pex = ensure_pex(a.pex or None)
    print("pex:", pex, "build:", a.build, flush=True)
    total = Checks()
    hunts = [h for h in HUNTS if a.only in (None, h["id"])]
    for i, hunt in enumerate(hunts):
        part = run_hunt(a.build, pex, hunt, a.port + i)
        total.n += part.n
        total.fails += part.fails
    print("%d checks, %d failed" % (total.n, total.fails), flush=True)
    return 0 if total.fails == 0 and total.n else 1


if __name__ == "__main__":
    sys.exit(main())
