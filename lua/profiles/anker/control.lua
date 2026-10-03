--[[
A meeting point between the Anker driver and other scripts.

Scripts cannot call ops or subscribe to events -- ops.register and bus.emit
go one way, out to WebSocket clients -- but they share one Lua state. So the
driver registers each device it holds here, and a script that wants to act
on one (ac_pulse.lua) requires this module and finds it:

  devices[addr] = {
    set    = function(field, value) -> result,   -- what anker.set does:
             validated against the model's captured setters, then queued
             for the driver's poll loop; returns {queued=true,...} or
             {error=..., message=...}
    sample = the latest decoded telemetry table,
    at     = sys.ms() when that sample arrived,
  }

A script may also leave its result here for a client to read back with
script.eval (ac_pulse.lua does: `last_pulse`). Nothing survives a reboot.

WHY THE STATE HANGS OFF `sys` and not off this module's table: require()'s
cache is flushed every time a script is loaded (lrt.c flush_modules, so an
edited module is re-read), so a script started after the driver gets a NEW
copy of this module -- an empty one. The first version kept `devices` in the
module table and ac_pulse found nothing. Every script's environment falls
through (__index) to the one global table, so `sys` is the same object for
all of them and outlives any flush.

Two launchers (lua/anker.lua, lua/anker2.lua) share two more things here:

  claims[addr] = launcher name   -- which launcher holds which device, so
             two of them never both take one. Set and checked without a
             yield in between, which makes the take atomic: scripts are
             coroutines and only switch where they block.
  handshake  = { who, until_ms } -- held from connect through authentication.
             Two handshakes at once double the negotiation code's arena peak
             and share one radio; one at a time costs a few seconds. Expires
             by itself, so a launcher that dies holding it delays the other
             by at most that long. Never held while looking for a device.

`devices` and `claims` are added to rather than replaced, so a board that
built this table with an older copy of the module keeps working.
]]

local S = sys.anker_control
if not S then
  S = {}
  sys.anker_control = S
end
S.devices = S.devices or {}
S.claims = S.claims or {}

--[[
For scripts, the same writes the web UI makes (anker.set), by name:

  local anker = require 'profiles.anker.control'
  anker.set('A2687', 'c3_on', 0)                  -- queued; returns at once
  local ok, why = anker.set_wait('A2687', 'backlight_pct', 60)
  anker.set_wait('A2687', 'protocols', { c1 = 59, c2 = 59, c3 = 59 })
  anker.set_wait('A2687', 'custom_mode', { mode = 1, auto_exit = 1, c1_w = 100, c2_w = 45, c3_w = 15 })
  print(anker.get('A2687', 'c2_w'))               -- latest telemetry value

`which` is an address, a product code ('A2687', 'A1761'), or nil for the only
device held. set_wait blocks (a script is a coroutine; never call it from an
op handler) until the charger's own telemetry shows the value, or until the
send was accepted for a setting it never reports (the A2687's protocols).
Functions are (re)assigned on every require, so an edited copy wins.
]]

function S.find(which)
  local only, n = nil, 0
  for addr, d in pairs(S.devices) do
    n = n + 1
    only = only or addr
    local code = d.sample and d.sample.product_code
    if which and (addr == which or code == which
        or addr:gsub('[^%x]', ''):upper() == tostring(which):gsub('[^%x]', ''):upper()) then
      return addr, d
    end
  end
  if which == nil and n == 1 then return only, S.devices[only] end
  return nil, nil
end

function S.set(which, field, value)
  local addr, d = S.find(which)
  if not d or not d.set then return { error = 'no_device', message = 'no Anker device ' .. tostring(which) } end
  return d.set(field, value)
end

function S.get(which, field)
  local _, d = S.find(which)
  return d and d.sample and d.sample[field]
end

-- The writable fields and their rules: { field = {opcode, values, min, max,
-- names, kind, verified} } -- what anker.setters returns to a client.
function S.setters(which)
  local _, d = S.find(which)
  return d and d.setters_info and d.setters_info() or {}
end

local function same(a, b)
  if type(a) == 'table' and type(b) == 'table' then
    for k, v in pairs(b) do if a[k] ~= v then return false end end
    return true
  end
  return a == b or tostring(a) == tostring(b)
end

function S.set_wait(which, field, value, timeout_ms)
  local addr, d = S.find(which)
  if not d then return false, 'no Anker device ' .. tostring(which) end
  local r = d.set(field, value)
  if not r or r.error then return false, r and (r.message or r.error) or 'refused' end
  -- what to watch: the field itself, except custom_mode (the mode becomes 2)
  local watch, want = field, value
  if field == 'custom_mode' then watch, want = 'charging_protocol', 2 end
  local deadline = sys.ms() + (timeout_ms or 8000)
  while sys.ms() < deadline do
    local v = d.sample and d.sample[watch]
    if v ~= nil and (same(v, want) or same(v, r.value)) then return true end
    sys.sleep(200)
  end
  return false, ('%s did not read back within %d ms'):format(watch, timeout_ms or 8000)
end

return S
