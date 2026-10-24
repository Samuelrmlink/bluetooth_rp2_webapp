#!/usr/bin/env python3
"""Deploy the browser PEX compiler and the Anker app.

    python3 tools/deploy_pex.py --base http://10.255.255.23

The compiler cache is web/.pex-cc/. This refreshes it with
web/pex-cc/make_pexcc.sh (the WASI pexcc, headers, host_api.json, and the
driver sources) and copies that tree to the SD card as pex-cc/. The editor
Build button reads it from there. Nothing under web/js/ is changed.

Anker's .c and .h files are copied to the SD card as projects/anker/src/,
which the editor shows as sd/projects/anker/src/. The Pexfile in
sd/projects/anker/ names those sources. The same sources also sit in the
compiler pack at pex-cc/drivers/. Nothing under littlefs pex/ is a source:
the compiled anker.m33.pex stays there. Copies of these sources left on
littlefs are removed. This script does not write or delete the older sd/pex/
tree.

The signed app file is lfs:/pex/anker.m33.pex. When build/pex-signed/anker.m33.pex
exists it is the file deployed; otherwise pass --module. An unsigned file is
refused. A different build that is already installed is left in place unless
--replace is given. iti is never uninstalled. cfg/ is never written. Lua is
never started. The board is not rebooted.

A file whose bytes already match the device is not sent again.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import struct
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CACHE = ROOT / "web" / ".pex-cc"
DRIVERS = ROOT / "pex" / "modules" / "drivers"
MAKE = ROOT / "web" / "pex-cc" / "make_pexcc.sh"
DEFAULT_MODULE = ROOT / "build" / "pex-signed" / "anker.m33.pex"
DEFAULT_BASE = "http://192.168.8.1"

ANKER_SOURCES = (
    "anker_app.c",
    "anker_adv.c",
    "anker_adv.h",
    "anker_wire.c",
    "anker_wire.h",
    "anker_models.c",
    "anker_models.h",
)

MAGIC = b"PEX\x1a"
SIG_TYPE = 0xF0


class HttpError(RuntimeError):
    def __init__(self, method: str, url: str, code: int, body: bytes):
        self.method = method
        self.url = url
        self.code = code
        self.body = body
        text = body.decode("utf-8", "replace")[:300]
        super().__init__(f"{method} {url} -> {code} {text}")


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def quote_path(rel: str) -> str:
    return "/".join(urllib.parse.quote(seg, safe="") for seg in rel.split("/"))


def pex_info(buf: bytes) -> tuple[str, bool]:
    """Return (build_id hex, signed). Refuses a buffer that is not a module.

    The header layout matches pexlink.parse: 16 bytes of identity, then
    features, api major/minor, and thirteen uint32 fields ending in
    section_count, section_table_off, image_off, crc32. The build id is
    the 16 bytes at offset 76.
    """
    if len(buf) < 92 or buf[:4] != MAGIC:
        raise SystemExit("not a .pex file")
    fields = struct.unpack_from("<IHHIIIIIIIIIIIII", buf, 16)
    count, table = fields[12], fields[13]
    end = table + 12 * count
    if count > 32 or end > len(buf):
        raise SystemExit("pex section table is outside the file")
    signed = False
    for i in range(count):
        typ = struct.unpack_from("<H", buf, table + 12 * i)[0]
        if typ == SIG_TYPE:
            signed = True
    return buf[76:92].hex(), signed


def request(url: str, method: str = "GET", data: bytes | None = None,
            timeout: float = 180, retries: int = 6) -> tuple[int, bytes]:
    last: Exception | None = None
    for attempt in range(retries):
        req = urllib.request.Request(url, data=data, method=method)
        if data is not None:
            req.add_header("Content-Type", "application/octet-stream")
        try:
            with urllib.request.urlopen(req, timeout=timeout) as resp:
                return resp.status, resp.read()
        except urllib.error.HTTPError as e:
            body = e.read() if e.fp else b""
            if e.code == 503 and attempt + 1 < retries:
                time.sleep(2)
                last = HttpError(method, url, e.code, body)
                continue
            if e.code == 404 and method == "GET":
                return 404, b""
            raise HttpError(method, url, e.code, body) from e
        except urllib.error.URLError as e:
            last = e
            reason = str(getattr(e, "reason", e)).lower()
            # A full LittleFS closes the socket instead of returning 500.
            # Retrying the same PUT only repeats the failed write.
            if method == "PUT" and ("broken pipe" in reason or "connection reset" in reason):
                raise HttpError(method, url, 0, reason.encode()) from e
            if attempt + 1 < retries:
                time.sleep(2)
                continue
            raise SystemExit(f"{method} {url} failed: {e.reason}") from e
    raise SystemExit(f"{method} {url} failed: {last}")


def op(base: str, name: str, params: dict | None = None) -> tuple[bool, dict]:
    payload = json.dumps({"op": name, "params": params or {}}).encode()
    status, body = request(base + "/api/op", "POST", payload, timeout=60)
    try:
        msg = json.loads(body)
    except json.JSONDecodeError as e:
        raise SystemExit(f"{name}: HTTP {status} is not JSON") from e
    if msg.get("ok"):
        return True, msg.get("result") or {}
    err = msg.get("error") or {"code": "error", "message": body[:200].decode("utf-8", "replace")}
    return False, err


def cache_files() -> dict[str, Path]:
    if not CACHE.is_dir():
        raise SystemExit(f"compiler cache missing: {CACHE} (make_pexcc.sh did not produce it)")
    out: dict[str, Path] = {}
    for path in sorted(CACHE.rglob("*")):
        if not path.is_file() or path.name.startswith("."):
            continue
        rel = "pex-cc/" + path.relative_to(CACHE).as_posix()
        if len(rel) > 96:
            raise SystemExit(f"{rel}: SD path longer than 96 characters")
        out[rel] = path
    wasm = out.get("pex-cc/pexcc.wasm")
    if wasm is None or wasm.stat().st_size < 100000:
        raise SystemExit("pex-cc/pexcc.wasm is missing from the cache")
    return out


def refresh_cache() -> None:
    if not MAKE.is_file():
        raise SystemExit(f"missing {MAKE}")
    print(f"refresh {MAKE}")
    subprocess.run([str(MAKE)], check=True)


def put_if_changed(url_base: str, api_prefix: str, rel: str, data: bytes, dry: bool) -> str:
    url = f"{url_base}{api_prefix}{quote_path(rel)}"
    status, remote = request(url)
    if status == 200 and sha256(remote) == sha256(data):
        print(f"  keep {rel} ({len(data)} B)")
        return "keep"
    if dry:
        print(f"  would put {rel} ({len(data)} B)")
        return "dry"
    status, body = request(url, "PUT", data)
    print(f"  put  {rel} ({len(data)} B) -> {status} {body.decode('utf-8', 'replace')[:80]}")
    return "put"


def deploy_tree(base: str, files: dict[str, bytes], api_prefix: str, dry: bool) -> None:
    for rel in sorted(files, key=lambda rel: (len(files[rel]), rel)):
        put_if_changed(base, api_prefix, rel, files[rel], dry)


def drop_lfs_sources(base: str, dry: bool) -> None:
    """Remove littlefs copies of the Anker sources. Compiled modules stay."""
    print("littlefs source copies")
    for name in ANKER_SOURCES:
        rel = f"pex/{name}"
        if dry:
            print(f"  would drop {rel}")
            continue
        url = f"{base}/api/fs/{quote_path(rel)}"
        try:
            status, body = request(url, "DELETE")
        except HttpError as e:
            if e.code == 404:
                print(f"  none {rel}")
                continue
            raise
        print(f"  drop {rel} -> {status} {body.decode('utf-8', 'replace')[:80]}")


def anker_sources() -> dict[str, bytes]:
    out: dict[str, bytes] = {}
    for name in ANKER_SOURCES:
        path = DRIVERS / name
        if not path.is_file():
            raise SystemExit(f"missing driver source {path}")
        out[f"projects/anker/src/{name}"] = path.read_bytes()
    return out


def module_bytes(path: Path | None) -> bytes | None:
    chosen = path if path else (DEFAULT_MODULE if DEFAULT_MODULE.is_file() else None)
    if chosen is None:
        return None
    data = chosen.read_bytes()
    build, signed = pex_info(data)
    if not signed:
        raise SystemExit(f"{chosen} is unsigned; this firmware loads signed modules only")
    print(f"module {chosen} {len(data)} B build {build}")
    return data


def installed_anker(base: str) -> dict | None:
    ok, inst = op(base, "pex.installed")
    if not ok:
        raise SystemExit(f"pex.installed: {inst.get('code')}: {inst.get('message')}")
    for mod in inst.get("modules") or []:
        if mod.get("name") == "anker":
            return mod
    return None


def same_build(installed: str | None, full: str) -> bool:
    """True when the installed id is this file.

    pex.info prints all 16 build-id bytes. pex.installed prints the first 8,
    so a list entry is a prefix of the file's id.
    """
    if not installed:
        return False
    a, b = installed.lower(), full.lower()
    if len(a) > len(b):
        a, b = b, a
    return len(a) >= 16 and b.startswith(a)


def live_build(base: str, have: dict | None) -> str | None:
    ok, info = op(base, "pex.info", {"name": "anker"})
    if ok and info.get("build_id"):
        return info["build_id"]
    return (have or {}).get("build_id")


def lua_anker_running(base: str) -> bool:
    ok, listed = op(base, "script.list")
    if not ok:
        return False
    for row in listed.get("scripts") or listed.get("running") or []:
        if isinstance(row, dict) and row.get("name") == "anker" and row.get("running"):
            return True
    return False


def ensure_running(base: str, dry: bool) -> None:
    if lua_anker_running(base):
        raise SystemExit("lua/anker.lua is running; the PEX app is not started beside it")
    ok, info = op(base, "pex.info", {"name": "anker"})
    if ok and info.get("state") == "running":
        print("anker is running")
        return
    if dry:
        print("would load and start anker")
        return
    if not ok and info.get("code") != "not_loaded":
        raise SystemExit(f"pex.info anker: {info.get('code')}: {info.get('message')}")
    if not ok:
        ok, loaded = op(base, "pex.load", {"name": "anker"})
        if not ok:
            raise SystemExit(f"pex.load anker: {loaded.get('code')}: {loaded.get('message')}")
        print(f"loaded anker xip={loaded.get('xip')}")
    ok, started = op(base, "pex.start", {"name": "anker"})
    if not ok:
        raise SystemExit(f"pex.start anker: {started.get('code')}: {started.get('message')}")
    print("started anker")


def deploy_module(base: str, data: bytes | None, replace: bool, dry: bool) -> None:
    have = installed_anker(base)
    if data is None:
        if have is None:
            raise SystemExit("no signed module locally and anker is not installed")
        print(f"anker already installed (build {have.get('build_id')}); module file left as it is")
        ensure_running(base, dry)
        return
    build, _signed = pex_info(data)
    remote_build = live_build(base, have)
    if have and remote_build and not same_build(remote_build, build) and not replace:
        print(f"installed anker build {remote_build} differs from {build}; left in place (pass --replace to swap it)")
        ensure_running(base, dry)
        return
    put_if_changed(base, "/api/fs/", "pex/anker.m33.pex", data, dry)
    if have and same_build(remote_build, build):
        print("installed build matches; not rewriting flash")
        ensure_running(base, dry)
        return
    if dry:
        print("would install anker from pex/anker.m33.pex and start it")
        return
    if have and replace:
        if lua_anker_running(base):
            raise SystemExit("lua/anker.lua is running; not replacing the PEX app")
        ok, info = op(base, "pex.info", {"name": "anker"})
        if ok:
            op(base, "pex.stop", {"name": "anker"})
            op(base, "pex.unload", {"name": "anker"})
        ok, gone = op(base, "pex.uninstall", {"name": "anker"})
        if not ok:
            raise SystemExit(f"pex.uninstall anker: {gone.get('code')}: {gone.get('message')}")
        print("uninstalled anker (iti left installed)")
    ok, inst = op(base, "pex.install", {"name": "anker"})
    if not ok:
        raise SystemExit(f"pex.install anker: {inst.get('code')}: {inst.get('message')}")
    print(f"installed anker offset {inst.get('offset')} xip_ready={inst.get('xip_ready')} signed={inst.get('signed')}")
    if inst.get("xip_ready"):
        ensure_running(base, dry)
    else:
        print("xip_ready is false until the next boot; anker was not loaded")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--base", default=DEFAULT_BASE, help=f"device HTTP base (default {DEFAULT_BASE})")
    ap.add_argument("--module", type=Path, help="signed anker.m33.pex (default build/pex-signed/anker.m33.pex when present)")
    ap.add_argument("--no-rebuild", action="store_true", help="use web/.pex-cc as it is")
    ap.add_argument("--replace", action="store_true", help="replace an installed Anker whose build id differs")
    ap.add_argument("--dry-run", action="store_true", help="show the copies and do not write")
    args = ap.parse_args()
    base = args.base.rstrip("/")
    sys.stdout.reconfigure(line_buffering=True)

    if not args.no_rebuild and not args.dry_run:
        refresh_cache()
    elif not args.no_rebuild and args.dry_run:
        print(f"would refresh {MAKE}")

    files = cache_files()
    print(f"compiler {len(files)} files from {CACHE}")
    deploy_tree(base, {rel: path.read_bytes() for rel, path in files.items()}, "/api/sd/files/", args.dry_run)

    print("anker sources -> SD projects/anker/src/")
    deploy_tree(base, anker_sources(), "/api/sd/files/", args.dry_run)
    drop_lfs_sources(base, args.dry_run)

    deploy_module(base, module_bytes(args.module), args.replace, args.dry_run)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except HttpError as e:
        print(e, file=sys.stderr)
        raise SystemExit(1) from e
    except subprocess.CalledProcessError as e:
        print(f"compiler refresh failed ({e.returncode})", file=sys.stderr)
        raise SystemExit(e.returncode) from e
