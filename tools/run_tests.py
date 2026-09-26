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
    return _grep(r"anker|8c8500|0x0840",
                 ["src/bt/", "src/ops/", "src/api/", "src/lua/", "src/io/"])

def t_registry_transport_neutral():
    return _grep(r"\ble_[a-z]|gatt_|att_", ["src/bt/registry.c"])

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
    step(0, "device registry stays transport-neutral", t_registry_transport_neutral)

    if a.tier >= 1:
        # Both configs, because a compile gate that is never exercised rots:
        # a module that only ever builds with its neighbours enabled is a
        # module that will not build when you need it off.
        step(1, "build pico2w (everything)", build("pico2w"))
        step(1, "build pico2w_minimal (no wifi/lua/io)", build("pico2w_minimal"))

    print()
    if FAILS:
        print("FAILED: " + ", ".join(FAILS))
        sys.exit(1)
    print("all checks passed")

if __name__ == "__main__":
    main()
