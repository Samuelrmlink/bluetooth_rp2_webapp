-- Start the Pinecil PEX driver. No credentials.
local info = pex.info('pinecil')
if info and info.state == 'running' then
  log.info('pinecil: already running')
  return
end
if not info then
  local m, err = pex.load('pinecil')
  if not m then log.error('pinecil: ' .. tostring(err)); return end
end
local ok, err = pex.start('pinecil')
if not ok then log.error('pinecil: ' .. tostring(err)) end
