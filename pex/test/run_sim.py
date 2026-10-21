#!/usr/bin/env python3
"""Build the two PEX test hosts (RP2350/M33 and RP2040/M0+, no Lua), run them in
mcusim, and print the result matrix (case x target x model).

  pex/test/run_sim.py [--work DIR] [--mcusim PATH] [--no-build] [--keep-going]

Also checks pexlink's refusals (a const pointer table under ROPI, absolute
MOVW/MOVT from -mslow-flash-data, a call to a symbol the host API lacks).
Exit status 0 only if everything passed.
"""
import argparse, collections, os, re, shutil, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
PEX = os.path.dirname(HERE)
HOSTS = [("m33", "rp2350-arm-s", "pico2", ["--bootrom", "A2"]), ("m0p", "rp2040", "pico", [])]
GCC = os.path.expanduser("~/.pico-sdk/toolchain/15_2_Rel1/bin/arm-none-eabi-gcc")


def sh(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


def build(work, t, plat, board):
    b = os.path.join(work, f"build-{t}")
    if not os.path.exists(os.path.join(b, "build.ninja")):
        r = sh(["cmake", "-S", os.path.join(HERE, "host"), "-B", b, "-G", "Ninja",
                f"-DPICO_PLATFORM={plat}", f"-DPICO_BOARD={board}"])
        if r.returncode:
            sys.exit(f"configure {t} failed:\n{r.stdout[-2000:]}{r.stderr[-2000:]}")
    r = sh(["cmake", "--build", b])
    if r.returncode:
        sys.exit(f"build {t} failed:\n{r.stdout[-3000:]}")
    return b


def run(sim, elf, board, extra):
    r = sh([sim, "--board", board, *extra, "--uart0", "stdio",
            "-t", "60s", "--exit-on", "PEXT DONE", "--fail-on", "fault UNEXPECTED", elf], timeout=600)
    return r.stdout + r.stderr


def pexlink_refusals(build_dir):
    """Negative builds: pexlink must refuse, with an actionable message."""
    out = []
    gen = os.path.join(build_dir, "pex_gen")
    inc = ["-I", os.path.join(PEX, "sdk", "include"), "-I", os.path.join(gen, "include")]
    ld = os.path.join(PEX, "sdk", "ld")
    with tempfile.TemporaryDirectory() as d:
        def link_and_check(name, cc, src, model, expect):
            c = os.path.join(d, name + ".c")
            open(c, "w").write(src)
            o, e = os.path.join(d, name + ".o"), os.path.join(d, name + ".elf")
            r = sh(cc + ["-Os", "-ffunction-sections", "-fdata-sections", "-fno-unwind-tables", "-c", c, "-o", o] + inc)
            if r.returncode:
                out.append((name, False, "compile: " + r.stderr[-200:]))
                return
            stub = os.path.join(gen, f"pex_imports_m33_{model}.o")
            if model == "pic":
                r = sh(["ld.lld", "-T", os.path.join(ld, "pex_m33_pic.ld"), "-L", ld, "--emit-relocs", "--gc-sections",
                        "-e", "pex_main", o, stub, "-o", e])
            else:
                r = sh([GCC, "-mthumb", "-mcpu=cortex-m33", "-nostdlib", "-T", os.path.join(ld, "pex_m33_reloc.ld"),
                        "-L", ld, "-Wl,--emit-relocs", "-Wl,--gc-sections", "-Wl,-e,pex_main",
                        o, stub, "-lgcc", "-o", e])
            if r.returncode:
                out.append((name, False, "link: " + r.stderr[-200:]))
                return
            r = sh([sys.executable, os.path.join(PEX, "tools", "pexlink.py"), "link", e, "-o", e + ".pex",
                    "--target", "m33", "--model", model])
            ok = r.returncode != 0 and expect in r.stderr
            msg = [x.strip() for x in r.stderr.splitlines() if expect in x][:1]
            out.append((name, ok, msg[0][:150] if msg else r.stderr[-150:]))
        clang = ["clang", "--target=thumbv8m.main-none-eabi", "-mcpu=cortex-m33", "-mfloat-abi=softfp", "-fropi", "-frwpi"]
        link_and_check("ropi_const_table", clang,
                       '#include "pex.h"\nstatic int f(void){return 1;}\nstatic int g(void){return 2;}\n'
                       'static int (*const t[2])(void) = {f, g};\nint pex_main(const pex_host_t *h, uint32_t r, void *a)'
                       '{ return t[r & 1](); }\n', "pic", "ROPI code must not contain one")
        link_and_check("movw_movt_abs", [GCC, "-mthumb", "-mcpu=cortex-m33", "-mslow-flash-data"],
                       '#include "pex.h"\nint g_x;\nint pex_main(const pex_host_t *h, uint32_t r, void *a)'
                       '{ g_x += (int)r; return g_x; }\n', "reloc", "MOVW/MOVT")
        link_and_check("undefined_symbol", [GCC, "-mthumb", "-mcpu=cortex-m33"],
                       '#include "pex.h"\nextern int not_in_the_api(int) __attribute__((weak));\nint pex_main(const pex_host_t *h, uint32_t r, '
                       'void *a){ return not_in_the_api((int)r); }\n', "reloc", "undefined symbol 'not_in_the_api'")
    return out


def unit_tests(work):
    """The portable runtime parts, compiled for the build host."""
    gen = os.path.join(work, "unit-gen")
    os.makedirs(gen, exist_ok=True)
    sh([sys.executable, os.path.join(PEX, "tools", "gen_host_api.py"), "header", os.path.join(PEX, "host_api.json"),
        os.path.join(gen, "pex_api.h")])
    exe = os.path.join(work, "pex_unit")
    r = sh(["gcc", "-O1", "-Wall", "-Wextra", "-Werror", "-I", os.path.join(PEX, "runtime"), "-I",
            os.path.join(PEX, "sdk", "include"), "-I", gen, os.path.join(HERE, "unit", "pex_unit.c"),
            os.path.join(PEX, "runtime", "groups", "pex_json.c"), os.path.join(PEX, "runtime", "pex_hash.c"), "-o", exe])
    if r.returncode:
        return [("unit.build", False, r.stderr[-300:])]
    r = sh([exe])
    out = [("unit.json_crc_sha", r.returncode == 0, r.stdout.strip().splitlines()[-1] if r.stdout else r.stderr[-200:])]
    # XIP on the build host: install/uninstall/compaction with a power cut at every flash operation
    for g in ("core", "log", "mem", "sched", "pex", "json"):
        sh([sys.executable, os.path.join(PEX, "tools", "gen_host_api.py"), "table", os.path.join(PEX, "host_api.json"), g,
            os.path.join(gen, f"pex_host_{g}.c")])
    rt = os.path.join(PEX, "runtime")
    exe = os.path.join(work, "pex_xip_unit")
    r = sh(["gcc", "-O1", "-Wall", "-Wextra", "-DPEX_FORCE_TARGET=2", "-DPEX_FORCE_FEATURES=5", "-fsanitize=address,undefined",
            "-I", rt, "-I", os.path.join(PEX, "sdk", "include"), "-I", gen, os.path.join(HERE, "unit", "pex_xip_unit.c")]
           + [os.path.join(rt, f) for f in ("pex_core.c", "pex_load.c", "pex_mem.c", "pex_sched.c", "pex_hash.c", "pex_xip.c",
                                          os.path.join("groups", "pex_json.c"))]
           + [os.path.join(gen, f"pex_host_{g}.c") for g in ("core", "log", "mem", "sched", "pex", "json")] + ["-o", exe])
    if r.returncode:
        return out + [("unit.xip.build", False, r.stderr[-300:])]
    r = sh([exe], env=dict(os.environ, ASAN_OPTIONS="detect_leaks=0"), timeout=900)
    open(os.path.join(work, "pex_xip_unit.log"), "w").write(r.stdout + r.stderr)
    lines = [l for l in r.stdout.splitlines() if not l.startswith("  |")]
    for l in lines:
        if "cut points" in l or l.startswith("random workload"):
            print("  xip unit: " + l)
    out.append(("unit.xip_power_cuts", r.returncode == 0, lines[-1] if lines else r.stderr[-200:]))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--work", default=os.path.expanduser("~/Work/pexwork/run"))
    ap.add_argument("--mcusim", default=os.path.expanduser("~/Work/mcusim/target/release/mcusim"))
    ap.add_argument("--no-build", action="store_true")
    a = ap.parse_args()
    os.makedirs(a.work, exist_ok=True)
    # Something on this machine kills processes named 'mcusim': run a renamed copy.
    sim = os.path.join(a.work, "pexsim-run")
    if not os.path.exists(sim) or os.path.getmtime(sim) < os.path.getmtime(a.mcusim):
        shutil.copy2(a.mcusim, sim)
    results = collections.OrderedDict()   # case -> {(target, model): (ok, detail)}
    done = {}
    builds = {}
    for t, plat, board, extra in HOSTS:
        b = os.path.join(a.work, f"build-{t}") if a.no_build else build(a.work, t, plat, board)
        builds[t] = b
        log = run(sim, os.path.join(b, "pex_test.elf"), board, extra)
        open(os.path.join(a.work, f"sim-{t}.log"), "w").write(log)
        for line in log.splitlines():
            m = re.match(r"PEXT (\S+) (\S+) (\S+) (PASS|FAIL) ?(.*)", line)
            if m:
                tg, model, case, st, det = m.groups()
                results.setdefault(case, {})[(tg, model)] = (st == "PASS", det)
            m = re.match(r"PEXT DONE target=(\S+) pass=(\d+) fail=(\d+)", line)
            if m:
                done[m.group(1)] = (int(m.group(2)), int(m.group(3)))
            if "fault UNEXPECTED" in line:
                results.setdefault("unexpected_fault", {})[(t, "-")] = (False, line)
        if t not in done:
            results.setdefault("completed", {})[(t, "-")] = (False, "no 'PEXT DONE' (see sim log)")
    refusals = pexlink_refusals(builds["m33"]) + unit_tests(a.work)

    cols = [(t, m) for t, *_ in HOSTS for m in ("reloc", "pic", "-")]
    used = [c for c in cols if any(c in v for v in results.values())]
    w = max(len(c) for c in results) + 2
    print("case".ljust(w) + "".join(f"{t}/{m}".ljust(11) for t, m in used))
    fails = 0
    for case, row in results.items():
        cells = ""
        for c in used:
            if c in row:
                cells += ("PASS" if row[c][0] else "FAIL").ljust(11)
                fails += 0 if row[c][0] else 1
            else:
                cells += ".".ljust(11)
        print(case.ljust(w) + cells)
    print()
    for case, row in results.items():
        for c, (ok, det) in row.items():
            if not ok:
                print(f"FAIL {case} {c[0]}/{c[1]}: {det}")
    print("pexlink refusals and host unit tests:")
    for name, ok, msg in refusals:
        print(f"  {'PASS' if ok else 'FAIL'} {name}: {msg}")
        fails += 0 if ok else 1
    total = sum(len(r) for r in results.values()) + len(refusals)
    print(f"\n{total - fails}/{total} passed" + "".join(f"; {t}: {p} pass {f} fail" for t, (p, f) in done.items()))
    return 1 if fails or len(done) != len(HOSTS) else 0


if __name__ == "__main__":
    sys.exit(main())
