-- The protocol's Lua half, tested on a host against the real C bindings.
--
-- What this pins is the thing most likely to be wrong and least likely to
-- announce it: the exact bytes of every frame the profile builds. A wrong
-- type byte or a missing FE trailer produces a frame the charger silently
-- ignores, and on hardware that is indistinguishable from a dead link.
--
-- The expected bytes come from a2687-re/A2687-CRACKED.md, where they were
-- captured from the official app.

local fails, checks = 0, 0

local function eq(name, got, want)
  checks = checks + 1
  if got ~= want then
    fails = fails + 1
    print(('FAIL %s\n       got  %s\n       want %s'):format(name, tostring(got), tostring(want)))
  else
    print('OK   ' .. name)
  end
end

local function check(name, cond)
  checks = checks + 1
  if not cond then
    fails = fails + 1
    print(('FAIL %s\n       condition was false'):format(name))
  else
    print('OK   ' .. name)
  end
end

local function hex(s) return bytes.hex(s) end
local function le32(v)
  return string.char(v & 0xff, (v >> 8) & 0xff, (v >> 16) & 0xff, (v >> 24) & 0xff)
end

-- ---------------------------------------------------------------------------
print('\n== bytes ==')
eq('hex round trip', hex(bytes.unhex('00ff10ab')), '00ff10ab')
eq('xor stops at the shorter operand', hex(bytes.xor('\xff\xff\xff', '\x0f\xf0')), 'f00f')

-- ---------------------------------------------------------------------------
print('\n== frame ==')
-- ff09 | len | 03 | slave | ft | feat^opHi | opLo | payload | xor
local function xorsum(hexstr)
  local x = 0
  for i = 1, #hexstr, 2 do x = x ~ tonumber(hexstr:sub(i, i + 1), 16) end
  return string.format('%02x', x)
end
-- ff09 | len=16 LE | ver 03 | slave 00 | ft 01 | feat^opHi 00 | opLo 01 | payload | xor
local WANT0001 = 'ff09' .. '1000' .. '03' .. '00' .. '01' .. '00' .. '01' .. 'a10400000000'
local f = frame.build(0x01, 0x0001, false, 0, '\xa1\x04\x00\x00\x00\x00')
eq('frame.build 0x0001 plain', hex(f), WANT0001 .. xorsum(WANT0001))

local p = frame.parse(f)
eq('frame.parse opcode', p.opcode, 0x0001)
eq('frame.parse checksum_ok', p.checksum_ok, true)
eq('frame.parse length_ok', p.length_ok, true)
eq('frame.parse payload', hex(p.payload), 'a10400000000')
eq('frame.parse not encrypted', p.encrypted, false)

-- The encrypted bit is bit 6 of byte 7, XORed with the opcode high nibble.
local e = frame.build(0x01, 0x0021, true, 0, 'x')
eq('frame.build sets the encrypted bit', frame.parse(e).encrypted, true)
eq('0x0821 is seen as a response', frame.parse(frame.build(0x01, 0x0821, false, 0, '')).is_response, true)
eq('0x0300 is NOT a response (trap 3)', frame.parse(frame.build(0x11, 0x0300, false, 0, '')).is_response, false)

-- ---------------------------------------------------------------------------
print('\n== tlv.build: the exact auth-burst payloads ==')
-- 0x021d setPortTimers: A1 01 21 · A2 07 04 3B 00 3B 00 3B 00.  Trap 7: no FE.
eq('0x021d', hex(tlv.build{
  { tag = 0xa1, enc = 'u8',    value = 0x21, type_byte = false },
  { tag = 0xa2, enc = 'bytes', value = '\x3b\x00\x3b\x00\x3b\x00', type_byte = 0x04 },
}), 'a1012' .. '1a207043b003b003b00')

-- 0x0200 getAllDeviceInfo: A1 01 21 · F0 05 03 01 00 00 00 · FE 04 <epoch>
eq('0x0200', hex(tlv.build{
  { tag = 0xa1, enc = 'u8',    value = 0x21, type_byte = false },
  { tag = 0xf0, enc = 'u32',   value = 1,    type_byte = 0x03 },
  { tag = 0xfe, enc = 'bytes', value = le32(0), type_byte = false },
}), 'a10121f005030100000' .. '0fe0400000000')

-- 0x0005 setCapability, negotiation dialect (no type byte anywhere).
-- Trap 6: a4 02 29 01 is u16 LE 297.
eq('0x0005', hex(tlv.build{
  { tag = 0xa1, enc = 'bytes', value = le32(0),  type_byte = false },
  { tag = 0xa3, enc = 'bytes', value = '\x20',   type_byte = false },
  { tag = 0xa4, enc = 'bytes', value = '\x29\x01', type_byte = false },
  { tag = 0xa5, enc = 'bytes', value = '\x44',   type_byte = false },
  { tag = 0xa6, enc = 'bytes', value = '\x02',   type_byte = false },
}), 'a10400000000a30120a40229' .. '01a5014' .. '4a60102')

local t = tlv.parse(bytes.unhex('a4022901'), true)
eq('trap 6: 0x2901 parses as 297, not 2901', t.a4.value, 297)

-- ---------------------------------------------------------------------------
print('\n== tlv.parse ==')
-- Charging dialect: tag, len, typeByte, value. a5 07 01 <status><mV><mA><cW>
-- 7 data bytes + the type byte = length 8
local port = bytes.unhex('a50804' .. '01' .. '6023' .. '5e02' .. '1f02')
local pt = tlv.parse(port, false)
eq('charging item found by hex name', pt.a5 ~= nil, true)
eq('array part keeps wire order', pt.n, 1)
eq('raw includes the type byte', hex(pt.a5.raw), '04016023' .. '5e021f02')

local dup = tlv.parse(bytes.unhex('a1012' .. '1a10122'), false)
eq('a repeated tag is not lost', dup.n, 2)
-- length 1 is a bare selector: no type byte, so the parser cannot call it
-- a number and hands back the raw byte.
eq('first occurrence wins the named slot', dup.a1.value, '\x21')

-- ---------------------------------------------------------------------------
print('\n== tlv.stamp_fe (both FE forms) ==')
local typed = bytes.unhex('a10121fe050300000000')
local out, n = tlv.stamp_fe(typed, 0x11223344)
eq('typed fe 05 03 stamped', hex(out), 'a10121fe05034433221' .. '1')
eq('typed fe count', n, 1)

local bare = bytes.unhex('a1040000000' .. '0fe0400000000')
out, n = tlv.stamp_fe(bare, 0x11223344)
eq('bare fe 04 stamped', hex(out), 'a10400000000fe0444332211')
eq('bare fe count', n, 1)

-- ---------------------------------------------------------------------------
print('\n== crypto ==')
-- The session cipher. Nonce reuse is the device's behaviour, not ours: the
-- same key/nonce must produce the same keystream on every call.
local key = bytes.unhex('000102030405060708090a0b0c0d0e0f')
local iv  = bytes.unhex('0f0e0d0c0b0a09080706050403')  -- 13 B, deliberately wrong
local okc = pcall(crypto.gcm_encrypt, key, iv, '', 'hi')
eq('gcm rejects a nonce that is not 12 bytes', okc, false)

iv = bytes.unhex('0f0e0d0c0b0a090807060504')
local aad = bytes.unhex('3322110077665544bbaa9988ffeeddcc')
local c1 = crypto.gcm_encrypt(key, iv, aad, 'hello world')
local c2 = crypto.gcm_encrypt(key, iv, aad, 'hello world')
eq('gcm is deterministic under a reused nonce', hex(c1), hex(c2))
eq('gcm output is plaintext + 16 byte tag', #c1, 11 + 16)
eq('gcm decrypt round trip', crypto.gcm_decrypt(key, iv, c1), 'hello world')
eq('gcm decrypt ignores a corrupted tag (by design)',
   crypto.gcm_decrypt(key, iv, c1:sub(1, #c1 - 1) .. '\x00'), 'hello world')
eq('recomputed tag matches the one encrypt produced',
   hex(crypto.gcm_tag(key, iv, aad, c1:sub(1, 11))), hex(c1:sub(12)))

-- ---------------------------------------------------------------------------
-- The Anker profile checks (advertisement, setters, history, screensaver)
-- moved with the protocol into pex/modules/drivers/. host_test.c covers
-- those vectors.

print('\n== tlv.each ==')
do
  local reports = dofile('test/host/c1000_reports.lua')
  local function same_as_parse(body)
    local t = tlv.parse(body, false)
    local i = 0
    for h, raw, value in tlv.each(body) do
      i = i + 1
      local it = t[i]
      if not it or it.hex ~= h or it.raw ~= raw then return false, 'item ' .. i end
      local want = it.value
      if type(want) ~= 'number' and #raw > 1 then want = nil end
      local both_nan = value ~= value and want ~= want   -- a fuzzed f32 can be NaN
      if value ~= want and not both_nan then return false, ('item %d value %s vs %s'):format(i, tostring(value), tostring(want)) end
    end
    if i ~= t.n then return false, ('count %d vs %d'):format(i, t.n) end
    return true
  end
  for k, h in ipairs(reports) do
    local ok, why = same_as_parse(bytes.unhex(h))
    check(('real C1000 report %d: each == parse%s'):format(k, ok and '' or ' (' .. why .. ')'), ok)
  end

  -- Fuzz, seeded so a failure reproduces. Bodies are built from items with
  -- every type byte (0x00..0x07 and junk), lengths 0, 1 and up, and a random
  -- tail that may be a truncated item.
  math.randomseed(1761)
  local bad, worst = 0, nil
  for _ = 1, 3000 do
    local parts = {}
    for _ = 1, math.random(0, 12) do
      local n = ({0, 1, 2, 3, 5, math.random(0, 20)})[math.random(1, 6)]
      local v = {}
      if n >= 1 then v[1] = string.char(({0, 1, 2, 3, 4, 5, 6, 0x41, 0x7f, 0xff})[math.random(1, 10)]) end
      for j = 2, n do v[j] = string.char(math.random(0, 255)) end
      parts[#parts + 1] = string.char(math.random(0, 255), n) .. table.concat(v)
    end
    if math.random() < 0.3 then parts[#parts + 1] = string.char(math.random(0, 255), 40, 1, 2) end
    local body = table.concat(parts)
    local ok, why = same_as_parse(body)
    if not ok then bad = bad + 1; worst = worst or (bytes.hex(body) .. ': ' .. why) end
  end
  check('3000 fuzzed bodies: each == parse' .. (worst and (' -- first: ' .. worst) or ''), bad == 0)

end

-- ---------------------------------------------------------------------------
print(('\n%d checks, %d failed'):format(checks, fails))
return fails == 0 and 0 or 1
