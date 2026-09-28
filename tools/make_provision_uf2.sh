#!/usr/bin/env bash
# Build a single drag-and-drop UF2 that provisions a bare board: partition
# table + firmware (slot A) + Wi-Fi blob, the same three regions
# tools/provision.sh writes over SWD -- but delivered over USB mass storage,
# so a board can be brought up with no debug probe.
#
#   make_provision_uf2.sh [build-dir]   ->   <build-dir>/btgw-provision.uf2
#
# To apply: hold BOOTSEL while plugging the board in, then copy the file onto
# the RP2350 drive (or `picotool load btgw-provision.uf2`). The board writes
# all three regions and reboots into the new firmware.
#
# Every block is ABSOLUTE-addressed (PT at 0x10000000, firmware at 0x10010000
# = slot A, blob at 0x101C0000), so nothing depends on a partition table being
# resident mid-transfer -- the bytes written are identical to the SWD path,
# which tools/run's equivalence check in the repo history confirms. LittleFS
# (0x200000+) is not touched, so a reprovision keeps cfg/, lua/ and web assets.
#
# The firmware MUST be a non-TBYB build (cmake -DBTGW_TBYB=OFF): a
# try-before-you-buy image only boots via a flash-update, not the normal boot
# that follows provisioning. OTA releases are TBYB; this is not one.
set -eu
cd "$(dirname "$0")/.."
B=${1:-build}
PT=$B/_deps/picotool/picotool
[ -x "$PT" ] || PT=picotool

for f in partitions.json "$B/btgw.bin" "$B/btgw_wifi_firmware.bin"; do
  [ -s "$f" ] || { echo "missing $f (build with CONFIG_OTA=y first)"; exit 1; }
done

# Refuse a TBYB image: it would not boot after provisioning.
if "$PT" info "$B/btgw.bin" 2>/dev/null | grep -qi "tbyb"; then
  echo "ERROR: $B/btgw.bin is a try-before-you-buy image; it will not boot on a"
  echo "       normal boot. Rebuild for provisioning:  cmake -B $B -DBTGW_TBYB=OFF"
  exit 1
fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
"$PT" partition create partitions.json "$tmp/pt.uf2"   --family absolute -o 0x10000000 >/dev/null
"$PT" uf2 convert "$B/btgw.bin"               -t bin "$tmp/app.uf2"  -o 0x10010000 --family absolute >/dev/null
"$PT" uf2 convert "$B/btgw_wifi_firmware.bin" -t bin "$tmp/blob.uf2" -o 0x101C0000 --family absolute >/dev/null
"$PT" uf2 combine "$tmp/pt.uf2"  "$tmp/app.uf2"  "$tmp/mid.uf2"          >/dev/null
"$PT" uf2 combine "$tmp/mid.uf2" "$tmp/blob.uf2" "$B/btgw-provision.uf2" >/dev/null

echo "wrote $B/btgw-provision.uf2 ($(stat -c%s "$B/btgw-provision.uf2") bytes)"
echo "  hold BOOTSEL, plug in, and copy it onto the RP2350 drive (or: picotool load ...)"
