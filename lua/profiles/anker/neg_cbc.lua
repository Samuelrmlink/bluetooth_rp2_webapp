--[[
Negotiation for devices whose session is AES-CBC (the C1000 and the power
station family).

The negotiation frames go out IN CLEAR here, which is the visible difference
from the GCM path.

The invisible one is that "AES-CBC" is not one scheme but three, and the
advertisement does not say which:

  ecdh          ankerKdf(Z,64) -> AES-256, key = kdf[0:32], iv = kdf[32:48].
                What the current app does.
  ecdh-legacy   AES-128, key = Z[0:16], iv = Z[16:32]. No KDF. Older firmware.
  account       AesConferV1: key = utf8(accountId)[0:32], iv = 16 zero bytes.

So this module does not choose, it ASKS: it installs each candidate in turn
and calls the caller's probe.

STATUS: UNTESTED ON HARDWARE. Both devices on this bench negotiate GCM, so
nothing here has been exercised against a real peer -- and the one time it
looked necessary, it was not.

That episode is worth recording, because the conclusion was wrong in an
instructive way. An earlier C1000 profile ran the negotiation IN CLEAR and
then found that only ecdh-legacy could read the session; it was written up as
"this firmware is too old for GCM". It was not. A plaintext 0x0021 makes the
charger derive a DIFFERENT shared secret (the trap neg_gcm.lua describes), and
ecdh-legacy happened to be the one derivation that could still read the
result. Encrypt the negotiation properly and the same unit speaks GCM, exactly
as its capability bit advertises. The lesson: when a device appears to need an
older scheme, suspect the handshake before blaming the firmware.
]]

local link_mod = require 'profiles.anker.link'

local N = { name = 'cbc' }

local function cbc_session(mode, key, iv)
  return {
    mode = mode, key = key, iv = iv,
    encrypt = function(pt) return crypto.cbc(true,  key, iv, pt) end,
    decrypt = function(ct) return crypto.cbc(false, key, iv, ct) end,
  }
end

--[[
Run the handshake and settle on a cipher. Returns true, or nil plus a reason.

`probe(link) -> bool` is supplied by the caller and must send something under
the session and say whether the reply made sense. It lives there because
which request is meaningful depends on the model, which this module does not
know and should not.
]]
function N.negotiate(link, cfg, probe)
  link.neg_encrypt, link.neg_decrypt = nil, nil     -- plaintext negotiation

  local Z, err = link_mod.negotiate_stages(link, cfg,
                                           { client_id = cfg.client_id })
  if not Z then return nil, err end
  link.Z = Z

  local candidates = {}
  local kdf = crypto.kdf(Z, 64)
  if kdf then
    candidates[#candidates + 1] = cbc_session('ecdh', kdf:sub(1, 32), kdf:sub(33, 48))
  end
  candidates[#candidates + 1] = cbc_session('ecdh-legacy', Z:sub(1, 16), Z:sub(17, 32))
  if cfg.user_id then
    local ak, aiv = crypto.account_key(cfg.user_id)
    if ak then candidates[#candidates + 1] = cbc_session('account', ak, aiv) end
  end

  for _, s in ipairs(candidates) do
    link.session = s
    bt.notify_flush(link.addr)
    if probe(link) then
      log.info(('anker: session cipher is %s (%d-byte key)'):format(s.mode, #s.key))
      return true
    end
  end

  link.session = nil
  return nil, 'no candidate session cipher produced a readable reply'
end

return N
