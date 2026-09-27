--[[
Entry point for the Anker SOLIX C1000 profile (product type B105).

  {"op":"script.start","params":{"script":"c1000"}}

Credentials come from cfg/anker.json, which is git-ignored and which the HTTP
filesystem API will not serve. The C1000 needs only private_key to reach a
session -- unlike the A2687 it asks for no account id to get that far.
]]

local c1000 = require 'profiles.c1000'
-- Required HERE, before the link is up, not after the session is negotiated.
-- Compiling a module is the peak-memory moment, and by the time a session
-- exists the heap is holding the profile, the link, the TLV scratch and the
-- session state: loading it then hit "not enough memory" on a 128 KB arena
-- every time. Same lesson as the A2687's split -- what matters is WHEN a
-- chunk compiles, not how much is resident afterwards.
local telemetry = require 'profiles.c1000_telemetry'

local function load_cfg()
  local pk = sys.secret('anker', 'private_key')
  if not pk then return nil, 'cfg/anker.json needs private_key (see lua/anker.lua)' end
  local key = bytes.unhex(pk)
  if not key or #key ~= 32 then return nil, 'private_key must be 64 hex characters' end
  return {
    private_key = key,
    client_id = sys.secret('anker', 'client_id') or 'btgw-0000',
  }
end

local cfg, err = load_cfg()
if not cfg then log.error('c1000: ' .. err); return end

log.info('c1000: looking for a C1000')
local addr, dev
repeat
  for _, d in ipairs(bt.devices()) do
    if c1000.matches(d) then addr, dev = d.addr, d end
  end
  if not addr then sys.sleep(500) end
until addr

local adv = c1000.decode_adv(dev.mfg_data)
log.info(('c1000: found %s (%s, sku %s, %s)'):format(
  addr, adv.product_type, adv.sku, adv.bound and 'bound' or 'unbound'))

local function is_connected()
  for _, d in ipairs(bt.devices()) do
    if d.addr == addr and d.connected then return true end
  end
  return false
end

-- Connect and discover, with retries. These chargers drop the first
-- connection often enough that one attempt is not a test of anything: the
-- link comes up, then dies mid-discovery with ATT 0x1f (HCI disconnect).
-- Observed on both the A2687 and this unit at around -75 dBm.
local function connect_and_discover()
  if not is_connected() then
    log.info('c1000: connecting to ' .. addr)
    if not bt.connect(addr) then return false, 'connect refused' end
    local deadline = sys.ms() + 12000
    while not is_connected() and sys.ms() < deadline do sys.sleep(250) end
    if not is_connected() then return false, 'link never came up' end
  end
  log.info('c1000: link up')

  local deadline = sys.ms() + 20000
  repeat
    local ready = bt.find(addr, c1000.SVC, c1000.CMD)
    if ready then return true end
    if not is_connected() then return false, 'link dropped during discovery' end
    sys.sleep(250)
  until sys.ms() > deadline
  return false, 'discovery never finished'
end

local ok, why
for attempt = 1, 4 do
  ok, why = connect_and_discover()
  if ok then break end
  log.warn(('c1000: attempt %d failed (%s)'):format(attempt, tostring(why)))
  sys.sleep(1500)
end
if not ok then log.error('c1000: giving up: ' .. tostring(why)); return end
log.info('c1000: discovery ready')

local link = c1000.link(addr)
if not bt.subscribe(addr, link.tel, 'notify') then
  log.error('c1000: could not subscribe to the telemetry characteristic'); return
end
bt.notify_flush(addr)

local hok, herr = c1000.handshake(link, cfg)
if not hok then log.error('c1000: handshake failed: ' .. tostring(herr)); return end

local mode, first = c1000.negotiate_session(link, cfg)
if not mode then log.error('c1000: ' .. tostring(first)); return end

local first_report = true
local function emit(dec)
  local t, terr = telemetry.decode(dec.body, first_report)
  first_report = false
  if not t then log.warn('c1000: ' .. tostring(terr)); return end
  t.addr = addr
  t.serial = t.serial_number or link.serial
  t.cipher = mode
  bus.emit('c1000.telemetry', t)
end

log.info('c1000: authenticated; polling status')
emit(first)

while true do
  sys.sleep(5000)
  local dec, err = c1000.status(link)
  if dec then
    emit(dec)
    -- A 353-byte status decodes into ~68 TLV item tables every poll. On a
    -- 128 KB arena that churn fragments faster than the incremental GC keeps
    -- up with, so collect deliberately rather than waiting to run out.
    sys.collect()
  else
    log.warn('c1000: status: ' .. tostring(err))
    if not is_connected() then log.error('c1000: link dropped'); return end
  end
end
