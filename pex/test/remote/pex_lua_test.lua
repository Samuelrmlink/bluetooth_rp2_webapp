-- PEX from Lua on the gateway: the launcher API first (Lua as a launcher of
-- native apps), then require 'hello' finding hello.m33.pex. Uploaded and
-- started by pex/tools/pex_remote_test.py; writes lfs:/pex_lua_result.json.
local R = {}
local function ok(name, cond, extra)
  R[#R + 1] = { name = name, ok = cond and true or false, extra = extra ~= nil and tostring(extra) or nil }
end
local function save()
  local h = file.open("lfs:/pex_lua_result.json", "w")
  h:write(json.encode(R)) h:close()
end

-- the launcher: a 3-line script starts an app
local info, err = pex.load("app")
ok("launcher.load", info and info.name == "app" and info.state == "loaded", err)
ok("launcher.start", pex.start("app", { ticks = 3, period = 20 }))
local i = pex.info("app")
ok("launcher.info", i and i.state == "running" and i.tasks >= 1 and #i.build_id == 32, i and i.state)
ok("launcher.unload_refused_running", select(2, pex.unload("app")) ~= nil)
for _ = 1, 50 do
  i = pex.info("app")
  if i and i.state == "exited" then break end
  sys.sleep(20)
end
ok("launcher.app_ran", i and i.state == "exited" and pex.call("app", "app_ticks") == 3, i and i.state)
local found = false
for _, m in ipairs(pex.list()) do found = found or m.name == "app" end
ok("launcher.list", found)
ok("launcher.stop", pex.stop("app"))
ok("launcher.unload", pex.unload("app"))
ok("launcher.gone", pex.info("app") == nil)
local nope, nerr = pex.load("does_not_exist")
ok("launcher.missing", nope == nil and type(nerr) == "string", nerr)

-- require: x.<arch>.pex before any Lua source
local h = require("hello")
ok("require.table", type(h) == "table" and type(h.greet) == "function")
ok("require.greet", h.greet("pex") == "hello, pex", h.greet and h.greet("pex"))
ok("require.add", h.add(2, 3) == 5)
ok("require.answer", h.answer == 42)
ok("require.cached", require("hello") == h)
local good, e = pcall(h.add, "x", 1)
ok("require.arg_error_unwinds", not good and tostring(e):find("number expected") ~= nil, e)
local hi = pex.info("hello")
ok("require.not_active_after_error", hi and hi.active == 0 and hi.refs >= 1, hi and (hi.active .. "/" .. hi.refs))
h = nil
ok("require.unload", pex.unload("hello"), select(2, pex.unload("hello")))
ok("require.gone", pex.info("hello") == nil)
ok("done", true)
save()
