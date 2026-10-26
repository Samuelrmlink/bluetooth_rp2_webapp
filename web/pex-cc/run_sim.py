#!/usr/bin/env python3
"""Boot the SD+unsigned TBYB image in mcusim and run the browser Build test.

Does not flash a board and does not touch lua/. The simulator binary is copied
to pexsim-cc before exec (a process named mcusim gets killed on this machine).
pexsim-editor and pexsim-gw are left alone.

Development images set CONFIG_PEX_ALLOW_UNSIGNED. This sim still boots the
image in build-pexcc. The user's build/ is not used.
"""
import os, shutil, subprocess, sys, time, urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
WEB = ROOT / "web"
WORK = Path("/tmp/pexcc-sim-work")
PORT = 8096
MCUSIM_SRC = Path.home() / "Work/mcusim-agents/blegatt/target/release/mcusim"
ROM = Path.home() / "Work/mcusim/third_party/bootrom/rp2350-A2/bootrom-combined.bin"


def wait_http(url, timeout):
    t0 = time.time()
    last = ""
    while time.time() - t0 < timeout:
        try:
            with urllib.request.urlopen(url, timeout=2) as r:
                if r.status == 200:
                    return True
                last = f"HTTP {r.status}"
        except Exception as e:
            last = str(e)
        time.sleep(1)
    print("HTTP never came up:", last, file=sys.stderr)
    return False


def main():
    if not MCUSIM_SRC.is_file():
        sys.exit(f"missing {MCUSIM_SRC}")
    fw = ROOT / "build-pexcc"
    # btgw.elf is the TBYB application (version minor 238). The file
    # btgw_wifi_firmware_tbyb.elf is the CYW43 blob, not the app.
    elf = fw / "btgw.elf"
    wifi = fw / "btgw_wifi_firmware.bin"
    if not elf.is_file() or not wifi.is_file():
        sys.exit("build-pexcc firmware is missing")
    cache = WEB / ".pex-cc"
    if not (cache / "pexcc.wasm").is_file():
        sys.exit("run web/pex-cc/make_pexcc.sh first")
    WORK.mkdir(parents=True, exist_ok=True)
    exe = WORK / "pexsim-cc"
    shutil.copy2(MCUSIM_SRC, exe)
    os.chmod(exe, 0o755)
    mk = WORK / "mkcard"
    r = subprocess.run(["clang", "-O2", "-std=c11", f"-I{ROOT}/third_party/fatfs",
                        "-o", str(mk), str(WEB / "pex-cc/mkcard.c"),
                        str(ROOT / "third_party/fatfs/ff.c"),
                        str(ROOT / "third_party/fatfs/ffunicode.c")],
                       capture_output=True, text=True)
    if r.returncode:
        sys.exit(r.stderr or r.stdout)
    card = WORK / "card.img"
    r = subprocess.run([str(mk), str(card), str(cache)], capture_output=True, text=True)
    sys.stderr.write(r.stderr)
    if r.returncode:
        sys.exit("mkcard failed")
    toml = WORK / "board.toml"
    toml.write_text(f"""
[project]
name = "btgw-pexcc"
board = "pico2_w"
[boot]
bootrom = "A2"
reboot = "flash_update:Main A"
[flash]
partition_table = "{ROOT}/partitions.json"
images = [ {{ file = "{wifi}", partition = "Wi-Fi Firmware" }} ]
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
[[device]]
type = "sdcard"
name = "sd"
mode = "sdio"
clk = 2
cmd = 3
d0 = 4
detect = 9
image = "{card}"
writeback = true
init_time = "1ms"
[run]
time = "14400s"
rtt = "stdio"
usb_host = "net"
usb_net_forward = ["{PORT}:80"]
""")
    log = open(WORK / "sim.log", "w", buffering=1)
    # Toml bootrom = "A2" is how the blegatt projects boot a TBYB image.
    # Do not pass boot=direct.
    proc = subprocess.Popen([str(exe), "-p", str(toml)],
                            stdout=log, stderr=subprocess.STDOUT)
    print(f"pexsim-cc pid {proc.pid} log {WORK}/sim.log", flush=True)
    try:
        if not wait_http(f"http://127.0.0.1:{PORT}/", 180):
            sys.exit("simulator HTTP did not answer")
        # the card must answer before the page tries to compile
        if not wait_http(f"http://127.0.0.1:{PORT}/api/sd/files/pex-cc/pexcc.wasm", 120):
            sys.exit("SD toolchain was not served")
        r = subprocess.run([sys.executable, str(ROOT / "tools/make_release.py")], cwd=ROOT)
        if r.returncode:
            sys.exit("make_release failed")
        r = subprocess.run([sys.executable, str(ROOT / "tools/upload_fw_assets.py"),
                            "--base", f"http://127.0.0.1:{PORT}"], cwd=ROOT)
        if r.returncode:
            sys.exit("upload failed")
        env = os.environ.copy()
        env["PEXCC_BASE"] = f"http://127.0.0.1:{PORT}"
        r = subprocess.run(["npx", "playwright", "test", "--config", "playwright.sim.config.js"],
                           cwd=WEB, env=env)
        if r.returncode:
            sys.exit(r.returncode)
        print("sim build test passed")
    finally:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
        log.close()


if __name__ == "__main__":
    main()
