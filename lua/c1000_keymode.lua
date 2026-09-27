--[[
Which confer strategy does THIS C1000 use?

The handshake completes and the device answers an encrypted 0x0040, but the
reply decrypts to high-entropy garbage -- so the session key is wrong, not the
framing. The KDF and the ECDH are both pinned by frozen vectors, which leaves
the STRATEGY: the app picks one of three per device, and this unit advertises
as unbound.

  ecdh         Z -> ankerKdf(Z,64); key = kdf[0:32] (AES-256), iv = kdf[32:48]
  ecdh-legacy  key = Z[0:16] (AES-128), iv = Z[16:32]
  account      key = utf8(accountId)[0:32] (AES-256), iv = 16 zero bytes

Send the same encrypted 0x0040 under each and see which reply decrypts into
something that looks like a status byte followed by TLV, rather than noise.
]]

local c1000 = require 'profiles.c1000'

local pk  = sys.secret('anker', 'private_key')
local uid = sys.secret('anker', 'user_id')
local cfg = { private_key = pk and bytes.unhex(pk), client_id = 'btgw-0000' }
if not cfg.private_key then log.error('keymode: no private_key'); return end

log.info('keymode: looking for a C1000')
local addr
repeat
  for _, d in ipairs(bt.devices()) do if c1000.matches(d) then addr = d.addr end end
  if not addr then sys.sleep(500) end
until addr

local function is_connected()
  for _, d in ipairs(bt.devices()) do
    if d.addr == addr and d.connected then return true end
  end
  return false
end

for _ = 1, 4 do
  if not is_connected() then
    bt.connect(addr)
    local dl = sys.ms() + 12000
    while not is_connected() and sys.ms() < dl do sys.sleep(250) end
  end
  if is_connected() then
    local dl = sys.ms() + 15000
    repeat
      if bt.find(addr, c1000.SVC, c1000.CMD) then break end
      if not is_connected() then break end
      sys.sleep(250)
    until sys.ms() > dl
  end
  if is_connected() and bt.find(addr, c1000.SVC, c1000.CMD) then break end
  sys.sleep(1500)
end
if not (is_connected() and bt.find(addr, c1000.SVC, c1000.CMD)) then
  log.error('keymode: no discovered link'); return
end

local link = c1000.link(addr)
if not bt.subscribe(addr, link.tel, 'notify') then log.error('keymode: subscribe failed'); return end
bt.notify_flush(addr)

-- Run the handshake to get Z, but keep the raw shared secret around so the
-- alternative derivations can be tried without redoing the key exchange.
local ok, err = c1000.handshake(link, cfg)
if not ok then log.error('keymode: handshake: ' .. tostring(err)); return end
local kdfkey, kdfiv = link.session.key, link.session.iv
log.info('keymode: handshake done; trying strategies')

-- Does this look like a decrypted response? Status byte then a TLV tag.
local function plausible(b)
  if not b or #b < 2 then return false end
  local s, tag = b:byte(1), b:byte(2)
  if s > 0x0c then return false end
  return (tag >= 0xa0 and tag <= 0xaf) or tag == 0xf0 or tag == 0xfe
end

local function try_mode(name, key, iv)
  if not key then log.warn('keymode: ' .. name .. ': no key available'); return end
  link.session = { key = key, iv = iv }
  bt.notify_flush(addr)
  local dec, e = link:secure(0x0F, 0x0040,
    '\xa1\x01\x21' .. '\xfe\x05\x03' .. c1000.le32(c1000.now_epoch()),
    0x0840, 5000)
  if dec then
    log.info(('keymode: %s -> STATUS OK, %d B: %s'):format(name, #dec.body,
             bytes.hex(dec.body):sub(1, 64)))
    return true
  end
  log.info(('keymode: %s -> %s'):format(name, tostring(e)))
  return false
end

-- 1. ecdh (what the profile does today)
try_mode('ecdh (kdf, AES-256)', kdfkey, kdfiv)

-- 2. ecdh-legacy. Recompute Z by redoing the derivation from the stored
--    session material is not possible, so ask the link for it: the handshake
--    kept the peer key on the link for exactly this.
if link.Z then
  try_mode('ecdh-legacy (AES-128)', link.Z:sub(1, 16), link.Z:sub(17, 32))
else
  log.warn('keymode: no Z retained; skipping ecdh-legacy')
end

-- 3. account / AesConferV1
if uid then
  local akey, aiv = crypto.account_key(uid)
  try_mode('account (AesConferV1)', akey, aiv)
else
  log.warn('keymode: no user_id in cfg; skipping account mode')
end

log.info('keymode: done')
