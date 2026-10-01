--[[
Anker Prime 160W 3-Port GaN Charger -- productType B405, SCP family.

Field map recovered empirically (live dumps plus toggle correlation); the
decoders below are carried over verbatim from the profile that was verified
against this charger on the bench, because retyping a dozen struct decoders
is exactly how a transcription error gets in.

SCP devices differ from the power stations in kind, not degree: instead of
aggregate per-group output tags they carry structured per-port objects, so
several fields decode to tables rather than numbers. That is why a field's
reader may be a function here and a type name in A1761.lua.

Telemetry arrives unsolicited as 0x0300 at about 1 Hz once authenticated;
0x0a00 is the polled response and carries the identical tag set.
]]

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
  -- It used to fall back to raw(it):byte(1) -- the FIRST byte only. A u16 or
  -- u32 arriving with an unexpected type byte then silently lost its high
  -- bytes: packageVersion 1606 would read as 70 and the E0 bitmask would be
  -- truncated to its low 8 bits, both looking entirely plausible. Decode by
  -- the width actually present, or report nothing.
  local b = raw(it)
  if #b == 1 then return b:byte(1) end
  if #b == 2 then return u16(b, 1) end
  if #b == 4 then return u32(b, 1) end
  return nil
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

local FIELDS = {
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
  -- 6x u16 LE: c1VID c1PID c2VID c2PID c3VID c3PID. 65530/65531 are the
  -- documented "unknown" sentinels, not real ids.
  b4 = { 'identify_device', function(it)
           local v = raw(it)
           if #v < 12 then return nil end
           local out = {}
           for i = 0, 2 do
             local vid, pid = u16(v, i * 4 + 1), u16(v, i * 4 + 3)
             out['c' .. (i + 1)] = {
               vid = (vid == 65530 or vid == 65531) and 'unknown' or vid,
               pid = (pid == 65530 or pid == 65531) and 'unknown' or pid,
             }
           end
           return out
         end },
  -- 6x u16 LE: Brand,Model per port. 65535 = nothing identified.
  b5 = { 'connected_device', function(it)
           local v = raw(it)
           if #v < 12 then return nil end
           local out = {}
           for i = 0, 2 do
             local brand, model = u16(v, i * 4 + 1), u16(v, i * 4 + 3)
             out['c' .. (i + 1)] = (brand == 65535 and model == 65535) and 'none'
               or { brand = brand, model = model }
           end
           return out
         end },
  -- u32 bitmask. Bit order is the doc's parse order, so b0 is the lowest bit.
  e0 = { 'ability_info', function(it)
           local n = num(it)
           if not n then return nil end
           local bits = { 'screensaver_local', 'screensaver_text', 'screensaver_network',
                          'screensaver_custom', 'screensaver_holiday', 'tomato_clock',
                          'custom_protocol' }
           local out = { raw = ('0x%x'):format(n) }
           for i, name in ipairs(bits) do
             out[name] = (n & (1 << (i - 1))) ~= 0
           end
           return out
         end },
  -- 11 B. Only the leading fields are pinned by the notes; the tail is
  -- carried as hex rather than guessed at.
  e1 = { 'screensaver', function(it)
           local v = raw(it)
           if #v < 11 then return bytes.hex(v) end
           return {
             enabled = v:byte(1) ~= 0,
             theme = v:byte(2),
             time_format = v:byte(3),
             start_min = u16(v, 4),
             end_min = u16(v, 6),
             rest = bytes.hex(v:sub(8)),
           }
         end },
}

return {
  product_code = 'A2687',
  name         = 'Anker Prime 160W 3-Port GaN Charger (smart display)',
  family       = 'scp',
  verified     = true,       -- decoded against this charger on the bench

  -- Needs the post-auth burst before it will say anything: bindUser and
  -- setPortTimers are what flip its state machine into reporting. The power
  -- stations neither need nor implement those opcodes.
  post_auth = true,

  -- It pushes; there is nothing to poll.
  report = 0x0300,
  -- Custom screensaver upload: profiles/anker/screensaver.lua, driven by the
  -- browser (web/js/devices/anker-scp).
  screensaver = true,
  also   = { [0x0a00] = true },

  -- a1 is a selector echo (always 0x31); fe is the anti-replay stamp and is
  -- zero in device->app frames. Neither is a data field.
  ignore = { a1 = true, fe = true },

  fields = FIELDS,
}
