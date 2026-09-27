--[[
The negotiation cipher, and the constants it is made of.

Split out of the single-file profile because COMPILING is what costs arena:
the whole profile in one 28.9 KB chunk peaked at 109 KB of the 131 KB heap,
which left no room for a second device's profile to be resident at the same
time. Peak tracks the largest single chunk, not the total, so four small
modules cost far less than one large one.

The app encrypts the entire handshake with a pre-shared key and a FIXED
nonce, so there is exactly one keystream, reused in both directions, in every
session, on both the C1000 and the A2687. The key was never recovered and
does not need to be: nonce reuse hands you the keystream, and the same reuse
lets the tag be forged from a recovered H and E_K(J0). This is GCM's
"forbidden attack" and it is why the handshake is readable at all.

test/host/neg_fixture.h keeps a copy of these bytes so the C parity test can
prove the generalised GHASH still reproduces the tags; tools/run_tests.py
asserts the two have not drifted. That guard has already caught a one-byte
error, so if it is ever repointed it must be repointed AT THIS FILE.
]]

local C = {}

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

C.KEYSTREAM_LEN = KEYSTREAM_LEN
C.AAD = AAD

-- ---------------------------------------------------------------------------
-- Framing helpers
-- ---------------------------------------------------------------------------
-- Unix seconds at boot, if anything ever knows them. Until then this stays 0
-- and every FE stamp is boot-relative, which the charger has accepted in
-- every session observed -- it checks freshness, not absolute time. If a
-- long-lived session ever starts refusing setters with STATUS 0x06 (stale
-- timestamp), this is the first thing to suspect.
--
-- It used to read M.epoch_base, and after the split there was no M here at
-- all: the lookup silently found a nil global and fell back to 0. Same
-- result, by accident rather than intent.
C.epoch_base = 0

function C.set_epoch_base(unix_seconds)
  C.epoch_base = tonumber(unix_seconds) or 0
end

local function now_epoch()
  return C.epoch_base + (sys.ms() // 1000)
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

-- Trap 1. Two canned NAK bodies, identical across sessions, that mean "I
-- never adopted your public key".
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

C.now_epoch = now_epoch
C.le32 = le32
C.neg_encrypt = neg_encrypt
C.neg_decrypt = neg_decrypt
C.nak_reason = nak_reason

return C
