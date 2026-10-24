-- Start the Anker PEX driver. Credentials stay in cfg/anker.json.
-- One app owns both devices. Starting this or anker2 starts slot 0 and,
-- when target2 is set, slot 1. A second pex.start fails with "already running".
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
