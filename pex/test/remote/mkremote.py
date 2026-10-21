#!/usr/bin/env python3
"""Make the set of files pex/tools/pex_remote_test.py uploads to a gateway.

  mkremote.py --mods <build>/pexmods --objs <build> --out DIR

Everything a real board loads is SIGNED with the OTA key (the gateway runs
signed-only): built that way by the module build (-DPEX_TEST_SIGN=ON). The
deliberately bad files are derived here. The fault modules (crashapp,
hangapp) are NOT included: they are for the simulator only.
  DIR/pex/<name>.m33.pex        hello arith counter imports needboard app memtest big (model B)
  DIR/pex/gwbus|gwfs|gwnet|gwbt.m33.pex
  DIR/pex/a/<name>.m33.pex      the same modules, model A
  DIR/bad/hello_unsigned.m33.pex, hello_badsig, hello_badcrc, hello.m0p.pex
  DIR/lua/pex_lua_test.lua
"""
import argparse, os, shutil, struct, subprocess, sys
HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.join(HERE, "..", "..", "tools")
sys.path.insert(0, TOOLS)
import pexlink  # noqa: E402

CORE = ["hello", "arith", "counter", "imports", "needboard", "app", "memtest", "big"]
GW = ["gwbus", "gwfs", "gwnet", "gwbt"]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--mods", required=True)
    ap.add_argument("--objs", required=True, help="the module build dir (pex_obj/ inside)")
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    shutil.rmtree(a.out, ignore_errors=True)
    for d in ("pex/a", "bad", "lua"):
        os.makedirs(os.path.join(a.out, d))
    for n in CORE:
        shutil.copy(os.path.join(a.mods, "pic", f"{n}.m33.pex"), os.path.join(a.out, "pex", f"{n}.m33.pex"))
        shutil.copy(os.path.join(a.mods, "reloc", f"{n}.m33.pex"), os.path.join(a.out, "pex", "a", f"{n}.m33.pex"))
    for n in GW:
        shutil.copy(os.path.join(a.mods, "gw", f"{n}.m33.pex"), os.path.join(a.out, "pex", f"{n}.m33.pex"))
    signed = open(os.path.join(a.mods, "pic", "hello.m33.pex"), "rb").read()
    h, secs = pexlink.parse(signed)
    assert any(s["type"] == pexlink.SEC["SIG"] for s in secs), "the modules must be built signed (-DPEX_TEST_SIGN=ON)"
    elf = os.path.join(a.objs, "pex_obj", "hello_pic", "m33", "hello.elf")
    uns = os.path.join(a.out, "bad", "hello_unsigned.m33.pex")
    subprocess.check_call([sys.executable, os.path.join(TOOLS, "pexlink.py"), "link", elf, "-o", uns, "--target", "m33",
                           "--model", "pic", "--name", "hello", "--version", "1.0.0", "--kind", "lua"],
                          stdout=subprocess.DEVNULL)
    b = bytearray(signed)
    b[-7] ^= 0x10                                 # in r||s: the CRC (which excludes SIG) still matches
    open(os.path.join(a.out, "bad", "hello_badsig.m33.pex"), "wb").write(b)
    b = bytearray(signed)
    b[h["image_off"] + 12] ^= 0x01
    open(os.path.join(a.out, "bad", "hello_badcrc.m33.pex"), "wb").write(b)
    shutil.copy(os.path.join(a.mods, "pic", "hello.m0p.pex"), os.path.join(a.out, "bad", "hello.m0p.pex"))
    shutil.copy(os.path.join(HERE, "pex_lua_test.lua"), os.path.join(a.out, "lua", "pex_lua_test.lua"))
    n = sum(len(f) for _, _, f in os.walk(a.out))
    print(f"mkremote: {a.out}: {n} files")


if __name__ == "__main__":
    main()
