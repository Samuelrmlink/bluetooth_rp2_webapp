--[[
Decoding 0x0840 DeviceAllBaseInfo for the C1000 (A1761).

Field map from the ACT-1 plaintext capture; tags marked confirmed there are
the ones whose meaning is anchored to a value we could read off the unit
(serial, capacity, model name, temperature scale). The rest come from the
solix-bluetooth reference table and are believed-correct rather than proven,
so unknown tags are PASSED THROUGH as hex rather than dropped -- a field we
cannot name is still evidence, and silently discarding it is how a field map
stops improving.

This unit's firmware (0.0.0.3) emits tags past where the reference table
stops, which is exactly why the passthrough matters.
]]

local T = {}

-- Accessors, same idiom as the A2687 module and for the same reason: a tag's
-- TYPE BYTE is not something to bet on. tlv.parse turns u8/u16/u32 into a Lua
-- number and everything else into a string, so a decoder that assumes one or
-- the other throws the moment a firmware picks a different type byte for a
-- field. it.raw is the value region INCLUDING the type byte and is always a
-- string, so width is read from what actually arrived.
local function raw(it) return (it and it.raw) and it.raw:sub(2) or '' end

local function num(it)
  if type(it.value) == 'number' then return it.value end
  local b = raw(it)
  if #b == 1 then return b:byte(1) end
  if #b == 2 then return b:byte(1) | (b:byte(2) << 8) end
  if #b == 4 then
    return b:byte(1) | (b:byte(2) << 8) | (b:byte(3) << 16) | (b:byte(4) << 24)
  end
  return nil
end

-- Signed reads take the width from the bytes present rather than the map, so
-- a field the reference calls s16 that arrives as one byte still decodes.
local function snum(it)
  local v = num(it)
  if v == nil then return nil end
  local w = #raw(it)
  if w == 2 and v >= 0x8000 then return v - 0x10000 end
  if w == 1 and v >= 0x80 then return v - 0x100 end
  return v
end

local function str(it)
  local b = raw(it)
  if b == '' and type(it.value) == 'string' then b = it.value end
  return (b:gsub('%z.*$', ''))
end

-- tag -> { name, reader, scale }
local F = {
  a2 = { 'counter_a2',        num },
  a3 = { 'counter_a3',        num },
  a4 = { 'wifi_rssi',         snum },
  a5 = { 'ac_power_in_w',     num },
  a6 = { 'ac_power_out_w',    num },
  a7 = { 'dc_power_out_w',    num },
  a8 = { 'type_c_power_out_w',num },
  a9 = { 'usb_power_out_w',   num },
  aa = { 'total_output_w',    num },
  ab = { 'battery_pct',       num },
  ac = { 'battery_power_w',   snum },   -- + charging / - discharging
  ad = { 'battery_voltage',   num, 0.01 },
  ae = { 'solar_power_in_w',  num },
  af = { 'ac_switch',         num },
  b0 = { 'dc_switch',         num },
  b1 = { 'charging_state',    num },
  b2 = { 'online_status',     num },
  b3 = { 'temperature_c',     num, 0.1 },
  b4 = { 'error_code',        num },
  b5 = { 'battery_cycles',    num },
  b6 = { 'battery_health_pct',num },
  b8 = { 'battery_resistance',num },
  ba = { 'battery_temp_c',    num, 0.1 },
  bb = { 'ac_output_active',  num },
  bc = { 'led_mode',          num },
  bd = { 'current_hour',      num },
  c0 = { 'charge_speed',      num },
  c1 = { 'max_charge_soc_pct',num },
  c3 = { 'max_discharge_soc_pct', num },
  c7 = { 'solar_pv1_w',       num },
  c8 = { 'solar_pv2_w',       num },
  cc = { 'dc_output_active',  num },
  d1 = { 'capacity_wh',       num },
  d2 = { 'max_ac_input_w',    num },
  d3 = { 'display_timeout_s', num },
  d4 = { 'idle_timeout_min',  num },
  d5 = { 'ups_mode',          num },
  d6 = { 'ups_reserve_pct',   num },
  d7 = { 'ac_enabled',        num },
  d8 = { 'dc_enabled',        num },
  d9 = { 'light_mode',        num },   -- 0 off, 1 on, 2 auto
  da = { 'min_soc_pct',       num },
  f9 = { 'hw_version',        num },
}

-- Strings rather than numbers.
local STR = { d0 = 'serial_number', fd = 'model_name' }

function T.decode(body, want_unknown)
  -- false = the REPORT dialect, not the negotiation one. The two differ in
  -- whether a type byte is present, so parsing a status reply as negotiation
  -- shifts every scalar by one byte: u8 fields came out as 0x_01 pairs
  -- (min_soc 60 read as 15361, ac_enabled 1 as 257) -- plausible-looking
  -- numbers, which is the dangerous kind of wrong.
  local t = tlv.parse(body, false)
  if not t then return nil, 'tlv parse failed' end
  local out, unknown = {}, {}
  for i = 1, (t.n or 0) do
    local it = t[i]
    local tag = it and it.hex
    if tag then
      local spec = F[tag]
      if spec then
        local v = spec[2](it)
        if v and spec[3] then v = v * spec[3] end
        out[spec[1]] = v
      elseif STR[tag] then
        out[STR[tag]] = str(it)
      elseif tag ~= 'a1' and want_unknown then
        -- Not in the map. Worth reporting -- this firmware emits tags the
        -- reference table does not have, and they are the next thing to
        -- identify -- but only ONCE. The set does not change between polls,
        -- and hexing ~20 of them every five seconds churned the arena badly
        -- enough to exhaust it after about ten reports: 81 KB used of 131 KB
        -- and still no block large enough, which is fragmentation rather
        -- than a leak.
        unknown[tag] = bytes.hex(raw(it))
      end
    end
  end
  if want_unknown and next(unknown) then out.unknown = unknown end
  out.tlv_count = t.n
  return out
end

return T
