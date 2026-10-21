#!/usr/bin/env python3
"""The pre-PEX LittleFS migration (src/fs/fs_migrate.c) on the real firmware,
in mcusim, from a flash image made by the OLD firmware.

  test/sim/lfs_migrate.py --old <old fw build> --new <new fw build> [--tree X]
                          [--scenarios a,b,...] [--work DIR] [--keep]

--old  a build of the firmware BEFORE the PEX partition (btgw.elf,
       btgw_wifi_firmware.bin; non-TBYB), e.g. 1.180 = feature/lua-net-fs~4
--new  this tree's SD build (pico2w_sd, -DBTGW_TBYB=OFF)
--tbyb optional: this tree's TBYB build, for the "tbyb" scenario
--tree what goes into the old LittleFS: a directory or a .tar (e.g. a /api/tar
       backup of a real gateway). Default: a synthetic tree made here, with a
       Wi-Fi config for mcusim's AP and a throwaway WireGuard key.

The golden image: the old partition table (partitions.json without PEX,
LittleFS 2032 KB), the old firmware in slot A, the Wi-Fi blob, a LittleFS
made with the firmware's own littlefs (test/sim/lfs_tool.c) at 0x200000 --
then the OLD firmware is booted on it once, so the filesystem is one that
firmware has mounted and written. Every scenario starts from a copy of it,
boots the NEW firmware and checks what is left on flash and on the card
(test/sim/sd_tool.c reads the card image with the firmware's FatFs).

Nothing leaves the simulator: USB-NCM NAT off, Wi-Fi on a mock backend. File
CONTENTS are never printed (a real backup holds credentials): comparisons
print paths and counts only. --work is deleted afterwards unless --keep.
"""
import argparse, base64, hashlib, json, os, random, re, shutil, signal, stat, subprocess, sys
import tarfile, threading, time, urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
FS_OFF = 0x200000
OLD_BLOCKS, NEW_BLOCKS = 508, 444
PEX_OFF, PEX_SIZE = 0x3BC000, 256 * 1024
# The only files the new firmware itself writes after a migration: the
# WireGuard boot counter (src/net/wg.c) and the migration's own marker.
WRITTEN_AT_BOOT = {"var/wg_epoch.bin"}
MARKER = "cfg/migrated.json"

FAILS = []
CHECKS = [0]


def check(name, ok, extra=""):
    CHECKS[0] += 1
    print(("ok   " if ok else "FAIL ") + name + (f"  [{extra}]" if extra else ""), flush=True)
    if not ok:
        FAILS.append(name)
    return ok


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


# ---------------------------------------------------------------- trees

def tree_hashes(d):
    """{relative path: sha256} for files, plus "path/" for every folder."""
    out = {}
    for base, dirs, files in os.walk(d):
        rel = os.path.relpath(base, d)
        for x in dirs:
            out[os.path.normpath(os.path.join(rel, x)) + "/"] = None
        for x in files:
            p = os.path.join(base, x)
            out[os.path.normpath(os.path.join(rel, x))] = hashlib.sha256(open(p, "rb").read()).hexdigest()
    return out


def tree_diff(a, b):
    """Paths only in a, only in b, and in both but different."""
    only_a = sorted(set(a) - set(b))
    only_b = sorted(set(b) - set(a))
    differ = sorted(k for k in set(a) & set(b) if a[k] != b[k])
    return only_a, only_b, differ


def synthetic_tree(d):
    """A gateway-shaped tree: cfg/ that works in mcusim, Lua, web assets, and
    the shapes littlefs and FAT care about (inline vs CTZ files, a 0-byte file,
    an empty folder, a 64-char name, a ~120-char path, names differing only
    in case, a 300 KB file)."""
    rnd = random.Random(1234)
    def put(rel, data):
        p = os.path.join(d, rel)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        open(p, "wb").write(data if isinstance(data, bytes) else data.encode())
    put("cfg/wifi.json", json.dumps({"mode": "sta", "nets": [{"ssid": "mcusim", "psk": "mcusim123"}]}))
    put("cfg/wireguard.json", json.dumps({
        "private_key": base64.b64encode(rnd.randbytes(32)).decode(),
        "peer_public_key": base64.b64encode(rnd.randbytes(32)).decode(),
        "address": "10.99.0.2", "netmask": "255.255.255.0",
        "endpoint": "10.42.0.1", "endpoint_port": 51820}))
    put("cfg/bt.json", '{"name":"sim-gw"}')
    put("cfg/eth.json", '{"mode":"dhcp"}')
    for i in range(40):
        put(f"lua/mod{i:02d}.lua", "-- module %d\n" % i + "x = %d\n" % i * rnd.randint(1, 900))
    put("lua/empty.lua", b"")
    put("lua/Case.lua", "return 'upper'\n")
    put("lua/case.lua", "return 'lower'\n")
    put("cache/big.bin", rnd.randbytes(300 * 1024))
    put("cache/edge4096.bin", rnd.randbytes(4096))
    put("cache/edge4097.bin", rnd.randbytes(4097))
    put("cache/inline256.bin", rnd.randbytes(256))
    put("a" * 64 + "/f.txt", "long segment\n")
    deep = "/".join(["dir%08d" % i for i in range(7)])          # 7 levels, 83 chars
    put(deep + "/" + "n" * (124 - len(deep) - 1), "deep\n")     # a 124-char path
    os.makedirs(os.path.join(d, "empty_dir"), exist_ok=True)
    for i in range(30):
        put(f"www/js/m{i}.js.gz", rnd.randbytes(rnd.randint(100, 9000)))


def extract_tar(tar, d):
    with tarfile.open(tar) as t:
        t.extractall(d, filter="data")


# ---------------------------------------------------------------- tools

def build_tools(work):
    lt, st = os.path.join(work, "lfs_tool"), os.path.join(work, "sd_tool")
    r = run(["gcc", "-O2", "-DLFS_NAME_MAX=64", "-Isrc/fs/littlefs", "-o", lt, "test/sim/lfs_tool.c",
             "src/fs/littlefs/lfs.c", "src/fs/littlefs/lfs_util.c"], cwd=ROOT)
    if r.returncode:
        sys.exit("lfs_tool: " + r.stderr[-300:])
    r = run(["gcc", "-O2", "-Ithird_party/fatfs", "-o", st, "test/sim/sd_tool.c", "third_party/fatfs/ff.c",
             "third_party/fatfs/ffunicode.c"], cwd=ROOT)
    if r.returncode:
        sys.exit("sd_tool: " + r.stderr[-300:])
    return lt, st


def region(flash, blocks, out):
    data = open(flash, "rb").read()
    open(out, "wb").write(data[FS_OFF:FS_OFF + blocks * 4096])


def lfs_info(lt, flash, work):
    """block_count of the filesystem on flash (read with block_count 0)."""
    img = os.path.join(work, "region.img")
    region(flash, OLD_BLOCKS, img)
    r = run([lt, "info", img])
    m = re.search(r"block_count (\d+)", r.stdout)
    return int(m.group(1)) if m else None


def lfs_dump(lt, flash, work, name):
    img = os.path.join(work, "region.img")
    region(flash, OLD_BLOCKS, img)
    out = os.path.join(work, name)
    shutil.rmtree(out, ignore_errors=True)
    r = run([lt, "dump", img, out])
    if r.returncode:
        return None, None
    m = re.search(r"block_count (\d+)", r.stdout)
    return out, int(m.group(1))


def sd_copy_tree(st, card, work, name):
    """The card's copy of the latest run, rebuilt from its manifest; the state
    file's fields. (None, state) when there is no complete copy."""
    raw = os.path.join(work, name + "_raw")
    shutil.rmtree(raw, ignore_errors=True)
    r = run([st, "get", card, ".lfs-migrate", raw])
    if r.returncode:
        return None, {}
    state = {}
    sp = os.path.join(raw, "state")
    if os.path.exists(sp):
        for line in open(sp):
            if "=" in line:
                k, v = line.strip().split("=", 1)
                state[k] = v
    run_dir = os.path.join(raw, state.get("run", "0"))
    man = os.path.join(run_dir, "manifest")
    if not os.path.exists(man):
        return None, state
    out = os.path.join(work, name)
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(out)
    for line in open(man, "rb").read().decode().splitlines():
        if line.startswith("D "):
            os.makedirs(os.path.join(out, line[2:]), exist_ok=True)
        elif line.startswith("F "):
            _, idx, size, crc, path = line.split(" ", 4)
            p = os.path.join(out, path)
            os.makedirs(os.path.dirname(p), exist_ok=True)
            shutil.copyfile(os.path.join(run_dir, "f", idx), p)
    return out, state


# ---------------------------------------------------------------- the simulator

class Sim:
    def __init__(self, mcusim, toml, log):
        self.lines = []
        self.log = open(log, "w")
        self.p = subprocess.Popen([mcusim, "-p", toml], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                  text=True, errors="replace")
        self.t = threading.Thread(target=self._read, daemon=True)
        self.t.start()

    def _read(self):
        for line in self.p.stdout:
            self.lines.append(line.rstrip("\n"))
            self.log.write(line)
            self.log.flush()

    def wait(self, pattern, timeout, start=0):
        """Index of the first line from `start` matching pattern, or -1."""
        rx = re.compile(pattern)
        t0 = time.time()
        i = start
        while time.time() - t0 < timeout:
            while i < len(self.lines):
                if rx.search(self.lines[i]):
                    return i
                i += 1
            if self.p.poll() is not None and i >= len(self.lines):
                return -1
            time.sleep(0.05)
        return -1

    def grep(self, pattern):
        rx = re.compile(pattern)
        return [l for l in self.lines if rx.search(l)]

    def stop(self, sig=signal.SIGTERM):
        if self.p.poll() is None:
            self.p.send_signal(sig)
            try:
                self.p.wait(15)
            except subprocess.TimeoutExpired:
                self.p.kill()
                self.p.wait()
        self.t.join(5)
        self.log.close()


def op(port, name, params=None):
    body = json.dumps({"op": name, "params": params or {}}).encode()
    req = urllib.request.Request(f"http://127.0.0.1:{port}/api/op", data=body, method="POST")
    return json.load(urllib.request.urlopen(req, timeout=10))


def wait_http(port, timeout):
    t0 = time.time()
    while time.time() - t0 < timeout:
        try:
            urllib.request.urlopen(f"http://127.0.0.1:{port}/api/fs/", timeout=3).read()
            return True
        except Exception:
            time.sleep(1)
    return False


TOML = """
[project]
name = "btgw-lfs-migrate"
board = "pico2_w"
[boot]
bootrom = "A2"
{reboot}
[flash]
partition_table = "{pt}"
images = [ {{ file = "{wifi}", partition = "Wi-Fi Firmware" }}{extra_images} ]
persist = "{flash}"
[firmware]
image = "{elf}"
partition = "Main A"
[[device]]
type = "cyw43439"
name = "cyw43"
[device.network]
kind = "sim"
dns_upstream = "none"
backend = {{ kind = "mock", service = [] }}
{card}
[chip]
usb_host = {{ kind = "net", forward = ["{port}:80"], net = {{ nat = false }} }}
[run]
time = "{time}"
rtt = "stdio"
"""

CARD = """
[[device]]
type = "sdcard"
mode = "sdio"
clk = 2
cmd = 3
d0 = 4
detect = 9
image = "{img}"
writeback = true
{extra}
"""


class Harness:
    def __init__(self, a):
        self.a = a
        self.work = os.path.abspath(a.work)
        os.makedirs(self.work, exist_ok=True)
        os.chmod(self.work, stat.S_IRWXU)
        self.lt, self.st = build_tools(self.work)
        self.port = a.port
        self.n = 0

    # -- images
    def pt(self, json_obj, name):
        j = os.path.join(self.work, name + ".json")
        b = os.path.join(self.work, name + ".bin")
        json.dump(json_obj, open(j, "w"))
        picotool = os.path.join(self.a.new, "_deps/picotool/picotool")
        r = run([picotool, "partition", "create", j, b])
        if r.returncode:
            sys.exit("picotool: " + r.stderr[-300:])
        return b

    def card(self, name, size="64M", fs="exfat"):
        p = os.path.join(self.work, name)
        if os.path.exists(p):
            os.remove(p)
        with open(p, "wb") as f:
            f.truncate({"M": 1 << 20}[size[-1]] * int(size[:-1]))
        r = run(["mkfs.exfat", "-L", "GW", p] if fs == "exfat" else ["mkfs.fat", "-F", "32", "-n", "GW", p])
        if r.returncode:
            sys.exit("mkfs: " + r.stderr[-200:])
        return p

    def toml(self, elf, flash, card=None, card_extra="", tbyb=False, time_="300s", pt=None, extra_images=""):
        self.n += 1
        t = os.path.join(self.work, f"run{self.n:02d}.toml")
        open(t, "w").write(TOML.format(
            reboot='reboot = "flash_update:Main A"' if tbyb else "",
            pt=pt or self.new_pt, wifi=os.path.join(self.a.new, "btgw_wifi_firmware.bin"), flash=flash, elf=elf,
            extra_images=extra_images, port=self.port, time=time_,
            card=CARD.format(img=card, extra=card_extra) if card else ""))
        return t

    def sim(self, toml):
        return Sim(self.a.mcusim, toml, toml.replace(".toml", ".log"))

    def golden(self):
        """The old layout, as the old firmware leaves it."""
        newpt = json.load(open(os.path.join(ROOT, "partitions.json")))
        self.new_pt = self.pt(newpt, "pt_new")
        old = json.loads(json.dumps(newpt))
        old["partitions"] = [p for p in old["partitions"] if p["name"] != "PEX"]
        for p in old["partitions"]:
            if p["name"] == "LittleFS":
                p["size"] = "2032K"
        self.old_pt = self.pt(old, "pt_old")
        tree = os.path.join(self.work, "tree")
        shutil.rmtree(tree, ignore_errors=True)
        os.makedirs(tree, mode=0o700)
        if not self.a.tree:
            synthetic_tree(tree)
            self.synthetic = True
        else:
            self.synthetic = False
            if os.path.isdir(self.a.tree):
                shutil.copytree(self.a.tree, tree, dirs_exist_ok=True)
            else:
                extract_tar(self.a.tree, tree)
        img = os.path.join(self.work, "old_lfs.img")
        r = run([self.lt, "mk", img, str(OLD_BLOCKS), tree])
        if r.returncode:
            sys.exit("lfs_tool mk: " + r.stderr[-300:])
        print("old LittleFS:", r.stdout.strip())
        self.golden_flash = os.path.join(self.work, "golden.bin")
        if os.path.exists(self.golden_flash):
            os.remove(self.golden_flash)
        t = self.toml(os.path.join(self.a.old, "btgw.elf"), self.golden_flash, pt=self.old_pt,
                      extra_images=f', {{ file = "{img}", at = 0x10200000 }}')
        s = self.sim(t)
        ok = s.wait(r"^main: init done", 240) >= 0
        s.wait(r"^wg: tunnel up|^wg: .*missing", 30)
        time.sleep(3)
        s.stop()
        if not ok:
            sys.exit("the old firmware did not boot: see " + t.replace(".toml", ".log"))
        mounted = s.grep(r"^fs: mounted (\d+)KiB")
        if not mounted or "2032" not in mounted[0]:
            sys.exit("the old firmware did not mount its 2032 KB filesystem")
        self.ref, bc = lfs_dump(self.lt, self.golden_flash, self.work, "ref")
        if bc != OLD_BLOCKS:
            sys.exit("golden image: not a 508-block filesystem")
        self.ref_h = tree_hashes(self.ref)
        nf = sum(1 for k in self.ref_h if not k.endswith("/"))
        print(f"golden image: {nf} files, {len(self.ref_h) - nf} folders, written by the old firmware")

    def fresh(self, name):
        f = os.path.join(self.work, name + ".flash")
        shutil.copyfile(self.golden_flash, f)
        return f

    # -- checks
    def check_migrated(self, tag, flash, card, ref_h=None):
        """ref_h: the filesystem as it was right before the migrating boot
        (default: the golden image)."""
        ref_h = ref_h or self.ref_h
        tree, bc = lfs_dump(self.lt, flash, self.work, tag + "_lfs")
        check(f"{tag}: LittleFS is the new {NEW_BLOCKS}-block geometry", bc == NEW_BLOCKS, f"block_count {bc}")
        if not tree:
            return
        h = tree_hashes(tree)
        only_ref, only_new, differ = tree_diff(ref_h, h)
        extra_new = [p for p in only_new if p != MARKER]
        unexpected = [p for p in differ if p not in WRITTEN_AT_BOOT]
        nf = sum(1 for k in ref_h if not k.endswith("/"))
        check(f"{tag}: every file and folder came back byte-identical",
              not only_ref and not extra_new and not unexpected,
              f"{nf} files; missing {only_ref[:3]} extra {extra_new[:3]} differ {unexpected[:3]}"
              + (f" (rewritten at boot, expected: {differ})" if differ and not unexpected else ""))
        cfg_ref = {k: v for k, v in ref_h.items() if k.startswith("cfg/")}
        cfg_new = {k: v for k, v in h.items() if k.startswith("cfg/") and k != MARKER}
        check(f"{tag}: cfg/ identical", cfg_ref == cfg_new, f"{len(cfg_ref)} entries")
        check(f"{tag}: marker {MARKER} written", MARKER in h)
        copy, state = sd_copy_tree(self.st, card, self.work, tag + "_sd")
        if copy:
            ch = tree_hashes(copy)
            a, b, c = tree_diff(ref_h, ch)
            check(f"{tag}: the SD copy is byte-identical to the old filesystem", not (a or b or c),
                  f"run {state.get('run')}; missing {a[:3]} extra {b[:3]} differ {c[:3]}")
        else:
            check(f"{tag}: the SD copy is byte-identical to the old filesystem", False, "no copy on the card")
        check(f"{tag}: the card's state says done", state.get("phase") == "done", f"phase {state.get('phase')}")

    def check_untouched(self, tag, flash):
        tree, bc = lfs_dump(self.lt, flash, self.work, tag + "_lfs")
        check(f"{tag}: LittleFS NOT reformatted (still {OLD_BLOCKS} blocks)", bc == OLD_BLOCKS, f"block_count {bc}")
        if not tree:
            return
        h = tree_hashes(tree)
        a, b, c = tree_diff(self.ref_h, h)
        c = [p for p in c if p not in WRITTEN_AT_BOOT]
        check(f"{tag}: every file intact (cfg/ included)", not (a or b or c), f"missing {a[:3]} extra {b[:3]} differ {c[:3]}")

    def check_boot(self, tag, s, migrated):
        """Config read AFTER the migration, network up from it, sys.info."""
        s.wait(r"^main: init done", 300)
        s.wait(r"^wg: (tunnel up|.*missing)", 60)
        lines = list(s.lines)
        done = next((i for i, l in enumerate(lines) if l.startswith("migrate: DONE")), -1)
        cfg = next((i for i, l in enumerate(lines) if l.startswith("cfg: ")), -1)
        wg = next((i for i, l in enumerate(lines) if l.startswith("wg: tunnel up")), -1)
        if migrated:
            check(f"{tag}: config read after the migration", done >= 0 and cfg > done, f"DONE @{done}, cfg @{cfg}")
            check(f"{tag}: WireGuard up from the restored config", wg > done >= 0)
        else:
            check(f"{tag}: config read, WireGuard up", cfg >= 0 and wg >= 0)
        nets = re.search(r"cfg: (\d+) network", lines[cfg]) if cfg >= 0 else None
        check(f"{tag}: Wi-Fi config loaded", bool(nets) and int(nets.group(1)) > 0,
              f"{nets.group(1) if nets else 0} network(s)")
        if not wait_http(self.port, 120):
            check(f"{tag}: HTTP answers over USB-NCM", False)
            return None
        info = op(self.port, "sys.info").get("result", {})
        fs = info.get("fs", {})
        pex = fs.get("pex", {})
        check(f"{tag}: sys.info reports the PEX partition",
              pex.get("offset") == PEX_OFF and pex.get("size") == PEX_SIZE,
              f"offset 0x{pex.get('offset', 0):x} size {pex.get('size')} state '{pex.get('state')}', "
              f"core0 stack max {info.get('stack_max')} B")
        wgs = op(self.port, "wg.status").get("result", {})
        check(f"{tag}: wg.status: tunnel configured", bool(wgs.get("address")), f"address {wgs.get('address')}")
        if self.synthetic and migrated is not None:
            ip = None
            for _ in range(40):
                try:
                    ip = json.load(urllib.request.urlopen(f"http://127.0.0.1:{self.port}/api/wifi", timeout=5)).get("ip")
                except Exception:
                    ip = None
                if ip and ip != "0.0.0.0":
                    break
                time.sleep(1)
            check(f"{tag}: Wi-Fi joined with the restored config", bool(ip) and ip != "0.0.0.0", f"ip {ip}")
        return fs

    # -- scenarios
    def sc_happy(self):
        flash, card = self.fresh("happy"), self.card("happy.img")
        s = self.sim(self.toml(self.a.new_elf, flash, card,
                               card_extra=f'trace = "{os.path.join(self.work, "happy_sd.jsonl")}"'))
        t0 = time.time()
        i = s.wait(r"^migrate: DONE|^migrate: migration pending|PANIC", 900)
        took = time.time() - t0
        ok = i >= 0 and s.lines[i].startswith("migrate: DONE")
        check("happy: migration ran and finished", ok, (s.lines[i] if i >= 0 else "no outcome")[:150] +
              f" (wall {took:.0f} s)")
        fs = self.check_boot("happy", s, True) if ok else None
        if fs:
            check("happy: sys.info fs: 1776 KB mounted, migrated this boot",
                  fs.get("kb") == 1776 and fs.get("migration", "").startswith("migrated this boot"),
                  f"kb {fs.get('kb')}, migration '{fs.get('migration')}'")
            check("happy: PEX partition state reserved", fs.get("pex", {}).get("state") == "reserved")
        s.stop()
        self.check_migrated("happy", flash, card)
        # Second boot: nothing to do, and it says so.
        s = self.sim(self.toml(self.a.new_elf, flash, card))
        i = s.wait(r"^main: init done", 300)
        fs = self.check_boot("happy-reboot", s, False) if i >= 0 else None
        check("happy-reboot: no migration on the second boot", not s.grep(r"^migrate: (copying|formatting)"))
        if fs:
            check("happy-reboot: sys.info migration 'done'", fs.get("migration") == "done" and fs.get("kb") == 1776,
                  f"'{fs.get('migration')}', kb {fs.get('kb')}")
        s.stop()
        self.check_migrated("happy-reboot", flash, card)
        self.write_lbas = []
        try:
            for line in open(os.path.join(self.work, "happy_sd.jsonl")):
                j = json.loads(line)
                if j.get("cmd") in (24, 25) and j.get("arg") is not None:
                    self.write_lbas.append(j["arg"])
        except OSError:
            pass

    def pending(self, tag, flash, card, why_rx, card_extra=""):
        s = self.sim(self.toml(self.a.new_elf, flash, card, card_extra=card_extra))
        i = s.wait(r"^migrate: (DONE|migration pending)|PANIC", 900)
        line = s.lines[i] if i >= 0 else ""
        check(f"{tag}: migration pending, with the reason", bool(re.search(why_rx, line)), line[:150])
        check(f"{tag}: nothing formatted", not s.grep(r"^migrate: formatting"))
        fs = self.check_boot(tag, s, False)
        if fs:
            check(f"{tag}: sys.info says pending, mounts the old 2032 KB",
                  fs.get("migration", "").startswith("migration pending") and fs.get("kb") == 2032,
                  f"'{fs.get('migration')}', kb {fs.get('kb')}")
            try:
                root = json.load(urllib.request.urlopen(f"http://127.0.0.1:{self.port}/api/fs/", timeout=5))
                check(f"{tag}: /api/fs says pending too", root.get("migration", "").startswith("migration pending"))
            except Exception as e:
                check(f"{tag}: /api/fs says pending too", False, str(e)[:60])
        s.stop()
        self.check_untouched(tag, flash)

    def sc_nocard(self):
        flash = self.fresh("nocard")
        self.pending("nocard", flash, None, r"pending: no SD card")
        # That boot ran on the old geometry and wrote (the WireGuard boot
        # counter): what the next boot migrates is this, not the golden image.
        before, _ = lfs_dump(self.lt, flash, self.work, "nocard_before")
        before_h = tree_hashes(before)
        # The card arrives: the next boot migrates.
        card = self.card("nocard.img")
        s = self.sim(self.toml(self.a.new_elf, flash, card))
        i = s.wait(r"^migrate: DONE|^migrate: migration pending|PANIC", 900)
        check("nocard+card: the next boot with a card migrates", i >= 0 and s.lines[i].startswith("migrate: DONE"),
              s.lines[i][:120] if i >= 0 else "")
        s.stop()
        self.check_migrated("nocard+card", flash, card, before_h)

    def sc_full(self):
        self.pending("full", self.fresh("full"), self.card("full.img", "8M"), r"pending: SD card full")

    def sc_writefail(self):
        lbas = getattr(self, "write_lbas", [])
        if not lbas:
            check("writefail: LBAs from the happy run's trace", False, "run 'happy' first")
            return
        lba = sorted(lbas)[len(lbas) // 2]
        self.pending("writefail", self.fresh("writefail"), self.card("writefail.img"), r"pending: SD (copy failed|card not writable)",
                     card_extra=f'[[device.fault]]\nkind = "write_error"\nlba = {lba}\nonce = false')
        lba2 = sorted(lbas)[len(lbas) // 3]
        self.pending("pulled", self.fresh("pulled"), self.card("pulled.img"), r"pending: SD (copy failed|card not writable)",
                     card_extra=f'[[device.fault]]\nkind = "pull"\nlba = {lba2}')

    def sc_powercut(self):
        """Power off (SIGKILL: flash and card are write-through) at points
        through the migration, then boot again: it must end migrated and
        identical, never without cfg/."""
        # The firmware reports progress every 25 files / entries.
        nf = sum(1 for k in self.ref_h if not k.endswith("/"))
        ne = len(self.ref_h)
        late_f, late_e = (nf - 1) // 25 * 25, (ne - 1) // 25 * 25
        points = [
            ("copy", r"^migrate: 25 files", 0.0),
            ("copy-late", rf"^migrate: {late_f} files", 0.3),
            ("before-format", r"^migrate: copy verified", 0.0),
            ("format", r"^migrate: formatting", 0.0),
            ("restore-cfg", r"^migrate: formatting", 0.4),
            ("restore", r"^migrate: 25 entries restored", 0.0),
            ("restore-late", rf"^migrate: {late_e} entries restored", 0.2),
            ("done", r"^migrate: DONE", 0.0),
        ]
        # And a few anywhere in the format + restore (a cut between the format
        # and the in-progress note was found this way).
        rnd = random.Random(7)
        points += [(f"restore-rand{k}", r"^migrate: formatting", round(rnd.uniform(0.02, 1.5), 2)) for k in range(4)]
        for name, rx, delay in points:
            tag = "cut@" + name
            flash, card = self.fresh(tag), self.card(tag + ".img")
            s = self.sim(self.toml(self.a.new_elf, flash, card))
            i = s.wait(rx, 900)
            if delay:
                time.sleep(delay)
            s.stop(signal.SIGKILL)
            if not check(f"{tag}: reached the cut point", i >= 0, rx):
                continue
            # What is on flash right now must keep cfg/ reachable: either the
            # old filesystem intact, or a new one with cfg/ (or empty and
            # resumable from the card).
            s = self.sim(self.toml(self.a.new_elf, flash, card))
            j = s.wait(r"^main: init done", 900)
            out = s.grep(r"^migrate: (DONE|migration pending|resuming|run \d+ had begun)")
            s2 = s.grep(r"^cfg: \d+ network")
            check(f"{tag}: the reboot finishes the migration", j >= 0 and (any(l.startswith("migrate: DONE") for l in out)
                  or not s.grep(r"^migrate: ")), "; ".join(o[:70] for o in out) or "no migrate lines")
            check(f"{tag}: config loaded on the reboot", bool(s2) and not s2[0].startswith("cfg: 0"), (s2 or ["none"])[0])
            s.stop()
            self.check_migrated(tag, flash, card)

    def sc_doublecut(self):
        """Cut during the restore twice in a row, then let it finish."""
        tag = "doublecut"
        flash, card = self.fresh(tag), self.card(tag + ".img")
        for k, rx in enumerate([r"^migrate: 25 entries restored", r"^migrate: 50 entries restored"]):
            s = self.sim(self.toml(self.a.new_elf, flash, card))
            i = s.wait(rx, 900)
            s.stop(signal.SIGKILL)
            check(f"{tag}: cut {k + 1} reached", i >= 0, rx)
        s = self.sim(self.toml(self.a.new_elf, flash, card))
        i = s.wait(r"^migrate: DONE|^migrate: migration pending", 900)
        check(f"{tag}: third boot finishes", i >= 0 and s.lines[i].startswith("migrate: DONE"), s.lines[i][:100] if i >= 0 else "")
        st = s.grep(r"attempt (\d+)")
        check(f"{tag}: the restore was retried from the card", any("attempt 3" in l for l in st), "; ".join(l[-30:] for l in st))
        s.stop()
        self.check_migrated(tag, flash, card)

    def sc_tbyb(self):
        if not self.a.tbyb:
            print("skip tbyb: no --tbyb build")
            return
        flash = self.fresh("tbyb")
        s = self.sim(self.toml(os.path.join(self.a.tbyb, "btgw.elf"), flash, self.card("tbyb.img"), tbyb=True))
        i = s.wait(r"^migrate: (DONE|migration pending)|PANIC", 600)
        line = s.lines[i] if i >= 0 else ""
        check("tbyb: an unbought OTA image does not migrate", "waiting for the OTA confirm" in line, line[:120])
        s.stop()
        self.check_untouched("tbyb", flash)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--old", required=True)
    ap.add_argument("--new", required=True)
    ap.add_argument("--tbyb")
    ap.add_argument("--tree")
    ap.add_argument("--mcusim", default=os.path.expanduser("~/Work/mcusim/target/release/mcusim"))
    ap.add_argument("--work", default=os.path.join(ROOT, "build-lfsmig"))
    ap.add_argument("--port", type=int, default=18291)
    ap.add_argument("--scenarios", default="happy,nocard,full,writefail,powercut,doublecut,tbyb")
    ap.add_argument("--keep", action="store_true")
    a = ap.parse_args()
    a.old, a.new = os.path.abspath(a.old), os.path.abspath(a.new)
    a.tbyb = os.path.abspath(a.tbyb) if a.tbyb else None
    a.new_elf = os.path.join(a.new, "btgw.elf")
    if not os.path.exists(a.mcusim):
        print("SKIP: no mcusim")
        return 77
    h = Harness(a)
    try:
        h.golden()
        for sc in a.scenarios.split(","):
            print(f"--- {sc}", flush=True)
            getattr(h, "sc_" + sc)()
    finally:
        if not a.keep:
            shutil.rmtree(h.work, ignore_errors=True)
    print(f"{CHECKS[0]} checks, {len(FAILS)} failed" + (": " + ", ".join(FAILS[:8]) if FAILS else ""))
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
