#!/usr/bin/env bash
# Assemble the notice bundle that must accompany a distributed firmware binary.
#
# BTstack, littlefs, lwIP, the pico-sdk and the cyw43 driver are BSD-3-Clause or
# similar, and their clause 2 requires binary redistribution to reproduce their
# copyright notice and disclaimer "in the documentation and/or other materials
# provided with the distribution". A .uf2 handed to someone is such a
# distribution, so this is not optional.
set -euo pipefail
cd "$(dirname "$0")/.."
out="${1:-build/NOTICES.txt}"
mkdir -p "$(dirname "$out")"
{
  echo "bluetooth_rp2_webapp -- third-party notices"
  echo "Generated $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo
  echo "This firmware is MIT licensed (see LICENSE). It incorporates the"
  echo "following third-party components, whose licences follow in full."
  echo "See THIRD_PARTY.md for how the Raspberry Pi grants for BTstack and the"
  echo "cyw43 driver apply."
  for f in licenses/*; do
    echo; echo "================================================================"
    echo "== $(basename "$f")"
    echo "================================================================"; echo
    cat "$f"
  done
} > "$out"
echo "wrote $out ($(wc -l < "$out") lines, $(ls licenses | wc -l) licences)"
