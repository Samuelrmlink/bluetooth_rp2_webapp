--[[
Entry point for the Anker A2687 profile.

Start it with  {"op":"script.start","params":{"name":"anker"}}  and it waits
for a matching charger to appear in the scan table, connects, runs the
handshake and then streams telemetry as profile.anker.telemetry events.

CREDENTIALS ARE NOT IN THIS FILE AND MUST NOT BE. They come from
cfg/anker.json, which is git-ignored and which the HTTP filesystem API will
not serve. Create it once on the device:

  {
    "user_id":     "<40-character Anker account id>",
    "private_key": "<64 hex characters, a P-256 scalar>",
    "tz":          "CST6CDT,M3.2.0,M11.1.0",
    "region":      "US"
  }

The private key is OURS, not the app's. Nothing requires us to impersonate
the official app's keypair -- the charger does ECDH against whatever public
key arrives in 0x0021 -- so generate a fresh one and keep it on the device.
]]

local a2687 = require 'profiles.anker_a2687'

local function load_cfg()
  local uid = sys.secret('anker', 'user_id')
  local pk  = sys.secret('anker', 'private_key')
  if not uid or not pk then
    return nil, 'cfg/anker.json needs user_id and private_key (see lua/anker.lua)'
  end
  if #uid ~= 40 then
    return nil, ('user_id must be 40 characters, got %d'):format(#uid)
  end
  local key = bytes.unhex(pk)
  if not key or #key ~= 32 then
    return nil, 'private_key must be 64 hex characters'
  end

  local tz = sys.secret('anker', 'tz') or 'UTC0'
  local off = tonumber(sys.secret('anker', 'tz_offset_sec') or '0') or 0
  return {
    user_id = uid,
    private_key = key,
    tz = tz,
    tz_offset_sec = off,
    region = sys.secret('anker', 'region') or 'US',
  }
end

-- The A2687 advertises 0xFF09 in its MANUFACTURER DATA. Do not try to match
-- on 8c850001: that is a GATT service UUID and is invisible until after the
-- connection, so a scan filtered on it finds nothing, every time.
local function find_charger()
  for _, d in ipairs(bt.devices()) do
    if d.connected and d.name and d.name:match('^A2687') then return d.addr end
  end
  for _, d in ipairs(bt.devices()) do
    if d.connected then
      -- Fall back to anything that actually exposes the Anker service.
      if bt.find(d.addr, a2687.SVC, a2687.CMD) then return d.addr end
    end
  end
  return nil
end

local cfg, err = load_cfg()
if not cfg then
  log.error('anker: ' .. err)
  return
end

log.info('anker: waiting for a connected charger')
local addr
repeat
  addr = find_charger()
  if not addr then sys.sleep(500) end
until addr

log.info('anker: found ' .. addr)
a2687.run(addr, cfg)
