#!/usr/bin/env python3
"""Make an OTA release file: build/btgw.bin || 64-byte signature.

Usage: ota_release.py [image.bin] [--out btgw-<ver>.smp.bin]

Then, with `pipx install smpmgr` (any address: 192.168.8.1 over USB-NCM, the
LAN address, or 10.255.255.23 through WireGuard):

  smpmgr --ip <addr> --mtu 1200 image upload --format any <release>
  smpmgr --ip <addr> image state-read              # note the new slot's hash
  smpmgr --ip <addr> image state-write --hash <hash>   # arm the test boot
  smpmgr --ip <addr> os reset                      # flash-update boot
  ... the new image buys itself after a healthy start; check with
  smpmgr --ip <addr> image state-read              # confirmed: true

or in one go: smpmgr --ip <addr> --mtu 1200 upgrade <release>
"""
import pathlib, re, subprocess, sys
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import ota_sign  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parent.parent


def version_of(build_dir):
    cache = build_dir / "CMakeCache.txt"
    m = re.search(r"BTGW_VERSION_MINOR[^=]*=(\d+)", cache.read_text()) if cache.exists() else None
    return f"1.{m.group(1)}" if m else "unknown"


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    image = pathlib.Path(args[0]) if args else ROOT / "build/btgw.bin"
    out = pathlib.Path(sys.argv[sys.argv.index("--out") + 1]) if "--out" in sys.argv else None
    data = image.read_bytes()
    digest, sig = ota_sign.sign(image)
    if out is None:
        out = image.parent / f"btgw-{version_of(image.parent)}.smp.bin"
    out.write_bytes(data + sig)
    print(f"{out}: {len(data)} image bytes + 64 signature bytes; image sha256 {digest.hex()}")


if __name__ == "__main__":
    main()
