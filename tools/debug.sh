#!/usr/bin/env bash
# Bring up openocd against OUR probe only, on OUR ports only.
#
# An unrelated project shares this machine. Every safety check here exists
# because getting it wrong halts that project's target mid-run.
set -euo pipefail
cd "$(dirname "$0")/.."
source tools/bench.env

die() { echo "error: $*" >&2; exit 1; }

# --- locate OUR probe by serial, never "the first CMSIS-DAP device" -----------
sysfs=""
for d in /sys/bus/usb/devices/*; do
    [ -f "$d/serial" ] || continue
    [ "$(cat "$d/serial")" = "$BENCH_PROBE_SERIAL" ] || continue
    sysfs="$d"; break
done
[ -n "$sysfs" ] || die "probe $BENCH_PROBE_SERIAL not found -- plugged in?"
node=$(printf "/dev/bus/usb/%03d/%03d" "$(cat "$sysfs/busnum")" "$(cat "$sysfs/devnum")")

# --- access check BEFORE openocd, so we report the cause, not a libusb error --
# uaccess does not work on this machine (known, worked around by hand), and the
# node reverts to root:root 0660 on every re-enumeration -- including when a
# pacman install triggers a udev re-trigger. Never assume a past chmod survived.
if [ ! -w "$node" ]; then
    echo "Probe $BENCH_PROBE_SERIAL is not writable at $node" >&2
    echo "Run:  sudo chmod 0777 $node" >&2
    exit 1
fi

# --- reap ONLY an openocd proven ours: our port AND our serial ----------------
ours() {
    for p in "$BENCH_GDB_PORT" "$BENCH_TELNET_PORT" "$BENCH_TCL_PORT" "$BENCH_RTT_PORT"; do
        ss -ltnpH "sport = :$p" 2>/dev/null | grep -oP 'pid=\K[0-9]+' || true
    done | sort -u | while read -r pid; do
        [ -r "/proc/$pid/cmdline" ] || continue
        cmd=$(tr '\0' ' ' < "/proc/$pid/cmdline")
        case "$cmd" in
            *openocd*"$BENCH_PROBE_SERIAL"*) echo "$pid" ;;
            *openocd*) echo "REFUSE:$pid" ;;
        esac
    done
}
for hit in $(ours); do
    case "$hit" in
      REFUSE:*) die "an openocd NOT matching our serial holds one of our ports (pid ${hit#REFUSE:}); refusing to touch it" ;;
      *) echo "reaping our stale openocd pid $hit"; kill "$hit"; sleep 0.3 ;;
    esac
done

exec "$BENCH_OCD_DIR/openocd" -s "$BENCH_OCD_DIR/scripts" \
    -f interface/cmsis-dap.cfg \
    -c "adapter serial $BENCH_PROBE_SERIAL" \
    -c "adapter speed 5000" \
    -f target/rp2350.cfg \
    -c "gdb port $BENCH_GDB_PORT" \
    -c "telnet port $BENCH_TELNET_PORT" \
    -c "tcl port $BENCH_TCL_PORT" \
    -c "init" \
    -c "rtt setup 0x20000000 0x80000 \"SEGGER RTT\"" \
    -c "rtt start" \
    -c "rtt server start $BENCH_RTT_PORT 0" \
    "$@"
