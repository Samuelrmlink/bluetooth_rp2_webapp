# Lua: files, HTTP(S), JSON, ops over HTTP, autostart

Generic additions to the Lua runtime for scripts that work on their own: read
and write files on LittleFS and the SD card, fetch over HTTP and HTTPS, decode
JSON, be driven by plain HTTP, start at boot. Nothing here knows any device,
protocol or service.

**Conventions.** A call that waits *yields the calling script* (like
`bt.await`), so it may only be called from a script's own code -- not from an
`ops.register` handler, a `bt.on_pairing` handler or `script.eval`; there it
raises an error saying so. Failures return `nil, "<code>"` and never raise,
unless an argument has the wrong type.

| Module | Waits (yields) |
|---|---|
| `file` on `sd:/` | every call except `h:tell`, `h:size`, `file.media` |
| `file` on `lfs:/` | only `h:write` of more than 4 KiB (written in 4 KiB pieces, a yield between) |
| `http` | `http.await` |
| `json`, `sys.time`, `sys.hostname`, `bt.mtu` | never |

## 1. `file`

Paths carry their volume:

* `lfs:/a/b.ext` -- LittleFS on flash, the tree `/api/fs/` serves. Characters
  `[A-Za-z0-9._-]`, no segment starting with `.` (the same rule as the HTTP API).
* `sd:/a/b.ext` -- the SD card, the tree `/api/sd/files/` serves (SD builds
  only). Any FAT long-name character: not `\ : * ? " < > |` or control characters.

On both: a segment is at most 64 bytes, the path at most 128; `.` and `..`
segments, empty segments (`a//b`) and NUL are refused (`badpath`). The slash
after the colon is optional and a trailing slash is dropped (`sd:logs/` is
`sd:/logs`); `lfs:/` and `sd:/` are the roots.

| Call | Returns |
|---|---|
| `file.open(path, mode)` | a handle, or `nil, err`. `mode` `"r"` (must exist), `"w"` (create/truncate), `"a"` (create/append); parent folders are made for `"w"`/`"a"` |
| `h:read(n)` | up to `n` bytes (default and maximum 4096) as a string, `""` at the end |
| `h:write(s)` | `true` |
| `h:seek(pos)` | the new position (absolute) |
| `h:tell()`, `h:size()` | the position, the size |
| `h:close()` | `true`; closing twice is fine |
| `file.stat(path)` | `{size=n, dir=bool}`, or `nil, "enoent"` |
| `file.list(dir)` | `{ {name=, dir=, size=}, ... }`, one level |
| `file.mkdir(path)` | `true`; makes parents; an existing folder is success (`eexist` if a *file* is in the way) |
| `file.remove(path)` | `true`; a file or an empty folder |
| `file.rename(from, to)` | `true`; `eexist` if `to` exists; makes `to`'s parents; not across volumes (`badpath`) |
| `file.media()` | `{lfs={ok=, free=, total=}, sd={state=, detail=, free=, total=, fs=}}` |

`sd.state` is `"ok"`, `"none"` (no card, or one still settling after
insertion), `"error"` (unmountable, or being formatted) or `"absent"` (a build
without SD support); `sd.detail` is the card state `/api/sd` reports (`ok`,
`none`, `settling`, `error`, `formatting`).

Errors: `enoent`, `eexist`, `enospc`, `eio`, `nomedia`, `busy`, `badpath`,
`toolarge`, `nohandle`, and `notempty` (removing a folder with something in it).

Limits and rules:

* **Handles:** 4 on LittleFS and 4 on the SD card, at the same time. A handle
  belongs to the script that opened it and closes when that script stops, on
  `h:close()`, at the end of a `local h <close> = ...` scope, and when collected.
* **1 MiB** per LittleFS file (`FS_FILE_MAX`): a write that would go past it
  fails with `toolarge` and writes nothing. The same limit now applies to
  `PUT /api/fs/<path>` (was 128 KiB) and SMP file upload. SD files are FAT's.
* **busy:** a LittleFS file one handle is writing cannot be opened again, and one
  that is open anywhere (a script, an upload, a download) cannot be opened for
  writing, removed or renamed. LittleFS keeps two handles on one file coherent
  only in narrow cases; refusing is the safe answer. An SD card being formatted
  or still settling also answers `busy`.
* **Flash writes stall the loop**: a LittleFS write may erase a 4 KiB sector,
  ~50-400 ms on this part, during which USB, the network and Bluetooth wait.
  Write in pieces of a few KiB (a large `h:write` is split for you).

How the SD side works: only the storage worker ever touches the card
(`src/sd/sd_jobs.c`). A call posts a *job*, the script yields, the worker runs
the job on its next turn, the script resumes with the result -- one main-loop
pass or a few. One job runs at a time; its data buffer is borrowed from an idle
`/api/sd` transfer ring, so while a script's read or write is in flight only one
browser transfer can run beside it. A script that stops mid-job lets the job
finish on its own (a file it was opening is closed). After the card is pulled,
an old handle answers `eio` rather than reaching a file that reused its slot.

## 2. `http`

```lua
local id, err = http.start{
  method = "GET",                 -- upper-case; default GET
  url = "https://host/path?q",    -- http:// or https://, optional :port; IPv4 / names
  headers = { Accept = "application/json" },
  body = "...",                   -- optional request body
  to_file = "sd:/x/y.tmp",        -- optional: stream a 2xx body into this file
  max_body = 16384,               -- in-memory body cap (default 16 KiB, at most 64 KiB)
  timeout_ms = 30000,             -- the whole request, redirects included
  follow = 3,                     -- redirects to follow (at most 10)
  insecure = false,               -- https: do not verify the certificate
}
http.status(id)    -- {state="dns"|"connect"|"tls"|"send"|"recv"|"done"|"error", status=n, bytes=n, total=n|nil}
http.await(id, ms) -- the result, or nil, "timeout" (the request keeps going) | nil, "badid"
http.cancel(id)    -- true (false: no such request)
http.stats()       -- {requests=, failures=, tls_sessions=, tls_bytes=, tls_peak_bytes=, tls_reserved=}
http.reserve(on)   -- the TLS record-buffer reservation (below): false gives it back
                   -- (false while a session uses it), true takes it now
```

`http.start` fails with `nil, "busy"` (3 requests already), `"badurl"`,
`"badpath"` (`to_file`), `"nomedia"` (`sd:/` on a build without SD), `"nomem"`,
or `"tls"` (an https URL on a build without TLS).

Result: `{status=200, headers={["content-type"]=...}, body="...", truncated=bool,
bytes=n}`. Header names are lower-case; a repeated header's values are joined
with `", "`. `body` is absent when the body went to `to_file`. `bytes` counts the
body bytes received. On failure: `{error=, message=[, status=]}` with `error`
one of `dns`, `connect`, `tls`, `timeout`, `toolarge`, `io`, `cancelled`, `nomem`.

* **await:** `ms` defaults to and is capped at 30 000 (the runtime's longest
  wait); `ms <= 0` only looks. Await again for a longer request -- `timeout`
  does not stop it. The first await that returns the result frees the request;
  a later one answers `badid`. A finished request nobody collects is dropped
  60 s after it finished, if its slot is wanted.
* **Bodies:** Content-Length, chunked (extensions, trailers) and read-until-close.
  `Accept-Encoding: identity` is sent and a compressed transfer coding is
  refused. Past `max_body` the transfer is stopped and the result has
  `truncated = true`.
* **to_file:** only a 2xx response is written; any other's body comes back in
  memory like a normal result, so an error page never replaces the target. A
  failed or cancelled download removes the partial file (on SD: a *cancelled*
  one stays, as cancel cannot wait for the card). LittleFS files obey the 1 MiB
  limit (`toolarge`). The download is held back by TCP flow control while the
  card or flash writes, so it never buffers more than ~4 KiB. On the card the
  body is written straight into the storage job's buffer (a borrowed `/api/sd`
  transfer ring), so a download needs no 4 KiB block of the arena; on LittleFS
  at most one flash sector (4 KiB) is written per main-loop pass, so a long
  download never holds the loop for more than one erase.
* **Redirects** 301/302/303/307/308: `Location` may be absolute, scheme-relative,
  absolute-path or relative. 303, and 301/302 after anything but GET/HEAD,
  continue as a GET without the body (what browsers do). https -> http is
  refused. `Authorization` and `Cookie` are not sent to a different host.
* **One connection per request** (`Connection: close`), over whatever interface
  routes to the server -- the active uplink (Wi-Fi, Ethernet, USB) via
  `uplink.c`. IPv4 only.
* **DNS:** the servers DHCP gave (Wi-Fi, Ethernet). Where DHCP named none (USB:
  the gateway *is* the DHCP server there), `cfg/dns.json` `{"server":"a.b.c.d"}`,
  else the default route's gateway.

### HTTPS

mbedTLS 3.6 cut to a TLS 1.2 client (`include/mbedtls_config.h`): ECDHE-ECDSA
(offered first) and ECDHE-RSA with AES-128/256-GCM; P-256 and P-384; RSA (to
4096 bits) and ECDSA certificates. TLS 1.3 is not built (about 25 KB more
flash); every server that matters here speaks 1.2.

**The handshake and the main loop.** Elliptic-curve arithmetic is slow on this
part, and a TLS call runs inside the cooperative loop. mbedTLS's *restartable*
ECC is on (`mbedtls_ecp_set_max_ops`), so with an ECDHE-ECDSA suite the key
generation, ECDH and ECDSA verification run in bounded steps across loop
passes. Measured in mcusim: without it one handshake call held the loop ~1 s;
with it no pass exceeded the 250 ms stall warning. mbedTLS does not restart the
ECDHE-RSA flow, so a server with an RSA certificate still costs one ~0.5 s
pass (USB, Wi-Fi and Bluetooth wait through it). X25519 is not offered at all:
its Montgomery ladder cannot be restarted and servers prefer it. Google's
front ends hold ECDSA certificates and pick ECDHE-ECDSA when it is offered first.

The server is verified -- chain, signature, host name (SNI is sent) -- against
the roots in `third_party/ca_roots/` (generated into `src/net/ca_roots.h` by
`tools/gen_ca_roots.py`, which pins each one's SHA-256 and is checked by
`tools/run_tests.py`):

| Root | Source | SHA-256 |
|---|---|---|
| GTS Root R1 | pki.goog/repo/certs/gtsr1.pem | `d947432a...57a7f4cf` |
| GTS Root R2 | pki.goog/repo/certs/gtsr2.pem | `8d25cd97...841c7ea8` |
| GTS Root R3 | pki.goog/repo/certs/gtsr3.pem | `34d8a73e...010a6f48` |
| GTS Root R4 | pki.goog/repo/certs/gtsr4.pem | `349dfa40...031b3c7d` |
| GlobalSign Root CA (R1) | secure.globalsign.com/cacert/root-r1.crt | `ebd41040...d4331c99` |
| ISRG Root X1 | system trust store (Mozilla) | `96bcec06...bddf08c6` |
| ISRG Root X2 | system trust store (Mozilla) | `69729b8e...3ccb1470` |

The roots are parsed (in place, from flash) only for the handshake state
that verifies the server's chain -- ~9 KB for all seven, freed the moment that
state is done -- and handed to mbedTLS as an ordinary CA chain, so the chain
verification is restartable like the key exchange. (A trusted-CA callback
would cost less, but mbedTLS verifies a callback's chain in one call, and
Google's all-ECDSA chain then held the loop ~1.35 s per handshake.) SHA-1 is
compiled in only so GlobalSign Root CA R1, self-signed with SHA-1, parses; the
verification profile still refuses SHA-1 inside a chain. Certificate validity
dates are checked once the clock is set (`sys.time()` ~= nil, SNTP); before
that they are not, rather than failing every handshake on a board that has not
heard from a time server yet. `insecure = true` skips verification entirely.

**Memory.** Every request, its buffers and its TLS session come from the *top*
of the Lua arena (`lheap_alloc_high`), so they sit above everything Lua packs
from the bottom. A TLS session needs a 16 KiB receive record buffer (servers --
Google's included -- ignore `max_fragment_length`) and a 4 KiB send buffer,
each one contiguous block, and a script that has run for a while leaves the
first-fit arena in pieces (measured: 51 KB free, no 17 KB hole). So the first
HTTPS request **reserves one pair, 22.5 KB, and keeps it for the life of the Lua
VM** (`http.stats().tls_reserved`); a second session at the same time takes
its buffers from the arena like everything else. The reservation gives way to
scripts: `http.reserve(false)` returns it while no HTTPS is needed, and a Lua
allocation that fails even after the emergency collection frees it (if idle)
and retries. Either way the next HTTPS request reserves it again, or fails
cleanly with `nomem` if the arena cannot give 22.5 KB in two pieces. And because memory a script
has dropped is garbage until the collector runs -- which only Lua's own
allocations trigger -- an HTTP/TLS allocation that fails runs a full
collection and tries once more. A handshake that still runs out says
`error = "nomem"` with what it wanted, never a certificate error.

| | bytes |
|---|---|
| a session, after setup / steady after the handshake (4 KiB out buffer; now 2 KiB, ~2 K less) | 22.5 K / 22.7 K |
| a session at its handshake peak (RSA-4096 root, RSA-2048 intermediate, ECDSA or RSA leaf) | ~37 K |
| on the device against firebasestorage.googleapis.com (mcusim, real network), `tls_peak_bytes` | 33.0 K |
| reserved for the record buffers after the first HTTPS request, kept | 22.5 K |
| an in-memory body | up to `max_body` (2 KiB chunks) |
| response headers kept | at most 8 KiB |

(measured with the firmware's mbedTLS configuration by `test/host/tls_mem_test.c`
on a 64-bit host, which slightly overstates the device; `http.stats()` reports
the device's own figures). Two HTTPS requests at once therefore want ~75 KB of
the 176 KB arena at their peaks. Static RAM for all of `http` is the slot table,
one shared TLS configuration and a 512-byte scratch.

## 3. `json`

`json.decode(s)` -> a value, or `nil, "<what> at byte N"`. Objects are tables
with string keys, arrays tables with `1..n`, JSON `null` is `json.null` (so it
survives in an array and as an object value). Integral numbers that fit are
integers. `\u` escapes and surrogate pairs become UTF-8. Nesting is limited to
32 levels (the parser runs on core0's 4 KB stack).

`json.encode(v)` -> a string, or `nil, "toolarge"` (over 64 KiB, or nested deeper
than 6). Tables with a length are arrays, others objects (non-string keys are
skipped); `json.null` and `nil` are `null`; an empty table is `{}`.

## 4. `POST /api/op`

Body `{"op":"<name>","params":{...}}`, at most 4 KiB, answered `200` with exactly
the WebSocket's reply: `{"id":0,"ok":true,"result":...}` or
`{"id":0,"ok":false,"error":{"code":..,"message":..}}`. Built-in and Lua-registered
ops alike. `413` for a larger body, `503` when there is no memory for it (the body
is held in the Lua arena). Like every route here it is unauthenticated.

```sh
curl -X POST http://<gateway>/api/op -d '{"op":"script.start","params":{"name":"worker"}}'
```

## 5. Autostart

`cfg/autostart.json` = `{"scripts":["name", ...]}`: started once per boot,
3 s after the runtime is up, in order; one that fails to start is logged (and a
`script.error` event with `autostart:true` sent) and the rest still start. A VM
restart does not re-run it (`script.vm_restart {resume=true}` does that job).
`script.autostart {name, enabled=true}` adds or removes a name and replies with
the file's new contents.

## 6. `bt` additions

* `bt.devices()` entries gain `uuids128` -- every 128-bit service UUID in the
  stored advertisement and scan response (complete and incomplete lists),
  canonical upper-case `8-4-4-4-12`, an empty table when there are none -- and
  `age_ms`, the milliseconds since the device's last advertising report (the
  table keeps devices long gone). `uuids128` is also in the `scan.device` and
  `scan.list` WebSocket events.
* `bt.mtu(addr)` -> the negotiated ATT MTU, or `nil` before the exchange (or
  when not connected).
* `bt.write(addr, path, hex, with_resp, want_done)`: with `want_done = true` the
  outcome arrives through `bt.await` / `bt.notify_take` as
  `{addr, id=path, source="write", ok=bool, att_error=n}` (no `value`), plus
  `error="timeout"` when the write never completed within the link's 5 s op
  deadline. With response it completes when the peer answers; without, when the
  controller takes it. Without `want_done` nothing changes.
* A write longer than `MTU-3` returns `nil, "too_long"` at once -- with or
  without response, since this stack does not do long writes. A full per-link
  queue now returns `false, "queue_full"` (it used to be a bare `false`).

## 7. `sys` additions

* `sys.time()` -> Unix seconds once SNTP has set the clock, else `nil`. The
  server is `pool.ntp.org`, or `cfg/ntp.json` `{"server":"..."}`; polled hourly.
* `sys.hostname()` -> the mDNS / DHCP host name (`mdns.hostname()`).

## Notes for whoever changes this

* **Modules and memory.** When a module's cached bytecode cannot be loaded
  because the arena is full, `require` fails with "not enough memory to load
  it" and keeps the cache -- compiling the source instead needs about twice as
  much and only failed later.
* **USB-NCM drops frames with ERR_USE.** `usb_ncm.c`'s `ncm_linkoutput`
  returns `ERR_USE` when its single transmit buffer is busy and the frame is
  gone. lwIP then leaves the TCP segment on `unsent` with no timer to retry it:
  an outgoing SYN dropped that way was a connect that never completed (found in
  mcusim, when a reply to the host went out at the same moment). `httpc_tick`
  now calls `tcp_output()` for any request with something unsent. The HTTP
  server and anything else on that interface have the same exposure; returning
  `ERR_OK` on a drop (and letting TCP's retransmission recover) is the usual
  netif convention and probably the right upstream fix.
* **Integers are 64-bit.** The firmware asks for `LUA_32BITS`, but
  `third_party/lua/luaconf.h` redefines it to 0 after `lua_pico_conf.h` has set
  it (the build warns), so Lua runs with 64-bit integers and doubles. Everything
  above works with either width; scripts relying on 32-bit wrap-around would not.
* **Flash and RAM**, Release builds, before (6cda1b8) and after this work:

  | | flash used | free in the 864 KiB slot | static RAM | C heap left |
  |---|---|---|---|---|
  | pico2w, before | 729 504 | 155 232 | 493 108 | 31 180 |
  | pico2w, after | 783 936 | 100 800 | 480 876 | 43 412 |
  | pico2w_sd, after | 862 168 | 22 568 | 500 980 | 23 308 |
  | pico2w_sdspi, after | 855 784 | 28 952 | 499 812 | 24 476 |
  | pico2w_eth, after | 800 704 | 84 032 | 481 928 | 42 360 |

  ("after" is with the 176 KB arena below; static RAM includes the arena.)

  The feature itself is ~105 KB (mbedTLS ~60 KB at `-Os`, the client ~17 KB
  with the 7 KB of roots, `file`/`json` ~12 KB, DNS + SNTP ~5 KB); two
  unrelated savings pay for half of it: newlib's `iswspace` (pulled in by
  `sscanf`, dragging 28 KB of JIS/Unicode tables) is replaced by an ASCII one
  (`src/util/libc_slim.c`), and BTstack (except micro-ecc and rijndael, which
  Lua's `crypto` uses) and FatFs are built `-Os` (~30-45 KB). Building
  micro-ecc `-Os` too would free another ~19 KB at some cost to `crypto.ecdh`.
  `pico2w_noanker`, which embeds the Wi-Fi blob, builds the client without TLS.
* **C heap.** Everything that could live in the Lua arena does (TLS sessions,
  HTTP requests and bodies, LittleFS handle storage, the `/api/op` body, SD job
  requests); the static cost is the slot tables, one TLS configuration, the DNS
  table and the extra FatFs file objects.
* **Keep the C heap above ~16 KB.** The pico-sdk `malloc` panics when it fails
  (`PICO_MALLOC_PANIC`), and the CYW43 driver, lwIP's DHCP/mDNS and BTstack
  still allocate there at boot -- the CYW43 driver alone asks for 4 KB in one
  piece. An early build of this branch had grown static RAM until the SD
  build's heap was 6.9 KB: it panicked "Out of memory" on every boot, which on
  a try-before-you-buy update reads as a revert to the old image (found by
  replaying the update in mcusim from slot B). The script loader's source
  buffer (`src_buf` in `lrt.c`, up to 32 KB) used to be a static array; it is
  now reserved from the top of the Lua arena only while a source is read and
  compiled, and the arena is 176 KB in the shipped configs (was 160 KB), which
  leaves ~23 KB of C heap on the SD build and ~43 KB on pico2w.
* **Stack.** `sys.info` reports `stack_max`, the deepest core-0 stack use since
  boot (the stack region is painted at start-up, `src/util/stackmark.c`);
  measured ~3.9 KB at boot and through a 5-minute file/json/http soak, ~4.9 KB
  after an HTTPS request with certificate verification. core0 has 8 KB
  (0x20080000-0x20082000, the crumb record at its foot, the C heap right
  below), and Lua's own limit (200 nested C calls or parser levels) assumes a
  desktop stack: a `script.eval` of 40 nested parentheses ran off the end of
  it. Lua now also checks the stack pointer (`LUAI_CSTACK_SOFT`/`_HARD` in
  `third_party/lua/lua_pico_conf.h`, checked in `ldo.c` `ccall`, the parser's
  `enterlevel` and `lstrlib.c` `match`): below 0x20080900 a nested call raises
  "C stack overflow" (patterns: "pattern too complex"), below 0x20080500 it is
  "error in error handling". The worst case measured with it (deep
  `string.gsub` / `table.sort` / `__index` / `pcall` recursion, 300 nested
  parentheses) is 6.7 KB. About 18 nested parentheses still compile in a
  `script.eval`; ordinary code is nowhere near. `script.eval` builds its reply
  in the shared API scratch instead of 3 KB of stack.

## Tests

* `test/host/json_decode_test.c` -- every JSON form, malformed input under a guard page.
* `test/host/lfile_path_test.c` -- volume/path parsing.
* `test/host/httpc_parse_test.c` -- the response parser at every split point, URLs, redirects.
* `test/host/sd_stack_test.c` (jobs section) -- the SD jobs through the real FatFs on both buses.
* `test/host/tls_mem_test.c` -- the mbedTLS configuration against a local `openssl s_server`.
* `test/host/heap_test.c` (alloc from the top) -- `lheap_alloc_high`.
* `test/host/lfile_sd_test.c` -- `lfile.c` on the SD card end to end (binding, yields, jobs, worker, FatFs).
* `test/host/lhttp_sd_test.c` -- `to_file` onto the SD card through `lhttp.c`'s state machine.
* `test/sim/lua_net_fs.py` -- the firmware in mcusim over USB-NCM: `file`, `json`,
  `http` (plain and TLS against local servers), `sys`, `POST /api/op` (48 checks);
  `--radio`: SNTP, DNS and HTTP over the Wi-Fi uplink against mcusim's offline
  virtual AP, and `uuids128` / `age_ms` from a simulated BLE advertiser (11 checks).
  Both run from `tools/run_tests.py --sim`.

Not covered anywhere yet: `bt.mtu`, `want_done` and `too_long` with a real link
(mcusim has no LE connection model), and HTTPS against the real
`firebasestorage.googleapis.com` (needs hardware and the network).
