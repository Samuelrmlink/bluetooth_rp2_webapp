--[[
A second Anker device, alongside lua/anker.lua.

Two lines, because the driver is a module: profiles/anker/run.lua is compiled
once and both launchers share it. A second copy of the driver would not fit
in the arena; a second launcher shares the compiled modules (lrt.c
revalidates require()'s cache rather than flushing it) and costs its
session state and its own model.

Takes whichever Anker device in range the other launcher does not hold --
the two claim devices through profiles/anker/control.lua, so they never both
take one, and neither waits for the other: each works alone. cfg/anker.json's
"target2" (an address or a productType such as "B405") pins it if wanted.
]]

local run = require 'profiles.anker.run'
-- Optional pin; without it this takes any Anker device lua/anker.lua does
-- not already hold (profiles/anker/control.lua claims).
run(sys.secret('anker', 'target2'), 'anker2')
