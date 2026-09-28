#!/usr/bin/env bash
# One-time flash layout for OTA (see partitions.json). Over SWD, on the bench
# probe (tools/debug.sh). Afterwards updates go over MCUmgr/SMP and this is
# never needed again -- unless the partition table itself changes.
#
#   1. partition table       -> flash 0x000000 (first 64 KB erased first)
#   2. firmware (slot A)     -> flash 0x010000   build/btgw.bin, built with
#                               -DBTGW_TBYB=OFF so a NORMAL boot runs it
#   3. CYW43 firmware blob   -> flash 0x1C0000   build/btgw_wifi_firmware.bin
#
# LittleFS (0x200000+) and BTstack's NVM are not touched: cfg/, lua/ and the
# web assets survive. Usage: tools/provision.sh [build-dir]
set -eu
cd "$(dirname "$0")/.."
B=${1:-build}
PT=$B/_deps/picotool/picotool
[ -x "$PT" ] || PT=picotool
"$PT" partition create partitions.json "$B/pt.bin" >/dev/null
for f in "$B/pt.bin" "$B/btgw.bin" "$B/btgw_wifi_firmware.bin"; do
  [ -s "$f" ] || { echo "missing $f (build with CONFIG_OTA=y first)"; exit 1; }
done
grep -q "PICO_CRT0_IMAGE_TYPE_TBYB" "$B/compile_commands.json" 2>/dev/null && \
  echo "WARNING: this build is TBYB; a normal boot will not run it. Reconfigure with -DBTGW_TBYB=OFF for provisioning."
tools/debug.sh \
  -c "reset halt" \
  -c "flash erase_address 0x10000000 0x10000" \
  -c "program $B/pt.bin 0x10000000 verify" \
  -c "program $B/btgw.bin 0x10010000 verify" \
  -c "program $B/btgw_wifi_firmware.bin 0x101C0000 verify reset" \
  -c "exit"
