--[[
Pine64 Pinecil V2 (IronOS) -- a second device, to show the profile mechanism
is not an Anker-shaped hole.

It is almost the opposite of the charger in every way that matters, which is
why it is worth having:

  * NO NOTIFY CHARACTERISTIC ANYWHERE. All 65 characteristics are read-only
    or read/write; none can push. So this profile polls, where the Anker one
    subscribes. bt.read() asks, and the answer lands in the same inbox a
    notification would, tagged source="read".
  * No pairing, no handshake, no crypto. Connect and read.
  * One characteristic carries everything. 9eae1001 returns all 14 live
    values in a single 56-byte read, so the poll is ONE round trip per second
    rather than fourteen. Reading them individually works and is how the
    fields below were identified, but it is 14x the radio traffic.

FIELD IDENTIFICATION. Read off the bench unit with a cold iron on a USB-PD
supply, cross-checked against values that could only mean one thing: the
setpoint read back identical to settings characteristic f6d70000, the handle
temperature matched room temperature, and the tip resistance matched the
V2's 8 ohm tip. Anything below marked "inferred" matched no such anchor and
should be treated as a guess until something proves it.
]]

local M = {}

M.SVC_LIVE = 'd85ef000-168e-4a71-aa55-33e27f9bc533'
M.SVC_BULK = '9eae1000-9d0d-48c5-aa55-33e27f9bc533'
M.SVC_SET  = 'f6d80000-5a10-4eba-aa55-33e27f9bc533'

M.CHR_BULK_LIVE = '9eae1001-9d0d-48c5-aa55-33e27f9bc533'  -- all 14 values, 56 B
M.CHR_BUILD     = '9eae1003-9d0d-48c5-aa55-33e27f9bc533'  -- "v2.23"
M.CHR_SERIAL    = '9eae1004-9d0d-48c5-aa55-33e27f9bc533'
-- Settings are u16 LE, where live values are u32 LE. Mixing the two reads
-- garbage, and only index 0 and 1 below are confirmed.
M.CHR_SETPOINT  = 'f6d70000-5a10-4eba-aa55-33e27f9bc533'  -- soldering temp, u16
M.CHR_SLEEPTEMP = 'f6d70001-5a10-4eba-aa55-33e27f9bc533'  -- sleep temp, u16

M.MODE = {
  [0] = 'idle', [1] = 'soldering', [2] = 'boost',
  [3] = 'sleeping', [4] = 'settings', [5] = 'debug',
}

-- Observed 3 on a USB-PD supply. The other values are inferred from IronOS's
-- ordering and are not confirmed on this unit.
M.POWER_SOURCE = { [0] = 'DC', [1] = 'QC', [2] = 'PD-VBUS', [3] = 'PD' }

local function u32(s, i)
  return s:byte(i) | (s:byte(i+1) << 8) | (s:byte(i+2) << 16) | (s:byte(i+3) << 24)
end

-- 14 x u32 LE. The order is the live service's d85ef001..d85ef00e, which is
-- how it was identified: each bulk slot matched the individual read.
function M.decode_live(b)
  if not b or #b < 56 then return nil end
  local v = {}
  for i = 1, 14 do v[i] = u32(b, (i - 1) * 4 + 1) end
  return {
    tip_c        = v[1],            -- whole degrees C
    setpoint_c   = v[2],
    input_v      = v[3] / 10.0,     -- tenths of a volt
    handle_c     = v[4] / 10.0,     -- tenths of a degree
    power_pct    = v[5],
    power_source = M.POWER_SOURCE[v[6]] or v[6],
    tip_ohms     = v[7] / 10.0,     -- tenths of an ohm; 8.0 on the V2
    uptime_s     = v[8] / 10.0,     -- tenths of a second
    last_move_s  = v[9] / 10.0,
    max_tip_c    = v[10],
    tip_uv       = v[11],           -- inferred: raw tip microvolts
    hall_uT      = v[12],           -- inferred: hall sensor, 0 with no magnet
    mode         = M.MODE[v[13]] or v[13],
    watts        = v[14] / 10.0,    -- inferred scale; 0 while idle, so the
                                    -- divisor is unproven until it heats up
  }
end

-- Matching. The Pinecil advertises its name and no manufacturer data, which
-- is the opposite of the Anker: there the name is useless and the mfg data
-- carries the model. Two devices, two answers, which is exactly why matching
-- belongs in a profile rather than in the firmware.
function M.matches(dev)
  return type(dev.name) == 'string' and dev.name:match('^Pinecil') ~= nil
end

-- ---------------------------------------------------------------------------
local Link = {}
Link.__index = Link

function M.link(addr)
  local l = setmetatable({ addr = addr }, Link)
  l.live   = assert(bt.find(addr, M.SVC_BULK, M.CHR_BULK_LIVE), 'no bulk live characteristic')
  l.build  = assert(bt.find(addr, M.SVC_BULK, M.CHR_BUILD),     'no build characteristic')
  l.serial = assert(bt.find(addr, M.SVC_BULK, M.CHR_SERIAL),    'no serial characteristic')
  l.setpt  = assert(bt.find(addr, M.SVC_SET,  M.CHR_SETPOINT),  'no setpoint characteristic')
  return l
end

-- Ask, then wait for the answer in the inbox. A GATT read is a round trip;
-- blocking on it would stall the radio, so this yields between polls exactly
-- as an await on a notification does.
function Link:read(id, timeout_ms)
  bt.notify_flush(self.addr)
  if not bt.read(self.addr, id) then return nil, 'read refused' end
  local deadline = sys.ms() + (timeout_ms or 2000)
  repeat
    local n = bt.notify_take(self.addr)
    if n and n.id == id and n.source == 'read' then return n.value end
    if not n then sys.sleep(5) end
  until sys.ms() > deadline
  return nil, 'read timed out'
end

function Link:set_temp(celsius)
  -- Clamped to what the iron will accept. Writing past max_tip_c is silently
  -- ignored by the firmware, which looks like the write failed.
  local c = math.floor(tonumber(celsius) or 0)
  if c < 10 then c = 10 end
  if c > (self.max_tip_c or 450) then c = self.max_tip_c or 450 end
  local lo, hi = c & 0xff, (c >> 8) & 0xff
  local ok = bt.write(self.addr, self.setpt, bytes.hex(string.char(lo, hi)), true)
  return ok, c
end

function M.publish(link, t)
  t.addr = link.addr
  t.build = link.build_str
  t.serial = link.serial_str
  bus.emit('pinecil.telemetry', t)
end

function M.run(addr, cfg)
  cfg = cfg or {}
  local link = M.link(addr)

  -- Identity, once. These never change, so polling them would be waste.
  local b = link:read(link.build)
  link.build_str = b and b or nil
  local s = link:read(link.serial)
  if s and #s >= 6 then
    link.serial_str = ('%02X%02X%02X%02X%02X%02X')
      :format(s:byte(1), s:byte(2), s:byte(3), s:byte(4), s:byte(5), s:byte(6))
  end
  log.info(('pinecil: %s build %s'):format(addr, link.build_str or '?'))

  ops.register('pinecil.set_temp', function(p)
    local ok, c = link:set_temp(p and p.celsius)
    return { ok = ok and true or false, celsius = c }
  end)

  local period = tonumber(cfg.poll_ms) or 1000
  while true do
    local raw, err = link:read(link.live, 2500)
    if raw then
      local t = M.decode_live(raw)
      if t then
        link.max_tip_c = t.max_tip_c
        M.publish(link, t)
      else
        log.warn('pinecil: short live read (' .. #raw .. ' B)')
      end
    else
      log.warn('pinecil: ' .. tostring(err))
    end
    sys.sleep(period)
  end
end

return M
