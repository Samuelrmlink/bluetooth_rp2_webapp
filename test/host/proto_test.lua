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
print('\n== the profile ==')
-- One advertisement-driven stack now, in lua/profiles/anker/. The host
-- harness has no require(), so modules are loaded the way the device's loader
-- would and registered under the names they require each other by.
local function mod(name)
  local chunk = assert(loadfile('lua/profiles/' .. name .. '.lua'))
  return chunk()
end
package.loaded['profiles.anker.adv']    = mod('anker/adv')
package.loaded['profiles.anker.cipher'] = mod('anker/cipher')
package.loaded['profiles.anker.link']   = mod('anker/link')
local L = package.loaded['profiles.anker.link']

eq('link exposes the service uuid', L.SVC,
   '8c850001-0302-41c5-b46e-cf057c562025')
eq('opcode table names 0x0821', L.OPCODE[0x0821], 'getPublicKey.resp')
eq('status table explains a stale epoch', L.STATUS[0x06], 'stale timestamp (FE epoch)')

-- The A2687 telemetry decoders, now a model schema, against a synthesised
-- report carrying the documented shape.
do
  local schema = mod('anker/schema')
  local model  = mod('anker/models/A2687')
  eq('A2687 schema is for A2687', model.product_code, 'A2687')
  eq('A2687 is the SCP family', model.family, 'scp')

  local report = table.concat{
    bytes.unhex('a10131'),                             -- selector echo, ignored
    bytes.unhex('a303024606'),                         -- a3 u16 1606
    bytes.unhex('a4020100'),                           -- a4 u8 0  (error code)
    bytes.unhex('a50804' .. '01' .. '6023' .. '5e02' .. '1f02'),  -- c1 power, 7 B
    bytes.unhex('a8020102'),                           -- a8 u8, screen-off index 2
    bytes.unhex('a9020150'),                           -- a9 u8 backlight 80
    -- c1 control, 12 B: on, no countdown, cableInfo 3 = nothing plugged
    bytes.unhex('ac0d04' .. '01' .. '00' .. '00000000' .. '00000000' .. '03' .. '00'),
  }
  local d = assert(schema.decode(model, report, false))
  eq('a5 decodes to volts/amps/watts', d.c1_power and d.c1_power.mv, 0x2360)
  eq('a5 watts are centiwatts on the wire', d.c1_power and d.c1_power.watts, 5.43)
  eq('a8 is a preset index, not seconds', d.screen_off, '5min')
  eq('a9 backlight', d.backlight_pct, 80)
  eq('ac cableInfo 3 means nothing plugged', d.c1_control and d.c1_control.cable, 'none')
  eq('ac switch is on', d.c1_control and d.c1_control.on, true)
end

-- The productType and country tables.
do
  local lookup  = mod('anker/lookup')
  local country = mod('anker/country')
  local code, name = lookup.lookup('B105')
  eq('B105 resolves to A1761', code, 'A1761')
  check('B105 has a human name', name and name:find('C1000') ~= nil)
  eq('B405 resolves to A2687', (lookup.lookup('B405')), 'A2687')
  eq('an unknown productType resolves to nil', lookup.lookup('ZZZZ'), nil)
  -- Keyed by (pn, sku): a sku alone is not unique across products.
  eq('A1761 + 5YX is us', country.lookup('A1761', '5YX'), 'us')
  eq('A1761 + 5ZU is uk', country.lookup('A1761', '5ZU'), 'uk')
  eq('a sku from another pn does not leak', country.lookup('A1761', 'X2Y'), nil)
  -- The 4-char v2 skus are simply not in this table; nil, not a wrong answer.
  eq('a v2 sku is absent rather than guessed', country.lookup('A2687', 'DJW6'), nil)
end

-- ---------------------------------------------------------------------------
print('\n== the advertisement parser ==')
-- Vectors: the three worked examples in adv-manufacturer-data-re.md, with the
-- FFFF company id prepended because that is how bt.devices() reports it.
do
  local adv = mod('anker/adv')

  -- v1, bound, capability 0x00
  local a = assert(adv.decode('ffff' .. '01e8eeccb6897e02b10535595800'))
  eq('adv v1 version',      a.version,      1)
  eq('adv v1 mac',          a.mac,          'E8:EE:CC:B6:89:7E')
  eq('adv v1 bound',        a.bound,        true)
  eq('adv v1 productType',  a.product_type, 'B105')
  eq('adv v1 sku',          a.sku,          '5YX')
  eq('adv v1 capability',   a.capability,   0x00)
  eq('adv v1 gcm hint',     a.gcm_hint,     false)

  -- The same unit unbound, capability 0x04. Worth pinning: the capability
  -- byte CHANGES with bind state on one physical device, which is another
  -- reason it cannot be the sole input to a cipher decision.
  local b = assert(adv.decode('ffff' .. '01e8eeccb6897e00b10535595804'))
  eq('adv v1 unbound',        b.bound,      false)
  eq('adv v1 capability 04',  b.capability, 0x04)
  eq('adv v1 gcm hint set',   b.gcm_hint,   true)

  -- v2: 4-char sku, so the capability byte moves one later.
  local d = assert(adv.decode('ffff' .. '027ce913601fd802b405444a573604'))
  eq('adv v2 version',      d.version,      2)
  eq('adv v2 mac',          d.mac,          '7C:E9:13:60:1F:D8')
  eq('adv v2 productType',  d.product_type, 'B405')
  eq('adv v2 sku',          d.sku,          'DJW6')
  eq('adv v2 capability',   d.capability,   0x04)

  -- THE TRAP. Reading a v2 payload at the v1 offsets gives sku "DJW" and
  -- capability 0x36 -- and 0x36 has bit 2 set, so it still looks like a GCM
  -- capability. Both assertions are needed; either alone would pass.
  check('adv v2 sku is not truncated to 3 chars', d.sku ~= 'DJW')
  check('adv v2 capability is not the last sku char', d.capability ~= 0x36)

  -- Rejections.
  local nope, why = adv.decode('fffe' .. '01e8eeccb6897e00b10535595804')
  check('adv rejects a foreign company id', nope == nil)
  nope = adv.decode('ffff' .. '03e8eeccb6897e00b105')
  check('adv rejects an unknown version', nope == nil)
  nope = adv.decode('ffff' .. '01e8ee')
  check('adv rejects a truncated payload', nope == nil)

  -- Identifiable without a capability: bytes [0:10] are version-independent,
  -- so a short payload still yields the model, with capability 0.
  local short = assert(adv.decode('ffff' .. '01e8eeccb6897e00b105'))
  eq('adv short still identifies', short.product_type, 'B105')
  eq('adv short has no sku',       short.sku,          nil)
  eq('adv short capability is 0',  short.capability,   0)
  eq('adv short is not gcm',       short.gcm_hint,     false)

  -- identify() also checks the uuid and that the payload MAC agrees with the
  -- advertised address.
  local dev = { addr = '7C:E9:13:60:1F:D8', service_uuids = { 'ff09' },
                mfg_data = 'ffff027ce913601fd802b405444a573604' }
  local id = assert(adv.identify(dev))
  eq('identify returns the product type', id.product_type, 'B405')

  dev.service_uuids = { '180a' }
  check('identify rejects a non-Anker uuid', adv.identify(dev) == nil)

  dev.service_uuids = { 'ff09' }
  dev.addr = 'AA:BB:CC:DD:EE:FF'
  check('identify rejects a mac mismatch', adv.identify(dev) == nil)
end

-- Setter payloads, held to what the official app sent (solix-esp32
-- docs/anker-protocol.md §5a-iii: the AC countdown cancelled at epoch
-- 0x6AA0C8EB) and to the u8 shape the switches use.
print('\n== setters ==')
do
  local setter = mod('anker/setter')
  local model = mod('anker/models/A1761')
  local ac = model.setters.ac_countdown_s
  eq('AC timer cancel = the captured bytes', hex(setter.payload(ac, 0, 0x6AA0C8EB)),
     'a10121a2050300000000fe0503ebc8a06a')
  eq('AC timer 1 h', hex(setter.payload(ac, 3600, 0x6AA0C8EB)), 'a10121a20503100e0000fe0503ebc8a06a')
  eq('DC timer uses 0x0043', model.setters.car_countdown_s.opcode, 0x0043)
  eq('a switch is a2 02 01 <u8>', hex(setter.payload(model.setters.car_output_enabled, 1, 0)),
     'a10121a2020101fe050300000000')
  eq('a timer takes 0..86400', setter.check(ac, 86400), 86400)
  check('a timer refuses a day and a second', setter.check(ac, 86401) == nil)
  check('a timer refuses a negative', setter.check(ac, -1) == nil)
  check('a switch refuses 2', setter.check(model.setters.car_output_enabled, 2) == nil)
  check('not a number is refused', setter.check(ac, 'soon') == nil)
  local cp = model.setters.ac_charge_power_w
  eq('charge limit 700 W = the captured bytes', hex(setter.payload(cp, 700, 0x6ABE11FD)),
     'a10121a20302bc02fe0503fd11be6a')
  eq('charge limit uses 0x0044', cp.opcode, 0x0044)
  check('charge limit refuses 0', setter.check(cp, 0) == nil)
end

-- tlv.each, the report iterator schema.decode uses instead of tlv.parse (to
-- avoid a table per item). It must hand a reader EXACTLY what tlv.parse's item
-- would have: the same items in the same order, the same tag, the same raw
-- bytes, and the same value where a reader can use it (a number; or, for a
-- field of one byte or none, the string). Real C1000 reports, the A2687
-- sample, and fuzz -- floats, empty and one-byte fields, truncated tails.
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

  -- And the decoded C1000 report itself, pinned field by field: names come
  -- from the model's tag table, so a change to the iteration cannot move a
  -- value to another name without failing here.
  local schema = mod('anker/schema')
  local model  = mod('anker/models/A1761')
  local d = assert(schema.decode(model, bytes.unhex(reports[1]), true))
  eq('C1000: serial is the redacted fixture', d.serial_number, ('X'):rep(#(d.serial_number or '')))
  check('C1000: serial decoded at all', d.serial_number and #d.serial_number > 0)
  eq('C1000: every field counted', d.tlv_count, 68)

end

-- ---------------------------------------------------------------------------
print(('\n%d checks, %d failed'):format(checks, fails))
return fails == 0 and 0 or 1
