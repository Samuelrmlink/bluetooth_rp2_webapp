--[[
Negotiation for devices that speak AES-128-GCM (the A2687 / mini-charge
family, and anything else whose session turns out to decrypt this way).

Two things make this path different from the CBC one, and both are traps:

  1. THE WHOLE NEGOTIATION IS ENCRYPTED. Every frame from 0x0001 to 0x0021
     goes out under a pre-shared keystream nobody has the key for -- only the
     recovered keystream itself, in cipher.lua. A plaintext 0x0021 makes the
     charger derive a different secret and NAK everything afterwards, and it
     does so silently.
  2. 0x0022 getAesKey is the proof. It is the first frame under the session
     key and needs no credential, only a timezone, so a wrong Z shows up here
     as a canned NAK rather than as mysterious garbage three steps later.

Loaded only when this cipher is being tried, and unloaded once a session is
up: it is about 3 KB of keystream that is never needed again.
]]

local link_mod = require 'profiles.anker.link'
local cipher   = require 'profiles.anker.cipher'

local N = { name = 'gcm' }

--[[
Run the handshake. Returns true, or nil plus a reason.

`probe` is accepted for symmetry with neg_cbc but not used: 0x0022 is a
better check than any telemetry request, because it isolates the key
derivation from whether the device happens to support a given report.
]]
function N.negotiate(link, cfg, probe)
  -- Before any stage: Link:stage sends encrypted when these are present.
  link.neg_encrypt = cipher.neg_encrypt
  link.neg_decrypt = cipher.neg_decrypt

  local Z, err = link_mod.negotiate_stages(link, cfg, { cap6 = '\x02' })
  if not Z then return nil, err end

  local key, nonce = Z:sub(1, 16), Z:sub(17, 28)
  -- Copy what the session needs out of the module, rather than closing over
  -- the module table: these closures outlive this module by design, and
  -- holding `cipher` through them would pin the whole thing in the arena
  -- after it has been unloaded.
  local aad, nak = cipher.AAD, cipher.nak_reason
  link.session = {
    mode  = 'gcm',
    key   = key,
    nonce = nonce,
    encrypt = function(pt) return crypto.gcm_encrypt(key, nonce, aad, pt) end,
    decrypt = function(ct) return crypto.gcm_decrypt(key, nonce, ct) end,
    nak_reason = nak,
  }

  -- tzOffset is NEGATED seconds (US Central -> 18000).
  local ok, why = link:secure(link_mod.FT_ZX, 0x0022, tlv.build{
    { tag = 0xa1, enc = 'bytes', value = link_mod.le32(link_mod.now_epoch() + 2),
      type_byte = false },
    { tag = 0xa3, enc = 'bytes',
      value = link_mod.le32((-(cfg.tz_offset_sec or 0)) & 0xffffffff),
      type_byte = false },
    { tag = 0xa5, enc = 'bytes', value = cfg.tz or 'UTC0', type_byte = false },
  }, 0x0822)
  if not ok then
    link.session = nil
    return nil, 'session key rejected: ' .. tostring(why)
  end
  return true
end

--[[
getAuthentication (0x0027) -- the account id, no password.

COMMON TO BOTH FAMILIES, which is not obvious and cost a debugging round.
The burst below was originally one function, and skipping all of it for the
power station left a session that negotiated cleanly and then answered
nothing: every 0x0040 poll timed out. 0x0027 is the frame that authorises the
session; only the mini-charge frames after it are family-specific.
]]
function N.authenticate(link, cfg)
  local uid = cfg.user_id
  if not uid or #uid ~= 40 then
    return nil, 'cfg/anker.json has no 40-character user_id'
  end
  local ok, err = link:secure(link_mod.FT_ZX, 0x0027, tlv.build{
    { tag = 0xa1, enc = 'bytes', value = link_mod.le32(link_mod.now_epoch() + 2),
      type_byte = false },
    { tag = 0xa2, enc = 'bytes', value = uid, type_byte = false },
  }, 0x0827)
  if not ok then return nil, '0x0027: ' .. tostring(err) end
  return true
end

--[[
The mini-charge bind burst -- SCP chargers only, and only for models whose
schema asks for it (model.post_auth).

The last two are the ones that matter: bindUser and setPortTimers are what
flip the charger's state machine into reporting. A power station implements
none of these and simply times out on each, four seconds at a time, which is
why this is gated on the model rather than sent hopefully.

Traps, all observed:
  0x0200 is the CHARGING dialect, so its items carry a type byte, and its FE
         is the typed form.
  0x0030 is an ft=0x0F frame carrying NEGOTIATION TLV.
  0x021d has NO FE trailer.
]]
function N.bind_burst(link, cfg)
  local uid = cfg.user_id
  if not uid or #uid ~= 40 then return nil, 'no 40-character user_id' end
  local FT_CHARGING = 0x0F
  local le32, now = link_mod.le32, link_mod.now_epoch
  local ok, err

  ok, err = link:secure(FT_CHARGING, 0x0200, tlv.build{
    { tag = 0xa1, enc = 'u8',    value = 0x21, type_byte = false },
    { tag = 0xf0, enc = 'u32',   value = 1,    type_byte = 0x03 },
    { tag = 0xfe, enc = 'bytes', value = le32(now()), type_byte = false },
  }, 0x0a00)
  if not ok then return nil, '0x0200: ' .. tostring(err) end

  ok, err = link:secure(FT_CHARGING, 0x0030, tlv.build{
    { tag = 0xa1, enc = 'bytes', value = le32(now()), type_byte = false },
    { tag = 0xa2, enc = 'bytes', value = uid,         type_byte = false },
  }, 0x0830)
  if not ok then return nil, '0x0030: ' .. tostring(err) end

  ok, err = link:secure(FT_CHARGING, 0x020a, tlv.build{
    { tag = 0xa1, enc = 'u8',     value = 0x21, type_byte = false },
    { tag = 0xa2, enc = 'string', value = cfg.region or 'US', type_byte = 0x04 },
    { tag = 0xa3, enc = 'string', value = uid,  type_byte = 0x04 },
    { tag = 0xa5, enc = 'u8',     value = 1,    type_byte = 0x01 },
    { tag = 0xfe, enc = 'bytes',  value = le32(now()), type_byte = false },
  }, 0x0a0a)
  if not ok then return nil, '0x020a bindUser: ' .. tostring(err) end

  ok, err = link:secure(FT_CHARGING, 0x021d, tlv.build{
    { tag = 0xa1, enc = 'u8',    value = 0x21, type_byte = false },
    { tag = 0xa2, enc = 'bytes', value = '\x3b\x00\x3b\x00\x3b\x00', type_byte = 0x04 },
  }, 0x0a1d)
  if not ok then return nil, '0x021d setPortTimers: ' .. tostring(err) end

  -- If the charger's TFT has not left "Connecting to Bluetooth" by now, the
  -- session did not take, whatever the ACKs said.
  log.info('anker: bind burst accepted; expect reports at about 1 Hz')
  return true
end

return N
