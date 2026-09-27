--[[
The telemetry field dictionary.

Split from the profile for the arena reason in cipher.lua, and it is the
natural seam anyway: this is 24 tags whose meanings were recovered
empirically and which will keep moving, sitting on top of a TLV codec that
has been stable across three implementations.

0x0300 (report) and 0x0a00 (response) carry the identical payload; only the
leading status byte differs, and that is stripped by the caller before the
body reaches here.
]]

local T = {}

-- ---------------------------------------------------------------------------
-- Telemetry. 0x0300 (report, no status byte -- trap 3) and 0x0a00 (response,
-- status byte) carry the identical 24-tag set. Recovered empirically, so this
-- is the table most likely to keep moving, which is exactly why it is in Lua.
-- ---------------------------------------------------------------------------
local function u16(s, i) return s:byte(i) | (s:byte(i + 1) << 8) end
local function u32(s, i)
  return s:byte(i) | (s:byte(i+1) << 8) | (s:byte(i+2) << 16) | (s:byte(i+3) << 24)
end

local CABLE = { [0] = '60W', [1] = '100W', [2] = '240W', [3] = 'none' }
local SCREEN_OFF = { [0] = '30s', [1] = '1min', [2] = '5min', [3] = '30min', [4] = '12h' }
local DIRECTION = { [0] = 'up', [1] = 'left', [2] = 'down', [3] = 'right' }
local LANGUAGE = { [0] = 'en', [1] = 'zh', [2] = 'ja', [3] = 'de' }

-- A decoder gets the whole item and picks its own view of it, because the
-- two are not interchangeable: tlv.parse hands back a NUMBER for a scalar
-- type byte (0x01/02/03 unsigned, 0x05 f32) and a STRING otherwise. Reading
-- a 12-byte control struct as a number, or a u8 as a string, throws -- and
-- the type byte a given firmware chooses for a given tag is exactly the kind
-- of thing that is not worth betting on. it.raw is the value region INCLUDING
-- the type byte and is always a string, so struct decoders take it.raw:sub(2)
-- and are indifferent to which type byte arrived.
local function raw(it) return it.raw and it.raw:sub(2) or '' end
local function num(it)
  if type(it.value) == 'number' then return it.value end
  local b = raw(it)
  return #b >= 1 and b:byte(1) or nil
end

-- [cPowerStatus u8][mV u16][mA u16][cW u16] -- 7 bytes
local function port_power(it)
  local v = raw(it)
  if #v < 7 then return nil end
  return {
    charging = v:byte(1) == 1,
    mv = u16(v, 2), ma = u16(v, 4),
    watts = u16(v, 6) / 100.0,       -- centiwatts on the wire
  }
end

-- [switch u8][countdownTask u8][countdown u32][remain u32][cable u8][vid u8]
local function port_control(it)
  local v = raw(it)
  if #v < 12 then return nil end
  return {
    on = v:byte(1) == 1,
    countdown_active = v:byte(2) == 1,
    countdown_s = u32(v, 3),
    countdown_remain_s = u32(v, 7),
    cable = CABLE[v:byte(11)] or v:byte(11),
    vid = v:byte(12),
  }
end

local function lookup(tbl)
  return function(it)
    local n = num(it)
    return n and (tbl[n] or n) or nil
  end
end

T.FIELDS = {
  a2 = { 'package_version', num },          -- u16, 1606 <-> v1.6.0.6
  a3 = { 'support_function', num },         -- u16 capability bitmap
  a4 = { 'error_code', num },               -- u8, 0 = OK
  a5 = { 'c1_power', port_power },
  a6 = { 'c2_power', port_power },
  a7 = { 'c3_power', port_power },
  a8 = { 'screen_off', lookup(SCREEN_OFF) },  -- a preset INDEX, not seconds
  a9 = { 'backlight_pct', num },            -- u8 0-100
  aa = { 'charging_protocol', num },        -- u8, 0 = default
  ab = { 'custom_power', function(it)
           local v = raw(it)
           if #v < 8 then return nil end
           return { mode = v:byte(1), auto_exit = v:byte(2) == 1,
                    c1_w = v:byte(3), c2_w = v:byte(4), c3_w = v:byte(5),
                    c1_proto = v:byte(6), c2_proto = v:byte(7), c3_proto = v:byte(8) }
         end },
  ac = { 'c1_control', port_control },
  ad = { 'c2_control', port_control },
  ae = { 'c3_control', port_control },
  af = { 'direction', lookup(DIRECTION) },
  b0 = { 'language', lookup(LANGUAGE) },
  b1 = { 'fixed_allocation', function(it)
           -- 0 is "C1 port priority", NOT "dynamic" -- that label was wrong
           -- everywhere it ever appeared.
           local n = num(it)
           if n == nil then return nil end
           return n == 0 and 'c1_priority' or 'laptop_mode'
         end },
  b2 = { 'gyroscope', num },                -- u8 auto-rotate
  b3 = { 'custom_mode_number', num },       -- u8, 255 = none
  b4 = { 'identify_device', function(it) return bytes.hex(raw(it)) end },
  b5 = { 'connected_device', function(it) return bytes.hex(raw(it)) end },
  e0 = { 'ability_info', num },             -- u32 bitmask
  e1 = { 'screensaver', function(it) return bytes.hex(raw(it)) end },
}

-- a1 is a selector echo (always 0x31) and fe is the anti-replay stamp, zero in
-- device->app frames. Neither is a data field, so neither is in FIELDS.
function T.decode_report(body)
  local t = tlv.parse(body, false)
  local out = {}
  if t.truncated then
    log.warn('a2687: telemetry had more items than the parser holds')
    out.truncated = true
  end
  for i = 1, t.n do
    local it = t[i]
    local spec = T.FIELDS[it.hex]
    if spec then
      -- One bad field must not cost the whole report: a firmware revision
      -- that widens a struct would otherwise blank the telemetry pane.
      local ok, v = pcall(spec[2], it)
      if ok then
        out[spec[1]] = v
      else
        log.warn(('a2687: tag %s did not decode: %s'):format(it.hex, tostring(v)))
      end
    end
  end
  return out
end

return T
