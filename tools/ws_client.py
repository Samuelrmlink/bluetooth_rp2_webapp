#!/usr/bin/env python3
"""Minimal raw RFC6455 WebSocket client (stdlib only) for talking directly
to the RP2040 firmware's ws://<host>/ws API — the same ASCII line protocol
km003c-backend speaks over raw TCP (see src/proto/proto.c and
src/net/ws.c), just framed as WebSocket text frames. Useful for
live-debugging the meter protocol (checking what's actually flowing, or
issuing commands) without a browser tab or the ImGui frontend attached.

Mirrors km003c-frontend's src/net.cpp WS client logic (masked outgoing
frames, unmasked incoming frame parsing, no fragmentation/ping-pong —
matching src/net/ws.h's documented scope on the firmware side).

Usage:
    python3 tools/ws_client.py "ROLE CONTROL" "STATUS"
    python3 tools/ws_client.py --host 192.168.7.1 "ROLE CONTROL" "FOLLOW ON" --duration 0
    python3 tools/ws_client.py --duration 10 "ROLE CONTROL" "FOLLOW ON" "START 10"
"""
from __future__ import annotations

import argparse
import base64
import os
import socket
import struct
import sys
import time


class WsClient:
    def __init__(self, host: str, port: int = 80, path: str = "/ws", connect_timeout: float = 5):
        self.sock = socket.create_connection((host, port), timeout=connect_timeout)
        self.sock.settimeout(0.2)
        key = base64.b64encode(os.urandom(16)).decode()
        req = (
            f"GET {path} HTTP/1.1\r\n"
            f"Host: {host}\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            f"Sec-WebSocket-Key: {key}\r\n"
            "Sec-WebSocket-Version: 13\r\n\r\n"
        )
        self.sock.sendall(req.encode())
        resp = b""
        while b"\r\n\r\n" not in resp:
            resp += self.sock.recv(4096)
        head, _, rest = resp.partition(b"\r\n\r\n")
        if b"101" not in head.split(b"\r\n")[0]:
            raise RuntimeError(f"WS upgrade failed: {head!r}")
        self._buf = rest

    def send_line(self, line: str) -> None:
        payload = line.encode()
        mask = os.urandom(4)
        masked = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
        n = len(payload)
        if n <= 125:
            hdr = bytes([0x81, 0x80 | n])
        elif n <= 0xFFFF:
            hdr = bytes([0x81, 0x80 | 126]) + struct.pack(">H", n)
        else:
            hdr = bytes([0x81, 0x80 | 127]) + struct.pack(">Q", n)
        self.sock.sendall(hdr + mask + masked)

    def recv_lines(self, duration: float):
        """Yields decoded text-frame payloads (protocol lines) received
        within `duration` seconds (runs forever if duration <= 0)."""
        end = None if duration <= 0 else time.time() + duration
        while end is None or time.time() < end:
            try:
                data = self.sock.recv(4096)
                if data:
                    self._buf += data
            except socket.timeout:
                pass
            while True:
                if len(self._buf) < 2:
                    break
                b0, b1 = self._buf[0], self._buf[1]
                opcode = b0 & 0x0F
                masked = (b1 & 0x80) != 0
                plen = b1 & 0x7F
                hdr = 2
                if plen == 126:
                    if len(self._buf) < 4:
                        break
                    plen = struct.unpack(">H", self._buf[2:4])[0]
                    hdr = 4
                elif plen == 127:
                    if len(self._buf) < 10:
                        break
                    plen = struct.unpack(">Q", self._buf[2:10])[0]
                    hdr = 10
                mask_len = 4 if masked else 0
                if len(self._buf) < hdr + mask_len + plen:
                    break
                payload = self._buf[hdr + mask_len : hdr + mask_len + plen]
                if masked:
                    mask = self._buf[hdr : hdr + 4]
                    payload = bytes(c ^ mask[i % 4] for i, c in enumerate(payload))
                self._buf = self._buf[hdr + mask_len + plen :]
                if opcode == 0x1:  # text
                    yield payload.decode(errors="replace")
                elif opcode == 0x8:  # close
                    return

    def close(self) -> None:
        self.sock.close()


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("cmds", nargs="*", help="protocol lines to send right after connecting, e.g. \"ROLE CONTROL\"")
    ap.add_argument("--host", default="192.168.7.1", help="device host/IP (default 192.168.7.1)")
    ap.add_argument("--port", type=int, default=80, help="device HTTP/WS port (default 80)")
    ap.add_argument("--duration", type=float, default=5.0, help="seconds to listen after sending (0 = forever, Ctrl+C to stop; default 5)")
    args = ap.parse_args()

    ws = WsClient(args.host, args.port)
    print(f"connected to ws://{args.host}:{args.port}/ws", file=sys.stderr)
    for cmd in args.cmds:
        ws.send_line(cmd)
        print(">>", cmd)
    try:
        for line in ws.recv_lines(args.duration):
            print("<<", line)
    except KeyboardInterrupt:
        pass
    ws.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
