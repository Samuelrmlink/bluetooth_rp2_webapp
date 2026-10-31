#!/bin/bash
# Refresh the gitignored browser toolchain cache at web/.pex-cc/.
#
# Build fetches these files from the SD card (pex-cc/...) the first time it
# is clicked. Nothing here is copied into web/js/. pexcc.wasm is the WASI
# compiler from the pexcc repo (make wasm). It is not web/pex-cc/pexcc.c.
#
#   web/pex-cc/make_pexcc.sh
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
PEXCC=${PEXCC:-$(cd "$ROOT/../pexcc" && pwd)}
CACHE="$HERE/../.pex-cc"
DRV="$ROOT/pex/modules/drivers"

make -C "$PEXCC" wasm
rm -rf "$CACHE"
mkdir -p "$CACHE/include" "$CACHE/modinc" "$CACHE/drivers"
cp "$PEXCC/build/pexcc.wasm" "$CACHE/pexcc.wasm"
cp "$PEXCC/include/pex.h" "$CACHE/include/pex.h"
# The SDK pex.h includes pex_api.h: generated here from this repo's
# host_api.json, so the page compiles against the API it links against.
python3 "$ROOT/pex/tools/gen_host_api.py" header "$ROOT/pex/host_api.json" "$CACHE/include/pex_api.h"
cp "$PEXCC/modinc/stddef.h" "$PEXCC/modinc/stdint.h" "$PEXCC/modinc/string.h" \
   "$PEXCC/modinc/stdbool.h" "$PEXCC/modinc/stdarg.h" "$CACHE/modinc/"
cp "$ROOT/pex/host_api.json" "$CACHE/host_api.json"
cp "$DRV"/anker_app.c "$DRV"/anker_adv.c "$DRV"/anker_adv.h \
   "$DRV"/anker_wire.c "$DRV"/anker_wire.h \
   "$DRV"/anker_models.c "$DRV"/anker_models.h \
   "$DRV"/pinecil_app.c "$DRV"/pinecil_live.c "$DRV"/pinecil_live.h \
   "$CACHE/drivers/"

echo "cache $CACHE"
ls -la "$CACHE" "$CACHE/include" "$CACHE/modinc" "$CACHE/drivers"
