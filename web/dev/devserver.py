#!/usr/bin/env python3
"""Serve web/ and answer the WebSocket API, so the app can be built without a board.

Stdlib only -- no npm, no pip. Two modes:

  --mock              synthetic devices; the default, and what the Playwright
                      tests run against
  --device HOST       proxy /ws and /api to a real gateway, so the same page can
                      be driven against hardware with live reload

The mock speaks the real envelope (id/op/params -> id/ok/result, plus evt
frames) because a mock that speaks a dialect of its own tests nothing.
"""
import argparse, base64, hashlib, json, os, re, socket, struct, threading, time
from http.server import ThreadingHTTPServer, SimpleHTTPRequestHandler

GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
WEB = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")

# --- a small synthetic estate -------------------------------------------------
MOCK_DEVICES = [
    {"addr": "7C:E9:13:60:1F:D8", "transport": "le", "addr_type": 0, "rssi": -67,
     "connectable": True, "connected": False, "name": "ASHDJW61F45100348",
     "mfg_data": "ffff027ce913601fd802b405444a573604", "service_uuids": ["ff09"]},
    {"addr": "C4:11:22:33:44:55", "transport": "le", "addr_type": 1, "rssi": -52,
     "connectable": True, "connected": False, "name": "HRM-9000",
     "service_uuids": ["180d", "180f"]},
    {"addr": "D9:AA:BB:CC:DD:EE", "transport": "le", "addr_type": 1, "rssi": -88,
     "connectable": False, "connected": False, "mfg_data": "4c0012020003"},
]

MOCK_TREE = {
    "mtu": 255, "ready": True, "truncated": False,
    "services": [
        {"id": "0", "uuid": "1800", "characteristics": [
            {"id": "0.0", "uuid": "2a00", "properties": 2, "flags": ["read"], "descriptors": []}]},
        {"id": "1", "uuid": "180d", "characteristics": [
            {"id": "1.0", "uuid": "2a37", "properties": 16, "flags": ["notify"],
             "descriptors": [{"id": "1.0.0", "uuid": "2902"}]}]},
        {"id": "2", "uuid": "180f", "characteristics": [
            {"id": "2.0", "uuid": "2a19", "properties": 18, "flags": ["read", "notify"],
             "descriptors": [{"id": "2.0.0", "uuid": "2902"}]}]},
    ],
}

class MockGateway:
    """One instance per connected browser."""
    def __init__(self, send):
        self.send = send
        self.devices = {d["addr"]: dict(d) for d in MOCK_DEVICES}
        self.scanning = False
        self.subscribed = set()
        self.policy = "on_demand"
        threading.Thread(target=self._tick, daemon=True).start()

    def _tick(self):
        hr = 62
        while True:
            time.sleep(1.0)
            try:
                if self.scanning:
                    for d in self.devices.values():
                        d["rssi"] = max(-95, min(-40, d["rssi"] + (hash(time.time()) % 5) - 2))
                        self.send({"evt": "scan.device", "data": d})
                for key in list(self.subscribed):
                    addr, cid = key
                    hr = 60 + (hr + 3) % 40
                    self.send({"evt": "char.value", "data": {
                        "addr": addr, "id": cid, "uuid": "2a37", "source": "notify",
                        "len": 2, "value": "00%02x" % hr}})
            except Exception:
                return

    def op(self, op, p):
        if op == "sys.info":
            return {"firmware": "bluetooth_rp2_webapp (mock)", "uptime_ms": int(time.time() * 1000) % 10**7,
                    "transports": ["le"], "clients": 1, "modules": ["sys", "bt"]}
        if op == "bt.state":
            return {"ready": True, "address": "2C:CF:67:C7:DB:56", "max_links": 4}
        if op == "scan.start":
            self.scanning = True
            return {"scanning": True, "active": p.get("active", True),
                    "duration_ms": p.get("duration_ms", 0)}
        if op == "scan.stop":
            self.scanning = False
            self.send({"evt": "scan.done", "data": {"reason": "stopped"}})
            return {"scanning": False}
        if op == "scan.list":
            return {"devices": list(self.devices.values()), "scanning": self.scanning}
        if op == "dev.list":
            return {"devices": [d for d in self.devices.values() if d["connected"]]}
        if op == "dev.connect":
            d = self.devices.get(p.get("device"))
            if not d: raise ApiError("unknown_device", "not in the scan table")
            d["connected"] = True
            self.send({"evt": "dev.connected", "data": d})
            self.send({"evt": "gatt.discovered", "data": {"addr": d["addr"], "gatt": MOCK_TREE}})
            return {"connecting": True}
        if op == "dev.disconnect":
            d = self.devices.get(p.get("device"))
            if not d or not d["connected"]: raise ApiError("not_connected", "not connected")
            d["connected"] = False
            self.subscribed = {k for k in self.subscribed if k[0] != d["addr"]}
            self.send({"evt": "dev.disconnected", "data": d})
            return {"disconnecting": True}
        if op == "gatt.tree":
            d = self.devices.get(p.get("device"))
            if not d or not d["connected"]: raise ApiError("not_connected", "not connected")
            return MOCK_TREE
        if op == "gatt.read":
            d = self.devices.get(p.get("device"))
            if not d or not d["connected"]: raise ApiError("not_connected", "not connected")
            self.send({"evt": "char.value", "data": {
                "addr": d["addr"], "id": p.get("id", "0.0"), "uuid": "2a00",
                "source": "read", "len": 4, "value": "6d6f636b"}})
            return {"reading": True}
        if op == "gatt.write":
            return {"written": len(p.get("value", "")) // 2, "response": p.get("response", True)}
        if op == "gatt.subscribe":
            d = self.devices.get(p.get("device"))
            if not d or not d["connected"]: raise ApiError("not_connected", "not connected")
            key = (d["addr"], p.get("id", "1.0"))
            if p.get("mode") == "off": self.subscribed.discard(key)
            else: self.subscribed.add(key)
            return {"mode": p.get("mode", "notify")}
        if op == "bond.policy":
            if "policy" in p:
                if p["policy"] not in ("never", "on_demand", "always"):
                    raise ApiError("bad_request", "policy is never|on_demand|always")
                self.policy = p["policy"]
            return {"policy": self.policy}
        if op == "bond.list":
            return {"policy": self.policy, "bonds": []}
        raise ApiError("unknown_op", op)

class ApiError(Exception):
    def __init__(self, code, msg): super().__init__(msg); self.code, self.msg = code, msg

def ws_send(sock, text):
    p = text.encode()
    if len(p) < 126:  hdr = struct.pack("!BB", 0x81, len(p))
    elif len(p) < 1 << 16: hdr = struct.pack("!BBH", 0x81, 126, len(p))
    else: hdr = struct.pack("!BBQ", 0x81, 127, len(p))
    sock.sendall(hdr + p)

def ws_recv(sock):
    h = sock.recv(2)
    if len(h) < 2: return None
    ln = h[1] & 0x7F
    if ln == 126: ln = struct.unpack("!H", sock.recv(2))[0]
    elif ln == 127: ln = struct.unpack("!Q", sock.recv(8))[0]
    mask = sock.recv(4) if h[1] & 0x80 else None
    data = b""
    while len(data) < ln:
        c = sock.recv(ln - len(data))
        if not c: return None
        data += c
    if mask: data = bytes(b ^ mask[i % 4] for i, b in enumerate(data))
    if (h[0] & 0x0F) == 0x8: return None      # close
    return data.decode(errors="replace")

class Handler(SimpleHTTPRequestHandler):
    def __init__(self, *a, **kw): super().__init__(*a, directory=WEB, **kw)
    def log_message(self, fmt, *args):
        if "--verbose" in os.environ.get("DEVSERVER_OPTS", ""): super().log_message(fmt, *args)

    def do_GET(self):
        if self.path.split("?")[0] == "/ws":
            return self._ws()
        return super().do_GET()

    def _ws(self):
        key = self.headers.get("Sec-WebSocket-Key")
        if not key:
            self.send_error(400); return
        acc = base64.b64encode(hashlib.sha1((key + GUID).encode()).digest()).decode()
        self.send_response(101)
        self.send_header("Upgrade", "websocket")
        self.send_header("Connection", "Upgrade")
        self.send_header("Sec-WebSocket-Accept", acc)
        self.end_headers()
        sock = self.connection
        lock = threading.Lock()
        def send(obj):
            with lock: ws_send(sock, json.dumps(obj))
        gw = MockGateway(send)
        try:
            while True:
                txt = ws_recv(sock)
                if txt is None: break
                try: msg = json.loads(txt)
                except Exception: continue
                mid = msg.get("id", 0)
                try:
                    send({"id": mid, "ok": True, "result": gw.op(msg.get("op", ""), msg.get("params") or {})})
                except ApiError as e:
                    send({"id": mid, "ok": False, "error": {"code": e.code, "message": e.msg}})
                except Exception as e:
                    send({"id": mid, "ok": False, "error": {"code": "internal", "message": str(e)}})
        except OSError:
            pass

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--bind", default="127.0.0.1")
    a = ap.parse_args()
    print(f"serving {WEB} on http://{a.bind}:{a.port}  (mock gateway on /ws)")
    ThreadingHTTPServer((a.bind, a.port), Handler).serve_forever()

if __name__ == "__main__":
    main()
