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
  -- 10 B, matched against the app's own decoded screensaverInfo (2026-10-03):
  -- [flags][type][id u32][holiday id u32]. Bit 7 of flags is the "Screensaver
  -- Display" switch (0x80 on, 0x00 off, toggled in the app); the app also names
  -- displayTime / timeFormat / holiday / clock-type flags, all 0 here, whose
  -- bits are not pinned yet -- carried as `flags`. type 3 = a custom image.
  e1 = { 'screensaver', function(it)
           local v = raw(it)
           if #v < 10 then return bytes.hex(v) end
           return {
             enabled = (v:byte(1) & 0x80) ~= 0,
             flags = v:byte(1) & 0x7f,
             type = v:byte(2),
             id = u32(v, 3),
             holiday_id = u32(v, 7),
           }
         end },
}

-- Names by value, inverted for the setters (which take a name or its index).
local function names(t)
  local out = {}
  for k, v in pairs(t) do out[v] = k end
  return out
end

--[[
Setters, every one captured byte-exact from the Anker app on 2026-10-03
(anker_c1000_protocol a2687-re/captures/20261003-181234, firmware 1.6.0.6):
FT 0x0F, the reply is opcode | 0x0800, the body  a1 01 21 · items · fe 04 <epoch>
unless `fe` says otherwise. A port's setters carry the port (0..2) in a2 and the
value in a3. The field names are the telemetry fields that confirm them.
]]
local function port(i)
  return {
    on = { opcode = 0x0207, fe = 'bare', values = { 0, 1 }, verified = true,
           items = { { tag = 0xa2, const = i }, { tag = 0xa3, enc = 'u8' } } },
    -- seconds, a u32 sent as type-04 bytes; 0 cancels the timer
    timer = { opcode = 0x0209, fe = 'bare', min = 0, max = 86400, verified = true,
              items = { { tag = 0xa2, const = i }, { tag = 0xa3, enc = 'u32b' } } },
  }
end
local P1, P2, P3 = port(0), port(1), port(2)
local function one(opcode, t)
  t.opcode = opcode
  t.fe = t.fe or 'bare'
  t.items = { { tag = 0xa2, enc = 'u8' } }
  t.verified = true
  return t
end

local SETTERS = {
  c1_on = P1.on, c2_on = P2.on, c3_on = P3.on,
  c1_timer_s = P1.timer, c2_timer_s = P2.timer, c3_timer_s = P3.timer,
  -- 0 AI, 1 Standard. Custom (2) carries its own block: custom_mode below.
  charging_protocol = one(0x0206, { values = { 0, 1 }, names = { ai = 0, standard = 1 } }),
  -- {mode, auto_exit, c1_w, c2_w, c3_w}: switches to Custom with those watts.
  -- The charger answers it twice -- status 4, then 0 -- and applies it.
  custom_mode = { opcode = 0x0206, fe = 'bare', kind = 'custom', busy_ok = true, verified = true },
  -- Standard mode's option: 0 C1 port priority, 1 dual-laptop
  fixed_allocation = one(0x0205, { values = { 0, 1 }, names = { c1_priority = 0, laptop_mode = 1 } }),
  backlight_pct = one(0x0204, { min = 25, max = 100 }),           -- the app's slider: 25..100
  screen_off = one(0x0203, { values = { 0, 1, 2, 3, 4 }, names = names(SCREEN_OFF) }),
  -- this firmware replies 0x0A0B (the app waits for an echo and times out)
  direction = one(0x020B, { values = { 0, 1, 2, 3 }, names = names(DIRECTION) }),
  gyroscope = one(0x020D, { values = { 0, 1 } }),                  -- auto-rotate
  language = one(0x0202, { values = { 0, 1, 2, 3 }, names = names(LANGUAGE) }),
  screensaver_enabled = one(0x021F, { values = { 0, 1 }, fe = 'typed' }),
  -- {c1, c2, c3} bit masks; all three ports in one command, no stamp. The
  -- charger reports no read-back: the gateway publishes what it last set.
  protocols = { opcode = 0x021D, fe = 'none', kind = 'protocols', verified = true, no_readback = true },
}

-- Flat fields beside the structured ones, so a write can be confirmed against
-- a plain value (and a script can read c2_on rather than c2_control.on).
local function derive(t)
  for i = 1, 3 do
    local c, p = t['c' .. i .. '_control'], t['c' .. i .. '_power']
    if type(c) == 'table' then
      t['c' .. i .. '_on'] = c.on and 1 or 0
      -- the timer as set, only while one runs: after a cancel the charger
      -- keeps the old countdown and remaining values with the task flag off
      t['c' .. i .. '_timer_s'] = c.countdown_active and c.countdown_s or 0
      t['c' .. i .. '_timer_remain_s'] = c.countdown_active and c.countdown_remain_s or 0
    end
    if type(p) == 'table' then t['c' .. i .. '_w'] = p.watts end
  end
  if type(t.screensaver) == 'table' then t.screensaver_enabled = t.screensaver.enabled and 1 or 0 end
end

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
  derive = derive,

  setters = SETTERS,
  setter_ft = 0x0F,
}
