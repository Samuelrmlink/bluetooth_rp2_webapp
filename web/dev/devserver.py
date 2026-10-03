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
     "mfg_data": "ffff027ce913601fd802b405444a573604", "service_uuids": ["ff09"],
     "adv": "020106030209ff12ffffff027ce913601fd802b405444a573604", "rsp": "1209415348444a573631463435313030333438"},
    {"addr": "C4:11:22:33:44:55", "transport": "le", "addr_type": 1, "rssi": -52,
     "connectable": True, "connected": False, "name": "HRM-9000",
     "service_uuids": ["180d", "180f"], "tx_power": -12,
     "adv": "02010605030d180f18020af4040848524d04160f1857", "rsp": "090948524d2d3930303011069ecadc240ee5a9e093f3a3b50100406e"},
    {"addr": "D9:AA:BB:CC:DD:EE", "transport": "le", "addr_type": 1, "rssi": -88,
     "connectable": False, "connected": False, "mfg_data": "4c0012020003",
     "adv": "02010607ff4c0012020003"},
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
        # A vendor service: 128-bit UUIDs, and every kind of write.
        {"id": "3", "uuid": "12345678-1234-5678-9abc-def012345678", "characteristics": [
            {"id": "3.0", "uuid": "12345678-1234-5678-9abc-def012345679", "properties": 14,
             "flags": ["read", "write_no_response", "write"], "descriptors": []},
            {"id": "3.1", "uuid": "12345678-1234-5678-9abc-def01234567a", "properties": 4,
             "flags": ["write_no_response"], "descriptors": []},
            {"id": "3.2", "uuid": "12345678-1234-5678-9abc-def01234567b", "properties": 32,
             "flags": ["indicate"], "descriptors": [{"id": "3.2.0", "uuid": "2902"}]}]},
    ],
}

# What a read returns, per characteristic id; writes to a readable one update it.
MOCK_VALUES_BASE = {"0.0": "6d6f636b", "2.0": "57", "3.0": "0102"}
MOCK_VALUES = dict(MOCK_VALUES_BASE)

def _chr(cid):
    for s in MOCK_TREE["services"]:
        for c in s["characteristics"]:
            if c["id"] == cid:
                return c
    return None

# --- device state shared by every connection, as on a real gateway ----------
#
# A real gateway is one device that every tab talks to, so the filesystem, the
# Lua runtime and the C1000's settings live here, not per connection.
# `test.reset` (mock only) puts all of it back, so each Playwright test starts
# from the same device.
REPO = os.path.join(WEB, "..")

def _png(w, h):
    """A small, valid RGB PNG (green), for the image preview."""
    import zlib
    def chunk(t, d): return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    raw = b"".join(b"\0" + b"\x4f\xd0\x7f" * w for _ in range(h))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))

def _seed_fs():
    fs = {}
    for rel in ["lua/anker.lua", "lua/anker2.lua", "lua/pinecil.lua",
                "lua/profiles/anker/run.lua", "lua/profiles/anker/link.lua",
                "lua/profiles/anker/models/A1761.lua"]:
        path = os.path.join(REPO, rel)
        if os.path.exists(path):
            fs[rel] = open(path, "rb").read()
    fs["cfg/wifi.json"] = b'{"mode":"auto","networks":[{"ssid":"bench"}]}'
    fs["var/wg_epoch.bin"] = b"\x4e\x00\x00\x00"
    # a web app's folder, stored as the device stores web files: compressed,
    # plus pictures (fw 1.95 editor: .gz editing, .bak, image preview)
    import gzip as _gz
    fs["www/demo.js.gz"] = _gz.compress(b"// demo asset\nexport const answer = 42;\n", mtime=0)
    fs["www/logo.svg.gz"] = _gz.compress(b'<svg xmlns="http://www.w3.org/2000/svg" width="40" height="20">'
                                          b'<rect width="40" height="20" fill="#4fd07f"/></svg>\n', mtime=0)
    fs["www/dot.png"] = _png(3, 2)
    return fs

def _mime(path):
    ext = path.rsplit(".", 1)[-1].lower() if "." in path else ""
    return {"js": "text/javascript", "css": "text/css", "html": "text/html", "json": "application/json",
            "svg": "image/svg+xml", "png": "image/png", "jpg": "image/jpeg", "jpeg": "image/jpeg",
            "lua": "text/plain"}.get(ext, "application/octet-stream")

def fnv1a(b):
    h = 2166136261
    for x in b: h = ((h ^ x) * 16777619) & 0xFFFFFFFF
    return "%08x" % h

C1000_BASE = {
    "addr": "E8:EE:CC:B6:89:7E", "product_code": "A1761", "product_type": "B105",
    "model_name": "SOLIX C1000(X) Portable Power Station", "serial": "APC5YX0D43200118",
    "battery_pct": 87, "remaining_time_h": 19.5, "total_output_power_w": 24, "total_input_power_w": 0,
    "ac_output_power_w": 24, "ac_input_power_w": 0, "solar_input_power_w": 0, "bms_temp_major_c": 29,
    "ac_output_enabled": 1, "car_output_enabled": 0, "screen_enabled": 1, "ambient_light": 0,
    "ac_frequency_hz": 60, "ac_charge_power_w": 600, "device_timeout_min": 720, "ultra_fast_charging": 0,
    "unknown_b1": 3, "capability_block": "\x02\x01\x01\x01", "fault_major": 0,
    "ac_countdown_s": 0, "car_countdown_s": 0,
}
# The A2687 charger, alongside: on the bench lua/anker2.lua drives it while
# lua/anker.lua drives the C1000. The mock publishes both while "anker" runs.
# Shape from a real report (2026-10-01); ports are tables, as in the model.
A2687_BASE = {
    "addr": "7C:E9:13:60:1F:D8", "product_type": "B405", "product_code": "A2687", "cipher": "gcm",
    "model_name": "Anker Prime 160W 3-Port GaN Charger (smart display)", "serial": "ASHDJW61F45100348",
    "c1_power": {"mv": 20080, "ma": 2980, "watts": 59.8, "charging": True},
    "c2_power": {"mv": 5096, "ma": 100, "watts": 0.5, "charging": True},
    "c3_power": {"mv": 0, "ma": 0, "watts": 0.0, "charging": False},
    "c1_control": {"on": True, "countdown_s": 300, "countdown_remain_s": 300, "vid": 0, "cable": "100W", "countdown_active": False},
    "c2_control": {"on": True, "countdown_s": 300, "countdown_remain_s": 300, "vid": 0, "cable": "60W", "countdown_active": False},
    "c3_control": {"on": False, "countdown_s": 300, "countdown_remain_s": 300, "vid": 0, "cable": "none", "countdown_active": False},
    "fixed_allocation": "c1_priority", "backlight_pct": 25, "screen_off": "12h", "error_code": 0,
    "charging_protocol": 0, "package_version": 1606, "language": "en", "direction": "up",
}

C1000_SETTERS = {"ac_output_enabled": (0, 1), "car_output_enabled": (0, 1),
                 "ambient_light": (0, 1, 3), "screen_enabled": (0, 1),
                 # the countdown timers: seconds, 0 cancels (a range, not a list)
                 "ac_countdown_s": range(0, 86401), "car_countdown_s": range(0, 86401),
                 "ac_charge_power_w": range(100, 1301)}
# A countdown running out switches its output off, as on the C1000.
TIMER_OUTPUT = {"ac_countdown_s": "ac_output_enabled", "car_countdown_s": "car_output_enabled"}

STATE = {}
def reset_state():
    STATE.clear()
    STATE["fs"] = _seed_fs()
    # anker runs from boot, as on the bench, with its modules resident
    STATE["scripts"] = {"anker": {"running": True, "draft": None, "tier": "committed",
                                  "hash": fnv1a(STATE["fs"].get("lua/anker.lua", b""))}}
    STATE["modules"] = {m: fnv1a(STATE["fs"][f"lua/{m.replace('.', '/')}.lua"])
                        for m in ["profiles.anker.run", "profiles.anker.link", "profiles.anker.models.A1761"]
                        if f"lua/{m.replace('.', '/')}.lua" in STATE["fs"]}
    STATE["c1000"] = dict(C1000_BASE)
    STATE["drafts"] = {}
    MOCK_VALUES.clear(); MOCK_VALUES.update(MOCK_VALUES_BASE)
reset_state()

def script_file(name, tier):
    return f"{'staged' if tier == 'staged' else 'lua'}/{name}.lua"

class MockGateway:
    """One instance per connected browser."""
    def __init__(self, send):
        self.send = send
        self.devices = {d["addr"]: dict(d) for d in MOCK_DEVICES}
        self.scanning = False
        self.subscribed = set()     # (addr, id, mode)
        self.policy = "on_demand"
        threading.Thread(target=self._tick, daemon=True).start()

    def _tick(self):
        hr = 62
        while True:
            time.sleep(1.0)
            try:
                if STATE["scripts"].get("anker", {}).get("running"):
                    c = STATE["c1000"]
                    for f, out in TIMER_OUTPUT.items():
                        end = STATE.setdefault("timer_end", {}).get(f)
                        if end:
                            left = max(0, int(end - time.time()))
                            c[f] = left
                            if left == 0:
                                STATE["timer_end"].pop(f, None)
                                c[out] = 0
                    c["ac_output_power_w"] = c["total_output_power_w"] = (22 + int(time.time()) % 5) if c["ac_output_enabled"] else 0
                    self.send({"evt": "profile.anker.telemetry", "data": dict(c)})
                    self.send({"evt": "profile.anker.telemetry", "data": dict(A2687_BASE)})
                if self.scanning:
                    for d in self.devices.values():
                        d["rssi"] = max(-95, min(-40, d["rssi"] + (hash(time.time()) % 5) - 2))
                        self.send({"evt": "scan.device", "data": d})
                for key in list(self.subscribed):
                    addr, cid, mode = key
                    hr = 60 + (hr + 3) % 40
                    c = _chr(cid) or {"uuid": "2a37"}
                    self.send({"evt": "char.value", "data": {
                        "addr": addr, "id": cid, "uuid": c["uuid"], "source": mode,
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
            # Paged like the firmware (1.85+), two per page here so the
            # client's page walk is exercised whatever `limit` it asks for.
            devs = list(self.devices.values())
            off = int(p.get("offset", 0)); lim = min(int(p.get("limit", 1000)), 2)
            page = devs[off:off + lim]
            nxt = off + len(page)
            return {"devices": page, "scanning": self.scanning, "total": len(devs),
                    "next": nxt if nxt < len(devs) else None}
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
            cid = p.get("id", "0.0")
            c = _chr(cid)
            if not c or "read" not in c["flags"]: raise ApiError("not_permitted", "not readable")
            v = MOCK_VALUES.get(cid, "")
            self.send({"evt": "char.value", "data": {
                "addr": d["addr"], "id": cid, "uuid": c["uuid"],
                "source": "read", "len": len(v) // 2, "value": v}})
            return {"reading": True}
        if op == "gatt.write":
            cid = p.get("id", "")
            c = _chr(cid)
            if not c or not ({"write", "write_no_response"} & set(c["flags"])): raise ApiError("not_permitted", "not writable")
            STATE.setdefault("gatt_writes", []).append({"id": cid, "value": p.get("value", ""), "response": p.get("response", True)})
            if "read" in c["flags"]: MOCK_VALUES[cid] = p.get("value", "")
            return {"written": len(p.get("value", "")) // 2, "response": p.get("response", True)}
        if op == "test.gatt_writes":
            return {"writes": STATE.get("gatt_writes", [])}
        if op == "bt.mtu":
            if "max_mtu" in p:
                v = int(p["max_mtu"])
                if v < 23 or v > 255: raise ApiError("bad_request", "max_mtu is 23..255 on this build")
                STATE["max_mtu"] = v
            m = STATE.get("max_mtu", 255)
            return {"max_mtu": m, "ceiling": 255, "saved": STATE.get("max_mtu", 0), "reboot_needed": False,
                    "links": [{"addr": d["addr"], "mtu": MOCK_TREE["mtu"]} for d in self.devices.values() if d["connected"]]}
        if op == "gatt.subscribe":
            d = self.devices.get(p.get("device"))
            if not d or not d["connected"]: raise ApiError("not_connected", "not connected")
            cid = p.get("id", "1.0")
            self.subscribed = {k for k in self.subscribed if not (k[0] == d["addr"] and k[1] == cid)}
            if p.get("mode") != "off": self.subscribed.add((d["addr"], cid, p.get("mode", "notify")))
            return {"mode": p.get("mode", "notify")}
        if op == "bond.policy":
            if "policy" in p:
                if p["policy"] not in ("never", "on_demand", "always"):
                    raise ApiError("bad_request", "policy is never|on_demand|always")
                self.policy = p["policy"]
            return {"policy": self.policy}
        if op == "bond.list":
            return {"policy": self.policy, "bonds": [], "io_capability": STATE.get("io", "no_input_no_output"),
                    "mitm": STATE.get("mitm", False)}
        if op == "bond.io":
            opts = ["no_input_no_output", "display_only", "display_yes_no", "keyboard_only", "keyboard_display"]
            if "io_capability" in p:
                if p["io_capability"] not in opts:
                    raise ApiError("bad_request", "io_capability is " + "|".join(opts))
                STATE["io"] = p["io_capability"]
            if "mitm" in p:
                STATE["mitm"] = bool(p["mitm"])
            return {"io_capability": STATE.get("io", "no_input_no_output"), "mitm": STATE.get("mitm", False), "options": opts}
        if op in ("bond.passkey", "bond.confirm"):
            # As the firmware: any answer is announced so every prompt closes.
            STATE.setdefault("answers", []).append({"op": op, **p})
            self.send({"evt": "security.answered", "data": {"addr": p.get("device"), "by": "api"}})
            if p.get("cancel") or p.get("accept") is False:
                self.send({"evt": "security.failed", "data": {"addr": p.get("device"), "status": 0x13, "reason": 8}})
            else:
                self.send({"evt": "security.bonded", "data": {"addr": p.get("device")}})
            return {"supplied": True}
        if op == "sys.reboot":
            self.send({"evt": "sys.rebooting", "data": {}})
            STATE["rebooted"] = STATE.get("rebooted", 0) + 1
            return {"rebooting": True, "in_ms": 500}
        if op == "script.heap":
            used = 18016 if not STATE["scripts"].get("anker", {}).get("running") else 85424
            return {"arena": {"bytes": 163840, "used": used, "peak": 133768, "blocks": 412, "free_blocks": 9,
                              "free_bytes": 163840 - used, "largest_free": 55120, "alloc_fails": 0,
                              "alloc_fail_last": 0, "header": 16,
                              "free_hist": [{"size": "<64", "n": 3, "bytes": 96}, {"size": ">=16K", "n": 1, "bytes": 55120}]},
                    "objects": {"short_string": {"n": 812, "bytes": 21500}, "table": {"n": 140, "bytes": 30200},
                                "lua_closure": {"n": 320, "bytes": 9100}, "proto": {"n": 290, "bytes": 15800},
                                "thread": {"n": 2, "bytes": 3100}, "total": 79700},
                    "strtab": {"size": 1024, "used": 812},
                    "tables": [{"bytes": 6200, "array": 0, "hash": 256, "name": "registry"},
                               {"bytes": 4100, "array": 0, "hash": 128, "name": "module:profiles.anker.run"}],
                    "threads": [2600, 500], "unaccounted": used - 79700}
        if op == "script.arena":
            # a tiny but well-formed arena: one 64 B live table block, then one free block
            import struct
            size, base, off = 4096, 0x20010000, int(p.get("offset", 0))
            raw = bytearray(size)
            struct.pack_into("<IIB", raw, 0, 64, base + 80, 0)
            struct.pack_into("<IBB", raw, 16, 0, 0x05, 0)
            struct.pack_into("<IIB", raw, 80, size - 96, 0, 1)
            chunk = bytes(raw[off:off + min(int(p.get("len", 1024)), 1024)])
            return {"base": base, "size": size, "header": 16, "offset": off, "len": len(chunk), "hex": chunk.hex()}
        if op == "script.vm_restart":
            running = [k for k, v in STATE["scripts"].items() if v.get("running")]
            for v in STATE["scripts"].values():
                v["running"] = False
            if p.get("resume"):
                for k in running:
                    STATE["scripts"][k]["running"] = True
            r = {"restarted": True, "stopped": len(running), "resumed": len(running) if p.get("resume") else 0,
                 "before": 85424, "after": 15640, "largest_free_before": 61640, "largest_free": 144000}
            self.send({"evt": "script.vm_restarted", "data": r})
            return r
        if op == "test.pairing":
            # e2e only: the firmware's question, as if a device had asked.
            topic = {"input": "security.passkey_request", "compare": "security.numeric_comparison",
                     "display": "security.passkey_display"}[p.get("action", "input")]
            data = {"addr": p.get("device", "C4:11:22:33:44:55"), "action": p.get("action", "input")}
            if "number" in p:
                data["number"] = p["number"]
            self.send({"evt": topic, "data": data})
            return {"sent": topic}
        if op == "test.mkdir":
            STATE.setdefault("dirs", set()).add(str(p.get("path", "")).strip("/"))
            return {"ok": True}
        if op == "test.answers":
            return {"answers": STATE.get("answers", [])}
        if (op.startswith("script.") and op not in ("script.heap", "script.arena", "script.vm_restart")) \
                or op in ("anker.set", "anker.screensaver", "test.reset"):
            return self._script_op(op, p)
        raise ApiError("unknown_op", op)

    # --- the Lua runtime, as far as the web app sees it ---------------------
    def _script_op(self, op, p):
        fs, scripts = STATE["fs"], STATE["scripts"]
        name = p.get("script") or p.get("name")
        if op == "test.reset":
            reset_state(); return {"reset": True}
        if op == "script.list":
            files = [{"name": k[4:-4], "kind": "script", "tier": "committed", "bytes": len(v)}
                     for k, v in fs.items() if k.startswith("lua/") and k.count("/") == 1 and k.endswith(".lua")]
            files += [{"name": k[7:-4], "kind": "staged", "tier": "staged", "bytes": len(v)}
                      for k, v in fs.items() if k.startswith("staged/") and k.endswith(".lua")]
            return {"scripts": [{"name": n, "running": s["running"], "dead": False, "draft": s["draft"] is not None,
                                 "resumes": 1, "error": ""} for n, s in scripts.items()],
                    "ops": ["anker.set", "anker.screensaver"] if scripts.get("anker", {}).get("running") else [],
                    "files": files,
                    "runtime": {"arena_bytes": 163840, "used": 81920, "peak": 120000, "hook_kills": 0,
                                "budget": 20000, "slice_overruns": 0, "slice_max_ms": 25}}
        if op == "script.loaded":
            units = []
            for n, s in scripts.items():
                if not s["running"]: continue
                f = None if s["tier"] == "draft" else fs.get(script_file(n, s["tier"]))
                units.append({"name": n, "kind": "script", "tier": s["tier"], "hash": s["hash"],
                              "now": fnv1a(f) if f is not None else None})
            if scripts.get("anker", {}).get("running"):
                for m, h in STATE["modules"].items():
                    f = fs.get(f"lua/{m.replace('.', '/')}.lua")
                    units.append({"name": m, "kind": "module", "tier": "committed", "hash": h,
                                  "now": fnv1a(f) if f is not None else None, "users": 1})
            return {"units": units, "truncated": False}
        if op == "script.draft":
            buf = STATE["drafts"].get(name, "") if p.get("offset", 0) else ""
            buf += p.get("source", "")
            STATE["drafts"][name] = buf
            s = scripts.setdefault(name, {"running": False, "draft": None, "tier": "committed", "hash": ""})
            if p.get("final", True):
                s["draft"] = buf
                if p.get("run", True):
                    s.update(running=True, tier="draft", hash=fnv1a(buf.encode()))
            return {"draft": True, "bytes": len(buf), "flash_written": False}
        if op in ("script.start", "script.reload"):
            s = scripts.setdefault(name, {"running": False, "draft": None, "tier": "committed", "hash": ""})
            if s["draft"] is not None:
                s.update(running=True, tier="draft", hash=fnv1a(s["draft"].encode()))
            else:
                for tier in ("staged", "committed"):
                    f = fs.get(script_file(name, tier))
                    if f is not None:
                        s.update(running=True, tier=tier, hash=fnv1a(f)); break
                else:
                    raise ApiError("start_failed", "no draft, staged or committed source for that name")
            return {"started": True} if op == "script.start" else {"reloaded": True}
        if op == "script.stop":
            if name in scripts: scripts[name]["running"] = False
            return {"stopped": True}
        if op == "script.revert":
            s = scripts.get(name)
            if s and s["draft"] is not None: s["draft"] = None
            else: fs.pop(script_file(name, "staged"), None)
            return {"reverted": True}
        if op == "script.stage":
            s = scripts.get(name)
            if not s or s["draft"] is None: raise ApiError("stage_failed", "no draft to stage")
            fs[script_file(name, "staged")] = s["draft"].encode()
            return {"staged": True}
        if op == "script.commit":
            s = scripts.get(name) or {}
            src = s.get("draft").encode() if s.get("draft") is not None else fs.get(script_file(name, "staged"))
            if src is None: raise ApiError("commit_failed", "nothing staged")
            fs[script_file(name, "committed")] = src
            return {"committed": True}
        if op == "script.eval":
            return {"ok": True, "value": f"(mock) {p.get('chunk', '')}"}
        if op == "anker.screensaver":
            # The A2687's upload, as decrypted from the app's own: select says
            # 0x11 (not cached) unless the id was sent before; begin is 0;
            # data acks at window ends with packets received, 0x10 when done.
            if str(p.get("device", "")).upper() != A2687_BASE["addr"]:
                return {"error": "not_this_device", "message": "no screensaver-capable device there"}
            ss = STATE.setdefault("screensaver", {"cached": set(), "got": set(), "count": 0, "id": None})
            step = p.get("step")
            ev = {"addr": A2687_BASE["addr"], "step": step}
            if step == "select":
                ev.update(id=p.get("id"), status=0 if p.get("id") in ss["cached"] else 0x11)
            elif step == "begin":
                ss.update(got=set(), count=int(p.get("count", 0)), id=p.get("id"))
                ev.update(id=p.get("id"), status=0)
            elif step == "data":
                if len(str(p.get("a", ""))) + len(str(p.get("b", ""))) == 0:
                    return {"error": "bad_request", "message": "data needs n and a/b hex"}
                ss["got"].add(int(p.get("n", -1)))
                if not p.get("ack"):
                    return {"queued": True, "step": step}
                have = 0
                while have in ss["got"]:
                    have += 1
                done = have >= ss["count"] > 0
                if done:
                    ss["cached"].add(ss["id"])
                ev.update(n=p.get("n"), status=0x10 if done else 0, received=have)
            else:
                return {"error": "bad_request", "message": "step is select, begin or data"}
            threading.Timer(0.05, lambda: self.send({"evt": "profile.anker.screensaver", "data": ev})).start()
            return {"queued": True, "step": step}
        if op == "anker.set":
            # As the driver: only captured setters, only their listed values;
            # a refusal is a result carrying `error`, not a failed call.
            f, v = p.get("field"), p.get("value")
            if f not in C1000_SETTERS: return {"error": "unknown_field", "message": f"{f} has no captured setter"}
            if v not in C1000_SETTERS[f]: return {"error": "bad_value", "message": f"{f} takes {C1000_SETTERS[f]}"}
            if f in TIMER_OUTPUT:
                def start(f=f, v=v):
                    STATE.setdefault("timer_end", {})
                    if v: STATE["timer_end"][f] = time.time() + v
                    else: STATE["timer_end"].pop(f, None)
                    STATE["c1000"][f] = v
                threading.Timer(0.3, start).start()
            else:
                threading.Timer(0.3, lambda: STATE["c1000"].__setitem__(f, v)).start()
            return {"queued": True, "field": f, "value": v}
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
        if self.path.startswith("/api/fs"):
            return self._fs("GET")
        if self.path.startswith("/api/tar"):
            return self._tar()
        return super().do_GET()

    def do_PUT(self): self._fs("PUT")
    def do_DELETE(self): self._fs("DELETE")
    def do_POST(self): self._fs("POST")

    def do_OPTIONS(self):
        self.send_response(204)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")
        self.end_headers()

    def _reply(self, code, body, ctype="application/json", extra=None):
        data = body if isinstance(body, bytes) else json.dumps(body).encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        for k, v in (extra or {}).items(): self.send_header(k, v)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        self.wfile.write(data)

    # /api/fs, in memory, with the device's shapes: a one-level listing
    # {"dir", "block", "files": [{name, dir:true} | {name, size, alloc}]}, raw
    # file bodies, {"ok":false,"error"} on failure. Folders are implicit,
    # except empty ones (STATE["dirs"], made by test.mkdir); DELETE removes a
    # file or an EMPTY folder, 409 for a folder with anything in it, as fw 1.94.
    def _fs(self, method):
        from urllib.parse import unquote
        fs = STATE["fs"]
        path = unquote(self.path.split("?")[0][len("/api/fs"):]).strip("/")
        if method == "POST" and path == "rename":
            req = json.loads(self.rfile.read(int(self.headers.get("Content-Length", 0))) or b"{}")
            src, dst = req.get("from"), req.get("to")
            dirs = STATE.setdefault("dirs", set())
            if dst in fs or dst in dirs or any(k.startswith(dst + "/") for k in fs):
                return self._reply(409, {"ok": False, "error": "exists"})
            if src in fs:
                fs[dst] = fs.pop(src); return self._reply(200, {"ok": True})
            inside = [k for k in list(fs) if k.startswith(src + "/")]
            sub = [d for d in list(dirs) if d == src or d.startswith(src + "/")]
            if not inside and not sub: return self._reply(404, {"ok": False, "error": "not found"})
            for k in inside: fs[dst + k[len(src):]] = fs.pop(k)          # a folder moves whole, as lfs_rename
            for d in sub: dirs.discard(d); dirs.add(dst + d[len(src):])
            return self._reply(200, {"ok": True})
        dirs = STATE.setdefault("dirs", set())
        if method == "PUT" and self.path.split("?")[0].endswith("/"):
            # PUT /api/fs/<folder>/ makes the folder, parents too (fw 1.95)
            if path in fs: return self._reply(409, {"ok": False, "error": "a file has that name"})
            parts = path.split("/")
            for i in range(1, len(parts) + 1): dirs.add("/".join(parts[:i]))
            return self._reply(200, {"ok": True, "dir": True})
        if method == "PUT":
            n = int(self.headers.get("Content-Length", 0))
            if n > 128 * 1024: return self._reply(413, {"ok": False, "error": "too large"})
            fs[path] = self.rfile.read(n); return self._reply(200, {"ok": True, "name": path, "bytes": n})
        if method == "DELETE":
            if path in fs:
                del fs[path]; return self._reply(200, {"ok": True})
            if any(k.startswith(path + "/") for k in list(fs) + list(dirs)):
                return self._reply(409, {"ok": False, "error": "folder is not empty"})
            if path in dirs:
                dirs.discard(path); return self._reply(200, {"ok": True})
            return self._reply(404, {"ok": False, "error": "not found"})
        if path in fs:
            if path.endswith(".gz"):
                # as the device: any .gz goes out Content-Encoding: gzip, typed
                # as what it holds, so a browser fetch gets the contents
                return self._reply(200, fs[path], _mime(path[:-3]), {"Content-Encoding": "gzip"})
            return self._reply(200, fs[path], _mime(path))
        pre = path + "/" if path else ""
        names = {}
        for k in list(fs) + [d + "/" for d in dirs]:
            if k.startswith(pre):
                rest = k[len(pre):]
                if not rest: continue
                if "/" in rest: names[rest.split("/")[0]] = None
                else: names[rest] = len(fs[k])
        if not names and path and path not in dirs: return self._reply(404, {"ok": False, "error": "not found"})
        def alloc(sz):     # as fs.c fs_file_alloc: inline up to 256 B, else CTZ blocks
            if sz <= 256: return 0
            off, b = sz - 1, 4096 - 8
            i = off // b
            if i: i = (off - 4 * (bin(i - 1).count("1") + 2)) // b
            return (i + 1) * 4096
        files = [{"name": n, "dir": True} if sz is None else {"name": n, "size": sz, "alloc": alloc(sz)}
                 for n, sz in sorted(names.items())]
        return self._reply(200, {"dir": path, "block": 4096, "files": files} if path else {"block": 4096, "files": files})

    # GET /api/tar/<folder> (or /api/tar/ for everything), as fw 1.95 streams
    # it: ustar, the folder's own entry first, names relative to its parent,
    # file data exactly as stored, mtime from ?mtime=.
    def _tar(self):
        import io, tarfile
        from urllib.parse import unquote, urlparse, parse_qs
        u = urlparse(self.path)
        folder = unquote(u.path[len("/api/tar"):]).strip("/")
        mtime = int((parse_qs(u.query).get("mtime") or ["0"])[0] or 0)
        fs, dirs = STATE["fs"], STATE.setdefault("dirs", set())
        pre = folder + "/" if folder else ""
        files = sorted(k for k in fs if k.startswith(pre))
        alld = set(d for d in dirs if d == folder or d.startswith(pre))
        for k in files:
            parts = k.split("/")
            for i in range(1, len(parts)): alld.add("/".join(parts[:i]))
        alld = {d for d in alld if d == folder or d.startswith(pre)}
        if folder and folder not in alld:
            return self._reply(404, {"ok": False, "error": "no such folder"})
        base = folder.rsplit("/", 1)[0] + "/" if "/" in folder else ""
        b = io.BytesIO()
        with tarfile.open(fileobj=b, mode="w", format=tarfile.USTAR_FORMAT) as t:
            for name in sorted(alld | set(files)):
                i = tarfile.TarInfo(name[len(base):])
                i.mtime, i.uname, i.gname = mtime, "btgw", "btgw"
                if name in fs:
                    i.size, i.mode = len(fs[name]), 0o644
                    t.addfile(i, io.BytesIO(fs[name]))
                else:
                    i.type, i.mode = tarfile.DIRTYPE, 0o755
                    t.addfile(i)
        name = (folder.rsplit("/", 1)[-1] if folder else "btgw") + ".tar"
        return self._reply(200, b.getvalue(), "application/x-tar",
                           {"Content-Disposition": f'attachment; filename="{name}"'})

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
