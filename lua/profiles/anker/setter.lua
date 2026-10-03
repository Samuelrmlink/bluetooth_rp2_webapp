--[[
A setter's value check and payload, without a link -- so the host test can
hold the exact bytes against captures (test/host/proto_test.lua).

Every setter body is  a1 01 21 · <value items> · <fe stamp>.

The C1000 (solix-esp32 docs/anker-protocol.md §6e) has ONE value item, a2,
and a typed stamp  fe 05 03 <epoch32 LE>. `enc` in the model picks the a2 item:
  u8    a2 02 01 <byte>              switches, the ambient light
  u16   a2 03 02 <2 bytes LE>        the AC charge limit, in watts
  u32   a2 05 03 <4 bytes LE>        the AC / DC countdown timers, in seconds

The A2687 (captured from the Anker app 2026-10-03, anker_c1000_protocol
a2687-re/captures/20261003-181234) differs in three ways, each declared in
the model rather than special-cased here:
  items   more than one value item: a port's setters are a2 <port> · a3 <value>
          ({ tag, const } is a fixed item, { tag, enc } carries the value)
  fe      'typed' (fe 05 03 <epoch>, the default and the C1000's), 'bare'
          (fe 04 <epoch>, most A2687 setters) or 'none' (0x021D has no stamp)
  kind    a value that is a table: 'custom' (the custom charging mode block)
          and 'protocols' (the three ports' fast-charge protocol masks)
Encodings for an item: u8 / u16 / u32 (types 01/02/03) and u32b, a u32 sent as
type 04 bytes (the A2687's port timer: a3 05 04 <secs LE>).

A setter lists its allowed `values`, or a `min` / `max` range; `names` lets a
value be given by name ("laptop_mode", "12h") as the telemetry shows it.
]]

local S = {}

local function le16(v) return string.char(v & 0xff, (v >> 8) & 0xff) end
local function le32(v)
  return string.char(v & 0xff, (v >> 8) & 0xff, (v >> 16) & 0xff, (v >> 24) & 0xff)
end

-- the value region of an item: its type byte, then the value
local function encode(enc, v)
  if enc == 'u32' then return '\x03' .. le32(v) end
  if enc == 'u32b' then return '\x04' .. le32(v) end
  if enc == 'u16' then return '\x02' .. le16(v) end
  if enc == 'bytes' then return '\x04' .. v end
  return '\x01' .. string.char(v)
end

local function int(x)
  if x == true then return 1 elseif x == false then return 0 end
  return math.tointeger(tonumber(x))
end

-- Fast-charge protocol bits, per port (0x021D): bit0 UFCS, bit1 SCP, bit2 PD
-- 12V, bit3 PPS 5-11V, bit4 PPS 5-16V, bit5 PPS 4.5-21V. At most three of the
-- four PD bits (2..5) at once -- the app greys the fourth out.
S.PROTOCOL_BITS = { ufcs = 0, scp = 1, pd12v = 2, pps11 = 3, pps16 = 4, pps21 = 5 }

local function check_custom(v)
  if type(v) ~= 'table' then return nil, 'takes {mode, auto_exit, c1_w, c2_w, c3_w}' end
  local t = {
    mode = int(v.mode or v.mode_number or v[1] or 1),
    auto_exit = int(v.auto_exit == nil and 1 or v.auto_exit),
    c1_w = int(v.c1_w or v[3]), c2_w = int(v.c2_w or v[4]), c3_w = int(v.c3_w or v[5]),
  }
  if not t.mode or t.mode < 1 or t.mode > 254 then return nil, 'mode takes 1..254' end
  if t.auto_exit ~= 0 and t.auto_exit ~= 1 then return nil, 'auto_exit takes 0 or 1' end
  local total = 0
  for _, k in ipairs{ 'c1_w', 'c2_w', 'c3_w' } do
    local w = t[k]
    if not w then return nil, k .. ' is missing' end
    -- the app's slider: 0, or 15..140 W
    if w ~= 0 and (w < 15 or w > 140) then return nil, k .. ' takes 0 or 15..140' end
    total = total + w
  end
  if total > 160 then return nil, ('the ports add up to %d W; the charger has 160'):format(total) end
  return t
end

local function check_protocols(v)
  if type(v) ~= 'table' then return nil, 'takes {c1, c2, c3} bit masks (0..63)' end
  local t = {}
  for i = 1, 3 do
    local m = int(v['c' .. i] or v[i])
    if not m or m < 0 or m > 63 then return nil, ('c%d takes 0..63'):format(i) end
    local pd = 0
    for b = 2, 5 do if m & (1 << b) ~= 0 then pd = pd + 1 end end
    if pd > 3 then return nil, ('c%d: at most three of PD 12V and the PPS ranges'):format(i) end
    t['c' .. i] = m
  end
  return t
end

-- The value, normalized (an integer, or a table for a 'kind'), or nil and why not.
function S.check(s, value)
  if s.kind == 'custom' then return check_custom(value) end
  if s.kind == 'protocols' then return check_protocols(value) end
  if type(value) == 'string' and s.names and s.names[value] ~= nil then value = s.names[value] end
  local v = int(value)
  if v == nil then
    if s.names then
      local n = {}
      for k in pairs(s.names) do n[#n + 1] = k end
      table.sort(n)
      return nil, 'takes ' .. table.concat(n, ', ')
    end
    return nil, 'not a whole number'
  end
  if s.values then
    for _, a in ipairs(s.values) do if a == v then return v end end
    return nil, 'takes ' .. table.concat(s.values, ', ')
  end
  if (s.min and v < s.min) or (s.max and v > s.max) then
    return nil, ('takes %d..%d'):format(s.min or 0, s.max or 0)
  end
  local enc = s.enc or (s.items and s.items[#s.items].enc) or 'u8'
  if enc == 'u8' and (v < 0 or v > 255) then return nil, 'takes 0..255' end
  if enc == 'u16' and (v < 0 or v > 0xffff) then return nil, 'takes 0..65535' end
  return v
end

-- The a2 item's value region of a one-item (C1000-style) setter.
function S.value_bytes(s, v)
  return encode(s.enc, v)
end

-- The value items, in order: { {tag, value-region}, ... }
local function value_items(s, v)
  if s.kind == 'custom' then
    return { { 0xa2, encode('u8', 2) },
             { 0xa3, encode('bytes', string.char(v.mode, v.auto_exit, v.c1_w, v.c2_w, v.c3_w)) } }
  end
  if s.kind == 'protocols' then
    return { { 0xa2, encode('bytes', le16(v.c1) .. le16(v.c2) .. le16(v.c3)) } }
  end
  if s.items then
    local out = {}
    for _, it in ipairs(s.items) do
      out[#out + 1] = { it.tag, encode(it.enc or 'u8', it.const ~= nil and it.const or v) }
    end
    return out
  end
  return { { 0xa2, S.value_bytes(s, v) } }
end

function S.payload(s, v, epoch)
  local items = { { tag = 0xa1, enc = 'bytes', value = '\x21', type_byte = false } }
  for _, it in ipairs(value_items(s, v)) do
    items[#items + 1] = { tag = it[1], enc = 'bytes', value = it[2], type_byte = false }
  end
  local fe = s.fe or 'typed'
  if fe == 'typed' then
    items[#items + 1] = { tag = 0xfe, enc = 'bytes', value = '\x03' .. le32(epoch), type_byte = false }
  elseif fe == 'bare' then
    items[#items + 1] = { tag = 0xfe, enc = 'bytes', value = le32(epoch), type_byte = false }
  end
  return tlv.build(items)
end

-- The "value" member of a request's raw JSON, when it is an object or an
-- array of numbers / booleans ({"c1":59,...} or [0,59,59]) -- what the flat
-- params decoder cannot hand over. nil when there is no such member.
function S.value_from_json(json)
  local open = json:match('"value"%s*:%s*()[%[{]')
  if not open then return nil end
  local close = json:sub(open, open) == '{' and '}' or ']'
  local depth, stop = 0, nil
  for i = open, #json do
    local c = json:sub(i, i)
    if c == '{' or c == '[' then depth = depth + 1
    elseif c == '}' or c == ']' then
      depth = depth - 1
      if depth == 0 then stop = i; break end
    end
  end
  if not stop or json:sub(stop, stop) ~= close then return nil end
  local body = json:sub(open + 1, stop - 1)
  local function num(x)
    if x == 'true' then return 1 elseif x == 'false' then return 0 end
    return math.tointeger(tonumber(x)) or tonumber(x)
  end
  local out = {}
  if close == '}' then
    for k, x in body:gmatch('"([%w_]+)"%s*:%s*(%-?[%w%.]+)') do out[k] = num(x) end
  else
    for x in body:gmatch('(%-?[%w%.]+)') do out[#out + 1] = num(x) end
  end
  return out
end

-- A value for a log line: numbers as they are, tables as k=v pairs.
function S.show(v)
  if type(v) ~= 'table' then return tostring(v) end
  local keys = {}
  for k in pairs(v) do keys[#keys + 1] = tostring(k) end
  table.sort(keys)
  local parts = {}
  for _, k in ipairs(keys) do parts[#parts + 1] = k .. '=' .. tostring(v[k] or v[tonumber(k)]) end
  return '{' .. table.concat(parts, ' ') .. '}'
end

return S
