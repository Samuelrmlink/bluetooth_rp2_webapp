--[[
Anker SOLIX C1000 (product type B105).

WHY THIS IS SEPARATE FROM profiles/anker.lua, WHICH SPEAKS THE SAME PROTOCOL:
the two devices differ in exactly the places that matter and agree everywhere
else, and the A2687 path was hard to get right. Sharing the plumbing would
mean every C1000 experiment risks a working charger. The frame/TLV/crypto
algorithms they both lean on are in C and genuinely shared; what is duplicated
here is a few dozen lines of sequence. Worth revisiting once the C1000 is as
well proven as the A2687 is.

What differs:

  negotiation   The A2687 GCM-encrypts the whole 0x0001..0x0021 negotiation
                with a pre-shared key nobody has, which is why that profile
                carries a recovered keystream. The C1000 sends those frames
                in PLAINTEXT. Nothing here needs the keystream.
  session       AES-256-CBC / PKCS#7, key and IV from the "ECIES" KDF over
                the ECDH secret, where the A2687 uses AES-128-GCM with a
                reused nonce.
  handshake     Five stages and done. No 0x0022 getAesKey round trip, and no
                account id needed to reach a session.

Same on both: the GATT addressing (service 8c850001..., one command
characteristic and one telemetry characteristic), the FF09 framing, the XOR
checksum and the TLV dialects. Confirmed on hardware -- the C1000 discovers
as 3 services / 7 characteristics with the same 8c850001 service.
]]

local M = {}

-- GATT addressing. Identical to the A2687; NEVER filter a scan on these.
M.SVC = '8c850001-0302-41c5-b46e-cf057c562025'
M.CMD = '8c850002-0302-41c5-b46e-cf057c562025'
M.TEL = '8c850003-0302-41c5-b46e-cf057c562025'

M.PRODUCT_TYPE = 'B105'        -- B405 is the A2687
M.ADV_UUID16   = 'ff09'

local FT_ZX = 0x01

M.OPCODE = {
  [0x0001] = 'startConnect',          [0x0801] = 'startConnect.resp',
  [0x0003] = 'capabilityNegotiation', [0x0803] = 'capabilityNegotiation.resp',
  [0x0029] = 'getDeviceInfo',         [0x0829] = 'getDeviceInfo.resp',
  [0x0005] = 'setCapability',         [0x0805] = 'setCapability.resp',
  [0x0021] = 'getPublicKey',          [0x0821] = 'getPublicKey.resp',
  [0x0300] = 'deviceReport',
}

M.STATUS = {
  [0x00] = 'ok',                      [0x01] = 'failed',
  [0x02] = 'unsupported opcode',      [0x03] = 'bad parameter',
  [0x04] = 'busy',                    [0x05] = 'not authenticated',
  [0x06] = 'stale timestamp (FE epoch)', [0x07] = 'unknown user',
  [0x08] = 'permission denied',       [0x09] = 'device locked',
  [0x0a] = 'crc/length error',        [0x0b] = 'not ready',
}

local function le32(v)
  v = v & 0xffffffff
  return string.char(v & 0xff, (v >> 8) & 0xff, (v >> 16) & 0xff, (v >> 24) & 0xff)
end
M.le32 = le32

-- The device checks FRESHNESS, not absolute time, so an epoch that merely
-- advances is enough -- we have no RTC and no NTP here.
local function now_epoch()
  return 1700000000 + (sys.ms() // 1000)
end
M.now_epoch = now_epoch

-- Match on the PRODUCT TYPE in the manufacturer data, never on the name.
-- `mfg_data` arrives as a HEX STRING from bt.devices().
--
-- Layout, which is shared with the A2687 and decoded the same way -- the only
-- variable is the sku length, which the version byte selects:
--   ff ff | ver | <6-byte MAC> | bound | <2-byte product type> | <sku> | caps
-- A live C1000: ffff 01 e8eeccb6897e 00 b105 355958 04
--            -> ver 1, B105, sku "5YX"; the A2687 is ver 2 with a 4-byte sku.
function M.decode_adv(mfg_hex)
  if not mfg_hex or #mfg_hex < 20 then return nil end
  local m = bytes.unhex(mfg_hex)
  if not m or #m < 10 then return nil end
  if m:byte(1) ~= 0xff or m:byte(2) ~= 0xff then return nil end
  local p = m:sub(3)
  local ver = p:byte(1)
  local sku_len = (ver == 2) and 4 or 3
  return {
    version = ver,
    mac = (('%02X:%02X:%02X:%02X:%02X:%02X')
             :format(p:byte(2), p:byte(3), p:byte(4), p:byte(5), p:byte(6), p:byte(7))),
    bound = p:byte(8) == 2,
    product_type = ('%02X%02X'):format(p:byte(9), p:byte(10)),
    sku = p:sub(11, 10 + sku_len),
  }
end

function M.matches(dev)
  local a = M.decode_adv(dev.mfg_data)
  return a ~= nil and a.product_type == M.PRODUCT_TYPE
end

-- ---------------------------------------------------------------------------
-- The link
-- ---------------------------------------------------------------------------
local Link = {}
Link.__index = Link

function M.link(addr)
  local l = setmetatable({ addr = addr, chan = 0, session = nil }, Link)
  l.cmd = assert(bt.find(addr, M.SVC, M.CMD), 'no C1000 command characteristic')
  l.tel = assert(bt.find(addr, M.SVC, M.TEL), 'no C1000 telemetry characteristic')
  return l
end

-- A write can be REFUSED while the link is busy with another GATT operation,
-- and bt.write reports that by returning false. Ignoring it is a silent
-- failure that looks exactly like a device that did not answer: the very
-- first frame of the handshake went out immediately after the CCCD write
-- from bt.subscribe, was refused, and every stage then "timed out" against a
-- charger that had been given nothing to reply to.
--
-- Retrying here is a workaround. The real fix is a per-link operation queue
-- in C so a refusal cannot happen at all -- bt.read/write/subscribe should
-- enqueue and return a token rather than failing.
function Link:send(ft, opcode, payload, encrypted)
  local f = assert(frame.build(ft, opcode, encrypted and true or false, 0, payload))
  local hex = bytes.hex(f)
  local deadline = sys.ms() + 2000
  repeat
    if bt.write(self.addr, self.cmd, hex, true) then return true end
    sys.sleep(20)
  until sys.ms() > deadline
  log.warn(('c1000: write of 0x%04x refused for 2 s (link busy?)'):format(opcode))
  return false
end

function Link:decode(f, body)
  if f.encrypted then
    if not self.session then return { body = body, status = nil, opaque = true } end
    body = crypto.cbc(false, self.session.key, self.session.iv, body) or body
  end
  local status = nil
  if f.is_response and #body >= 1 then
    status = body:byte(1)
    body = body:sub(2)
  end
  return { body = body, status = status }
end

-- Blocks rather than polls: see lua/lwait.h. A drop ends the wait at once and
-- says so, instead of burning the whole timeout on a link that is gone.
function Link:await(opcode, timeout_ms)
  local deadline = sys.ms() + (timeout_ms or 3000)
  repeat
    local left = deadline - sys.ms()
    if left <= 0 then break end
    local n, why = bt.await(self.addr, left)
    if not n then
      if why ~= 'timeout' then return nil, why end
      break
    end
    -- Accept a notification from ANY characteristic on this device, not just
    -- the one named TEL. The inbox is already scoped per device, so the id
    -- filter bought nothing -- and it cost the whole bring-up: the C1000
    -- answers the handshake on a characteristic the A2687 does not use, so
    -- every reply was discarded and every stage "timed out" against a device
    -- that had in fact replied in 200 ms.
    if n.source ~= 'read' then
      if n.id ~= self.tel and not self.notified_on then
        self.notified_on = n.id
        log.info(('c1000: notifications arrive on %s (TEL is %s)')
                 :format(n.id, tostring(self.tel)))
      end
      local f = frame.parse(n.value)
      if f and f.checksum_ok then
        local body = f.payload
        if f.fragmented then body = frame.reasm(self.chan, f) end
        if body then
          local dec = self:decode(f, body)
          if opcode == nil or f.opcode == opcode then return f, dec end
          if self.on_unsolicited then self:on_unsolicited(f, dec) end
        end
      elseif f then
        log.warn(('c1000: bad checksum on 0x%04x'):format(f.opcode))
      end
    end
  until sys.ms() > deadline
  return nil, 'timeout'
end

-- A negotiation stage. PLAINTEXT -- this is the whole difference from the
-- A2687, whose equivalent must go out encrypted or the session is poisoned.
function Link:stage(opcode, payload, timeout_ms)
  if not self:send(FT_ZX, opcode, payload, false) then
    return nil, ('%s: write refused'):format(M.OPCODE[opcode] or '?')
  end
  local f, dec = self:await(opcode | 0x0800, timeout_ms or 3000)
  if not f then
    return nil, ('%s: no reply to 0x%04x'):format(M.OPCODE[opcode] or '?', opcode)
  end
  if dec.status and dec.status ~= 0 then
    return nil, ('%s: %s'):format(M.OPCODE[opcode] or '?',
                                  M.STATUS[dec.status] or ('status ' .. dec.status))
  end
  return dec
end

-- A session command: AES-256-CBC / PKCS#7 under the ECDH key, which is what
-- every frame after 0x0021 uses. The captures show the same opcodes going out
-- plaintext before the session and encrypted after it, with the enc bit in
-- the frame header (byte7 & 0x40) as the only difference.
-- NOT named Link:cmd -- `l.cmd` is already the command characteristic's id,
-- and an instance field shadows a method of the same name.
function Link:secure(ft, opcode, payload, want, timeout_ms)
  if not self.session then return nil, 'no session' end
  local ct = crypto.cbc(true, self.session.key, self.session.iv, payload)
  if not ct then return nil, 'cbc encrypt failed' end
  if not self:send(ft, opcode, ct, true) then
    return nil, ('0x%04x: write refused'):format(opcode)
  end
  local f, dec = self:await(want, timeout_ms or 4000)
  if not f then return nil, ('0x%04x: %s'):format(opcode, tostring(dec)) end
  if dec.status and dec.status ~= 0 then
    -- Dump it. A status outside the documented 0x00..0x0c set almost always
    -- means the body did not DECRYPT -- a wrong session key produces a
    -- plausible-looking first byte and nothing else that parses -- rather
    -- than a device reporting an error we have not seen before.
    log.warn(('c1000: 0x%04x status 0x%02x enc=%s raw=%s'):format(
      opcode, dec.status, tostring(f.encrypted), bytes.hex(dec.body):sub(1, 64)))
    return nil, ('0x%04x: %s'):format(opcode, M.STATUS[dec.status] or dec.status)
  end
  return dec
end

-- DeviceAllBaseInfo. The captured request is a1 01 21 plus an FE freshness
-- stamp; the reply is the full status TLV, fragmented.
function M.status(link, epoch)
  return link:secure(0x0F, 0x0040,
    '\xa1\x01\x21' .. '\xfe\x05\x03' .. M.le32(epoch or M.now_epoch()),
    0x0840, 6000)
end

-- ---------------------------------------------------------------------------
-- The five-stage handshake
-- ---------------------------------------------------------------------------
function M.handshake(link, cfg)
  local e = now_epoch()

  -- 0  startConnect: a base timestamp and a client identifier.
  local ok, err = link:stage(0x0001, tlv.build{
    { tag = 0xa1, enc = 'bytes', value = le32(e), type_byte = false },
    { tag = 0xa2, enc = 'bytes', value = cfg.client_id or 'btgw-0000', type_byte = false },
  })
  if not ok then return nil, err end

  -- 1  capabilityNegotiation
  ok, err = link:stage(0x0003, tlv.build{
    { tag = 0xa1, enc = 'bytes', value = le32(e),     type_byte = false },
    { tag = 0xa3, enc = 'bytes', value = '\x20',      type_byte = false },
    { tag = 0xa4, enc = 'bytes', value = '\x00\xf0',  type_byte = false },
  })
  if not ok then return nil, err end

  -- 2  getDeviceInfo
  local info
  info, err = link:stage(0x0029, tlv.build{
    { tag = 0xa1, enc = 'bytes', value = le32(e), type_byte = false },
  })
  if not info then return nil, err end
  local inf = tlv.parse(info.body, true)
  link.chip     = inf.a2 and inf.a2.value
  link.firmware = inf.a3 and inf.a3.value
  link.serial   = inf.a4 and inf.a4.value
  log.info(('c1000: %s fw %s sn %s'):format(tostring(link.chip),
           tostring(link.firmware), tostring(link.serial)))

  -- 3  setCapability
  ok, err = link:stage(0x0005, tlv.build{
    { tag = 0xa1, enc = 'bytes', value = le32(e),     type_byte = false },
    { tag = 0xa3, enc = 'bytes', value = '\x20',      type_byte = false },
    { tag = 0xa4, enc = 'bytes', value = '\x29\x01',  type_byte = false },
    { tag = 0xa5, enc = 'bytes', value = '\x44',      type_byte = false },
  })
  if not ok then return nil, err end

  -- 4  getPublicKey. The reply takes ~900 ms: the charger does P-256 in
  -- software too.
  local d = cfg.private_key
  if not d or #d ~= 32 then return nil, 'cfg/anker.json has no 32-byte private_key' end
  local pub = assert(crypto.pubkey(d))

  local keyres
  keyres, err = link:stage(0x0021, tlv.build{
    { tag = 0xa1, enc = 'bytes', value = pub, type_byte = false },
  }, 8000)
  if not keyres then return nil, err end

  local kt = tlv.parse(keyres.body, true)
  local peer = kt.a1 and kt.a1.value
  if not peer or #peer < 64 then return nil, 'no device public key in 0x0821' end

  -- EcdhConfer: Z -> ankerKdf(Z,64) -> AES-256 key + IV. The KDF is in C and
  -- is pinned by frozen vectors; getting it wrong yields a session the
  -- charger cannot read rather than an error.
  local Z = assert(crypto.ecdh(d, peer))
  link.Z = Z          -- retained so alternative derivations can be tried
  local kdf = assert(crypto.kdf(Z, 64))
  link.session = { key = kdf:sub(1, 32), iv = kdf:sub(33, 48), mode = 'ecdh' }
  log.info('c1000: session established with ' .. (link.serial or link.addr))
  return true
end

-- Which confer strategy does this unit use? The app picks one per device and
-- nothing in the advertisement says which, so ASK: send a status request
-- under each candidate key and keep the one whose reply decrypts.
--
-- Measured on a live C1000 with firmware 0.0.0.3: `ecdh` (the current app's
-- AES-256 + ECIES KDF) decrypts to noise, and `ecdh-legacy` -- the older
-- AES-128 math with key = Z[0:16], iv = Z[16:32] -- returns a clean 353-byte
-- status TLV. Hardcoding either one would have been wrong for somebody, and
-- a wrong key is indistinguishable from a broken device unless you look.
function M.negotiate_session(link, cfg)
  local Z = link.Z
  if not Z then return nil, 'no shared secret; run the handshake first' end

  local candidates = {
    { name = 'ecdh',        key = nil, iv = nil },   -- filled in below
    { name = 'ecdh-legacy', key = Z:sub(1, 16), iv = Z:sub(17, 32) },
  }
  local kdf = crypto.kdf(Z, 64)
  if kdf then candidates[1].key, candidates[1].iv = kdf:sub(1, 32), kdf:sub(33, 48) end
  if cfg and cfg.user_id then
    local ak, aiv = crypto.account_key(cfg.user_id)
    if ak then candidates[#candidates + 1] = { name = 'account', key = ak, iv = aiv } end
  end

  for _, c in ipairs(candidates) do
    if c.key then
      link.session = { key = c.key, iv = c.iv, mode = c.name }
      bt.notify_flush(link.addr)
      local dec = M.status(link)
      if dec then
        log.info(('c1000: session cipher is %s (%d-byte key)'):format(c.name, #c.key))
        return c.name, dec
      end
    end
  end
  link.session = nil
  return nil, 'no candidate session cipher produced a readable reply'
end

-- Fall back to AesConferV1 when no ECDH session is wanted: the key is just
-- the account id's bytes. Useful as a cross-check if ECDH is ever refused.
function M.account_session(link, account_id)
  local key, iv = crypto.account_key(account_id)
  if not key then return nil, 'account key failed' end
  link.session = { key = key, iv = iv, mode = 'account' }
  return true
end

M.Link = Link
return M
