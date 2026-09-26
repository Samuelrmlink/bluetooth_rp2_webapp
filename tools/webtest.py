#!/usr/bin/env python3
"""Render the webapp in a real browser and check that it actually works.

Why this exists: the webapp shipped with every unit test green while the page
opened to an empty workspace in which no pane could be selected. Model tests
cannot catch that. The two bugs were a wrong argument shape at the single call
site that builds the layout, and a render method nobody ever called -- both
invisible until a browser runs the real page.

Stdlib plus chromium, matching the rest of the project's tooling: no npm, no
pip, nothing to install. Skips cleanly when chromium is absent so a machine
without it still gets the other tiers.

    python3 tools/webtest.py            # test web/
    python3 tools/webtest.py --keep     # leave the temp tree for inspection
"""
from __future__ import annotations

import argparse
import http.server
import json
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
WEB = ROOT / "web"

CHROME_NAMES = ["chromium", "chromium-browser", "google-chrome-stable", "google-chrome"]

# Injected ahead of the app. Collects load-time errors that would otherwise be
# invisible to a DOM dump, then hands them to the assertions.
PROBE = """<script>
window.__err = [];
addEventListener('error', e => __err.push((e.message || String(e)) + ' @' + (e.filename||'') + ':' + (e.lineno||0)));
addEventListener('unhandledrejection', e => __err.push('unhandled rejection: ' + ((e.reason && e.reason.stack) || e.reason)));
</script>
"""

RUNNER = """<script type="module">
import { run } from './tests/dom/smoke.js';
// After the app has had a turn: its modules are deferred, and the socket
// attempt must be allowed to fail before we judge the page.
setTimeout(() => {
  let results;
  try { results = run({ errors: window.__err }); }
  catch (e) { results = [{ name: 'smoke suite itself threw', pass: false, detail: String(e && e.stack || e) }]; }
  const pre = document.createElement('pre');
  pre.id = '__result';
  pre.textContent = JSON.stringify(results);
  document.body.appendChild(pre);
}, 1200);
</script>
"""


def find_chrome() -> str | None:
    for n in CHROME_NAMES:
        p = shutil.which(n)
        if p:
            return p
    return None


def free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def wanted_ids(web: Path) -> list[str]:
    """Every id the app's JS looks up, so a missing one is named rather than
    showing up as a mystery TypeError during a pane's mount."""
    ids: set[str] = set()
    for js in (web / "js").rglob("*.js"):
        ids |= set(re.findall(r"\$\('([^']+)'\)", js.read_text()))
        ids |= set(re.findall(r"getElementById\('([^']+)'\)", js.read_text()))
    return sorted(ids)


def build_tree(web: Path, dst: Path) -> None:
    shutil.copytree(web, dst, dirs_exist_ok=True)
    html = (dst / "index.html").read_text()
    ids = json.dumps(wanted_ids(web))
    head = PROBE + f"<script>window.__wantedIds = {ids};</script>\n"
    html = html.replace("<link rel=\"stylesheet\"", head + "<link rel=\"stylesheet\"", 1)
    html = html.replace("</body>", RUNNER + "</body>", 1)
    (dst / "index.html").write_text(html)


def serve(directory: Path, port: int) -> http.server.ThreadingHTTPServer:
    class Quiet(http.server.SimpleHTTPRequestHandler):
        def __init__(self, *a, **k):
            super().__init__(*a, directory=str(directory), **k)
        # The request log would drown the test output.
        def log_message(self, *a, **k):
            pass

    httpd = http.server.ThreadingHTTPServer(("127.0.0.1", port), Quiet)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    return httpd


def render(chrome: str, url: str, width=1400, height=900, budget=9000) -> str:
    with tempfile.TemporaryDirectory() as profile:
        out = subprocess.run(
            [chrome, "--headless", "--disable-gpu", "--no-sandbox",
             f"--user-data-dir={profile}",
             f"--window-size={width},{height}",
             f"--virtual-time-budget={budget}", "--dump-dom", url],
            capture_output=True, text=True, timeout=120,
        )
        return out.stdout


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--keep", action="store_true", help="leave the temp tree in place")
    ap.add_argument("--width", type=int, default=1400)
    ap.add_argument("--height", type=int, default=900)
    a = ap.parse_args()

    chrome = find_chrome()
    if not chrome:
        print("SKIP: no chromium on PATH; webapp DOM tests need a real browser")
        return 0

    tmp = Path(tempfile.mkdtemp(prefix="btgw-webtest-"))
    try:
        build_tree(WEB, tmp)
        port = free_port()
        serve(tmp, port)
        dom = render(chrome, f"http://127.0.0.1:{port}/index.html", a.width, a.height)

        m = re.search(r'id="__result">(.*?)</pre>', dom, re.S)
        if not m:
            print("FAIL: the page never produced a result -- it probably died at load")
            # The most useful thing to print is whatever did render.
            print(dom[:800])
            return 1
        import html as _html
        results = json.loads(_html.unescape(m.group(1)))

        bad = 0
        for r in results:
            mark = "OK  " if r["pass"] else "FAIL"
            detail = f"  ({r['detail']})" if r["detail"] else ""
            print(f"{mark} {r['name']}{detail}")
            if not r["pass"]:
                bad += 1
        print(f"\n{len(results) - bad}/{len(results)} DOM checks passed")
        return 1 if bad else 0
    finally:
        if a.keep:
            print(f"temp tree kept at {tmp}")
        else:
            shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
