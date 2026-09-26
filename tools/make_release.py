#!/usr/bin/env python3
"""Builds the release asset set: gzips every shipped file (as it would be
stored pre-compressed on the microcontroller's LittleFS) and reports the
total compressed size against the ~1-2MB flash budget.

Lives in tools/ rather than web/ — it's a repo-level concern (the future
RP2040 firmware build is the actual consumer of its output), not part of
the web app's own PC-only source/tooling.

By default this just copies web/'s source tree verbatim into the
top-level web-dist/ (a sibling of both web/ and tools/) and gzips it — no
minification/bundling. Pass --minify to additionally bundle+minify the JS
(via esbuild — resolved from web/node_modules/, a dev-only dependency of
the web app itself; never shipped, and never used at dev time, where the
app runs as plain unbundled ES modules straight off disk) and minify the
CSS; only reach for that if plain gzip alone doesn't clear the budget.

This is a verification/reporting step, not a correctness test — it always
exits 0 unless --strict is passed (for wiring into CI if that's ever
wanted).

Usage:
    python3 tools/make_release.py [--minify] [--budget-mb 2] [--strict]
"""
import argparse
import gzip
import shutil
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
WEB = REPO_ROOT / "web"  # source tree
DIST = REPO_ROOT / "web-dist"  # top-level, sibling to both web/ and tools/
ESBUILD = WEB / "node_modules" / ".bin" / "esbuild"

# Plain-copy asset set (no --minify): every file that actually ships,
# mirroring the source tree structure exactly so index.html's relative
# script/link paths need no rewriting either way.
PLAIN_FILES = [
    "index.html",
    "css/style.css",
]
# No vendored libraries. The GATT explorer needs no charting, and everything
# else is hand-written, so there is nothing here that a CDN or an npm install
# would have to supply.
JS_DIR = "js"  # copied recursively (app.js plus everything it imports)


def gzip_file(path: Path) -> Path:
    gz_path = path.with_name(path.name + ".gz")
    with open(path, "rb") as f_in:
        data = f_in.read()
    # mtime=0 for reproducible output — otherwise every rebuild changes the
    # .gz file's bytes (and thus its own git diff) even when the content is
    # identical.
    with gzip.GzipFile(filename="", mode="wb", fileobj=open(gz_path, "wb"), mtime=0, compresslevel=9) as f_out:
        f_out.write(data)
    return gz_path


def human(n: int) -> str:
    if n < 1024:
        return f"{n} B"
    if n < 1024 * 1024:
        return f"{n / 1024:.1f} KB"
    return f"{n / (1024 * 1024):.2f} MB"


def build_plain() -> None:
    for rel in PLAIN_FILES:
        dst = DIST / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(WEB / rel, dst)
    shutil.copytree(WEB / JS_DIR, DIST / JS_DIR)


def build_minified() -> None:
    if not ESBUILD.exists():
        print(f"error: {ESBUILD} not found — run `npm install` first", file=sys.stderr)
        sys.exit(1)
    for rel in PLAIN_FILES:
        dst = DIST / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        if rel.endswith(".css") and not rel.startswith("vendor/"):
            subprocess.run(
                [str(ESBUILD), str(WEB / rel), "--minify", f"--outfile={dst}"], check=True, cwd=WEB
            )
        else:
            # vendor/ is already minified upstream — copied, not re-run
            # through esbuild, so a vendor update can't silently get
            # "improved" by a different minifier pass than its own.
            shutil.copyfile(WEB / rel, dst)
    # Bundles every local ./modals/*.js etc. import into one file — the dev
    # tree's other js/*.js files have no equivalent in a minified dist/.
    dst = DIST / JS_DIR / "app.js"
    dst.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(
        [str(ESBUILD), str(WEB / JS_DIR / "app.js"), "--bundle", "--minify", "--format=esm", f"--outfile={dst}"],
        check=True,
        cwd=WEB,
    )


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--minify", action="store_true", help="bundle+minify JS, minify CSS (via esbuild)")
    ap.add_argument("--budget-mb", type=float, default=2.0, help="flagged total (default 2 MB)")
    ap.add_argument("--strict", action="store_true", help="exit 1 if the budget is exceeded")
    args = ap.parse_args()

    if DIST.exists():
        shutil.rmtree(DIST)
    DIST.mkdir(parents=True)

    if args.minify:
        build_minified()
    else:
        build_plain()

    rows = []
    total_raw = 0
    total_gz = 0
    for path in sorted(DIST.rglob("*")):
        if not path.is_file():
            continue
        gz = gzip_file(path)
        raw_size = path.stat().st_size
        gz_size = gz.stat().st_size
        total_raw += raw_size
        total_gz += gz_size
        rows.append((str(path.relative_to(DIST)), raw_size, gz_size))

    name_w = max(len(r[0]) for r in rows) + 2
    label = "minified" if args.minify else "raw"
    print(f"{'file'.ljust(name_w)}{label.rjust(10)}{'gzipped'.rjust(10)}")
    for name, raw_size, gz_size in rows:
        print(f"{name.ljust(name_w)}{human(raw_size).rjust(10)}{human(gz_size).rjust(10)}")
    print("-" * (name_w + 20))
    print(f"{'total'.ljust(name_w)}{human(total_raw).rjust(10)}{human(total_gz).rjust(10)}")

    budget_bytes = int(args.budget_mb * 1024 * 1024)
    over = total_gz > budget_bytes
    print()
    if over:
        print(f"OVER BUDGET: {human(total_gz)} gzipped > {args.budget_mb:g} MB target" + ("" if args.minify else " — try --minify"))
    else:
        headroom = budget_bytes - total_gz
        print(f"OK: {human(total_gz)} gzipped, {human(headroom)} under the {args.budget_mb:g} MB target")

    print(f"\nOutput: {DIST}/ (each file plus its .gz alongside)")
    return 1 if (over and args.strict) else 0


if __name__ == "__main__":
    sys.exit(main())
