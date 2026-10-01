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
return S
