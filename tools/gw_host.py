#!/usr/bin/env python3
"""Host the BTGW web app on a PC and manage boards found on the LAN.

The page is this tree's web/. Boards advertise _btgw._tcp (port 80). This
process browses that service, answers mdns.peers / mdns.browse on /ws, and
proxies /ws/peer/<name> and /api/peer/<name>/ to the board. Bind 0.0.0.0 so
a machine on the gateway-LAN switch (the W5500 side) can open the page.

    python3 tools/gw_host.py --web web --port 8080
    python3 tools/gw_host.py --self-test

No third-party packages. mDNS is a UDP/5353 querier.
"""

import argparse
import base64
import hashlib
import json
import os
import select
import socket
import struct
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

MCAST = "224.0.0.251"
MDNS_PORT = 5353
DROP_S = 15.0
WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
FILE_MAX = 1024 * 1024

MIME = {
    ".html": "text/html; charset=utf-8",
    ".js": "text/javascript; charset=utf-8",
    ".css": "text/css; charset=utf-8",
    ".json": "application/json",
    ".svg": "image/svg+xml",
    ".png": "image/png",
    ".ico": "image/x-icon",
    ".woff2": "font/woff2",
    ".map": "application/json",
}


def _name_at(pkt, off):
    """Decode a DNS name at off. Returns (text, next_offset)."""
    labels = []
    jumped = False
    nxt = off
    seen = 0
    while off < len(pkt) and seen < 16:
        n = pkt[off]
        if n == 0:
            if not jumped:
                nxt = off + 1
            return ".".join(labels), nxt
        if n & 0xC0 == 0xC0:
            if off + 1 >= len(pkt):
                break
            ptr = ((n & 0x3F) << 8) | pkt[off + 1]
            if not jumped:
                nxt = off + 2
            off = ptr
            jumped = True
            seen += 1
            continue
        if n > 63 or off + 1 + n > len(pkt):
            break
        labels.append(pkt[off + 1:off + 1 + n].decode("utf-8", "replace"))
        off += 1 + n
        if not jumped:
            nxt = off
        seen += 1
    return ".".join(labels), nxt


def _encode_name(name):
    out = bytearray()
    for label in name.rstrip(".").split("."):
        b = label.encode()
        out.append(len(b))
        out += b
    out.append(0)
    return bytes(out)


def parse_mdns(pkt):
    """Pull _btgw PTR/SRV/TXT/A records out of one message."""
    if len(pkt) < 12:
        return []
    qd, an, ns, ar = struct.unpack("!HHHH", pkt[4:12])
    off = 12
    for _ in range(qd):
        _, off = _name_at(pkt, off)
        off += 4
        if off > len(pkt):
            return []
    recs = []
    for _ in range(an + ns + ar):
        if off >= len(pkt):
            break
        name, off = _name_at(pkt, off)
        if off + 10 > len(pkt):
            break
        typ, _cls, _ttl, rdlen = struct.unpack("!HHIH", pkt[off:off + 10])
        off += 10
        rd_off = off
        off += rdlen
        if off > len(pkt):
            break
        recs.append((typ, name, rd_off, rdlen))
    return recs


def _label0(name):
    return name.split(".", 1)[0] if name else ""


class Browser:
    def __init__(self):
        self.peers = {}
        self.lock = threading.Lock()
        self._kick = threading.Event()
        self._stop = threading.Event()
        self.sock = None

    def start(self):
        t = threading.Thread(target=self._run, name="mdns", daemon=True)
        t.start()

    def kick(self):
        self._kick.set()

    def stop(self):
        self._stop.set()
        self._kick.set()

    def endpoint(self, name):
        now = time.monotonic()
        with self.lock:
            p = self.peers.get(name.lower())
            if not p or now - p["seen"] > DROP_S or not p.get("ip"):
                return None
            return p["ip"], p.get("port") or 80

    def snapshot(self):
        now = time.monotonic()
        local = _local_ips()
        out = []
        with self.lock:
            dead = []
            for key, p in self.peers.items():
                age = now - p["seen"]
                if age > DROP_S:
                    dead.append(key)
                    continue
                out.append({
                    "name": p["name"],
                    "ip": p.get("ip") or "",
                    "port": p.get("port") or 80,
                    "board": p.get("board") or "",
                    "assets": p.get("assets", -1),
                    "age_ms": int(age * 1000),
                    "self": p.get("ip") in local,
                })
            for key in dead:
                del self.peers[key]
        out.sort(key=lambda p: p["name"].lower())
        return {"peers": out}

    def _note(self, name, **fields):
        if not name:
            return
        with self.lock:
            p = self.peers.get(name.lower())
            if not p:
                p = {"name": name, "port": 80, "assets": -1, "board": "", "ip": ""}
                self.peers[name.lower()] = p
            p["seen"] = time.monotonic()
            for k, v in fields.items():
                if v is not None and v != "":
                    p[k] = v

    def _ingest(self, pkt):
        for typ, name, rd_off, rdlen in parse_mdns(pkt):
            rdata = pkt[rd_off:rd_off + rdlen]
            if typ == 12 and rdlen:
                target, _ = _name_at(pkt, rd_off)
                inst = _label0(target)
                if inst and "_btgw._tcp" in name:
                    self._note(inst)
            elif typ == 33 and rdlen >= 6:
                port = struct.unpack("!H", rdata[4:6])[0]
                self._note(_label0(name), port=port)
            elif typ == 16:
                board, assets = _txt(rdata)
                self._note(_label0(name), board=board, assets=assets)
            elif typ == 1 and rdlen == 4:
                ip = socket.inet_ntoa(rdata)
                self._note(_label0(name), ip=ip)

    def _run(self):
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        if hasattr(socket, "SO_REUSEPORT"):
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
        bound = False
        try:
            sock.bind(("", MDNS_PORT))
            mreq = struct.pack("4s4s", socket.inet_aton(MCAST), socket.inet_aton("0.0.0.0"))
            sock.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, mreq)
            bound = True
        except OSError:
            sock.close()
            sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock = sock
        sock.setblocking(False)
        query = _ptr_query()
        next_q = 0.0
        while not self._stop.is_set():
            now = time.monotonic()
            if self._kick.is_set() or now >= next_q:
                self._kick.clear()
                try:
                    sock.sendto(query, (MCAST, MDNS_PORT))
                except OSError:
                    pass
                next_q = now + 2.0
            r, _, _ = select.select([sock], [], [], 0.5)
            if not r:
                continue
            try:
                pkt, _addr = sock.recvfrom(1500)
            except OSError:
                continue
            self._ingest(pkt)


def _txt(rdata):
    board = ""
    assets = None
    i = 0
    while i < len(rdata):
        n = rdata[i]
        i += 1
        item = rdata[i:i + n].decode("utf-8", "replace")
        i += n
        if item.startswith("board="):
            board = item[6:]
        elif item.startswith("assets=") and item[7:].isdigit():
            assets = int(item[7:])
    return board, assets


def _ptr_query():
    name = _encode_name("_btgw._tcp.local")
    return struct.pack("!HHHHHH", 0, 0, 1, 0, 0, 0) + name + struct.pack("!HH", 12, 1)


def _local_ips():
    ips = {"127.0.0.1"}
    try:
        host = socket.gethostname()
        for res in socket.getaddrinfo(host, None, socket.AF_INET):
            ips.add(res[4][0])
    except OSError:
        pass
    return ips


def _js(obj):
    return json.dumps(obj, separators=(",", ":"))


def _ws_accept(key):
    dig = hashlib.sha1((key + WS_GUID).encode()).digest()
    return base64.b64encode(dig).decode()


def _recvn(sock, n):
    buf = bytearray()
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            return None
        buf += chunk
    return bytes(buf)


def _read_ws(sock):
    hdr = _recvn(sock, 2)
    if not hdr:
        return None
    opcode = hdr[0] & 0x0F
    ln = hdr[1] & 0x7F
    if ln == 126:
        ext = _recvn(sock, 2)
        if not ext:
            return None
        ln = struct.unpack("!H", ext)[0]
    elif ln == 127:
        ext = _recvn(sock, 8)
        if not ext:
            return None
        ln = struct.unpack("!Q", ext)[0]
    mask = _recvn(sock, 4) if hdr[1] & 0x80 else None
    data = _recvn(sock, ln) if ln else b""
    if data is None:
        return None
    if mask:
        data = bytes(b ^ mask[i % 4] for i, b in enumerate(data))
    return opcode, data


def _write_ws(sock, text):
    payload = text.encode()
    n = len(payload)
    if n < 126:
        hdr = bytes([0x81, n])
    elif n < 65536:
        hdr = bytes([0x81, 126]) + struct.pack("!H", n)
    else:
        hdr = bytes([0x81, 127]) + struct.pack("!Q", n)
    sock.sendall(hdr + payload)


def _shuttle(a, b):
    try:
        while True:
            r, _, _ = select.select([a, b], [], [], 30)
            if not r:
                continue
            for src in r:
                dst = b if src is a else a
                data = src.recv(8192)
                if not data:
                    return
                dst.sendall(data)
    except OSError:
        return
    finally:
        for s in (a, b):
            try:
                s.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass


def _peer_http(ip, port, method, path, headers, body):
    req = [f"{method} {path} HTTP/1.1", f"Host: {ip}", "Connection: close"]
    if body is not None:
        req.append(f"Content-Length: {len(body)}")
    ctype = headers.get("Content-Type")
    if ctype:
        req.append(f"Content-Type: {ctype}")
    raw = ("\r\n".join(req) + "\r\n\r\n").encode()
    if body:
        raw += body
    sock = socket.create_connection((ip, port), timeout=8)
    sock.settimeout(30)
    try:
        sock.sendall(raw)
        data = bytearray()
        while b"\r\n\r\n" not in data:
            chunk = sock.recv(4096)
            if not chunk:
                break
            data += chunk
            if len(data) > 65536:
                break
        head, _, rest = bytes(data).partition(b"\r\n\r\n")
        lines = head.decode("latin1", "replace").split("\r\n")
        status = 502
        if lines and lines[0].startswith("HTTP/"):
            parts = lines[0].split()
            if len(parts) >= 2 and parts[1].isdigit():
                status = int(parts[1])
        resp_headers = {}
        for line in lines[1:]:
            if ":" in line:
                k, v = line.split(":", 1)
                resp_headers[k.strip().lower()] = v.strip()
        body_out = rest
        need = resp_headers.get("content-length")
        if need and need.isdigit():
            while len(body_out) < int(need):
                chunk = sock.recv(8192)
                if not chunk:
                    break
                body_out += chunk
        else:
            while True:
                chunk = sock.recv(8192)
                if not chunk:
                    break
                body_out += chunk
        return status, resp_headers, body_out
    finally:
        sock.close()


def _peer_ws(ip, port):
    sock = socket.create_connection((ip, port), timeout=8)
    key = base64.b64encode(os.urandom(16)).decode()
    req = (
        "GET /ws HTTP/1.1\r\n"
        f"Host: {ip}\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        f"Sec-WebSocket-Key: {key}\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n"
    )
    sock.sendall(req.encode())
    data = bytearray()
    while b"\r\n\r\n" not in data:
        chunk = sock.recv(1024)
        if not chunk:
            break
        data += chunk
    if not bytes(data).startswith(b"HTTP/1.1 101"):
        sock.close()
        return None
    return sock


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    browser = None
    web = "."

    def log_message(self, fmt, *args):
        sys.stderr.write("%s\n" % (fmt % args))

    def _send(self, code, body, content_type="application/json", extra=None):
        if isinstance(body, str):
            body = body.encode()
        self.send_response(code)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Cache-Control", "no-cache")
        if extra:
            for k, v in extra.items():
                self.send_header(k, v)
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def _json_err(self, code, msg):
        self._send(code, _js({"ok": False, "error": msg}))

    def do_OPTIONS(self):
        self._send(204, b"", "text/plain", {
            "Access-Control-Allow-Methods": "GET, PUT, POST, DELETE, OPTIONS",
            "Access-Control-Allow-Headers": "Content-Type",
        })

    def do_HEAD(self):
        self.do_GET()

    def do_GET(self):
        path = self.path.split("?", 1)[0]
        if path == "/ws":
            return self._local_ws()
        if path.startswith("/ws/peer/"):
            return self._proxy_ws(path[len("/ws/peer/"):])
        if path.startswith("/api/peer/"):
            return self._proxy_http("GET")
        self._static(path)

    def do_PUT(self):
        self._proxy_http("PUT")

    def do_POST(self):
        self._proxy_http("POST")

    def do_DELETE(self):
        self._proxy_http("DELETE")

    def _split_peer(self):
        path = self.path.split("?", 1)[0]
        rest_q = self.path.split("?", 1)[1] if "?" in self.path else ""
        if not path.startswith("/api/peer/"):
            return None
        tail = path[len("/api/peer/"):]
        name, _, rest = tail.partition("/")
        from urllib.parse import unquote
        name = unquote(name)
        rest = "/" + rest if rest else "/"
        if rest_q:
            rest += "?" + rest_q
        return name, rest

    def _proxy_http(self, method):
        parts = self._split_peer()
        if not parts:
            self._json_err(404, "not found")
            return
        name, rest = parts
        ep = self.browser.endpoint(name)
        if not ep:
            self._json_err(404, "peer is not on this network")
            return
        length = int(self.headers.get("Content-Length") or 0)
        if length > FILE_MAX:
            self._json_err(413, "file too large")
            return
        body = self.rfile.read(length) if length else None
        try:
            status, hdrs, data = _peer_http(ep[0], ep[1], method, rest, self.headers, body)
        except OSError as e:
            self._json_err(502, f"could not reach the peer ({e})")
            return
        extra = {}
        if "content-type" in hdrs:
            ctype = hdrs["content-type"]
        else:
            ctype = "application/octet-stream"
        self._send(status, data, ctype, extra)

    def _proxy_ws(self, name):
        from urllib.parse import unquote
        name = unquote(name.split("/", 1)[0])
        key = self.headers.get("Sec-WebSocket-Key")
        if not key:
            self._json_err(400, "expected a WebSocket upgrade")
            return
        ep = self.browser.endpoint(name)
        if not ep:
            self._json_err(404, "peer is not on this network")
            return
        try:
            back = _peer_ws(*ep)
        except OSError:
            back = None
        if not back:
            self._json_err(502, "could not connect to the peer")
            return
        self.send_response(101, "Switching Protocols")
        self.send_header("Upgrade", "websocket")
        self.send_header("Connection", "Upgrade")
        self.send_header("Sec-WebSocket-Accept", _ws_accept(key))
        self.end_headers()
        _shuttle(self.connection, back)
        back.close()

    def _local_ws(self):
        key = self.headers.get("Sec-WebSocket-Key")
        if not key:
            self._json_err(400, "expected a WebSocket upgrade")
            return
        self.send_response(101, "Switching Protocols")
        self.send_header("Upgrade", "websocket")
        self.send_header("Connection", "Upgrade")
        self.send_header("Sec-WebSocket-Accept", _ws_accept(key))
        self.end_headers()
        sock = self.connection
        try:
            while True:
                frame = _read_ws(sock)
                if not frame:
                    break
                opcode, data = frame
                if opcode == 8:
                    break
                if opcode == 9:
                    sock.sendall(bytes([0x8A, len(data)]) + data)
                    continue
                if opcode != 1:
                    continue
                self._ws_op(sock, data)
        except OSError:
            pass

    def _ws_op(self, sock, data):
        try:
            msg = json.loads(data.decode())
        except (UnicodeError, json.JSONDecodeError):
            return
        op = msg.get("op")
        mid = msg.get("id")
        if op == "mdns.browse":
            self.browser.kick()
            time.sleep(0.3)
        if op in ("mdns.peers", "mdns.browse"):
            reply = {"id": mid, "ok": True, "result": self.browser.snapshot()}
        else:
            reply = {"id": mid, "ok": False, "error": {
                "code": "unsupported", "message": "not supported on this host"}}
        try:
            _write_ws(sock, _js(reply))
        except OSError:
            pass

    def _static(self, path):
        if path in ("/", ""):
            path = "/index.html"
        rel = path.lstrip("/")
        if ".." in rel.split("/"):
            self._json_err(400, "bad path")
            return
        full = os.path.join(self.web, rel)
        if not os.path.isfile(full):
            self._json_err(404, "not found")
            return
        ext = os.path.splitext(full)[1].lower()
        with open(full, "rb") as f:
            self._send(200, f.read(), MIME.get(ext, "application/octet-stream"))


def self_test():
    """One uncompressed announcement, the shape esp32c5/main/mdns_adv.c sends."""
    def nm(text):
        return _encode_name(text)

    inst = "ESP32C5-aabbccddeeff._btgw._tcp.local"
    host = "ESP32C5-aabbccddeeff.local"
    body = bytearray()

    def rr(name, typ, rdata):
        body.extend(nm(name))
        body.extend(struct.pack("!HHIH", typ, 0x8001, 10, len(rdata)))
        body.extend(rdata)

    rr("_btgw._tcp.local", 12, nm(inst))
    srv = struct.pack("!HHH", 0, 0, 80) + nm(host)
    rr(inst, 33, srv)
    txt = b""
    for item in (b"board=esp32c5", b"ws=/ws", b"api=/api", b"assets=0"):
        txt += bytes([len(item)]) + item
    rr(inst, 16, txt)
    rr(host, 1, socket.inet_aton("10.42.0.100"))
    pkt = struct.pack("!HHHHHH", 0, 0x8400, 0, 4, 0, 0) + bytes(body)
    b = Browser()
    b._ingest(pkt)
    snap = b.snapshot()
    peers = snap["peers"]
    assert len(peers) == 1, peers
    p = peers[0]
    assert p["name"] == "ESP32C5-aabbccddeeff", p
    assert p["ip"] == "10.42.0.100", p
    assert p["port"] == 80, p
    assert p["board"] == "esp32c5", p
    assert p["assets"] == 0, p
    assert b.endpoint("esp32c5-aabbccddeeff") == ("10.42.0.100", 80)
    assert b.endpoint("missing") is None
    print("gw_host self-test ok")


def main():
    ap = argparse.ArgumentParser(description="Host the BTGW web app and proxy mDNS peers")
    ap.add_argument("--web", default=None, help="directory that contains index.html")
    ap.add_argument("--addr", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()
    if args.self_test:
        self_test()
        return
    web = args.web or os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "web")
    if not os.path.isfile(os.path.join(web, "index.html")):
        sys.exit(f"no index.html in {web}")
    browser = Browser()
    browser.start()
    Handler.browser = browser
    Handler.web = web
    httpd = ThreadingHTTPServer((args.addr, args.port), Handler)
    print(f"btgw web on http://{args.addr}:{args.port}/  (mDNS _btgw._tcp)")
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        browser.stop()
        httpd.server_close()


if __name__ == "__main__":
    main()
