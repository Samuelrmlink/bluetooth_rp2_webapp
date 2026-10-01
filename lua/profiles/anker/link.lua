--[[
The Anker link: framing, waiting and the session cipher, shared by every
model.

This used to exist twice, once per profile, and the two copies drifted. Every
bug found on one was fixed only there: the C1000 copy learned to accept
notifications from any characteristic and to cope with a refused write, and
the A2687 copy learned neither. Both fixes are here, and there is one copy to
fix next time.

The session cipher is a pair of closures rather than a branch, so this file
knows that frames may be encrypted without knowing how. neg_gcm and neg_cbc
each install their own and are then unloaded; what they leave behind is two
functions and the key material they close over.
]]

-- GATT addressing. The defaults every device in the B0xx..B4xx range uses;
-- they live here rather than in adv.lua so that adv -- which is only needed
-- to identify a device -- can be unloaded once that is done.
local SVC = '8c850001-0302-41c5-b46e-cf057c562025'
local CMD = '8c850002-0302-41c5-b46e-cf057c562025'
local TEL = '8c850003-0302-41c5-b46e-cf057c562025'

local FT_ZX = 0x01          -- negotiation function type

local Link = {}
Link.__index = Link

local M = { Link = Link, FT_ZX = FT_ZX, SVC = SVC, CMD = CMD, TEL = TEL }

M.OPCODE = {
  [0x0001] = 'startConnect',          [0x0801] = 'startConnect.resp',
  [0x0003] = 'capabilityNegotiation', [0x0803] = 'capabilityNegotiation.resp',
  [0x0029] = 'getDeviceInfo',         [0x0829] = 'getDeviceInfo.resp',
  [0x0005] = 'setCapability',         [0x0805] = 'setCapability.resp',
  [0x0021] = 'getPublicKey',          [0x0821] = 'getPublicKey.resp',
  [0x0022] = 'getAesKey',             [0x0822] = 'getAesKey.resp',
  [0x0027] = 'getAuthentication',     [0x0827] = 'getAuthentication.resp',
  [0x0030] = 'getZXOTAState',         [0x0830] = 'getZXOTAState.resp',
  [0x0040] = 'getAllDeviceInfo',      [0x0840] = 'getAllDeviceInfo.resp',
  [0x0200] = 'getMiniChargeAllDeviceInfo', [0x0a00] = 'getMiniChargeAllDeviceInfo.resp',
  [0x020a] = 'bindUser',              [0x0a0a] = 'bindUser.resp',
  [0x021d] = 'setPortTimers',         [0x0a1d] = 'setPortTimers.resp',
  [0x0300] = 'deviceReport',          [0x0402] = 'workInfoReport',
}

M.STATUS = {
  [0x00] = 'ok',                          [0x01] = 'failed',
  [0x02] = 'unsupported opcode',          [0x03] = 'bad parameter',
  [0x04] = 'busy',                        [0x05] = 'not authenticated',
  [0x06] = 'stale timestamp (FE epoch)',  [0x07] = 'unknown user',
  [0x08] = 'permission denied',           [0x09] = 'device locked',
  [0x0a] = 'crc/length error',            [0x0b] = 'not ready',
  [0x0c] = 'out of range',
}

function M.le32(v)
  v = v & 0xffffffff
  return string.char(v & 0xff, (v >> 8) & 0xff, (v >> 16) & 0xff, (v >> 24) & 0xff)
end

-- The device checks FRESHNESS, not absolute time, so a counter that only
-- advances is enough: there is no RTC and no NTP here.
local epoch_base = 1700000000
function M.now_epoch() return epoch_base + (sys.ms() // 1000) end
function M.set_epoch_base(v) epoch_base = v end

function M.open(addr)
  local l = setmetatable({ addr = addr, chan = 0, session = nil }, Link)
  l.cmd = assert(bt.find(addr, SVC, CMD), 'no Anker command characteristic')
  l.tel = assert(bt.find(addr, SVC, TEL), 'no Anker telemetry characteristic')
  return l
end

-- GATT operations are queued per link in C (src/bt/le/central.h), so a write
-- issued straight after bt.subscribe is no longer refused and needs neither
-- the retry loop nor the sleep the two old copies used.
function Link:send(ft, opcode, payload, encrypted)
  local f = assert(frame.build(ft, opcode, encrypted and true or false, 0, payload))
  return bt.write(self.addr, self.cmd, bytes.hex(f), true)
end

function Link:decode(f, body)
  if f.encrypted then
    if self.session and self.session.decrypt then
      body = self.session.decrypt(body) or body
    elseif self.neg_decrypt then
      body = self.neg_decrypt(body)        -- the A2687's encrypted negotiation
    else
      return { body = body, status = nil, opaque = true }
    end
  end
  local status = nil
  if f.is_response and #body >= 1 then
    status = body:byte(1)
    body = body:sub(2)
  end
  return { body = body, status = status }
end

--[[
Wait for a frame, blocking rather than polling.

Accepts a notification from ANY characteristic on this device. The inbox is
already scoped per device, so filtering on TEL as well bought nothing -- and
it cost the entire C1000 bring-up, because that unit answers the handshake
somewhere the A2687 does not, so every reply was silently discarded.
]]
function Link:await(opcode, timeout_ms)
  local deadline = sys.ms() + (timeout_ms or 3000)
  repeat
    local left = deadline - sys.ms()
    if left <= 0 then break end
    local n, why = bt.await(self.addr, left)
    if not n then
      -- A timeout is OUR deadline and the caller's to interpret. Link loss
      -- and cancellation are retryable by nobody, so they pass straight out.
      if why ~= 'timeout' then return nil, why end
      break
    end
    if n.source ~= 'read' then
      if n.id ~= self.tel and not self.notified_on then
        self.notified_on = n.id
        log.info(('anker: notifications arrive on %s (TEL is %s)')
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
        log.warn(('anker: bad checksum on 0x%04x'):format(f.opcode))
      end
    end
  until sys.ms() > deadline
  return nil, 'timeout'
end

local function named(opcode) return M.OPCODE[opcode] or ('0x%04x'):format(opcode) end

local function check(dec, opcode)
  if dec.status and dec.status ~= 0 then
    return nil, ('%s: %s'):format(named(opcode),
                                  M.STATUS[dec.status] or ('status ' .. dec.status))
  end
  return dec
end

-- A negotiation stage. `seal` is nil when the negotiation is in clear (the
-- C1000) and a function when it is not (the A2687, whose whole negotiation is
-- encrypted with a recovered keystream).
function Link:stage(opcode, payload, timeout_ms)
  local body = self.neg_encrypt and self.neg_encrypt(payload) or payload
  if not self:send(FT_ZX, opcode, body, self.neg_encrypt ~= nil) then
    return nil, ('%s: write refused'):format(named(opcode))
  end
  local f, dec = self:await(opcode | 0x0800, timeout_ms or 3000)
  if not f then
    return nil, ('%s: no reply (%s)'):format(named(opcode), tostring(dec))
  end
  return check(dec, opcode)
end

-- A command under the established session cipher.
-- Send under the session cipher without waiting for a reply: for commands
-- the device answers only now and then (screensaver data, acked per window).
function Link:secure_send(ft, opcode, payload)
  if not (self.session and self.session.encrypt) then return nil, 'no session' end
  local ct = self.session.encrypt(payload)
  if not ct then return nil, 'session encrypt failed' end
  if not self:send(ft, opcode, ct, true) then
    return nil, ('%s: write refused'):format(named(opcode))
  end
  return true
end

function Link:secure(ft, opcode, payload, want, timeout_ms)
  local sent, err = self:secure_send(ft, opcode, payload)
  if not sent then return nil, err end
  local f, dec = self:await(want, timeout_ms or 4000)
  if not f then
    return nil, ('%s: no reply (%s)'):format(named(opcode), tostring(dec))
  end
  if self.session.nak_reason then
    local why = self.session.nak_reason(dec.body)
    if why then return nil, why end
  end
  return check(dec, opcode)
end

--[[
The five negotiation stages, which are the same on every device we have seen.

They live here rather than in the cipher modules because they are identical
between them -- what differs is only whether the frames go out encrypted
(Link:stage handles that through link.neg_encrypt) and what is done with the
shared secret afterwards. Two small per-device variations are declared rather
than copied:

  opts.client_id  a2 on 0x0001. The C1000 sequence was captured with one; the
                  A2687 sequence without. Whether it is required is unknown,
                  so both are reproduced as observed rather than unified.
  opts.cap6       a6 on 0x0005, present for the A2687 and absent for the C1000.

Returns the 32-byte ECDH shared secret Z, or nil plus a reason.
]]
function M.negotiate_stages(link, cfg, opts)
  opts = opts or {}
  local e = M.now_epoch()

  local items = { { tag = 0xa1, enc = 'bytes', value = M.le32(e), type_byte = false } }
  if opts.client_id then
    items[#items + 1] = { tag = 0xa2, enc = 'bytes', value = opts.client_id,
                          type_byte = false }
  end
  local ok, err = link:stage(0x0001, tlv.build(items))
  if not ok then return nil, err end

  -- a4 02 00 f0 -- this one really is 0xf000, not a byte-swap slip.
  ok, err = link:stage(0x0003, tlv.build{
    { tag = 0xa1, enc = 'bytes', value = M.le32(e),    type_byte = false },
    { tag = 0xa3, enc = 'bytes', value = '\x20',       type_byte = false },
    { tag = 0xa4, enc = 'bytes', value = '\x00\xf0',   type_byte = false },
  })
  if not ok then return nil, err end

  local info
  info, err = link:stage(0x0029, tlv.build{
    { tag = 0xa1, enc = 'bytes', value = M.le32(e), type_byte = false },
  })
  if not info then return nil, err end
  local inf = tlv.parse(info.body, true)
  link.chip     = inf.a2 and inf.a2.value
  link.firmware = inf.a3 and inf.a3.value
  link.serial   = inf.a4 and inf.a4.value

  -- a4 02 29 01 is u16 LE 297, the MTU.
  local caps = {
    { tag = 0xa1, enc = 'bytes', value = M.le32(e),    type_byte = false },
    { tag = 0xa3, enc = 'bytes', value = '\x20',       type_byte = false },
    { tag = 0xa4, enc = 'bytes', value = '\x29\x01',   type_byte = false },
    { tag = 0xa5, enc = 'bytes', value = '\x44',       type_byte = false },
  }
  if opts.cap6 then
    caps[#caps + 1] = { tag = 0xa6, enc = 'bytes', value = opts.cap6, type_byte = false }
  end
  ok, err = link:stage(0x0005, tlv.build(caps))
  if not ok then return nil, err end

  -- The reply takes ~900 ms: the peer does P-256 in software too.
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

  local Z = crypto.ecdh(d, peer)
  if not Z then return nil, 'ecdh failed' end
  return Z
end

return M
