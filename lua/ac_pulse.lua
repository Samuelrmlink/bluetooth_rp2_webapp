--[[
Switch the C1000's AC outlet OFF for OFF_S seconds, then back ON -- on the
gateway, unattended.

Why a script: on the bench the outlet powers the gateway's own network (a
Starlink router), so an "off" sent from a browser is the last thing that
browser sends. Here the whole cycle runs on the gateway, which is not powered
by the outlet, and does not need the network for any of it. It is also the
test of the WiFi rejoin in firmware 1.81: the router goes away and comes back
while Bluetooth holds the radio.

  {"op":"script.start","params":{"name":"ac_pulse"}}

Needs the Anker PEX module running. require 'anker' is the table that
module opens (device, get, age_ms, set).

Rules it keeps:
  - Acts only when the outlet is reported ON, from fresh telemetry.
  - Once OFF has been sent, ON is sent no matter what happened in between
    (an error in the middle is caught), and re-sent until telemetry confirms
    it or ON_DEADLINE_MS passes -- re-finding the device each time, since the
    driver may reconnect.
  - Records every step in ac_pulse.last, to be read back once the
    network returns:
      {"op":"script.eval","params":{"chunk":"return table.concat(ac_pulse.last.steps, '\\n')"}}
]]

ac_pulse = {}

local OFF_S          = 5        -- how long the outlet stays off
local FIELD          = 'ac_output_enabled'
local FRESH_MS       = 15000    -- telemetry older than this is not trusted
local CONFIRM_MS     = 15000    -- wait this long for telemetry to show a change
local ON_DEADLINE_MS = 300000   -- keep trying to switch back on for 5 minutes

local R = { started = sys.ms(), steps = {}, result = 'running' }
ac_pulse.last = R

local function note(s)
  R.steps[#R.steps + 1] = ('%6.1f s  %s'):format((sys.ms() - R.started) / 1000, s)
  log.info('ac_pulse: ' .. s)
end

local function device()
  -- Check pex.info before require: a missing module must not be loaded here.
  if not (pex and pex.info and pex.info('anker')) then return nil end
  local api = require 'anker'
  local a = api.device()
  if not a then return nil end
  local d = {
    set = function(field, value) return api.set(a, field, value) end,
    at = sys.ms() - (api.age_ms(a) or 1e9),
  }
  d.sample = setmetatable({}, { __index = function(_, k) return api.get(a, k) end })
  return a, d
end

-- Wait for a telemetry sample, newer than `since`, reporting FIELD == v.
local function wait_for(v, since, ms)
  while sys.ms() - since < ms do
    local _, d = device()
    if d and d.at and d.at > since and d.sample and d.sample[FIELD] == v then
      return d.at - since
    end
    sys.sleep(200)
  end
end

-- --- preconditions: nothing has been sent yet, so just stop -----------------
local addr, dev = device()
if not dev then
  note('no Anker driver is holding a device -- start the anker PEX module first')
  R.result = 'not started: no device'
  return
end
if not dev.at or sys.ms() - dev.at > FRESH_MS then
  note(('no fresh telemetry from %s (last %s ms ago)'):format(addr, dev.at and (sys.ms() - dev.at) or 'never'))
  R.result = 'not started: no fresh telemetry'
  return
end
if dev.sample[FIELD] ~= 1 then
  note(('%s reports %s=%s, not on; nothing to do'):format(addr, FIELD, tostring(dev.sample[FIELD])))
  R.result = 'not started: AC output was not on'
  return
end
note(('%s: AC on, %s W out; switching off for %d s'):format(addr, tostring(dev.sample.ac_output_power_w), OFF_S))

-- --- off ---------------------------------------------------------------------
local sent_off
local ok, err = pcall(function()
  local t0 = sys.ms()
  local r = dev.set(FIELD, 0)
  if not r or r.error then
    note('off refused: ' .. tostring(r and (r.message or r.error)))
    return
  end
  sent_off = t0
  note('off queued')
  local dt = wait_for(0, t0, CONFIRM_MS)
  note(dt and ('off confirmed by telemetry after %d ms'):format(dt)
          or ('off NOT confirmed by telemetry within %d ms'):format(CONFIRM_MS))
end)
if not ok then note('error after off: ' .. tostring(err)) end

if not sent_off then
  R.result = 'off was not sent; outlet unchanged'
  return
end

-- --- on: always, once off was sent --------------------------------------------
local wait = OFF_S * 1000 - (sys.ms() - sent_off)
if wait > 0 then sys.sleep(wait) end

local attempt = 0
while sys.ms() - sent_off < ON_DEADLINE_MS do
  attempt = attempt + 1
  local ok2, done = pcall(function()
    local a, d = device()
    if not d then note('on: no device in the driver (reconnecting?)'); return false end
    local t0 = sys.ms()
    local r = d.set(FIELD, 1)
    if not r or r.error then
      note(('on attempt %d refused: %s'):format(attempt, tostring(r and (r.message or r.error))))
      return false
    end
    local dt = wait_for(1, t0, CONFIRM_MS)
    if dt then
      note(('on attempt %d: confirmed by telemetry after %d ms (%s)'):format(attempt, dt, a))
      return true
    end
    note(('on attempt %d: not confirmed within %d ms'):format(attempt, CONFIRM_MS))
    return false
  end)
  if not ok2 then note('error during on: ' .. tostring(done)) end
  if ok2 and done then
    R.result = ('done: off %.1f s, on confirmed'):format((sys.ms() - sent_off) / 1000)
    note(R.result)
    return
  end
  sys.sleep(3000)
end

R.result = 'FAILED: AC output was switched off and could not be confirmed back on'
log.error('ac_pulse: ' .. R.result)
note(R.result)
