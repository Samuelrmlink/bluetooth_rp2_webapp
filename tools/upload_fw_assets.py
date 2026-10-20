#!/usr/bin/env python3
"""Uploads web-dist/ to a running km003c_fw device's /api/fs so web assets
and client behavior can be updated without reflashing.

Stdlib only, mirrors the confirmed-working reference's tools/pack_web.py
(--upload) at /home/samuel/Projects/pm220-pico2w — same PUT-per-file /
optional-sync-via-DELETE shape, adapted for: this project's separate
tools/make_release.py already produces web-dist/ (nothing is packed here),
and a nested asset tree (js/modals/*.js etc, not pm220's flat one) served
by GET/PUT/DELETE /api/fs/<path> on src/net/http.c.

Only *.gz files are uploaded: src/net/http.c's resolve_static() always
prefers "<name>.gz" over "<name>" when both exist, so shipping the plain
uncompressed copies too would just waste flash on the 6MiB LittleFS
region for no benefit — matching how the device is actually served.

Usage:
    python3 tools/upload_fw_assets.py                    # upload web-dist/*.gz
    python3 tools/upload_fw_assets.py --sync              # + delete remote .gz files not in web-dist/
    python3 tools/upload_fw_assets.py --list               # just list what's on the device
    python3 tools/upload_fw_assets.py --base http://btgw.local
"""
from __future__ import annotations

import argparse
import json
import sys
import urllib.error
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DIST = ROOT / "web-dist"

DEFAULT_BASE = "http://192.168.8.1"
NAME_MAX = 64      # must match src/fs/fs.h FS_NAME_MAX (per path segment)
FILE_MAX = 1024 * 1024  # must match src/fs/fs.h FS_FILE_MAX


def valid_segment(seg: str) -> bool:
    if not seg or len(seg) > NAME_MAX or seg[0] == ".":
        return False
    return all(ch.isalnum() or ch in "._-" for ch in seg)


def valid_path(rel: str) -> bool:
    return bool(rel) and all(valid_segment(seg) for seg in rel.split("/"))


def request(url: str, method: str = "GET", data: bytes | None = None, timeout: float = 30):
    req = urllib.request.Request(url, data=data, method=method)
    if data is not None:
        req.add_header("Content-Type", "application/octet-stream")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return resp.status, resp.read()
    except urllib.error.HTTPError as e:
        body = e.read() if e.fp else b""
        raise SystemExit(f"{method} {url} -> {e.code} {body.decode('utf-8', 'replace')}") from e
    except urllib.error.URLError as e:
        raise SystemExit(f"{method} {url} failed: {e.reason}") from e


def local_gz_files(src: Path) -> dict[str, Path]:
    if not src.is_dir():
        raise SystemExit(f"web-dist missing: {src} (run tools/make_release.py first?)")
    out: dict[str, Path] = {}
    for path in sorted(src.rglob("*.gz")):
        if not path.is_file():
            continue
        rel = path.relative_to(src).as_posix()
        if not valid_path(rel):
            raise SystemExit(f"{rel}: not a valid device fs path (see src/fs/fs.c valid_segment)")
        size = path.stat().st_size
        if size > FILE_MAX:
            raise SystemExit(f"{rel}: {size} bytes exceeds FS_FILE_MAX ({FILE_MAX})")
        out[rel] = path
    if not out:
        raise SystemExit(f"no *.gz files under {src}")
    return out


def remote_gz_files(base: str, dir_rel: str = "") -> dict[str, int]:
    """Recursively lists every *.gz file under dir_rel on the device,
    keyed by its path relative to the fs root. http.c's /api/fs listing
    is per-directory only (not recursive), so this walks it manually."""
    url = f"{base}/api/fs" if not dir_rel else f"{base}/api/fs/{dir_rel}"
    _, body = request(url)
    listing = json.loads(body.decode())
    out: dict[str, int] = {}
    for entry in listing.get("files", []):
        name = entry.get("name")
        if not name:
            continue
        rel = f"{dir_rel}/{name}" if dir_rel else name
        if entry.get("dir"):
            out.update(remote_gz_files(base, rel))
        elif name.endswith(".gz"):
            out[rel] = int(entry.get("size", 0))
    return out


def upload(base: str, files: dict[str, Path]) -> None:
    print(f"uploading {len(files)} files to {base}")
    for rel, path in files.items():
        data = path.read_bytes()
        status, body = request(f"{base}/api/fs/{rel}", method="PUT", data=data)
        print(f"  PUT {rel} {len(data)}B -> {status} {body.decode()}")


def sync(base: str, files: dict[str, Path]) -> None:
    remote = remote_gz_files(base)
    stale = [rel for rel in remote if rel not in files]
    if not stale:
        print("sync: nothing stale on device")
        return
    print(f"sync: deleting {len(stale)} stale file(s)")
    for rel in sorted(stale):
        status, body = request(f"{base}/api/fs/{rel}", method="DELETE")
        print(f"  DELETE {rel} -> {status} {body.decode()}")


def list_remote(base: str) -> None:
    remote = remote_gz_files(base)
    total = 0
    for rel in sorted(remote):
        size = remote[rel]
        total += size
        print(f"  {rel} {size}")
    print(f"  {len(remote)} files, {total} bytes")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--src", type=Path, default=DIST, help="local packed dir (default web-dist/)")
    ap.add_argument("--base", default=DEFAULT_BASE, help=f"device HTTP base URL (default {DEFAULT_BASE})")
    ap.add_argument("--sync", action="store_true", help="also DELETE remote .gz files not present locally")
    ap.add_argument("--list", action="store_true", help="list files already on the device and exit")
    args = ap.parse_args()

    base = args.base.rstrip("/")
    if args.list:
        list_remote(base)
        return 0

    files = local_gz_files(args.src)
    upload(base, files)
    if args.sync:
        sync(base, files)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
