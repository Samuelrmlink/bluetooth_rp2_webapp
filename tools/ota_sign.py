#!/usr/bin/env python3
"""Sign a firmware image: ECDSA P-256 over SHA-256(image), raw r||s.

Usage: ota_sign.py <image.bin> [--out <file>]
Writes <image.bin>.sig (64 bytes) and prints the SHA-256. The private key
is ~/.config/btgw/ota-signing.pem or $BTGW_OTA_KEY. Only openssl is needed.
"""
import hashlib, os, pathlib, subprocess, sys, tempfile

KEY = os.environ.get("BTGW_OTA_KEY", os.path.expanduser("~/.config/btgw/ota-signing.pem"))


def der_to_raw(der):
    # SEQUENCE { INTEGER r, INTEGER s }
    assert der[0] == 0x30
    i = 2 if der[1] < 0x80 else 2 + (der[1] & 0x7f)
    out = b""
    for _ in range(2):
        assert der[i] == 0x02
        n = der[i + 1]
        v = der[i + 2:i + 2 + n]
        i += 2 + n
        v = v.lstrip(b"\x00")
        out += v.rjust(32, b"\x00")
    assert len(out) == 64
    return out


def sign(image_path):
    data = pathlib.Path(image_path).read_bytes()
    digest = hashlib.sha256(data).digest()
    with tempfile.NamedTemporaryFile(suffix=".sha", delete=False) as t:
        t.write(digest)
        tname = t.name
    try:
        der = subprocess.check_output(["openssl", "pkeyutl", "-sign", "-inkey", KEY, "-in", tname,
                                       "-pkeyopt", "digest:sha256"])
    finally:
        os.unlink(tname)
    return digest, der_to_raw(der)


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    image = sys.argv[1]
    out = sys.argv[sys.argv.index("--out") + 1] if "--out" in sys.argv else image + ".sig"
    digest, sig = sign(image)
    pathlib.Path(out).write_bytes(sig)
    print(f"sha256 {digest.hex()}\nsig    {out} (64 bytes)")


if __name__ == "__main__":
    main()
