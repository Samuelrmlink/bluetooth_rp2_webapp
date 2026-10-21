#!/usr/bin/env python3
"""PEX on a gateway, over its HTTP API: upload the test modules, run the
pex.* ops and a Lua launcher/require check, check the refusal cases, print a
pass/fail table, and clean up after itself.

  pex/tools/pex_remote_test.py --base http://10.255.255.23 --files DIR
        [--sd] [--http-url URL --http-file-url URL] [--bt [UUID]] [--keep]

DIR is made by pex/test/remote/mkremote.py (the modules signed with the OTA
key). What it does to the board: PUTs files under lfs:/pex/, lfs:/pextest_bad/
and lfs:/lua/pex_lua_test.lua, loads/starts/unloads modules, starts and stops
one Lua script, and deletes all of it at the end (unless --keep). It never
loads a module that faults or hangs (those exist only in the simulator), never
writes cfg/pex_autostart.json, never reboots, OTAs or touches SMP.

  --sd        also the card part of the fs test (writes and removes sd:/pextest.txt)
  --http-url  a URL the gateway can fetch (net group test; skipped without it)
  --bt        scan for a device advertising UUID (default: the iTi OTAP service),
              connect, read, subscribe, write one ignored byte, disconnect
Exit status 0 when everything run passed.
"""
import argparse, json, os, sys, time, urllib.error, urllib.request

PASS, FAIL, SKIP = "PASS", "FAIL", "skip"
OTAP = "49646561-7346-6c6f-6174-40882af10000"


class GW:
    def __init__(self, base, timeout=30):
        self.base = base.rstrip("/")
        self.timeout = timeout

    def _req(self, method, path, data=None, ctype=None):
        req = urllib.request.Request(self.base + path, data=data, method=method)
        if ctype:
            req.add_header("Content-Type", ctype)
        try:
            with urllib.request.urlopen(req, timeout=self.timeout) as r:
                return r.status, r.read()
        except urllib.error.HTTPError as e:
            return e.code, e.read()

    def op(self, name, params=None):
        st, body = self._req("POST", "/api/op", json.dumps({"op": name, "params": params or {}}).encode(),
                             "application/json")
        try:
            j = json.loads(body)
        except Exception:
            return False, {"code": f"http {st}", "message": body[:200].decode(errors="replace")}
        if j.get("ok"):
            return True, j.get("result")
        return False, j.get("error") or j

    def put(self, path, data):
        st, body = self._req("PUT", "/api/fs/" + path, data)
        return st == 200, body

    def delete(self, path):
        return self._req("DELETE", "/api/fs/" + path)[0]

    def get(self, path):
        return self._req("GET", "/api/fs/" + path)


class Run:
    def __init__(self):
        self.rows = []

    def add(self, name, ok, detail=""):
        st = SKIP if ok is None else (PASS if ok else FAIL)
        self.rows.append((name, st, str(detail)[:110]))
        print(f"  {st:4s} {name:34s} {str(detail)[:110]}", flush=True)
        return ok


def err_text(e):
    if isinstance(e, dict):
        return f"{e.get('code')}: {e.get('message')}"
    return str(e)


def wait_info(gw, name, pred, secs):
    t0 = time.time()
    info = None
    while time.time() - t0 < secs:
        ok, info = gw.op("pex.info", {"name": name})
        if ok and pred(info):
            return info
        time.sleep(0.25)
    return info


def wait_op(gw, op, pred, secs):
    t0 = time.time()
    r = None
    while time.time() - t0 < secs:
        ok, r = gw.op(op)
        if ok and pred(r):
            return r
        time.sleep(0.5)
    return r


def main():
    ap = argparse.ArgumentParser(description="PEX remote test", formatter_class=argparse.RawDescriptionHelpFormatter,
                                 epilog=__doc__)
    ap.add_argument("--base", required=True)
    ap.add_argument("--files", required=True)
    ap.add_argument("--sd", action="store_true")
    ap.add_argument("--http-url")
    ap.add_argument("--http-file-url")
    ap.add_argument("--bt", nargs="?", const=OTAP)
    ap.add_argument("--keep", action="store_true")
    a = ap.parse_args()
    gw = GW(a.base)
    R = Run()
    uploaded = []

    print(f"PEX remote test against {gw.base}")
    ok, lst = gw.op("pex.list")
    if not R.add("pex.list", ok and "modules" in lst, err_text(lst) if not ok else
                 f"api {lst.get('api')} {lst.get('arch')} policy={lst.get('policy')} c_heap_free={lst.get('c_heap_free')}"):
        return 2
    arch = lst.get("arch", "m33")
    leftovers = [m["name"] for m in lst.get("modules", [])]
    if leftovers:
        print(f"  note: modules already loaded: {leftovers} (left alone)")
    ok, info = gw.op("sys.info")
    if ok:
        R.add("sys.info has fs", isinstance(info.get("fs"), dict) and "pex" in info["fs"], json.dumps(info.get("fs"))[:100])

    # ---- upload
    t0 = time.time()
    nbytes = 0
    for sub, dst in (("pex", "pex"), ("pex/a", "pex/a"), ("bad", "pextest_bad"), ("lua", "lua")):
        d = os.path.join(a.files, sub)
        for f in sorted(os.listdir(d)):
            p = os.path.join(d, f)
            if not os.path.isfile(p):
                continue
            data = open(p, "rb").read()
            okp, body = gw.put(f"{dst}/{f}", data)
            if not okp:
                R.add(f"upload {dst}/{f}", False, body[:100])
                return 2
            uploaded.append(f"{dst}/{f}")
            nbytes += len(data)
    R.add("upload", True, f"{len(uploaded)} files, {nbytes} bytes, {time.time() - t0:.1f} s")
    ok, files = gw.op("pex.files")
    R.add("pex.files", ok and any(x.get("name") == f"hello.{arch}.pex" for x in files.get("files", [])),
          err_text(files) if not ok else f"{len(files.get('files', []))} entries")

    try:
        # ---- hello: load, info, an export, unload
        ok, r = gw.op("pex.load", {"name": "hello"})
        R.add("hello.load (signed, model B)", ok and r.get("state") == "loaded", err_text(r) if not ok else r.get("build_id"))
        if ok:
            ok2, c = gw.op("pex.call", {"name": "hello", "fn": "hello_answer"})
            R.add("hello.call export", ok2 and c.get("result") == 42, c)
            ok2, i = gw.op("pex.info", {"name": "hello"})
            R.add("hello.info", ok2 and "hello_answer" in i.get("exports", []) and len(i.get("build_id", "")) == 32,
                  err_text(i) if not ok2 else f"code {i.get('code')} B rw {i.get('rw')} B")
            ok2, u = gw.op("pex.unload", {"name": "hello"})
            R.add("hello.unload", ok2, err_text(u) if not ok2 else "")

        # ---- both code models
        for model, path, nm in (("B", "pex/arith." + arch + ".pex", "arith"), ("A", "pex/a/arith." + arch + ".pex", "arith_a")):
            ok, r = gw.op("pex.load", {"path": path, "as": nm})
            if not R.add(f"arith.load model {model}", ok, err_text(r) if not ok else r.get("model")):
                continue
            ok2, c = gw.op("pex.call", {"name": nm, "fn": "arith_selftest"})
            R.add(f"arith.selftest model {model}", ok2 and c.get("result") == 0, c)
            ok2, c = gw.op("pex.call", {"name": nm, "fn": "arith_sum6", "args": [1, 2, 3, 4]})
            R.add(f"arith.4 args model {model}", ok2 and c.get("result") == 30, c)
            gw.op("pex.unload", {"name": nm})

        # ---- two instances, one code copy (model B)
        ok1, r1 = gw.op("pex.load", {"name": "counter"})
        ok2, r2 = gw.op("pex.load", {"name": "counter", "as": "counter2"})
        if R.add("counter.two instances", ok1 and ok2, err_text(r2 if ok1 else r1)):
            gw.op("pex.call", {"name": "counter", "fn": "counter_add", "args": [5]})
            _, c2 = gw.op("pex.call", {"name": "counter2", "fn": "counter_add", "args": [7]})
            _, c1 = gw.op("pex.call", {"name": "counter", "fn": "counter_add", "args": [1]})
            _, i1 = gw.op("pex.info", {"name": "counter"})
            _, i2 = gw.op("pex.info", {"name": "counter2"})
            R.add("counter.separate RW", c1.get("result") == 6 and c2.get("result") == 7 and i1["rw_addr"] != i2["rw_addr"],
                  f"{c1} {c2}")
            R.add("counter.shared code", i1["code_addr"] == i2["code_addr"] and i1.get("shared"), f"code @{i1['code_addr']:#x}")
        gw.op("pex.unload", {"name": "counter"})
        gw.op("pex.unload", {"name": "counter2"})

        # ---- host imports incl. the board group (present on the gateway)
        ok, r = gw.op("pex.load", {"name": "imports"})
        if ok:
            _, c = gw.op("pex.call", {"name": "imports", "fn": "imports_run"})
            R.add("imports.host calls + weak board", c.get("result") == 0xEF, f"mask {c.get('result', 0):#x} want 0xef")
            gw.op("pex.unload", {"name": "imports"})
        else:
            R.add("imports.load", False, err_text(r))
        ok, r = gw.op("pex.load", {"name": "needboard"})
        R.add("needboard.strong import loads", ok, err_text(r) if not ok else "")
        gw.op("pex.unload", {"name": "needboard"})

        # ---- an app on the main loop
        ok, r = gw.op("pex.load", {"name": "app"})
        if R.add("app.load", ok, err_text(r) if not ok else ""):
            ok, r = gw.op("pex.start", {"name": "app", "cfg": {"ticks": 5, "period": 50}})
            R.add("app.start", ok, err_text(r) if not ok else "")
            ok, r = gw.op("pex.unload", {"name": "app"})
            R.add("app.unload refused while running", not ok and "running" in err_text(r), err_text(r))
            i = wait_info(gw, "app", lambda x: x.get("state") == "exited", 10)
            _, c = gw.op("pex.call", {"name": "app", "fn": "app_ticks"})
            R.add("app.ran 5 ticks", i and i.get("state") == "exited" and c.get("result") == 5, f"{i and i.get('state')} {c}")
            gw.op("pex.start", {"name": "app", "cfg": "{\"ticks\":100000,\"period\":20}"})
            time.sleep(0.5)
            ok, r = gw.op("pex.stop", {"name": "app"})
            _, i = gw.op("pex.info", {"name": "app"})
            R.add("app.stop", ok and i.get("state") == "stopped" and i.get("tasks") == 0, i.get("state"))
            ok, r = gw.op("pex.unload", {"name": "app"})
            R.add("app.unload", ok, err_text(r) if not ok else "")

        # ---- memory: run, leak on purpose, unload reclaims it
        _, lst = gw.op("pex.list")
        base_free = (lst or {}).get("pool", {}).get("free")
        ok, r = gw.op("pex.load", {"name": "memtest"})
        if ok:
            _, c = gw.op("pex.call", {"name": "memtest", "fn": "mem_run"})
            R.add("mem.run", c.get("result") == 0, c)
            _, c = gw.op("pex.call", {"name": "memtest", "fn": "mem_leak"})
            _, i = gw.op("pex.info", {"name": "memtest"})
            R.add("mem.leak made", c.get("result") == 0 and i.get("blocks", 0) >= 5, f"{i.get('blocks')} blocks")
            gw.op("pex.unload", {"name": "memtest"})
            _, lst = gw.op("pex.list")
            R.add("mem.leak-free unload", lst["pool"]["free"] == base_free, f"pool free {lst['pool']['free']} (before {base_free})")
        else:
            R.add("mem.load", False, err_text(r))

        # ---- refusals
        for f, want, label in (("hello_unsigned", "unsigned", "unsigned refused"), ("hello_badsig", "signature", "bad signature refused"),
                               ("hello_badcrc", "CRC", "corrupted refused (CRC)")):
            ok, r = gw.op("pex.load", {"path": f"pextest_bad/{f}.{arch}.pex", "as": "bad"})
            R.add(label, not ok and want.lower() in err_text(r).lower(), err_text(r))
            if ok:
                gw.op("pex.unload", {"name": "bad"})
        other = "m0p" if arch == "m33" else "m33"
        ok, r = gw.op("pex.load", {"path": f"pextest_bad/hello.{other}.pex", "as": "bad"})
        R.add("wrong architecture refused", not ok and "architecture" in err_text(r), err_text(r))
        ok, r = gw.op("pex.load", {"path": "sd:/pex/hello.m33.pex"})
        R.add("sd:/ load refused (next version)", not ok, err_text(r))
        ok, r = gw.op("pex.unload", {"name": "nothing_loaded"})
        R.add("unload of an unknown module", not ok and r.get("code") == "not_loaded", err_text(r))

        # ---- bus + json + sys: a module serving ops
        ok, r = gw.op("pex.load", {"name": "gwbus"})
        if R.add("bus.load (registers ops)", ok, err_text(r) if not ok else ""):
            ok, e = gw.op("pextest.echo", {"a": [1, 2], "s": "x"})
            R.add("bus.op echo", ok and e.get("echo") == {"a": [1, 2], "s": "x"} and e.get("calls") == 1, e)
            ok, s = gw.op("pextest.sys")
            R.add("bus.op sys (sys group)", ok and s.get("host") and s.get("version") and s.get("pex_size", 0) > 0, s)
            ok, s = gw.op("pextest.emit", {"n": 3})
            R.add("bus.emit event", ok and s.get("emitted"), s)
            ok, s = gw.op("pextest.fail")
            R.add("bus.op error reply", not ok and "deliberate" in err_text(s), err_text(s))
            ok, s = gw.op("pex.ops")
            R.add("pex.ops lists them", ok and any(o["op"] == "pextest.echo" for o in s), s)
            gw.op("pex.unload", {"name": "gwbus"})
            ok, s = gw.op("pextest.echo")
            R.add("bus.ops gone after unload", not ok, err_text(s))

        # ---- fs
        ok, r = gw.op("pex.load", {"name": "gwfs"})
        if R.add("fs.load", ok, err_text(r) if not ok else ""):
            ok, r = gw.op("pex.start", {"name": "gwfs", "cfg": {"sd": bool(a.sd)}})
            res = wait_op(gw, "pextest.fs", lambda x: x["lfs"] != "pending" and x["sd"] != "pending", 30)
            R.add("fs.lfs (open/write/read/seek/stat/list/rename/remove/mkdir)", res and res["lfs"] == "ok", res)
            R.add("fs.sd (callbacks)", None if not a.sd else (res and res["sd"] == "ok"), res and res["sd"])
            gw.op("pex.unload", {"name": "gwfs"})

        # ---- net
        if a.http_url:
            ok, r = gw.op("pex.load", {"name": "gwnet"})
            if R.add("net.load", ok, err_text(r) if not ok else ""):
                cfg = {"url": a.http_url, "file": "lfs:/pextest_dl.bin"}
                if a.http_file_url:
                    cfg["file_url"] = a.http_file_url
                ok, r = gw.op("pex.start", {"name": "gwnet", "cfg": cfg})
                res = wait_op(gw, "pextest.net", lambda x: x["buffer"] != "pending" and
                              (x["file"] != "pending" or not a.http_file_url), 60)
                R.add("net.request into a buffer", res and res["buffer"].startswith("status=200 err=0"), res and res["buffer"])
                R.add("net.request into a file", None if not a.http_file_url else
                      (res and res["file"].startswith("status=200 err=0") and "file=0" in res["file"]), res and res["file"])
                gw.op("pex.stop", {"name": "gwnet"})
                gw.op("pex.unload", {"name": "gwnet"})
                gw.delete("pextest_dl.bin")
        else:
            R.add("net (needs --http-url)", None)

        # ---- bt
        if a.bt:
            ok, r = gw.op("pex.load", {"name": "gwbt"})
            if R.add("bt.load", ok, err_text(r) if not ok else ""):
                ok, r = gw.op("pex.start", {"name": "gwbt", "cfg": {"uuid": a.bt, "timeout": 40000}})
                res = wait_op(gw, "pextest.bt", lambda x: x["result"] != "pending", 60)
                R.add("bt.scan/connect/read/indicate/write", res and res["result"].startswith("ok"), res and res["result"])
                gw.op("pex.stop", {"name": "gwbt"})
                gw.op("pex.unload", {"name": "gwbt"})
        else:
            R.add("bt (needs --bt)", None)

        # ---- Lua: launcher + require
        gw.delete("pex_lua_result.json")
        ok, r = gw.op("script.start", {"name": "pex_lua_test"})
        if R.add("lua.script.start", ok, err_text(r) if not ok else ""):
            res = None
            t0 = time.time()
            while time.time() - t0 < 60:
                st, body = gw.get("pex_lua_result.json")
                if st == 200:
                    try:
                        res = json.loads(body)
                        break
                    except Exception:
                        pass
                time.sleep(1)
            for x in res or []:
                if x["name"] != "done":
                    R.add("lua." + x["name"], x["ok"], x.get("extra") or "")
            R.add("lua.finished", bool(res) and res[-1]["name"] == "done", "" if res else "no result file (script error?)")
            gw.op("script.stop", {"name": "pex_lua_test"})
    finally:
        # ---- clean up: our modules, our files
        ours = {"hello", "arith", "arith_a", "counter", "counter2", "imports", "needboard", "app", "memtest", "bad",
                "gwbus", "gwfs", "gwnet", "gwbt"}
        _, lst = gw.op("pex.list")
        for m in (lst or {}).get("modules", []):
            if m["name"] in ours:
                gw.op("pex.stop", {"name": m["name"]})
                gw.op("pex.unload", {"name": m["name"]})
        _, lst = gw.op("pex.list")
        left = [m["name"] for m in (lst or {}).get("modules", []) if m["name"] in ours]
        R.add("cleanup: none of ours loaded", not left, left)
        if not a.keep:
            for p in uploaded + ["pex_lua_result.json"]:
                gw.delete(p)
            gw.delete("pex/a")
            gw.delete("pextest_bad")
        R.add("pool after the run", True, json.dumps((lst or {}).get("pool")))

    n = {PASS: 0, FAIL: 0, SKIP: 0}
    for _, st, _ in R.rows:
        n[st] += 1
    print()
    print(f"{'test':36s} result")
    for name, st, det in R.rows:
        print(f"{name:36s} {st}")
    print(f"\n{n[PASS]} passed, {n[FAIL]} failed, {n[SKIP]} skipped")
    return 1 if n[FAIL] else 0


if __name__ == "__main__":
    sys.exit(main())
