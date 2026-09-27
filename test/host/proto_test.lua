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
-- The profile is four modules now (see cipher.lua for why). The host harness
-- has no require(), so load them the way the device's loader would and stitch
-- the pieces this test needs.
local function mod(name)
  local chunk = assert(loadfile('lua/profiles/' .. name .. '.lua'))
  return chunk()
end
package.loaded['profiles.anker_cipher'] = mod('anker_cipher')
package.loaded['profiles.anker_handshake'] = mod('anker_handshake')
package.loaded['profiles.anker_telemetry'] = mod('anker_telemetry')
local a = mod('anker')

eq('profile exposes the service uuid', a.SVC, '8c850001-0302-41c5-b46e-cf057c562025')
eq('opcode table names 0x0821', a.OPCODE[0x0821], 'getPublicKey.resp')
eq('status table explains a stale epoch', a.STATUS[0x06], 'stale timestamp (FE epoch)')

-- Telemetry decode against a synthesised report carrying the documented shape.
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
local d = a.decode_report(report)
eq('a5 decodes to volts/amps/watts', d.c1_power and d.c1_power.mv, 0x2360)
eq('a5 watts are centiwatts on the wire', d.c1_power and d.c1_power.watts, 5.43)
eq('a8 is a preset index, not seconds', d.screen_off, '5min')
eq('a9 backlight', d.backlight_pct, 80)
eq('ac cableInfo 3 means nothing plugged', d.c1_control and d.c1_control.cable, 'none')
eq('ac switch is on', d.c1_control and d.c1_control.on, true)

print('\n== advertisement decode (captured from the bench unit) ==')
-- Real bytes off the air, 2026-09-26. The company id is 0xFFFF on every
-- current Anker device, so the model is the productType at [8:10].
local adv = a.decode_adv('ffff027ce913601fd802b405444a573604')
eq('adv version', adv and adv.version, 2)
eq('adv mac', adv and adv.mac, '7C:E9:13:60:1F:D8')
eq('adv bound', adv and adv.bound, true)
eq('adv productType is the model', adv and adv.product_type, 'B405')
eq('adv sku is 4 chars on v2', adv and adv.sku, 'DJW6')
eq('adv capability bit 2 = AES-GCM', adv and adv.gcm, true)
eq('matches() accepts the A2687', a.matches({ mfg_data = 'ffff027ce913601fd802b405444a573604' }), true)
-- A C1000 (B105) must NOT match this profile.
eq('matches() rejects a C1000', a.matches({ mfg_data = 'ffff027ce913601fd802b105444a573604' }), false)
eq('matches() rejects a non-Anker advert', a.matches({ mfg_data = '4c0012020001' }), false)
eq('matches() survives no mfg data at all', a.matches({}), false)
-- v1 puts capability one byte earlier, because its sku is 3 chars not 4.
-- v1: 3-char sku, so 14 bytes total against v2's 15.
local v1 = a.decode_adv('ffff017ce913601fd800b405444a5704')
eq('adv v1 sku is 3 chars', v1 and v1.sku, 'DJW')
eq('adv v1 unbound', v1 and v1.bound, false)

-- ---------------------------------------------------------------------------
-- C1000 (A1761) 0x0840 DeviceAllBaseInfo
--
-- A REAL decrypted status body, captured from the unit on the bench after the
-- ecdh-legacy session was established. Frozen here so the field map can be
-- corrected without a charger in the room, and so the two mistakes that made
-- it through hardware testing cannot come back:
--
--   1. parsing a report with the NEGOTIATION dialect, which shifts every
--      scalar by one byte and turns min_soc 60 into 15361 -- a plausible
--      number, which is the dangerous kind of wrong;
--   2. reading a field's width from the map instead of from the bytes.
-- ---------------------------------------------------------------------------
local C1000_STATUS = table.concat{
  'a10131a2050300000000a3050300000000a40302c30da503020000a603020000a703020000a803020000a903020000aa',
  '03020000ab03020000ac03020000ad03020000ae03020000af03020000b003020000b103020000b203020100b30302a6',
  '00b403020000b503027300b603027300b703020000b803029a00b903020000ba0302a600bb03020100bc020100bd0201',
  '29be020100bf020100c0020100c1020164c2020100c3020163c4020100c5020100c6020100c7020100c8020100c90201',
  '00ca020100cb020100cc020101cd020100ce020100cf020100d0110041504335595830443433323030313138d1030258',
  '02d20302d002d303020807d403023c00d503020000d603020000d7020101d8020101d9020180da02013cdb020100dc02',
  '0100dd020101de020100e3020100e5020100f7050301000000f815040201010100010000000000000000000000000000',
  'f9020101fd0b0041313736315f33304168',
}

do
  local T = mod('c1000_telemetry')
  local body = bytes.unhex(C1000_STATUS)
  check('c1000 status body is 353 bytes', #body == 353)

  local t = assert(T.decode(body, true))
  check('c1000 tlv count', t.tlv_count == 68)

  -- Anchored to values readable off the unit itself, not to the reference
  -- table: these are the ones we can be sure of.
  eq('c1000 model_name',   t.model_name,   'A1761_30Ah')
  eq('c1000 serial',       t.serial_number, 'APC5YX0D43200118')

  -- Values that agree with the reference capture in the protocol notes.
  eq('c1000 min_soc_pct',           t.min_soc_pct, 60)
  eq('c1000 max_discharge_soc_pct', t.max_discharge_soc_pct, 99)
  eq('c1000 max_charge_soc_pct',    t.max_charge_soc_pct, 100)
  eq('c1000 battery_health_pct',    t.battery_health_pct, 115)
  eq('c1000 hw_version',            t.hw_version, 1)
  eq('c1000 idle_timeout_min',      t.idle_timeout_min, 60)
  eq('c1000 max_ac_input_w',        t.max_ac_input_w, 720)
  eq('c1000 ac_enabled',            t.ac_enabled, 1)
  eq('c1000 dc_enabled',            t.dc_enabled, 1)
  eq('c1000 error_code',            t.error_code, 0)

  -- The regression this locks: with the negotiation dialect these read 15361,
  -- 25345 and 257 respectively. All three look like real numbers.
  check('c1000 u8 fields are not byte-shifted',
        t.min_soc_pct < 256 and t.max_discharge_soc_pct < 256 and t.ac_enabled < 256)

  -- Unknown tags are passed through rather than dropped: this firmware emits
  -- tags the reference table does not have, and they are what to identify next.
  check('c1000 unknown tags are reported', t.unknown ~= nil and t.unknown.f8 ~= nil)
end

-- ---------------------------------------------------------------------------
print(('\n%d checks, %d failed'):format(checks, fails))
return fails == 0 and 0 or 1
