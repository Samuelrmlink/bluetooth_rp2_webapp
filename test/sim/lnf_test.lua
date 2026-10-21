-- Run on the firmware by test/sim/lua_net_fs.py (mcusim, USB-NCM). Exercises
-- json, file (LittleFS), http (plain + TLS against servers on the PC), sys,
-- and POST /api/op from a script, then writes lfs:/lnf_result.json.
local PC = "@PC@"          -- the PC's address on the USB network
local HP = "@HTTP_PORT@"
local TP = "@TLS_PORT@"
local R = {}
local function ok(name, cond, extra)
  R[#R + 1] = { name = name, ok = cond and true or false, extra = extra ~= nil and tostring(extra) or nil }
  log.info((cond and "ok   " or "FAIL ") .. name .. " " .. tostring(extra or ""))
end
local function save()
  local h = file.open("lfs:/lnf_result.json", "w")
  h:write(json.encode(R)) h:close()
end

-- json ---------------------------------------------------------------------
local v = json.decode('{"a":[1,2,{"b":null}],"s":"\\u00e9","n":-1.5e2}')
ok("json.decode", v and v.a[3].b == json.null and v.s == "\u{e9}" and v.n == -150)
ok("json.encode", json.encode({ x = 1 }) == '{"x":1}', json.encode({ x = 1 }))
local bad, err = json.decode("[1,")
ok("json.decode error", bad == nil and type(err) == "string", err)

-- sys ----------------------------------------------------------------------
ok("sys.hostname", type(sys.hostname()) == "string" and #sys.hostname() > 0, sys.hostname())
ok("sys.time is nil or a number", sys.time() == nil or math.type(sys.time()) == "integer", sys.time())

-- file (LittleFS) ------------------------------------------------------------
for _, f in ipairs{ "lfs:/t/c.txt", "lfs:/t/b.txt", "lfs:/t/sub/a.bin", "lfs:/t/sub", "lfs:/t",
                    "lfs:/dl/big.bin", "lfs:/dl/m.bin", "lfs:/dl" } do file.remove(f) end
local big = string.rep("0123456789abcdef", 768)   -- 12 KiB: three 4 KiB pieces with yields between
local h, e = file.open("lfs:/t/sub/a.bin", "w")
ok("file.open w (parents made)", h, e)
ok("h:write 12 KiB", h and h:write(big))
ok("h:size", h and h:size() == #big, h and h:size())
ok("h:close", h and h:close())
h = file.open("lfs:/t/sub/a.bin", "r")
local parts = {}
while h do local c = h:read(4096); if not c or c == "" then break end; parts[#parts + 1] = c end
local back = table.concat(parts); parts = nil
ok("read back", back == big, #back); back = nil
ok("seek/tell", h:seek(1000) and h:tell() == 1000 and h:read(4) == "89ab")
h:close()
local st = file.stat("lfs:/t/sub/a.bin")
ok("file.stat", st and st.size == #big and st.dir == false)
ok("file.stat dir", file.stat("lfs:/t").dir == true)
local l = file.list("lfs:/t/sub")
ok("file.list", l and #l == 1 and l[1].name == "a.bin" and l[1].size == #big)
ok("enoent", select(2, file.open("lfs:/nope.bin", "r")) == "enoent")
ok("badpath", select(2, file.open("lfs:/../x", "r")) == "badpath")
ok("append", (function()
  local a = file.open("lfs:/t/sub/a.bin", "a"); a:write("TAIL"); local s = a:size(); a:close(); return s == #big + 4 end)())
ok("rename eexist", (function()
  local w = file.open("lfs:/t/b.txt", "w"); w:write("b"); w:close()
  return select(2, file.rename("lfs:/t/b.txt", "lfs:/t/sub/a.bin")) == "eexist" end)())
ok("rename", file.rename("lfs:/t/b.txt", "lfs:/t/c.txt") and file.stat("lfs:/t/c.txt").size == 1)
ok("remove notempty", select(2, file.remove("lfs:/t/sub")) == "notempty")
ok("busy: a second writer", (function()
  local w1 = file.open("lfs:/t/c.txt", "a")
  local w2, why = file.open("lfs:/t/c.txt", "w")
  w1:close(); return w2 == nil and why == "busy" end)())
ok("4 handles, then nohandle", (function()
  local hs = {}
  for i = 1, 4 do hs[i] = file.open("lfs:/t/sub/a.bin", "r") end
  local x, why = file.open("lfs:/t/c.txt", "r")
  for i = 1, 4 do if hs[i] then hs[i]:close() end end
  return hs[4] ~= nil and x == nil and why == "nohandle", why end)())
ok("toolarge (1 MiB)", (function()
  local w = file.open("lfs:/t/huge.bin", "w")
  local chunk = string.rep("x", 16384)
  local r, why
  for i = 1, 65 do r, why = w:write(chunk); if not r then break end end
  w:close(); file.remove("lfs:/t/huge.bin")
  return r == nil and why == "toolarge", why end)())
do local m = file.media()
  ok("file.media", m.lfs.ok and m.lfs.total > 0 and (m.sd.state == "absent" or m.sd.state), json.encode(m)) end
ok("sd absent here", select(2, file.open("sd:/x", "r")) == "nomedia")
big = nil
collectgarbage()
save()

-- http -----------------------------------------------------------------------
local base = "http://" .. PC .. ":" .. HP
-- await for as long as the request may run (an await waits at most 30 s)
local function wait(id)
  for _ = 1, 20 do
    local r, why = http.await(id, 30000)
    if r or why ~= "timeout" then return r, why end
  end
  return nil, "timeout"
end
local function get(t) local id, why = http.start(t); if not id then return nil, why end; return wait(id) end

local r = get{ url = base .. "/small.json" }
ok("http GET content-length", r and r.status == 200 and json.decode(r.body).hello == "world"
   and r.headers["content-type"] == "application/json", r and (r.error or r.status))
r = get{ url = base .. "/chunked", max_body = 32768 }
ok("http chunked 20000", r and r.status == 200 and #r.body == 20000 and r.body:sub(1, 10) == string.rep("x", 10)
   and r.bytes == 20000, r and (r.error or #(r.body or "")))
r = nil; collectgarbage()
r = get{ url = base .. "/redirect" }
ok("redirect (absolute path)", r and r.status == 200 and r.body:find("world"), r and (r.error or r.status))
r = get{ url = base .. "/a/redirect-rel" }
ok("redirect (relative)", r and r.status == 200 and r.body == "rel ok", r and (r.error or r.status))
r = get{ url = base .. "/redirect", follow = 0 }
ok("follow = 0", r and r.status == 302 and r.headers.location == "/small.json", r and r.status)
r = get{ url = base .. "/echo", method = "POST", body = "ping", headers = { ["X-Test"] = "1" } }
ok("POST with body and header", r and r.status == 200 and r.body == "POST ping 1", r and (r.error or r.body))
r = get{ url = base .. "/missing" }
ok("404 is a result, not an error", r and r.status == 404 and r.body == "nope", r and (r.error or r.status))
r = get{ url = base .. "/chunked", max_body = 1000 }
ok("max_body truncates", r and r.truncated == true and #r.body == 1000, r and (r.error or r.truncated))
r = get{ url = base .. "/big", to_file = "lfs:/dl/big.bin", timeout_ms = 300000 }
local fst = file.stat("lfs:/dl/big.bin")
ok("to_file LittleFS 300000", r and r.status == 200 and r.body == nil and fst and fst.size == 300000,
   r and (r.error or r.message or r.status) .. " " .. tostring(fst and fst.size))
r = get{ url = base .. "/missing", to_file = "lfs:/dl/m.bin" }
ok("to_file: a 404 is not written", r and r.status == 404 and file.stat("lfs:/dl/m.bin") == nil)
r = get{ url = base .. "/slow", timeout_ms = 1500 }
ok("timeout", r and r.error == "timeout", r and (r.error or r.status))
r = get{ url = "http://" .. PC .. ":1/x" }
ok("connect refused", r and r.error == "connect", r and (r.error or r.status))
r = get{ url = "http://no.such.host.invalid/x" }
ok("dns failure", r and r.error == "dns", r and (r.error or r.status))
do -- two at once, and cancel
  local a = http.start{ url = base .. "/chunked", max_body = 32768, timeout_ms = 120000 }
  local b = http.start{ url = base .. "/big", timeout_ms = 120000 }
  local c = http.start{ url = base .. "/slow" }
  local st = http.status(c)
  ok("http.status", st and (st.state == "dns" or st.state == "connect" or st.state == "send" or st.state == "recv"), st and st.state)
  ok("cancel", http.cancel(c))
  local ra, rb = wait(a), wait(b)
  ok("two at once", ra and rb and ra.body and rb.body and #ra.body == 20000 and #rb.body == 16384 and rb.truncated,
     json.encode{ a = ra and (ra.error or #(ra.body or "")), b = rb and (rb.error or #(rb.body or "")) })
  ok("await after cancel", select(2, http.await(c, 10)) == "badid")
end
r = get{ url = "https://" .. PC .. ":" .. TP .. "/small.json", insecure = true, timeout_ms = 120000 }
ok("https (insecure, local test cert)", r and r.status == 200 and r.body and r.body:find("world"),
   r and (r.error and (r.error .. ": " .. tostring(r.message)) or r.status))
do -- two HTTPS requests at once: one TLS session at a time, the second waits its turn
  local w0 = http.stats().tls_waits or 0
  local a = http.start{ url = "https://" .. PC .. ":" .. TP .. "/small.json", insecure = true, timeout_ms = 120000 }
  local b = http.start{ url = "https://" .. PC .. ":" .. TP .. "/small.json", insecure = true, timeout_ms = 120000 }
  local ra, rb = wait(a), wait(b)
  local st = http.stats()
  ok("two https at once take the TLS session in turn", ra and rb and ra.status == 200 and rb.status == 200
     and (st.tls_waits or 0) > w0 and st.tls_pool_fails == 0,
     json.encode{ a = ra and (ra.error or ra.status), b = rb and (rb.error or rb.status), waits = st.tls_waits })
end
local s = http.stats()
ok("tls memory: the pool, not the arena", s.tls_pool and s.tls_pool > 0 and s.tls_bytes == 0 and s.tls_peak_bytes > 16384
   and s.tls_peak_bytes <= s.tls_pool, json.encode(s))
r = get{ url = "https://" .. PC .. ":" .. TP .. "/small.json", timeout_ms = 120000 }
ok("https verified: a test root is refused", r and r.error == "tls", r and (r.error and (r.error .. ": " .. tostring(r.message)) or r.status))
-- POST /api/op from a script, to this gateway itself.
r = get{ url = "http://192.168.8.1/api/op", method = "POST", body = '{"op":"sys.info","params":{}}' }
local d = r and r.body and json.decode(r.body)
ok("POST /api/op (sys.info) from Lua", d and d.ok == true and d.result.firmware == "bluetooth_rp2_webapp", r and (r.error or r.body))
ok("arena after", true, collectgarbage("count"))
R[#R + 1] = { name = "done", ok = true }
save()
