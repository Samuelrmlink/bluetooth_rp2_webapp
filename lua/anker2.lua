-- Same Anker PEX app as lua/anker.lua. One module owns both devices, so this
-- starts slot 0 and slot 1 (slot 1 only when cfg target2 is set). If anker is
-- already running, pex.start fails with "already running".
local info = pex.info('anker')
if info and info.state == 'running' then
  log.info('anker: already running')
  return
end
if not info then
  local m, err = pex.load('anker')
  if not m then log.error('anker: ' .. tostring(err)); return end
end
local ok, err = pex.start('anker')
if not ok then log.error('anker: ' .. tostring(err)) end
