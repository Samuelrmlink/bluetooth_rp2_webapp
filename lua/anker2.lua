--[[
A second Anker device, alongside lua/anker.lua.

Two lines, because the driver is a module: profiles/anker/run.lua is compiled
once and both launchers share it. A second copy of the driver would not fit
in a 128 KB arena; a second launcher costs almost nothing.

Pins its device with cfg/anker.json's "target2" (an address or a productType
such as "B105"), so the two launchers cannot both grab the same charger.
]]

local target = sys.secret('anker', 'target2')
if not target then
  -- Refuse rather than race. With no target this launcher would identify the
  -- same way lua/anker.lua does and the two would fight over one device,
  -- which looks like flaky hardware rather than a missing setting.
  log.error('anker2: set "target2" in cfg/anker.json (an address or a productType)')
  return
end

local run = require 'profiles.anker.run'
run(target)
