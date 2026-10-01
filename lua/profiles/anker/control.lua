--[[
A meeting point between the Anker driver and other scripts.

Scripts cannot call ops or subscribe to events -- ops.register and bus.emit
go one way, out to WebSocket clients -- but they share one Lua state, and
require() caches a module once for all of them. So the driver registers each
device it holds here, and a script that wants to act on one (ac_pulse.lua)
requires this module and finds it:

  devices[addr] = {
    set    = function(field, value) -> result,   -- what anker.set does:
             validated against the model's captured setters, then queued
             for the driver's poll loop; returns {queued=true,...} or
             {error=..., message=...}
    sample = the latest decoded telemetry table,
    at     = sys.ms() when that sample arrived,
  }

Nothing here is persistent: it lives as long as some script holds the module.
A script may also leave its result here for a client to read back with
script.eval (ac_pulse.lua does: `last_pulse`).
]]

return { devices = {} }
