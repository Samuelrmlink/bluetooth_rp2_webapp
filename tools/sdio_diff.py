#!/usr/bin/env python3
"""Differential test: our SDIO driver vs carlk3's, on the same simulated card.

Runs test/sdio_diff's ELFs (sddiff_{ours,carlk3}_{raw,fs}.elf) in mcusim, each
against a fresh copy of one blank card image with the same faults armed, and
compares what the two drivers did:

  raw  every step's result class, read hash and read-back match;
       identification (sector count, CID); the card's final image
       (byte-identical); the card's command trace after normalisation
       (an allow-list, test/sdio_diff/allowlist.txt, names accepted
       differences with a reason); times side by side (not a gate).
  fs   every step's result class and file/listing hashes (each driver brings
       its own FatFs build, so images are not compared byte for byte); the
       resulting image passes fsck.fat.

Exit 0 = same behaviour, 1 = a difference, 77 = skipped (mcusim has no SD
card model yet, or the ELFs are not built).

  tools/sdio_diff.py [--build build-sdio-diff] [--mcusim ~/Work/mcusim] [--keep DIR]

The card model is mcusim's (backlog: an `sdcard` device with SPI and SDIO
front ends). The device stanza this tool writes is CARD_TOML below; it is the
interface asked of that model (docs/sd.md, "Testing").
"""
import argparse, hashlib, json, os, re, shutil, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SECTORS = 256 * 2048                      # a 256 MiB SDHC card

# What the card is asked to do to each run (sddiff.c FAULT_* are the same LBAs).
FAULTS = [
    {"kind": "crc_read", "lba": 5000},                      # wrong CRC16 on that block, once
    {"kind": "crc_write", "lba": 5100},                     # CRC-status error token, once
    {"kind": "write_error", "lba": 5150},                   # write-error token, once
    {"kind": "no_response", "cmd": 17, "arg_lba": 5200},    # that CMD17 unanswered, once
    {"kind": "busy", "lba": 5300, "us": 1500000},           # 1.5 s programming after that block
    {"kind": "pull", "lba": 5402, "reinsert_after_us": 300000},
]

CARD_TOML = """
[[device]]
type = "sdcard"
mode = "sdio"
clk = 16
cmd = 17
d0 = 18                     # D1-D3 = 19-21
detect = 22
detect_active_low = true
image = "{image}"
writeback = true
trace = "{trace}"           # JSONL: one {{"cmd","arg","resp","t_us"}} per command
{faults}
"""


def card_toml(image, trace, faults):
    f = "".join("\n[[device.fault]]\n" + "".join(f'{k} = {json.dumps(v)}\n' for k, v in x.items()) + "once = true\n"
                for x in faults)
    return CARD_TOML.format(image=image, trace=trace, faults=f)


def mcusim_bin(root):
    b = os.path.join(root, "target/release/mcusim")
    return b if os.path.exists(b) else None


def has_sd_model(mcusim):
    out = subprocess.run([mcusim, "--list"], capture_output=True, text=True).stdout
    return re.search(r"\bsdcard\b", out) is not None


def run(mcusim, root, elf, project, timeout_s=60):
    rom = os.path.join(root, "third_party/bootrom/rp2350-A2/bootrom-combined.bin")
    cmd = [mcusim, "--bootrom", rom, "--set", "boot=direct", "-c", project, "--rtt", "stdio",
           "-t", f"{timeout_s}s", "--exit-on", r'^\{"done"', "--timeout-code", "124", elf]
    p = subprocess.run(cmd, capture_output=True, text=True)
    lines = [json.loads(l) for l in p.stdout.splitlines() if l.startswith("{")]
    return p.returncode, lines


def norm_trace(path, allow):
    """Commands as (cmd, arg, response class), CMD13 polls and repeats of a
    busy poll collapsed, allow-listed commands dropped."""
    out, last = [], None
    if not os.path.exists(path):
        return out
    for l in open(path):
        try:
            t = json.loads(l)
        except ValueError:
            continue
        key = (t.get("cmd"), t.get("arg"), t.get("resp_class", t.get("resp", "")))
        if t.get("cmd") in allow:
            continue
        if t.get("cmd") == 13 and last and last[0] == 13:
            continue
        out.append(key)
        last = key
    return out


def load_allow():
    p = os.path.join(ROOT, "test/sdio_diff/allowlist.txt")
    allow = set()
    if os.path.exists(p):
        for l in open(p):
            m = re.match(r"\s*CMD(\d+)\s", l)
            if m:
                allow.add(int(m.group(1)))
    return allow


def compare_steps(a, b, keys):
    diffs = []
    sa = {s["step"]: s for s in a if "step" in s}
    sb = {s["step"]: s for s in b if "step" in s}
    for n in sorted(set(sa) | set(sb)):
        x, y = sa.get(n), sb.get(n)
        if not x or not y:
            diffs.append(f"step {n}: only one driver reached it")
            continue
        for k in keys:
            if x.get(k) != y.get(k):
                diffs.append(f"step {n} '{x.get('name')}': {k} ours={x.get(k)!r} carlk3={y.get(k)!r}")
    return diffs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", default=os.path.join(ROOT, "build-sdio-diff"))
    ap.add_argument("--mcusim", default=os.path.expanduser("~/Work/mcusim"))
    ap.add_argument("--keep", default=None)
    a = ap.parse_args()

    mc = mcusim_bin(a.mcusim)
    if not mc:
        print(f"SKIP: no mcusim at {a.mcusim}/target/release/mcusim"); return 77
    if not has_sd_model(mc):
        print("SKIP: mcusim has no `sdcard` device yet (mcusim --list); see docs/sd.md 'Testing'"); return 77
    elfs = {f"{d}_{s}": os.path.join(a.build, f"sddiff_{d}_{s}.elf") for d in ("ours", "carlk3") for s in ("raw", "fs")}
    missing = [k for k, v in elfs.items() if not os.path.exists(v)]
    if missing:
        print(f"SKIP: not built: {', '.join(missing)} (cmake -S test/sdio_diff -B {a.build})"); return 77

    work = a.keep or tempfile.mkdtemp(prefix="sdio_diff_")
    os.makedirs(work, exist_ok=True)
    allow = load_allow()
    fails = []
    report = {}
    for scen in ("raw", "fs"):
        res = {}
        for drv in ("ours", "carlk3"):
            img = os.path.join(work, f"{drv}_{scen}.img")
            with open(img, "wb") as f:
                f.truncate(SECTORS * 512)
            trace = os.path.join(work, f"{drv}_{scen}.trace.jsonl")
            proj = os.path.join(work, f"{drv}_{scen}.toml")
            with open(proj, "w") as f:
                # mcusim's flat config (-c): the chip, then the card on its pins
                f.write('chip = "rp2350a"\n' + card_toml(img, trace, FAULTS if scen == "raw" else []))
            rc, lines = run(mc, a.mcusim, elfs[f"{drv}_{scen}"], proj)
            res[drv] = {"rc": rc, "lines": lines, "img": img, "trace": trace}
            if rc not in (0,):
                fails.append(f"{scen}/{drv}: mcusim exit {rc} after {len(lines)} lines")
        o, c = res["ours"], res["carlk3"]
        if scen == "raw":
            fails += compare_steps(o["lines"], c["lines"], ("res", "hash", "match", "readback", "sectors", "cid"))
            ho = hashlib.sha256(open(o["img"], "rb").read()).hexdigest()
            hc = hashlib.sha256(open(c["img"], "rb").read()).hexdigest()
            if ho != hc:
                fails.append(f"raw: final card images differ (ours {ho[:12]}, carlk3 {hc[:12]})")
            to, tc = norm_trace(o["trace"], allow), norm_trace(c["trace"], allow)
            if to != tc:
                i = next((k for k in range(min(len(to), len(tc))) if to[k] != tc[k]), min(len(to), len(tc)))
                fails.append(f"raw: command traces differ at #{i}: ours {to[i:i+3]} carlk3 {tc[i:i+3]} "
                             f"(accepted differences go in test/sdio_diff/allowlist.txt, with the reason)")
        else:
            fails += compare_steps(o["lines"], c["lines"], ("res", "hash", "size", "free_clusters"))
            for drv in ("ours", "carlk3"):
                img = res[drv]["img"]
                m = open(img, "rb").read(512)
                start = int.from_bytes(m[454:458], "little")
                count = int.from_bytes(m[458:462], "little")
                part = img + ".part"
                with open(img, "rb") as f, open(part, "wb") as g:
                    f.seek(start * 512); g.write(f.read(count * 512))
                if shutil.which("fsck.fat"):
                    p = subprocess.run(["fsck.fat", "-n", part], capture_output=True, text=True)
                    if p.returncode:
                        fails.append(f"fs/{drv}: fsck.fat: {p.stdout.strip().splitlines()[-1]}")
        report[scen] = {d: {"steps": [s for s in r["lines"] if "step" in s]} for d, r in res.items()}

    # times, side by side
    for scen in ("raw", "fs"):
        so = {s["step"]: s for s in report[scen]["ours"]["steps"]}
        sc = {s["step"]: s for s in report[scen]["carlk3"]["steps"]}
        for n in sorted(so):
            if "us" in so[n] and n in sc and "us" in sc[n]:
                print(f"  {scen} {so[n]['name']:<32} ours {so[n]['us']:>9} us   carlk3 {sc[n]['us']:>9} us")
    with open(os.path.join(work, "report.json"), "w") as f:
        json.dump(report, f, indent=1)
    if fails:
        print("DIFFERENT:")
        for x in fails:
            print("  " + x)
        print(f"(runs kept in {work})")
        return 1
    if a.keep:
        print(f"SAME: raw and fs scenarios match (runs in {work})")
    else:
        shutil.rmtree(work, ignore_errors=True)      # four 256 MiB card images
        print("SAME: raw and fs scenarios match")
    return 0


if __name__ == "__main__":
    sys.exit(main())
