--[[
Entry point for the Pinecil profile. No credentials, no pairing: connect and
read. Start it with {"op":"script.start","params":{"name":"pinecil"}}.
]]
local pinecil = require 'profiles.pinecil'

log.info('pinecil: looking for an iron')
local addr
repeat
  for _, d in ipairs(bt.devices()) do
    if pinecil.matches(d) then addr = d.addr end
  end
  if not addr then sys.sleep(500) end
until addr

local function is_connected()
  for _, d in ipairs(bt.devices()) do
    if d.addr == addr and d.connected then return true end
  end
  return false
end

-- TWO phases, and they must not be collapsed into one.
--
-- bt.connect only reports that the attempt was STARTED; the link takes about
-- a second to come up. A single loop that gives up as soon as the device is
-- "not connected" therefore fires on its very first pass, before the link has
-- had any chance -- which is exactly what happened here: the script reported
-- "link dropped before discovery finished" and exited, and discovery then
-- completed successfully 1.1 s later with nobody left to use it.
--
-- So: wait FOR the link, then watch for it dropping.
if not is_connected() then
  log.info('pinecil: connecting to ' .. addr)
  if not bt.connect(addr) then log.error('pinecil: connect failed'); return end
  local dl = sys.ms() + 12000
  while not is_connected() and sys.ms() < dl do sys.sleep(250) end
  if not is_connected() then log.error('pinecil: link never came up'); return end
end
log.info('pinecil: link up')

-- Now the link exists, so losing it is a real failure rather than a race.
local deadline = sys.ms() + 15000
local ready
repeat
  ready = bt.find(addr, pinecil.SVC_BULK, pinecil.CHR_BULK_LIVE)
  if not ready then
    if not is_connected() then
      log.error('pinecil: link dropped before discovery finished'); return
    end
    sys.sleep(250)
  end
until ready or sys.ms() > deadline
if not ready then log.error('pinecil: discovery never finished'); return end

pinecil.run(addr, {})
