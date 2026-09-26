#!/usr/bin/env python3
"""Minimal WebSocket client for the gateway's JSON API.

Stdlib only, so it runs anywhere python3 does -- the same reasoning as the
filesystem API staying on plain HTTP: a CLI client should never need a package
install to talk to the board.

  python3 tools/wsapi.py sys.info
  python3 tools/wsapi.py scan.start '{"duration_ms":6000}' --listen 8
"""
import argparse, base64, json, os, socket, struct, sys, time

class WS:
    def __init__(self, host, port=80, path="/ws", timeout=10):
        self.s = socket.create_connection((host, port), timeout=timeout)
        key = base64.b64encode(os.urandom(16)).decode()
        self.s.sendall(("GET %s HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\n"
                        "Connection: Upgrade\r\nSec-WebSocket-Key: %s\r\n"
                        "Sec-WebSocket-Version: 13\r\n\r\n" % (path, host, key)).encode())
        hdr = b""
        while b"\r\n\r\n" not in hdr:
            c = self.s.recv(1)
            if not c:
                raise RuntimeError("closed during handshake")
            hdr += c
        if b"101" not in hdr.split(b"\r\n")[0]:
            raise RuntimeError("upgrade refused: %s" % hdr.split(b"\r\n")[0])
        self.buf = b""

    def send(self, obj):
        p = json.dumps(obj).encode()
        m = os.urandom(4)
        # Extended length forms matter here: a script.draft payload is well over
        # the 125 bytes the short form can express.
        n = len(p)
        if n < 126:
            hdr = bytes([0x81, 0x80 | n])
        elif n < (1 << 16):
            hdr = bytes([0x81, 0x80 | 126]) + struct.pack("!H", n)
        else:
            hdr = bytes([0x81, 0x80 | 127]) + struct.pack("!Q", n)
        self.s.sendall(hdr + m + bytes(b ^ m[i % 4] for i, b in enumerate(p)))

    def recv(self, timeout=None):
        self.s.settimeout(timeout)
        try:
            h = self._need(2)
        except (socket.timeout, TimeoutError):
            return None
        ln = h[1] & 0x7F
        if ln == 126:
            ln = struct.unpack(">H", self._need(2))[0]
        elif ln == 127:
            ln = struct.unpack(">Q", self._need(8))[0]
        return self._need(ln).decode(errors="replace")

    def draft(self, script, source, chunk=600, run=True):
        """Push a Lua draft of any size. RAM only -- nothing is written to
        flash, which is the point of the draft tier; chunking exists so that
        stays true without needing a large per-connection receive buffer."""
        off, last = 0, None
        while off < len(source):
            part = source[off:off + chunk]
            final = (off + len(part)) >= len(source)
            self.send({"id": 1, "op": "script.draft", "params": {
                "script": script, "source": part, "offset": off,
                "final": final, "run": run}})
            while True:
                r = self.recv(timeout=8)
                if r is None:
                    raise RuntimeError("no reply to draft chunk at %d" % off)
                if r.startswith('{"id"'):
                    last = json.loads(r)
                    break
            if not last.get("ok"):
                return last
            off += len(part)
        return last

    def close(self):
        """Send a proper close frame. Without it the device holds the slot
        until keepalive expires, and a handful of test runs exhaust them."""
        try:
            m = os.urandom(4)
            self.s.sendall(bytes([0x88, 0x80]) + m)
            self.s.close()
        except OSError:
            pass

    def _need(self, n):
        while len(self.buf) < n:
            d = self.s.recv(4096)
            if not d:
                raise RuntimeError("closed")
            self.buf += d
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("op", nargs="?")
    ap.add_argument("params", nargs="?", default="{}")
    ap.add_argument("--host", default="192.168.8.1")
    ap.add_argument("--port", type=int, default=80)
    ap.add_argument("--listen", type=float, default=0, metavar="SECONDS",
                    help="after the reply, print events for this long")
    a = ap.parse_args()

    ws = WS(a.host, port=a.port)
    if a.op:
        ws.send({"id": 1, "op": a.op, "params": json.loads(a.params)})
        r = ws.recv(timeout=8)
        print(r)
    if a.listen:
        end = time.time() + a.listen
        while time.time() < end:
            line = ws.recv(timeout=max(0.2, end - time.time()))
            if line:
                print(line)
    ws.close()

if __name__ == "__main__":
    main()
