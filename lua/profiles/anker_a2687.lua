--[[
Anker A2687 (BLE model A17A5) -- the whole device-specific half of the protocol.

Everything here is data or sequence: opcode names, the negotiation keystream,
the handshake stages, the auth burst, the telemetry dictionary. None of it is
an algorithm; the algorithms (frame header, XOR checksum, both TLV dialects,
AES/GCM/ECDH) are C and are pinned by frozen vectors. The split is by RATE OF
CHANGE, not by layer: this file is what you edit against a real charger, over
the websocket, with no flash cycle. That is the point of the whole design.

  frame.*  tlv.*  crypto.*  bytes.*   -- the C primitives
  bt.*                                -- generic BLE, no handles anywhere
  bus.emit / log.*                    -- out to the browser

THE EIGHT TRAPS. Each of these cost real time to find, and each is invisible
until it is not. They are written here because this is the file someone will
be reading when one of them bites.

 1. A PLAINTEXT 0x0021 SILENTLY POISONS THE SESSION. The charger accepts the
    frame, ignores the key inside it, derives a different secret, and then
    NAKs everything with a canned 16-byte error -- b0733cc3... or 4c4d807e...,
    byte-identical across sessions. Seeing either constant means exactly this
    and nothing else. nak_reason() below detects it and says so, because the
    symptom otherwise looks like a wrong userId.
 2. ROUTE functionType 0x11 AS CHARGING. The A2687 answers on 0x11, not the
    C1000's 0x0F. Filtering on 0x0F alone drops 0x0a00, 0x0a0a, 0x0a1d and
    every telemetry report -- the device looks dead while working perfectly.
    This one stalled the previous attempt.
 3. 0x0300 HAS NO STATUS BYTE; 0x0a00 DOES. Strip only when opcodeHi & 0x08.
    Stripping from a report shifts everything by one and yields phantom tags
    01/02. This bug actually shipped once.
 4. 0x0030 AND 0x0830 USE THE NEGOTIATION DIALECT ON AN ft=0x0F FRAME. The two
    dialects differ by a type byte. Run the charging parser over these and it
    reads 'A' as a type byte and mangles every string in the reply.
 5. 67 BYTES OF KEYSTREAM IS ALL THERE IS, and stage 0x0021 already uses 66 of
    them. Stages 1-5 cannot grow. There is no more keystream to recover: one
    reused nonce gives you exactly one keystream.
 6. a4 02 29 01 IS u16 LE 297, NOT 2901. The notes write it as the hex digits
    and it reads like a decimal. It is the MTU.
 7. 0x021d TAKES NO FE TRAILER; every other setter does. And FE itself has two
    forms -- bare `fe 04 <e32>` and typed `fe 05 03 <e32>` -- which are not
    interchangeable per opcode.
 8. 0x0821 TAKES ABOUT 900 ms, because the charger is doing P-256 in software.
    Budget seconds. A 500 ms timeout here looks exactly like a dead device.
]]

local M = {}

-- ---------------------------------------------------------------------------
-- GATT addressing. NEVER filter a scan on these. 8c850001 is a GATT service
-- UUID and is invisible until after the connection, so a scan filtered on it
-- finds nothing, every time.
--
-- THREE DIFFERENT THINGS SHARE THE DIGITS FF09 AND NONE OF THEM IS ANOTHER:
--
--   AD type 0x03 (Complete List of 16-bit Service Class UUIDs) carries the
--   value 0xFF09, on air as the bytes 09 ff. THIS is what is advertised and
--   what a scan can filter on.
--
--   AD type 0xFF (Manufacturer Specific Data) is a separate structure in the
--   same advertisement. Its first two octets are the Company Identifier,
--   0xFFFF -- the reserved "no company" value, which every current Anker
--   device uses, so it identifies nothing. Our scanner reports mfg_data
--   INCLUDING those two octets, which is why decode_adv strips them: leave
--   them on and byte[0] reads 0xFF and the MAC and product code come out as
--   nonsense.
--
--   FF 09 is also the first two bytes of every protocol frame, at a layer
--   that has nothing to do with advertising. Reusing the number was surely
--   deliberate; treating the three as one thing is how you lose a day.
-- ---------------------------------------------------------------------------
M.SVC    = '8c850001-0302-41c5-b46e-cf057c562025'
M.CMD    = '8c850002-0302-41c5-b46e-cf057c562025'   -- write
M.TEL    = '8c850003-0302-41c5-b46e-cf057c562025'   -- notify

M.ADV_UUID16 = 'ff09'          -- advertised, filterable
M.PRODUCT_TYPE = 'B405'        -- this model. B105 is the C1000.

-- ---------------------------------------------------------------------------
-- Advertisement decode.
--
-- Every current Anker device uses company id 0xFFFF -- the reserved "no
-- company" value -- so the company id identifies nothing. The model lives in
-- the two bytes at [8:10] of the payload AFTER the company id is stripped,
-- read as four hex characters. Confirmed on the bench against this unit:
--
--   ffff 02 7ce913601fd8 02 b405 444a5736 04
--   |    |  |            |  |    |        capability, bit 2 = AES-GCM capable
--   |    |  |            |  |    sku "DJW6" (4 ASCII chars, because v2)
--   |    |  |            |  productType B405
--   |    |  |            bindType 2 = already bound to an account
--   |    |  BLE MAC, in order
--   |    versionCode 2  (v1 has a 3-char sku, so capability shifts one left)
--   company id
-- ---------------------------------------------------------------------------
function M.decode_adv(mfg_hex)
  if not mfg_hex or #mfg_hex < 20 then return nil end
  local m = bytes.unhex(mfg_hex)
  if not m or #m < 10 then return nil end
  if m:byte(1) ~= 0xff or m:byte(2) ~= 0xff then return nil end
  local p = m:sub(3)
  local ver = p:byte(1)
  local sku_len = (ver == 2) and 4 or 3
  local cap_at = 10 + sku_len + 1
  return {
    version = ver,
    mac = (('%02X:%02X:%02X:%02X:%02X:%02X')
             :format(p:byte(2), p:byte(3), p:byte(4), p:byte(5), p:byte(6), p:byte(7))),
    bound = p:byte(8) == 2,
    product_type = ('%02X%02X'):format(p:byte(9), p:byte(10)),
    sku = p:sub(11, 10 + sku_len),
    -- Absent when the payload is short, which happens; do not assume it.
    gcm = (#p >= cap_at) and ((p:byte(cap_at) & 0x04) ~= 0) or nil,
  }
end

-- True when this scan record is an A2687. Matching on the ADVERTISED name is
-- wrong: the name is the unit's SERIAL (ASHDJW...), not the model.
function M.matches(dev)
  local a = M.decode_adv(dev.mfg_data)
  return a ~= nil and a.product_type == M.PRODUCT_TYPE
end

-- ---------------------------------------------------------------------------
-- Negotiation cipher constants.
--
-- The app encrypts the entire handshake with a pre-shared key and a FIXED
-- nonce, so there is exactly one keystream, reused in both directions, in
-- every session, on both the C1000 and the A2687. The key was never
-- recovered and does not need to be: nonce reuse hands you the keystream, and
-- the same reuse lets the tag be forged from a recovered H and E_K(J0). This
-- is GCM's "forbidden attack" and it is why the handshake is readable at all.
--
-- test/host/neg_fixture.h keeps a copy so the C parity test can prove the
-- generalised GHASH still reproduces these tags; tools/run_tests.py asserts
-- the two have not drifted.
-- ---------------------------------------------------------------------------
local KEYSTREAM = bytes.unhex('ab863fd2e63a29b2feb408c94b6f66d5' ..
                              '837983d03ae4c082f6efe0563451fb3e' ..
                              'd09acc477e96b33fb96c1609abddcb5c' ..
                              '53512e5108944f9eab52975ced6d8c49' ..
                              '0c736e')
local GHASH_H   = bytes.unhex('c8add2d2fc099ee3ff848b0fc8cdc738')
local EJ0       = bytes.unhex('befdecb8a864653040b74bc0941caca2')
local AAD       = bytes.unhex('3322110077665544bbaa9988ffeeddcc')

local KEYSTREAM_LEN = 67

-- ---------------------------------------------------------------------------
-- Opcodes. A table, which is why it is here and not burning flash on string
-- literals in C.
-- ---------------------------------------------------------------------------
local FT_ZX            = 0x01
local FT_CHARGING      = 0x0F
local FT_CHARGING_RESP = 0x11   -- trap 2

M.OPCODE = {
  [0x0001] = 'startConnect',        [0x0801] = 'startConnect.resp',
  [0x0003] = 'capabilityNegotiation', [0x0803] = 'capabilityNegotiation.resp',
  [0x0029] = 'getDeviceInfo',       [0x0829] = 'getDeviceInfo.resp',
  [0x0005] = 'setCapability',       [0x0805] = 'setCapability.resp',
  [0x0021] = 'getPublicKey',        [0x0821] = 'getPublicKey.resp',
  [0x0022] = 'getAesKey',           [0x0822] = 'getAesKey.resp',
  [0x0027] = 'getAuthentication',   [0x0827] = 'getAuthentication.resp',
  [0x0030] = 'getZXOTAState',       [0x0830] = 'getZXOTAState.resp',
  [0x0200] = 'getAllDeviceInfo',    [0x0a00] = 'getAllDeviceInfo.resp',
  [0x020a] = 'bindUser',            [0x0a0a] = 'bindUser.resp',
  [0x021d] = 'setPortTimers',       [0x0a1d] = 'setPortTimers.resp',
  [0x0300] = 'deviceReport',
  [0x0204] = 'setBacklight',        [0x0207] = 'setPortSwitch',
}

M.STATUS = {
  [0x00] = 'ok',
  [0x01] = 'failed',
  [0x02] = 'unsupported opcode',
  [0x03] = 'bad parameter',
  [0x04] = 'busy',
  [0x05] = 'not authenticated',
  [0x06] = 'stale timestamp (FE epoch)',
  [0x07] = 'unknown user',
  [0x08] = 'permission denied',
  [0x09] = 'device locked',
  [0x0a] = 'crc/length error',
  [0x0b] = 'not ready',
  [0x0c] = 'out of range',
  [0x0d] = 'write protected',
  [0x0e] = 'timeout',
  [0x0f] = 'internal error',
  [0x10] = 'not bound',
}

-- Trap 1. Two canned NAK bodies, identical across sessions, that mean "I never
-- adopted your public key".
local NAK_POISON = {
  ['b0733cc3'] = true,
  ['4c4d807e'] = true,
}

local function nak_reason(body)
  if not body or #body < 4 then return nil end
  local head = bytes.hex(body:sub(1, 4))
  if NAK_POISON[head] then
    return 'the charger never adopted our public key -- 0x0021 almost ' ..
           'certainly went out in plaintext (trap 1)'
  end
  return nil
end

-- ---------------------------------------------------------------------------
-- Framing helpers
-- ---------------------------------------------------------------------------
local function now_epoch()
  -- Seconds. The device only checks freshness, so boot-relative time is fine
  -- until a real clock exists; a stale value comes back as STATUS 0x06.
  return (M.epoch_base or 0) + (sys.ms() // 1000)
end

local function le32(v)
  return string.char(v & 0xff, (v >> 8) & 0xff, (v >> 16) & 0xff, (v >> 24) & 0xff)
end

local function neg_encrypt(pt)
  -- Trap 5: there is no keystream past 67 bytes and there never will be.
  if #pt > KEYSTREAM_LEN then
    error(('negotiation payload %d B exceeds the %d B keystream (trap 5)')
          :format(#pt, KEYSTREAM_LEN))
  end
  local ct = bytes.xor(pt, KEYSTREAM)
  local tag = bytes.xor(crypto.ghash(GHASH_H, AAD, ct), EJ0)
  return ct .. tag
end

local function neg_decrypt(body)
  local n = #body >= 16 and #body - 16 or #body
  return bytes.xor(body:sub(1, n), KEYSTREAM)
end

-- ---------------------------------------------------------------------------
-- The link object. link:await() is why the handshake below reads as
-- straight-line code: every call that waits yields the coroutine, so the
-- natural way to write the protocol is also the way that does not stall the
-- radio. It polls the C notification inbox and sys.sleep()s between tries --
-- the scheduler needed no new yield discipline for that, and a 5 ms poll is
-- nothing against a device that takes ~900 ms to answer its key exchange.
-- ---------------------------------------------------------------------------
local Link = {}
Link.__index = Link

function M.link(addr)
  local l = setmetatable({ addr = addr, chan = 0, session = nil }, Link)
  l.cmd = assert(bt.find(addr, M.SVC, M.CMD), 'no Anker command characteristic')
  l.tel = assert(bt.find(addr, M.SVC, M.TEL), 'no Anker telemetry characteristic')
  return l
end

function Link:send(ft, opcode, payload, encrypted)
  local f = assert(frame.build(ft, opcode, encrypted and true or false, 0, payload))
  return bt.write(self.addr, self.cmd, bytes.hex(f), true)
end

-- Wait for the next frame whose opcode matches, reassembling fragments on the
-- way. Returns the parsed frame plus its decrypted payload, or nil on timeout.
function Link:await(opcode, timeout_ms)
  local deadline = sys.ms() + (timeout_ms or 3000)
  repeat
    local n = bt.notify_take()
    if n and n.id == self.tel then
      local f = frame.parse(n.value)
      if f then
        if not f.checksum_ok then
          log.warn('a2687: bad checksum on opcode ' .. string.format('0x%04x', f.opcode))
        else
          local body = f.payload
          if f.fragmented then
            body = frame.reasm(self.chan, f)      -- nil until the group closes
          end
          if body then
            if opcode == nil or f.opcode == opcode then
              return f, self:decode(f, body)
            end
            -- Not what we asked for: a telemetry report arriving mid-handshake
            -- is normal and must not be mistaken for the reply.
            self:on_unsolicited(f, self:decode(f, body))
          end
        end
      end
    end
    if not n then sys.sleep(5) end
  until sys.ms() > deadline
  return nil
end

-- Decrypt if the frame says so, then strip the status byte -- but only from a
-- RESPONSE (trap 3).
function Link:decode(f, body)
  if f.encrypted then
    if self.session then
      body = crypto.gcm_decrypt(self.session.key, self.session.nonce, body)
    else
      body = neg_decrypt(body)
    end
  end
  local status = nil
  if f.is_response and #body >= 1 then            -- opcodeHi & 0x08
    status = body:byte(1)
    body = body:sub(2)
  end
  return { body = body, status = status }
end

function Link:on_unsolicited(f, dec)
  if f.opcode == 0x0300 then
    M.publish(self, dec.body)
  end
end

-- One negotiation-stage round trip.
function Link:stage(opcode, payload, timeout_ms)
  self:send(FT_ZX, opcode, neg_encrypt(payload), true)
  local want = opcode | 0x0800
  local f, dec = self:await(want, timeout_ms or 3000)
  if not f then
    return nil, ('%s: no reply to 0x%04x'):format(M.OPCODE[opcode] or '?', opcode)
  end
  if dec.status and dec.status ~= 0 then
    return nil, ('%s: %s'):format(M.OPCODE[opcode] or '?',
                                  M.STATUS[dec.status] or ('status ' .. dec.status))
  end
  return dec
end

-- A session-GCM round trip, used for the post-auth burst.
function Link:cmd_gcm(ft, opcode, payload, want, timeout_ms)
  local ct = crypto.gcm_encrypt(self.session.key, self.session.nonce, AAD, payload)
  self:send(ft, opcode, ct, true)
  local f, dec = self:await(want, timeout_ms or 4000)
  if not f then
    return nil, ('0x%04x: no reply'):format(opcode)
  end
  local why = nak_reason(dec.body)
  if why then return nil, why end
  if dec.status and dec.status ~= 0 then
    return nil, ('0x%04x: %s'):format(opcode, M.STATUS[dec.status] or dec.status)
  end
  return dec
end

-- ---------------------------------------------------------------------------
-- The handshake. Five negotiation stages, then ECDH, then the auth burst.
-- ---------------------------------------------------------------------------
-- Stages 1-5 plus the ECDH. NOTHING here needs a credential: the five
-- negotiation payloads carry an epoch and our public key and nothing else.
-- That is why this is split out -- it means the entire session-key path can
-- be proven against real hardware with no account and no secrets, and only
-- the auth burst below has to wait for them.
function M.negotiate_only(link, cfg)
  local e = now_epoch()

  -- 1  startConnect
  local ok, err = link:stage(0x0001, tlv.build{
    { tag = 0xa1, enc = 'bytes', value = le32(e), type_byte = false },
  })
  if not ok then return nil, err end

  -- 2  capabilityNegotiation.  a4 02 00 f0 -- note this one really is 0xf000.
  ok, err = link:stage(0x0003, tlv.build{
    { tag = 0xa1, enc = 'bytes', value = le32(e),          type_byte = false },
    { tag = 0xa3, enc = 'bytes', value = '\x20',           type_byte = false },
    { tag = 0xa4, enc = 'bytes', value = '\x00\xf0',       type_byte = false },
  })
  if not ok then return nil, err end

  -- 3  getDeviceInfo -- serial, firmware, MAC come back here
  local info
  info, err = link:stage(0x0029, tlv.build{
    { tag = 0xa1, enc = 'bytes', value = le32(e), type_byte = false },
  })
  if not info then return nil, err end
  local inf = tlv.parse(info.body, true)
  link.firmware = inf.a3 and inf.a3.value
  link.serial   = inf.a4 and inf.a4.value

  -- 4  setCapability.  Trap 6: a4 02 29 01 is u16 LE 297, the MTU.
  ok, err = link:stage(0x0005, tlv.build{
    { tag = 0xa1, enc = 'bytes', value = le32(e),    type_byte = false },
    { tag = 0xa3, enc = 'bytes', value = '\x20',     type_byte = false },
    { tag = 0xa4, enc = 'bytes', value = '\x29\x01', type_byte = false },
    { tag = 0xa5, enc = 'bytes', value = '\x44',     type_byte = false },
    { tag = 0xa6, enc = 'bytes', value = '\x02',     type_byte = false },
  })
  if not ok then return nil, err end

  -- 5  getPublicKey.  66 bytes of the 67 available (trap 5), and it MUST go
  -- out encrypted or the session is silently poisoned (trap 1). Trap 8: the
  -- reply takes about 900 ms because the charger does P-256 in software.
  local d = cfg.private_key
  if not d or #d ~= 32 then return nil, 'cfg/anker.json has no 32-byte private_key' end
  local pub = assert(crypto.pubkey(d))

  local keyres
  keyres, err = link:stage(0x0021, tlv.build{
    { tag = 0xa1, enc = 'bytes', value = pub, type_byte = false },
  }, 6000)
  if not keyres then return nil, err end

  local kt = tlv.parse(keyres.body, true)
  local peer = kt.a1 and kt.a1.value
  if not peer or #peer < 64 then return nil, 'no device public key in 0x0821' end

  local Z = assert(crypto.ecdh(d, peer))
  link.session = { key = Z:sub(1, 16), nonce = Z:sub(17, 28) }
  log.info('a2687: session established with ' .. (link.serial or link.addr))

  -- 0x0022 getAesKey is the FIRST frame under the session key, and it needs no
  -- credential -- only a timezone. That makes it the proof that the ECDH and
  -- the key/nonce slicing are right: a wrong Z produces a frame the charger
  -- cannot read, and it answers with one of the canned NAK constants instead
  -- of a status-0 0x0822. Doing it here rather than in the auth burst means
  -- the whole key path can be verified against real hardware with no account.
  --
  -- tzOffset is NEGATED seconds (US Central -> 18000).
  local ok22, err22 = link:cmd_gcm(FT_ZX, 0x0022, tlv.build{
    { tag = 0xa1, enc = 'bytes', value = le32(now_epoch() + 2), type_byte = false },
    { tag = 0xa3, enc = 'bytes',
      value = le32((-(cfg.tz_offset_sec or 0)) & 0xffffffff), type_byte = false },
    { tag = 0xa5, enc = 'bytes', value = cfg.tz or 'UTC0', type_byte = false },
  }, 0x0822)
  if not ok22 then return nil, 'session key rejected: ' .. tostring(err22) end
  log.info('a2687: session key accepted (0x0822 ok)')
  return true
end

-- The post-auth burst. Everything from here needs the 40-character account
-- id. Gated on each ACK rather than blasted: we do not yet know whether the
-- charger needs pacing, and gating tells us WHICH frame failed instead of
-- leaving a silent stall.
function M.authenticate(link, cfg)
  local uid = cfg.user_id
  if not uid or #uid ~= 40 then return nil, 'cfg/anker.json has no 40-character user_id' end
  local ok, err
  local e2 = now_epoch() + 2

  -- 0x0027 getAuthentication. The 40-char userId; no password.
  ok, err = link:cmd_gcm(FT_ZX, 0x0027, tlv.build{
    { tag = 0xa1, enc = 'bytes', value = le32(e2), type_byte = false },
    { tag = 0xa2, enc = 'bytes', value = uid,      type_byte = false },
  }, 0x0827)
  if not ok then return nil, err end

  -- 0x0200 getAllDeviceInfo -- charging dialect, so items carry a type byte.
  -- Trap 7: FE here is the TYPED form.
  ok, err = link:cmd_gcm(FT_CHARGING, 0x0200, tlv.build{
    { tag = 0xa1, enc = 'u8',  value = 0x21, type_byte = false },
    { tag = 0xf0, enc = 'u32', value = 1,    type_byte = 0x03 },
    { tag = 0xfe, enc = 'bytes', value = le32(now_epoch()), type_byte = false },
  }, 0x0a00)
  if not ok then return nil, err end

  -- 0x0030 getZXOTAState. Trap 4: an ft=0x0F frame carrying NEGOTIATION TLV.
  ok, err = link:cmd_gcm(FT_CHARGING, 0x0030, tlv.build{
    { tag = 0xa1, enc = 'bytes', value = le32(now_epoch()), type_byte = false },
    { tag = 0xa2, enc = 'bytes', value = uid,               type_byte = false },
  }, 0x0830)
  if not ok then return nil, err end

  -- 0x020a bindUser. One of the two frames that actually flips the device's
  -- state machine.
  ok, err = link:cmd_gcm(FT_CHARGING, 0x020a, tlv.build{
    { tag = 0xa1, enc = 'u8',    value = 0x21,   type_byte = false },
    { tag = 0xa2, enc = 'string', value = cfg.region or 'US', type_byte = 0x04 },
    { tag = 0xa3, enc = 'string', value = uid,   type_byte = 0x04 },
    { tag = 0xa5, enc = 'u8',    value = 1,      type_byte = 0x01 },
    { tag = 0xfe, enc = 'bytes', value = le32(now_epoch()), type_byte = false },
  }, 0x0a0a)
  if not ok then return nil, err end

  -- 0x021d setPortTimers. The other one. Trap 7: NO FE trailer on this frame.
  ok, err = link:cmd_gcm(FT_CHARGING, 0x021d, tlv.build{
    { tag = 0xa1, enc = 'u8',    value = 0x21, type_byte = false },
    { tag = 0xa2, enc = 'bytes', value = '\x3b\x00\x3b\x00\x3b\x00', type_byte = 0x04 },
  }, 0x0a1d)
  if not ok then return nil, err end

  -- If the charger's TFT has not left "Connecting to Bluetooth" by now, the
  -- session did not take, whatever the ACKs said.
  log.info('a2687: authenticated; expect 0x0300 at about 1 Hz')
  return true
end

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

M.FIELDS = {
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
function M.decode_report(body)
  local t = tlv.parse(body, false)
  local out = {}
  if t.truncated then
    log.warn('a2687: telemetry had more items than the parser holds')
    out.truncated = true
  end
  for i = 1, t.n do
    local it = t[i]
    local spec = M.FIELDS[it.hex]
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

function M.publish(link, body)
  local ok, t = pcall(M.decode_report, body)
  if not ok then
    log.error('a2687: telemetry decode failed: ' .. tostring(t))
    return
  end
  t.addr = link.addr
  t.serial = link.serial
  bus.emit('anker.telemetry', t)
end

-- ---------------------------------------------------------------------------
-- Entry point. The caller supplies cfg (see cfg/anker.json): user_id,
-- private_key, tz, tz_offset_sec, region. NOTHING secret is in this file.
-- ---------------------------------------------------------------------------
function M.run(addr, cfg)
  local link = M.link(addr)
  if not bt.subscribe(addr, link.tel, 'notify') then
    return nil, 'could not subscribe to the telemetry characteristic'
  end
  -- Let the CCCD write land before sending anything. Without this the first
  -- stage goes out before the charger has been told we want notifications,
  -- its reply is dropped by the peer stack, and the symptom is a flat
  -- "no reply to 0x0001" that looks like a dead device.
  sys.sleep(400)
  bt.notify_flush()

  local ok, err = M.negotiate_only(link, cfg)
  if ok then ok, err = M.authenticate(link, cfg) end
  if not ok then
    log.error('a2687: ' .. tostring(err))
    return nil, err
  end

  ops.register('anker.a2687.refresh', function()
    local dec = link:cmd_gcm(FT_CHARGING, 0x0200, tlv.build{
      { tag = 0xa1, enc = 'u8',  value = 0x21, type_byte = false },
      { tag = 0xf0, enc = 'u32', value = 1,    type_byte = 0x03 },
      { tag = 0xfe, enc = 'bytes', value = le32(now_epoch()), type_byte = false },
    }, 0x0a00)
    if not dec then return { ok = false } end
    M.publish(link, dec.body)
    return { ok = true }
  end)

  -- Steady state: the device pushes 0x0300 on its own.
  while true do
    local f, dec = link:await(nil, 5000)
    if f and dec then
      link:on_unsolicited(f, dec)
    elseif not f then
      log.warn('a2687: no report for 5 s')
    end
  end
end

return M
