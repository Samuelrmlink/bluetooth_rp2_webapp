--[[
A setter's value check and payload, without a link -- so the host test can
hold the exact bytes against captures (test/host/proto_test.lua).

Every C1000 setter is  a1 01 21 · a2 <len> <type> <value> · fe 05 03 <epoch32 LE>
(solix-esp32 docs/anker-protocol.md §6e). `enc` in the model picks the a2 item:
  u8    a2 02 01 <byte>              switches, the ambient light
  u16   a2 03 02 <2 bytes LE>        the AC charge limit, in watts
  u32   a2 05 03 <4 bytes LE>        the AC / DC countdown timers, in seconds
A setter lists its allowed `values`, or a `min` / `max` range.
]]

local S = {}

local function le32(v)
  return string.char(v & 0xff, (v >> 8) & 0xff, (v >> 16) & 0xff, (v >> 24) & 0xff)
end

-- The value as an integer, or nil and why not.
function S.check(s, value)
  local v = math.tointeger(tonumber(value))
  if v == nil then return nil, 'not a whole number' end
  if s.values then
    for _, a in ipairs(s.values) do if a == v then return v end end
    return nil, 'takes ' .. table.concat(s.values, ', ')
  end
  if (s.min and v < s.min) or (s.max and v > s.max) then
    return nil, ('takes %d..%d'):format(s.min or 0, s.max or 0)
  end
  if s.enc == 'u8' and (v < 0 or v > 255) then return nil, 'takes 0..255' end
  if s.enc == 'u16' and (v < 0 or v > 0xffff) then return nil, 'takes 0..65535' end
  return v
end

-- The a2 item's value region: the type byte, then the value.
function S.value_bytes(s, v)
  if s.enc == 'u32' then return '\x03' .. le32(v) end
  if s.enc == 'u16' then return '\x02' .. string.char(v & 0xff, (v >> 8) & 0xff) end
  return '\x01' .. string.char(v)
end

function S.payload(s, v, epoch)
  return tlv.build{
    { tag = 0xa1, enc = 'bytes', value = '\x21', type_byte = false },
    { tag = 0xa2, enc = 'bytes', value = S.value_bytes(s, v), type_byte = false },
    { tag = 0xfe, enc = 'bytes', value = '\x03' .. le32(epoch), type_byte = false },
  }
end

return S
