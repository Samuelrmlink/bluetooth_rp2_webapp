#!/usr/bin/env python3
"""Upload a firmware release over SMP/UDP on a lossy link -- with retransmits.

  tools/smp_upload.py --ip 10.255.255.23 btgw-1.234.smp.bin [--mtu 512] [--rto 1.5]

smpmgr / smpclient send each request once over UDP and then wait out the whole
timeout: one lost datagram (request or reply) ends the upload, and on a link
that loses a few percent of its packets (WireGuard over a busy uplink) a
3000-packet upload rarely finishes. This sends the same request again after
--rto seconds instead. That is safe: an ImageUploadWrite carries its offset,
and the firmware answers an offset it has already passed with the offset it is
at (img_mgmt.c, the resume protocol), so a request whose reply was lost is
simply acknowledged again. The first request carries the file's SHA-256, so a
new run resumes an upload the board still has in progress.

A request that stays unanswered for --stall resends is reported, and the board
is asked an SMP echo then: an answer means only that request is being lost or
refused; silence means the SMP server itself is not answering.

Then, as with smpmgr: `image state-read`, `image state-write <hash>`,
`os reset`, and after checking the new image, `image state-write --confirm`.
Needs smpclient (pipx install smpmgr brings it).
"""
from __future__ import annotations

import argparse
import hashlib
import socket
import sys
import time

import cbor2
from smp import header as smphdr
from smpclient import SMPClient
from smpclient.requests.image_management import ImageUploadWrite
from smpclient.requests.os_management import EchoWrite
from smpclient.transport.udp import SMPUDPTransport


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("file")
    ap.add_argument("--ip", required=True)
    ap.add_argument("--port", type=int, default=1337)
    ap.add_argument("--mtu", type=int, default=512, help="IP MTU the datagrams are sized for (default 512)")
    ap.add_argument("--rto", type=float, default=1.5, help="seconds before a request is sent again")
    ap.add_argument("--stall", type=int, default=8, help="resends of one request before it is reported")
    ap.add_argument("--give-up", type=float, default=300.0, help="seconds without progress before giving up")
    a = ap.parse_args()

    image = open(a.file, "rb").read()
    tr = SMPUDPTransport(mtu=a.mtu)
    tr._is_ipv6 = ":" in a.ip
    cl = SMPClient(tr, a.ip)                      # only its packet builder is used
    s = socket.socket(socket.AF_INET6 if ":" in a.ip else socket.AF_INET, socket.SOCK_DGRAM)
    s.connect((a.ip, a.port))

    seq = [0]

    def build(off: int) -> tuple[bytes, int]:
        first = off == 0
        req = ImageUploadWrite(off=off, data=b"", len=len(image) if first else None, image=0 if first else None,
                               sha=hashlib.sha256(image).digest() if first else None)
        pkt = bytearray(bytes(cl._maximize_upload_packet(req, image)))
        seq[0] = (seq[0] + 1) & 0xFF
        pkt[6] = seq[0]                           # header byte 6: sequence
        return bytes(pkt), seq[0]

    def exchange(pkt: bytes, sq: int, rto: float) -> dict | None:
        s.send(pkt)
        end = time.time() + rto
        while True:
            left = end - time.time()
            if left <= 0:
                return None
            s.settimeout(left)
            try:
                r = s.recv(4096)
            except socket.timeout:
                return None
            if len(r) >= 8 and r[6] == sq:        # a stale reply to an earlier send is skipped
                return cbor2.loads(r[8:]) if len(r) > 8 else {}

    def echo() -> bool:
        e = bytearray(bytes(EchoWrite(d="smp_upload")))
        seq[0] = (seq[0] + 1) & 0xFF
        e[6] = seq[0]
        return exchange(bytes(e), seq[0], 3.0) is not None

    t0 = last_progress = time.time()
    off, sends, resends, stalls = 0, 0, 0, []
    while off < len(image):
        pkt, sq = build(off)
        tries = 0
        while True:
            sends += 1
            rsp = exchange(pkt, sq, a.rto)
            if rsp is not None:
                break
            tries += 1
            resends += 1
            if tries == a.stall:
                alive = echo()
                stalls.append((off, alive))
                print(f"\n  stall at {off}: {tries} sends unanswered; the SMP server "
                      f"{'answers an echo' if alive else 'does not answer an echo either'}", flush=True)
            if time.time() - last_progress > a.give_up:
                print(f"\ngave up at {off}/{len(image)}: nothing for {a.give_up:.0f} s", flush=True)
                return 1
        if rsp.get("rc") or rsp.get("err"):
            print(f"\nthe board refused the upload at {off}: {rsp}", flush=True)
            return 1
        new = rsp.get("off")
        if new is None:
            print(f"\nno offset in the reply at {off}: {rsp}", flush=True)
            return 1
        if new != off:
            last_progress = time.time()
        off = new
        el = time.time() - t0
        print(f"\r  {off}/{len(image)} B  {100 * off / len(image):5.1f}%  {off / max(el, 1e-3) / 1000:5.1f} kB/s  "
              f"{resends} resends", end="", flush=True)
    el = time.time() - t0
    print(f"\ndone: {len(image)} B in {el:.0f} s, {sends} requests, {resends} resends "
          f"({100 * resends / max(sends, 1):.1f}%), {len(stalls)} stalls{': ' + str(stalls) if stalls else ''}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
