--[[
The five negotiation stages and the post-auth burst.

Split from the profile so that no single chunk is large enough to dominate
the Lua arena while it compiles -- see cipher.lua for the measurement.

Everything here is SEQUENCE: which frame, in which order, carrying which
bytes. It is the highest-churn part of the protocol and the reason the
device-specific half is in Lua at all. The algorithms it leans on (framing,
TLV, AES-GCM, P-256) are C and do not change.

The link object is passed in rather than required, so this module knows
nothing about how a link is built.
]]

local cipher = require 'profiles.anker_cipher'

local H = {}

local now_epoch = cipher.now_epoch
local le32 = cipher.le32

-- ---------------------------------------------------------------------------
-- Opcodes. A table, which is why it is here and not burning flash on string
-- literals in C.
-- ---------------------------------------------------------------------------
local FT_ZX            = 0x01
local FT_CHARGING      = 0x0F
local FT_CHARGING_RESP = 0x11   -- trap 2

H.OPCODE = {
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

H.STATUS = {
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

-- ---------------------------------------------------------------------------
-- The handshake. Five negotiation stages, then ECDH, then the auth burst.
-- ---------------------------------------------------------------------------
-- Stages 1-5 plus the ECDH. NOTHING here needs a credential: the five
-- negotiation payloads carry an epoch and our public key and nothing else.
-- That is why this is split out -- it means the entire session-key path can
-- be proven against real hardware with no account and no secrets, and only
-- the auth burst below has to wait for them.
function H.negotiate_only(link, cfg)
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
function H.authenticate(link, cfg)
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

return H
