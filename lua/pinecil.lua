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

local connected = false
for _, d in ipairs(bt.devices()) do
  if d.addr == addr then connected = d.connected end
end
if not connected then
  log.info('pinecil: connecting to ' .. addr)
  if not bt.connect(addr) then log.error('pinecil: connect failed'); return end
end

-- Wait for discovery rather than a stopwatch.
local deadline = sys.ms() + 15000
local ready
repeat
  ready = bt.find(addr, pinecil.SVC_BULK, pinecil.CHR_BULK_LIVE)
  if not ready then sys.sleep(250) end
until ready or sys.ms() > deadline
if not ready then log.error('pinecil: discovery never finished'); return end

pinecil.run(addr, {})
