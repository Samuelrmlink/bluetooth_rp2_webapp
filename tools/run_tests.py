#!/usr/bin/env python3
"""Tiered test runner. Tiers 0-2 need no board; 3-4 do.

  tools/run_tests.py            # tier 0 only, the every-save default
  tools/run_tests.py --tier 2   # everything that needs no hardware
  tools/run_tests.py --tier 3   # adds the board

The tiering exists so the common case stays fast: a suite that is slow does not
get run, and a suite that does not get run is not a regression guard.
"""
import argparse, os, subprocess, sys, re
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FAILS = []

def step(tier, name, fn):
    print(f"[tier {tier}] {name} ... ", end="", flush=True)
    try:
        ok, detail = fn()
    except Exception as e:
        ok, detail = False, str(e)
    print("OK" + (f"  ({detail})" if detail else "") if ok else f"FAIL  {detail}")
    if not ok:
        FAILS.append(name)

ANSI = re.compile(r"\x1b\[[0-9;]*m")

def sh(cmd, cwd=ROOT, env_extra=""):
    # node --test colourises its summary, which quietly defeats any regex over
    # it, so strip escapes here rather than at every call site.
    p = subprocess.run(env_extra + cmd, shell=True, cwd=cwd, capture_output=True, text=True)
    return p.returncode, ANSI.sub("", p.stdout + p.stderr)

# --- tier 0 -------------------------------------------------------------------
def t_unit():
    rc, out = sh('node --test "tests/unit/**/*.test.js"', cwd=os.path.join(ROOT, "web"))
    m = re.search(r"^. pass (\d+)", out, re.M)
    f = re.search(r"^. fail (\d+)", out, re.M)
    npass = int(m.group(1)) if m else 0
    nfail = int(f.group(1)) if f else -1
    return (rc == 0 and nfail == 0), f"{npass} passed"

def _grep(pattern, paths):
    """True when the pattern is ABSENT. Note the exit status is grep's own --
    piping into head would make it head's, which is always 0, and the check
    would then pass or fail at random."""
    rc, out = sh(f"grep -rnE {pattern!r} {' '.join(paths)} 2>/dev/null")
    hits = [l for l in out.splitlines() if l.strip()]
    return (len(hits) == 0), (hits[0][:90] if hits else "")

def t_no_device_in_core():
    # src/lua/lbind_proto.c is excluded here and checked more strictly below.
    # It is the bridge to src/codec + src/crypto, so it cannot avoid naming
    # them in an #include; a blanket exemption would be too generous, so it
    # gets its own tighter rule instead.
    rc, out = sh("grep -rnE 'anker|8c8500|0x0840' "
                 "src/bt/ src/ops/ src/api/ src/lua/ src/io/ 2>/dev/null")
    hits = [l for l in out.splitlines()
            if l.strip() and not l.startswith("src/lua/lbind_proto.c:")]
    return (len(hits) == 0), (hits[0][:90] if hits else "")

def t_proto_binding_is_thin():
    """The protocol binding may NAME the codec it wraps; it may not know the
    protocol. Anything Anker-ish outside an #include line is device knowledge
    that belongs in lua/."""
    rc, out = sh("grep -nE 'anker|8c8500|0x0840' src/lua/lbind_proto.c 2>/dev/null")
    hits = [l for l in out.splitlines()
            if l.strip() and "#include" not in l and "ANKER_REASM_MAX" not in l
            and "anker_tlv_item_t" not in l and "anker_tlv_spec_t" not in l
            and "anker_enc_t" not in l and "anker_frame_t" not in l
            and "anker_reasm_t" not in l and "ANKER_ENC_" not in l
            and "ANKER_TLV_" not in l and "CONFIG_PROTO_ANKER" not in l
            and not re.search(r"\banker_(frame|tlv|reasm|pubkey|ecdh|aes)\w*\(", l)]
    return (len(hits) == 0), (hits[0][:90] if hits else "")

def t_neg_constants_agree():
    """The negotiation keystream lives in the Lua profile; the C parity test
    keeps a fixture copy so it can prove the generalised GHASH still matches
    the tags the Anker-specific original produced. Two copies can drift, and a
    drift here would be invisible until a charger stopped answering, so assert
    they are identical.

    Repointed at lua/profiles/anker_cipher.lua when the profile was split into
    modules. A guard that quietly stops matching anything is worse than no
    guard, so the missing-file case below fails rather than skips."""
    import pathlib
    fix = pathlib.Path(ROOT, "test/host/neg_fixture.h")
    prof = pathlib.Path(ROOT, "lua/profiles/anker_cipher.lua")
    if not fix.exists() or not prof.exists():
        return False, "fixture or profile missing"
    ftxt, ptxt = fix.read_text(), prof.read_text()

    def c_hex(name):
        m = re.search(re.escape(name) + r"\s*\[[^\]]*\]\s*=\s*\{(.*?)\};", ftxt, re.S)
        if not m:
            return None
        return "".join(b.lower() for b in re.findall(r"0x([0-9a-fA-F]{2})", m.group(1)))

    def lua_hex(name):
        # The value may be several quoted chunks joined with '..'.
        m = re.search(r"\b" + re.escape(name) + r"\s*=\s*bytes\.unhex\((.*?)\)\n", ptxt, re.S)
        if not m:
            return None
        return "".join(re.findall(r"'([0-9a-fA-F]*)'", m.group(1))).lower()

    for cname, lname in (("NEG_KEYSTREAM", "KEYSTREAM"), ("NEG_GHASH_H", "GHASH_H"),
                         ("NEG_EJ0", "EJ0"), ("ANKER_GCM_AAD", "AAD")):
        a, b = c_hex(cname), lua_hex(lname)
        if not a or not b:
            return False, f"could not read {cname} / {lname}"
        if a != b:
            return False, f"{cname} != the profile's {lname}"
    return True, "keystream, H, E(J0), AAD"

def t_lua_wait():
    """The wait/wake contract. These are the races -- a disconnect arriving
    while the coroutine is runnable rather than blocked, a frame from a
    previous connection arriving after a reconnect, an event that never comes
    -- and none of them is reachable from a hardware test with any
    reliability. The state machine is pure logic over an injected clock for
    exactly that reason."""
    binp = os.path.join(os.environ.get("TMPDIR", "/tmp"), "btgw_wait")
    rc, out = sh(f"gcc -O1 -Wall -Wextra -Isrc -o {binp} "
                 f"test/host/wait_test.c src/lua/lwait.c")
    if rc:
        errs = [l for l in out.splitlines() if "error:" in l]
        return False, errs[0][:100] if errs else "wait harness build failed"
    rc, out = sh(binp)
    if rc != 0:
        first = next((l.strip() for l in out.splitlines() if "FAIL" in l), "")
        return False, first[:100] or f"wait_test exited {rc}"
    return True, "delivered / timeout / link_lost / cancelled, and the races"

def t_lua_heap():
    """The Lua arena, on a host, with every structural invariant asserted after
    EVERY operation. This test exists because two attempts at the allocator's
    in-place realloc shipped to hardware and had to be reverted; each is
    reproduced as a deliberate defect in the test's own comments, and each is
    caught by a named assertion rather than by a wrapped byte counter.

    A small arena is used so the fuzz reaches exhaustion and the refused-grow
    paths actually run."""
    binp = os.path.join(os.environ.get("TMPDIR", "/tmp"), "btgw_heap")
    rc, out = sh(f"gcc -O1 -Wall -Wextra -Isrc -DLHEAP_KB=64 -o {binp} "
                 f"test/host/heap_test.c src/lua/lheap.c")
    if rc:
        errs = [l for l in out.splitlines() if "error:" in l]
        return False, errs[0][:100] if errs else "heap harness build failed"
    rc, out = sh(binp)
    if rc != 0:
        first = next((l.strip() for l in out.splitlines() if "FAIL" in l), "")
        return False, first[:100] or f"heap_test exited {rc}"
    peak = re.search(r"growth: final (\d+) B, peak (\d+) B", out)
    detail = "invariants hold after every op"
    if peak:
        detail += f"; doubling to {peak.group(1)} B peaked at {peak.group(2)} B"
    return True, detail

def t_lua_proto():
    """The profile's Lua, on a host, against the REAL C bindings and the REAL
    vendored Lua -- no board, no flash cycle, no charger. This is the dividend
    of putting the device-specific half in Lua: the frame construction and the
    telemetry decode, which are the parts most likely to be wrong, are pure
    functions of bytes and can be checked in milliseconds."""
    uecc = os.path.expanduser("~/.pico-sdk/sdk/2.3.0/lib/btstack/3rd-party/micro-ecc")
    binp = os.path.join(os.environ.get("TMPDIR", "/tmp"), "btgw_luaproto")
    rc, out = sh(f"gcc -O1 -w -DMAKE_LIB=1 "
                 f"-Isrc -Itest/host -Ithird_party/tiny-AES-c -Ithird_party/lua -I{uecc} "
                 f"-o {binp} test/host/luaproto_main.c src/lua/lbind_proto.c "
                 f"src/codec/anker_frame.c src/codec/anker_tlv.c "
                 f"src/crypto/ghash.c src/crypto/anker_crypto.c "
                 f"third_party/tiny-AES-c/aes.c {uecc}/uECC.c "
                 f"third_party/lua/onelua.c -lm")
    if rc:
        errs = [l for l in out.splitlines() if "error:" in l]
        return False, errs[0][:100] if errs else "harness build failed"
    rc, out = sh(f"{binp} test/host/proto_test.lua")
    m = re.search(r"(\d+) checks, (\d+) failed", out)
    if not m:
        return False, (out.strip().splitlines() or ["no output"])[-1][:90]
    nchk, nfail = int(m.group(1)), int(m.group(2))
    if nfail:
        first = next((l for l in out.splitlines() if l.startswith("FAIL")), "")
        return False, f"{nfail} of {nchk} failed: {first[:70]}"
    return rc == 0, f"{nchk} checks"

def t_parity():
    """The frozen protocol vectors. Two of these assertions are regression
    locks -- the lenient PKCS7 unpad and GCM without tag verification -- and a
    'fix' to either stops the device talking."""
    uecc = os.path.expanduser("~/.pico-sdk/sdk/2.3.0/lib/btstack/3rd-party/micro-ecc")
    binp = "/tmp/btgw_parity"
    rc, out = sh(f"gcc -O1 -Wall -Wextra -Wno-unused-parameter "
                 f"-Isrc -Itest/host -Ithird_party/tiny-AES-c -I{uecc} -o {binp} "
                 f"test/host/parity_main.c src/codec/anker_frame.c src/codec/anker_tlv.c "
                 f"src/crypto/ghash.c src/crypto/anker_crypto.c "
                 f"third_party/tiny-AES-c/aes.c {uecc}/uECC.c -lm")
    if rc:
        errs = [l for l in out.splitlines() if "error:" in l]
        return False, errs[0][:100] if errs else "parity build failed"
    rc, out = sh(binp)
    npass = len(re.findall(r"^OK", out, re.M))
    nfail = len(re.findall(r"^FAIL", out, re.M))
    nskip = len(re.findall(r"^SKIP", out, re.M))
    detail = f"{npass} passed"
    if nskip:
        detail += f", {nskip} skipped (C1000-only / no ref workspace)"
    if nfail:
        first = next((l for l in out.splitlines() if l.startswith("FAIL")), "")
        return False, f"{nfail} failed: {first[:70]}"
    return rc == 0, detail

def t_registry_transport_neutral():
    return _grep(r"\ble_[a-z]|gatt_|att_", ["src/bt/registry.c"])

def t_webapp_dom():
    """Render the real page in a real browser. Every model test passed while
    the app opened to an empty workspace nothing could be selected in, because
    a model test cannot see a wrong argument at the one call site that builds
    the layout, or a render method nobody called. Both of those shipped."""
    rc, out = sh("python3 tools/webtest.py")
    if "SKIP:" in out:
        return True, "skipped (no chromium)"
    m = re.search(r"(\d+)/(\d+) DOM checks passed", out)
    if not m:
        return False, (out.strip().splitlines() or ["no output"])[-1][:90]
    good, total = int(m.group(1)), int(m.group(2))
    if rc or good != total:
        first = next((l for l in out.splitlines() if l.startswith("FAIL")), "")
        return False, f"{total - good} of {total} failed: {first[:70]}"
    return True, f"{good} checks in chromium"

# --- tier 1 -------------------------------------------------------------------
def build(cfg):
    def fn():
        bdir = f"/tmp/btgw_{cfg}"
        env = ('PICO_SDK_PATH=$HOME/.pico-sdk/sdk/2.3.0 '
               'PICO_TOOLCHAIN_PATH=$HOME/.pico-sdk/toolchain/15_2_Rel1 ')
        sh(f"rm -rf {bdir}")
        rc, out = sh(f"{env} cmake -S . -B {bdir} -G Ninja "
                     f"-DAPP_DEFCONFIG={ROOT}/configs/{cfg}_defconfig")
        if rc: return False, out.strip().splitlines()[-1] if out.strip() else "configure failed"
        rc, out = sh(f"{env} cmake --build {bdir}")
        if rc:
            errs = [l for l in out.splitlines() if "error:" in l]
            return False, errs[0][:100] if errs else "build failed"
        rc, sz = sh(f"{os.path.expanduser('~/.pico-sdk/toolchain/15_2_Rel1')}/bin/arm-none-eabi-size {bdir}/btgw.elf")
        last = sz.strip().splitlines()[-1].split()
        return True, f"{last[0]} text / {last[2]} bss"
    return fn

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tier", type=int, default=0)
    a = ap.parse_args()

    step(0, "webapp + model unit tests", t_unit)
    step(0, "no device-specific code in the core", t_no_device_in_core)
    step(0, "protocol binding stays a thin wrapper", t_proto_binding_is_thin)
    step(0, "device registry stays transport-neutral", t_registry_transport_neutral)
    step(0, "frozen protocol vectors", t_parity)
    step(0, "neg constants agree with the Lua profile", t_neg_constants_agree)
    step(0, "protocol Lua on a host", t_lua_proto)
    step(0, "the Lua arena, invariants and fuzz", t_lua_heap)
    step(0, "the wait/wake contract and its races", t_lua_wait)

    if a.tier >= 1:
        # Both configs, because a compile gate that is never exercised rots:
        # a module that only ever builds with its neighbours enabled is a
        # module that will not build when you need it off.
        step(1, "build pico2w (everything)", build("pico2w"))
        step(1, "build pico2w_minimal (no wifi/lua/io/anker)", build("pico2w_minimal"))
        # The combination neither of the other two reaches: Lua present, the
        # device codec absent. Without it the #if in lbind.c that skips
        # lbind_proto_open is never once compiled out.
        step(1, "build pico2w_noanker (lua, no device codec)", build("pico2w_noanker"))

    if a.tier >= 2:
        step(2, "webapp renders and panes can be selected", t_webapp_dom)

    print()
    if FAILS:
        print("FAILED: " + ", ".join(FAILS))
        sys.exit(1)
    print("all checks passed")

if __name__ == "__main__":
    main()
