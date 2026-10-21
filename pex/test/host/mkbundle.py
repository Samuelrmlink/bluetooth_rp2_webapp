#!/usr/bin/env python3
"""Bundle the test .pex files into one blob the test hosts embed (flash).

  mkbundle.py --out bundle.bin --keyhdr pex_testkey.h --key test.pem \
              --mods <dir with reloc/ and pic/> [--copy-to DIR]

Besides the built modules it derives the refusal cases (bad/...):
  hello_signed   valid signature (test key, key id 1)
  hello_badsig   signature bytes changed (the CRC excludes SIG: only ECDSA catches it)
  hello_badcrc   one image byte flipped
  hello_needsig  NEEDS_SIG flag, unsigned
  arith_picro    a model A file with the PIC flag forced on (RO list not empty)
  hello_fmt2     format major 2
  hello_api99    api minor 99
  hello_trunc    truncated
Bundle: "PXB1", u32 count, count x {char name[40], u32 off, u32 size}, data (8-aligned).
"""
import argparse, os, shutil, struct, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.join(HERE, "..", "..", "tools")
sys.path.insert(0, TOOLS)
import pexlink  # noqa: E402


def fix_crc(b):
    h, secs = pexlink.parse(bytes(b))
    so = pexlink.sig_start(h, secs)
    struct.pack_into("<I", b, pexlink.CRC_OFF, pexlink.crc32_file(bytes(b[:so])))
    return b


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--keyhdr", required=True)
    ap.add_argument("--key", required=True)
    ap.add_argument("--mods", required=True)
    ap.add_argument("--copy-to")
    a = ap.parse_args()
    if not os.path.exists(a.key):
        subprocess.check_call(["openssl", "ecparam", "-name", "prime256v1", "-genkey", "-noout", "-out", a.key])
    der = subprocess.check_output(["openssl", "ec", "-in", a.key, "-pubout", "-outform", "DER"],
                                  stderr=subprocess.DEVNULL)
    raw = der[-64:]
    open(a.keyhdr, "w").write("/* generated test key (public half) */\nstatic const uint8_t PEX_TEST_PUBKEY[1][64] = {{\n"
                              + ",".join(f"0x{x:02x}" for x in raw) + "\n}};\n")
    files = {}
    for model in ("reloc", "pic"):
        d = os.path.join(a.mods, model)
        for f in sorted(os.listdir(d)):
            if f.endswith(".pex"):
                files[f"{model}/{f}"] = open(os.path.join(d, f), "rb").read()
    for t in ("m33", "m0p"):
        hello = files[f"pic/hello.{t}.pex"]
        tmp = a.out + f".unsigned.{t}.pex"
        sgn = a.out + f".signed.{t}.pex"
        open(tmp, "wb").write(hello)
        subprocess.check_call([sys.executable, os.path.join(TOOLS, "pexlink.py"), "sign", tmp, "-o", sgn,
                               "--key", a.key], stdout=subprocess.DEVNULL)
        signed = open(sgn, "rb").read()
        files[f"bad/hello_signed.{t}.pex"] = signed
        b = bytearray(signed)
        b[-10] ^= 0x40
        files[f"bad/hello_badsig.{t}.pex"] = bytes(b)          # CRC still valid: excludes SIG
        h, _ = pexlink.parse(hello)
        b = bytearray(hello)
        b[h["image_off"] + 8] ^= 0x01
        files[f"bad/hello_badcrc.{t}.pex"] = bytes(b)
        b = bytearray(hello)
        struct.pack_into("<I", b, 24, h["flags"] | pexlink.F["needs_sig"])
        files[f"bad/hello_needsig.{t}.pex"] = bytes(fix_crc(b))
        ar = files[f"reloc/arith.{t}.pex"]
        ha, _ = pexlink.parse(ar)
        b = bytearray(ar)
        struct.pack_into("<I", b, 24, ha["flags"] | pexlink.F["pic"])
        files[f"bad/arith_picro.{t}.pex"] = bytes(fix_crc(b))
        b = bytearray(hello)
        struct.pack_into("<H", b, 4, 2)
        files[f"bad/hello_fmt2.{t}.pex"] = bytes(fix_crc(b))
        b = bytearray(hello)
        struct.pack_into("<H", b, 22, 99)
        files[f"bad/hello_api99.{t}.pex"] = bytes(fix_crc(b))
        files[f"bad/hello_trunc.{t}.pex"] = hello[:len(hello) // 2]
        for p in (tmp, sgn):
            os.unlink(p)
    names = sorted(files)
    hdr = b"PXB1" + struct.pack("<I", len(names))
    off = len(hdr) + 48 * len(names)
    off = (off + 7) & ~7
    tbl, data = b"", b""
    for n in names:
        nb = n.encode()
        assert len(nb) < 40, n
        pad = (-(off + len(data))) & 7
        data += b"\0" * pad
        tbl += nb.ljust(40, b"\0") + struct.pack("<II", off + len(data), len(files[n]))
        data += files[n]
    blob = hdr + tbl
    blob += b"\0" * (((len(blob) + 7) & ~7) - len(blob))
    open(a.out, "wb").write(blob + data)
    if a.copy_to:
        for n in names:
            p = os.path.join(a.copy_to, n)
            os.makedirs(os.path.dirname(p), exist_ok=True)
            open(p, "wb").write(files[n])
    print(f"mkbundle: {a.out}: {len(names)} files, {len(blob) + len(data)} bytes")


if __name__ == "__main__":
    main()
