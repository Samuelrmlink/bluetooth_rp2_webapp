#!/usr/bin/env python3
"""The Lua file / json / http / sys APIs on the real firmware, in mcusim.

  test/sim/lua_net_fs.py [build-dir]           (default: build)
  test/sim/lua_net_fs.py --radio [build-dir]   the radio scenario instead

Starts, on this PC only (127.0.0.1 -- mcusim's USB-network NAT maps the PC's
address on the USB link to localhost):
  * an HTTP server with the shapes the client must handle: Content-Length,
    chunked, redirects (absolute and relative), POST echo, 404, a slow one;
  * `openssl s_server` with a throwaway certificate, for TLS (the device does
    not trust it, so the script tests both `insecure` and the refusal).
Then boots the firmware in mcusim over USB-NCM, PUTs test/sim/lnf_test.lua
to lua/, starts it through POST /api/op (so that is tested too), and waits
for lfs:/lnf_result.json. Exit status 0 when every check passed.

--radio boots the OTA image from its A/B partition table with the CYW43
model: Wi-Fi joins mcusim's virtual AP (DNS answered offline from fixed
records, NTP from the host clock, a mock HTTP server -- nothing leaves the
simulator) and one BLE advertiser carries a 128-bit service UUID; then
test/sim/radio_test.lua checks sys.time (SNTP), DNS + HTTP over Wi-Fi,
uuids128 / age_ms, bt.mtu and bt.write without a link.

Nothing here talks to anything but 127.0.0.1.
"""
import http.server, json, os, signal, socketserver, subprocess, sys, tempfile, threading, time, urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
MCUSIM = os.path.expanduser("~/Work/mcusim/target/release/mcusim")
BOOTROM = os.path.expanduser("~/Work/mcusim/third_party/bootrom/rp2350-A2/bootrom-combined.bin")
HTTP_PORT, TLS_PORT, FWD = 18090, 18443, 18082
PC_IP = "192.168.8.16"      # what the firmware's DHCP server hands the (virtual) PC


class H(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def send(self, code, body, ctype="text/plain", extra=()):
        b = body if isinstance(body, bytes) else body.encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(b)))
        for k, v in extra:
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(b)

    def do_GET(self):
        p = self.path
        if p == "/small.json":
            return self.send(200, '{"hello":"world"}', "application/json")
        if p == "/chunked":
            self.send_response(200)
            self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()
            left = 20000
            n = 1
            while left:
                k = min(left, n)
                self.wfile.write(b"%x;ext=1\r\n" % k + b"x" * k + b"\r\n")
                left -= k
                n = n * 3 % 4093 + 1
            self.wfile.write(b"0\r\nX-Trailer: t\r\n\r\n")
            return
        if p == "/redirect":
            return self.send(302, "", extra=[("Location", "/small.json")])
        if p == "/a/redirect-rel":
            return self.send(301, "moved", extra=[("Location", "target")])
        if p == "/a/target":
            return self.send(200, "rel ok")
        if p == "/big":
            return self.send(200, bytes(i * 7 % 251 for i in range(300000)), "application/octet-stream")
        if p == "/slow":
            time.sleep(8)
            return self.send(200, "late")
        return self.send(404, "nope")

    def do_POST(self):
        n = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(n).decode()
        if self.path == "/echo":
            return self.send(200, f"POST {body} {self.headers.get('X-Test', '-')}")
        return self.send(404, "nope")


class TS(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True


RADIO_TOML = """
[project]
name = "btgw-radio"
board = "pico2_w"
[boot]
bootrom = "A2"
reboot = "flash_update:Main A"
[flash]
partition_table = "{root}/partitions.json"
images = [ {{ file = "{bdir}/btgw_wifi_firmware.bin", partition = "Wi-Fi Firmware" }} ]
[firmware]
image = "{bdir}/btgw.elf"
partition = "Main A"
[[device]]
type = "cyw43439"
name = "cyw43"
[device.network]
kind = "sim"
dns_upstream = "none"
dns_records = {{ "files.example" = "10.42.0.50", "pool.ntp.org" = "10.42.0.1" }}
backend = {{ kind = "mock", service = [{{ kind = "http", port = 80, response = [{{ path = "/hello.json", body = '{{"via":"wifi"}}' }}] }}] }}
[[device.ble_peer]]
addr = "c0:11:22:33:44:55"
random = true
adv = [0x02, 0x01, 0x06, 0x11, 0x07, 0x50, 0xEA, 0xDA, 0x30, 0x88, 0x83, 0xB8, 0x9F, 0x60, 0x4F, 0x15, 0xF3, 0x01, 0x00, 0x40, 0x8E]
[run]
time = "900s"
rtt = "stdio"
usb_host = "net"
usb_net_forward = ["{fwd}:80"]
"""


def radio(bdir):
    d = tempfile.mkdtemp(prefix="lnf_radio_")
    toml = os.path.join(d, "radio.toml")
    with open(toml, "w") as f:
        f.write(RADIO_TOML.format(root=ROOT, bdir=os.path.join(ROOT, bdir), fwd=FWD))
    log = open(os.path.join(d, "rtt.log"), "w")
    sim = subprocess.Popen([MCUSIM, "-p", toml], stdout=log, stderr=subprocess.STDOUT)
    base = f"http://127.0.0.1:{FWD}"
    try:
        t0 = time.time()
        while time.time() - t0 < 120:
            try:
                urllib.request.urlopen(base + "/api/fs", timeout=5).read()
                break
            except Exception:
                time.sleep(2)
        else:
            print("FAIL: no /api/fs"); return 1
        urllib.request.urlopen(urllib.request.Request(base + "/api/wifi/connect", method="POST",
                               data=b'{"ssid":"mcusim","psk":"mcusim123"}'), timeout=30).read()
        st = {}
        for _ in range(60):
            st = json.load(urllib.request.urlopen(base + "/api/wifi", timeout=10))
            if st.get("ip") not in (None, "", "0.0.0.0"):
                break
            time.sleep(1)
        print("wifi:", st.get("ip"))
        src = open(os.path.join(ROOT, "test/sim/radio_test.lua")).read().encode()
        urllib.request.urlopen(urllib.request.Request(base + "/api/fs/lua/radio_test.lua", data=src, method="PUT"),
                               timeout=30).read()
        rep = json.load(urllib.request.urlopen(urllib.request.Request(
            base + "/api/op", method="POST", data=b'{"op":"script.start","params":{"name":"radio_test"}}'), timeout=30))
        print("POST /api/op script.start ->", rep)
        res = None
        for _ in range(60):
            time.sleep(5)
            try:
                res = json.load(urllib.request.urlopen(base + "/api/fs/radio_result.json", timeout=10))
                break
            except Exception:
                log.flush()
                if any("radio_test died" in l for l in open(log.name, errors="replace")):
                    break
        fails = 0
        for r in res or []:
            fails += 0 if r["ok"] else 1
            print(("ok   " if r["ok"] else "FAIL ") + r["name"] + (f"  [{r['extra']}]" if r.get("extra") else ""))
        done = bool(res) and res[-1].get("name") == "done"
        print(f"{len(res or [])} checks, {fails} failed" + ("" if done else " (did not finish)"))
        return 0 if done and not fails else 1
    finally:
        sim.send_signal(signal.SIGTERM)
        try:
            sim.wait(10)
        except Exception:
            sim.kill()
        log.close()
        print("RTT log:", log.name)


def main():
    if len(sys.argv) > 1 and sys.argv[1] == "--radio":
        if not os.path.exists(MCUSIM):
            print("SKIP: no mcusim"); return 77
        return radio(sys.argv[2] if len(sys.argv) > 2 else "build")
    bdir = sys.argv[1] if len(sys.argv) > 1 else "build"
    elf = os.path.join(ROOT, bdir, "btgw.elf")
    if not os.path.exists(MCUSIM):
        print("SKIP: no mcusim"); return 77
    srv = TS(("127.0.0.1", HTTP_PORT), H)
    threading.Thread(target=srv.serve_forever, daemon=True).start()

    d = tempfile.mkdtemp(prefix="lnf_tls_")
    subprocess.run("openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -keyout k.pem "
                   "-out c.pem -subj /CN=test -days 2 2>/dev/null", shell=True, cwd=d, check=True)
    with open(os.path.join(d, "small.json"), "w") as f:
        f.write('{"hello":"world"}')
    tls = subprocess.Popen(["openssl", "s_server", "-accept", f"127.0.0.1:{TLS_PORT}", "-cert", "c.pem", "-key",
                            "k.pem", "-WWW", "-tls1_2", "-quiet"], cwd=d,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    log = open(os.path.join(d, "rtt.log"), "w")
    sim = subprocess.Popen([MCUSIM, "--bootrom", BOOTROM, "--set", "boot=direct", "--rtt", "stdio",
                            "--usb-host", "net", "--usb-net-forward", f"{FWD}:80", "-t", "900s", elf],
                           stdout=log, stderr=subprocess.STDOUT)
    base = f"http://127.0.0.1:{FWD}"
    rc = 1
    try:
        t0 = time.time()
        while time.time() - t0 < 180:
            try:
                urllib.request.urlopen(base + "/api/fs", timeout=5).read()
                break
            except Exception:
                time.sleep(2)
        else:
            print("FAIL: the firmware never answered /api/fs"); return 1
        print(f"booted in {time.time() - t0:.0f} s")
        src = open(os.path.join(ROOT, "test/sim/lnf_test.lua")).read()
        src = src.replace("@PC@", PC_IP).replace("@HTTP_PORT@", str(HTTP_PORT)).replace("@TLS_PORT@", str(TLS_PORT))
        urllib.request.urlopen(urllib.request.Request(base + "/api/fs/lua/lnf_test.lua", data=src.encode(),
                                                      method="PUT"), timeout=30).read()
        req = urllib.request.Request(base + "/api/op", method="POST",
                                     data=json.dumps({"op": "script.start", "params": {"name": "lnf_test"}}).encode())
        rep = json.load(urllib.request.urlopen(req, timeout=30))
        print("POST /api/op script.start ->", rep)
        if not rep.get("ok"):
            return 1
        res = None
        t0 = time.time()
        while time.time() - t0 < 600:
            time.sleep(5)
            try:
                res = json.load(urllib.request.urlopen(base + "/api/fs/lnf_result.json", timeout=10))
            except Exception:
                continue
            if res and res[-1].get("name") == "done":
                break
            log.flush()
            died = [l for l in open(log.name, errors="replace") if "lnf_test died" in l]
            if died:
                print("script died:", died[-1].strip()); break
        fails = 0
        for r in res or []:
            mark = "ok  " if r["ok"] else "FAIL"
            fails += 0 if r["ok"] else 1
            print(f"{mark} {r['name']}" + (f"  [{r['extra']}]" if r.get("extra") else ""))
        done = bool(res) and res[-1].get("name") == "done"
        print(f"{len(res or [])} checks, {fails} failed" + ("" if done else " (did not finish)"))
        rc = 0 if done and fails == 0 else 1
    finally:
        sim.send_signal(signal.SIGTERM)
        try:
            sim.wait(10)
        except Exception:
            sim.kill()
        tls.terminate()
        srv.shutdown()
        log.close()
        print("RTT log:", os.path.join(d, "rtt.log"))
    return rc


if __name__ == "__main__":
    sys.exit(main())
