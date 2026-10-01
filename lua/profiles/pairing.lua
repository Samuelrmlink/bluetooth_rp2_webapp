--[[
Pairing from a script, in plain blocking code -- request it, answer the
questions, get the outcome:

  local pairing = require 'profiles.pairing'
  local ok, why = pairing.pair(addr, { pin = 123456 })
  if not ok then log.warn('pairing failed: ' .. why) end

opts (all optional):
  pin         the passkey to enter when the device asks for one (0..999999),
              or a function(addr) returning it
  confirm     the answer to a numeric comparison: true / false, or a
              function(number, addr) returning one. Absent: leave it to the web
              UI, which shows every question too
  on_display  function(number, addr), when WE must show a passkey for the
              device's user to type in (default: log it)
  timeout_ms  give up after this long (default 30000)

Returns true once bonded, or false and why ("timeout", "failed (reason N)",
"refused" when the bond policy is "never", "not connected").

Whatever is left unanswered here is still answerable from the web UI or the
API (bond.passkey / bond.confirm): this only answers what it was given. The
gateway's own pairing method (Settings -> Bluetooth: IO capability and MITM)
decides which questions can come up at all; with NoInputNoOutput and MITM off
every pairing is Just Works and none do.

For an event-driven script, bt.on_pairing(fn) is the other way: fn(ev) is
called with each question and answers by returning a value.
]]

local P = {}

function P.pair(addr, opts)
  opts = opts or {}
  local st = bt.pairing_state(addr)
  if st.state == 'not_connected' then return false, 'not connected' end
  if not bt.pair(addr) then return false, 'refused' end
  local deadline = sys.ms() + (opts.timeout_ms or 30000)
  local answered_for = nil          -- the question already answered, so it is not repeated
  while sys.ms() < deadline do
    st = bt.pairing_state(addr)
    if st.state == 'bonded' then return true end
    if st.state == 'failed' then return false, ('failed (reason %s)'):format(tostring(st.reason)) end
    if st.state == 'not_connected' then return false, 'not connected' end
    if st.state == 'asking' then
      local q = st.action .. ':' .. tostring(st.number)
      if q ~= answered_for then
        answered_for = q
        if st.action == 'input' and opts.pin ~= nil then
          local pin = type(opts.pin) == 'function' and opts.pin(addr) or opts.pin
          if pin then bt.passkey(addr, math.tointeger(pin)) end
        elseif st.action == 'compare' and opts.confirm ~= nil then
          local ok = opts.confirm
          if type(ok) == 'function' then ok = ok(st.number, addr) end
          bt.confirm(addr, ok and true or false)
        elseif st.action == 'display' then
          if opts.on_display then opts.on_display(st.number, addr)
          else log.info(('pairing: enter %06d on %s'):format(st.number, addr)) end
        end
      end
    end
    sys.sleep(200)
  end
  return false, 'timeout'
end

return P
