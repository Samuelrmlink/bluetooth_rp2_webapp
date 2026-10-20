-- Run by test/sim/lua_net_fs.py --radio: the CYW43 model with Wi-Fi joined to
-- mcusim's virtual AP (offline DNS, NTP from the host clock, a mock HTTP
-- server) and one BLE advertiser with a 128-bit service UUID.
local R = {}
local function ok(name, cond, extra)
  R[#R + 1] = { name = name, ok = cond and true or false, extra = extra ~= nil and tostring(extra) or nil }
  log.info((cond and "ok   " or "FAIL ") .. name .. " " .. tostring(extra or ""))
end
local function wait(id) for _ = 1, 6 do local r, w = http.await(id, 30000); if r or w ~= "timeout" then return r, w end end end

for _ = 1, 90 do if sys.time() then break end sys.sleep(1000) end
ok("sys.time set by SNTP", sys.time() and sys.time() > 1700000000, sys.time())
local r = wait(http.start{ url = "http://files.example/hello.json", timeout_ms = 60000 })
ok("DNS + HTTP over the Wi-Fi uplink", r and r.status == 200 and json.decode(r.body).via == "wifi", r and (r.error or r.status))
r = wait(http.start{ url = "http://nothere.example/x", timeout_ms = 30000 })
ok("NXDOMAIN is a dns error", r and r.error == "dns", r and (r.error or r.status))

ok("bt.scan", bt.scan(true))
local dev
for _ = 1, 50 do
  for _, d in ipairs(bt.devices()) do if d.addr == "C0:11:22:33:44:55" then dev = d end end
  if dev then break end
  sys.sleep(100)
end
ok("advertiser seen", dev, dev and json.encode(dev))
ok("uuids128", dev and dev.uuids128 and dev.uuids128[1] == "8E400001-F315-4F60-9FB8-838830DAEA50", dev and json.encode(dev.uuids128))
ok("age_ms while scanning", dev and dev.age_ms and dev.age_ms < 1000, dev and dev.age_ms)
bt.scan(false)
sys.sleep(2000)
for _, d in ipairs(bt.devices()) do if d.addr == "C0:11:22:33:44:55" then dev = d end end
ok("age_ms grows once the scan stops", dev and dev.age_ms >= 1900, dev and dev.age_ms)
ok("bt.mtu without a link is nil", bt.mtu("C0:11:22:33:44:55") == nil)
local w, why = bt.write("C0:11:22:33:44:55", "0.0", "00", false, true)
ok("bt.write without a link", w == false and why == "not connected", why)
R[#R + 1] = { name = "done", ok = true }
local h = file.open("lfs:/radio_result.json", "w")
h:write(json.encode(R)) h:close()
