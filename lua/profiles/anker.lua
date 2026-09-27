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

SPLIT ACROSS FOUR FILES, and not for tidiness. The whole profile in one
28.9 KB chunk peaked at 109 KB of the 131 KB Lua arena while COMPILING,
which left no room for a second device's profile to be resident at the same
time. Peak tracks the largest single chunk rather than the total, so:

  anker.lua            this file: GATT addressing, the advertisement, the link
  anker_cipher.lua     the negotiation keystream and the forged-tag constants
  anker_handshake.lua  the five stages and the post-auth burst
  anker_telemetry.lua  the 24-tag field dictionary

Flat, not a subdirectory, and deliberately. require maps dots to slashes and
appends .lua, so `profiles.anker` is lua/profiles/anker.lua -- there is no
init.lua convention, and adding one would also mean teaching script.list to
walk a second level. Four files sharing a prefix costs nothing and sorts
together in the editor.
]]


local cipher    = require 'profiles.anker_cipher'
local handshake = require 'profiles.anker_handshake'  -- released after the session is up
local telemetry = require 'profiles.anker_telemetry'

local M = {}

-- Re-exported so callers and the link below keep one place to look.
M.OPCODE = handshake.OPCODE
M.STATUS = handshake.STATUS
M.FIELDS = telemetry.FIELDS
M.decode_report = telemetry.decode_report
M.negotiate_only = handshake.negotiate_only
M.authenticate = handshake.authenticate

local AAD          = cipher.AAD
local now_epoch    = cipher.now_epoch
local le32         = cipher.le32
local neg_encrypt  = cipher.neg_encrypt
local neg_decrypt  = cipher.neg_decrypt
local nak_reason   = cipher.nak_reason

local FT_ZX       = 0x01
local FT_CHARGING = 0x0F

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
-- The link object. link:await() is why the handshake below reads as
-- straight-line code: every call that waits yields the coroutine, so the
-- natural way to write the protocol is also the way that does not stall the
-- radio.
--
-- It BLOCKS rather than polls. The earlier version took the C inbox every
-- 5 ms and slept between tries, which cost about 190 resumes per second to
-- deliver one telemetry report per second, and -- worse -- could not tell a
-- silent device from an absent one: it kept asking a dropped link until some
-- outer deadline gave up. bt.await hands the wait to the scheduler, which
-- resumes this coroutine when a frame arrives, when the deadline passes, or
-- the moment the LINK DROPS, and says which.
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
    local left = deadline - sys.ms()
    if left <= 0 then break end
    local n, why = bt.await(self.addr, left)
    if not n then
      -- A timeout here is OUR deadline and the caller's to interpret. Link
      -- loss and cancellation are not retryable by anyone, so they are
      -- returned as-is rather than flattened into "no reply".
      if why ~= 'timeout' then return nil, why end
      break
    end
    -- Read results share this inbox now (the Pinecil has no notify
    -- characteristic and can only be polled), so ignore anything that is not
    -- the device talking on its own.
    if n.id == self.tel and n.source ~= 'read' then
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
  until sys.ms() > deadline
  return nil, 'timeout'
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
  -- 0x0300 is the push and 0x0a00 the answer to a refresh; they carry the
  -- IDENTICAL field set and differ only by the status byte, which decode()
  -- has already stripped. Publishing only the push meant a 0x0a00 the loop
  -- happened to pick up was parsed and thrown away.
  if f.opcode == 0x0300 or f.opcode == 0x0a00 then
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

-- Re-require the handshake, for a reconnect. Costs a read from flash and a
-- recompile, which is exactly the peak the unload above avoided holding.
function M.reload_handshake()
  handshake = require 'profiles.anker_handshake'
  M.negotiate_only = handshake.negotiate_only
  M.authenticate = handshake.authenticate
  return handshake
end

function M.publish(link, body)
  local ok, t = pcall(telemetry.decode_report, body)
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
  bt.notify_flush(addr)

  local ok, err = M.negotiate_only(link, cfg)
  if ok then ok, err = M.authenticate(link, cfg) end

  -- The handshake is dead weight now. Several kilobytes of compiled chunk
  -- that will not run again unless the link drops, so give it back.
  --
  -- Clearing the module cache alone frees nothing: these re-exports and the
  -- upvalue in this function all keep it reachable, so every reference has to
  -- go first. That is the whole reason this is three lines and not one.
  --
  -- The trade, stated plainly: this buys steady-state room and costs a
  -- recompile if we ever need to handshake again. M.reconnect below pays it
  -- knowingly by re-requiring.
  if ok then
    M.negotiate_only, M.authenticate = nil, nil
    handshake = nil
    local freed_from = sys.collect()
    local had, after = sys.unload('profiles.anker_handshake')
    log.info(('a2687: released the handshake (%s), heap %d -> %d B')
             :format(had and 'unloaded' or 'was not cached', freed_from, after))
  end

  if not ok then
    log.error('a2687: ' .. tostring(err))
    return nil, err
  end

  -- An op handler MAY NOT AWAIT. Ops are dispatched with lua_pcall on the
  -- main Lua state, not inside this script's coroutine, and yielding across
  -- that pcall fails -- so the obvious version of this (call cmd_gcm, wait
  -- for 0x0a00, publish) returned script_error every single time. It is a
  -- trap for every future op, not a typo here.
  --
  -- So the op only raises a flag and returns; the loop below, which IS a
  -- coroutine and may yield, does the exchange. It also means the request
  -- cannot race the loop for the same reply.
  local want_refresh = false
  ops.register('anker.a2687.refresh', function()
    want_refresh = true
    return { queued = true }
  end)

  local function do_refresh()
    want_refresh = false
    local dec, err = link:cmd_gcm(FT_CHARGING, 0x0200, tlv.build{
      { tag = 0xa1, enc = 'u8',  value = 0x21, type_byte = false },
      { tag = 0xf0, enc = 'u32', value = 1,    type_byte = 0x03 },
      { tag = 0xfe, enc = 'bytes', value = le32(now_epoch()), type_byte = false },
    }, 0x0a00)
    if dec then
      M.publish(link, dec.body)
    else
      log.warn('a2687: refresh failed: ' .. tostring(err))
    end
  end

  -- Steady state: the device pushes 0x0300 on its own. No keepalive, and none
  -- should be added -- the notes are explicit that the app does no re-poll.
  local quiet = 0
  while true do
    if want_refresh then do_refresh() end
    local f, dec = link:await(nil, 5000)
    if f and dec then
      quiet = 0
      link:on_unsolicited(f, dec)
    else
      -- Silence means one of two things and they need different answers, so
      -- ask rather than guess. This loop used to log "no report for 5 s"
      -- forever after a drop, with no reconnect and no exit.
      local still = false
      for _, d in ipairs(bt.devices()) do
        if d.addr == addr and d.connected then still = true end
      end
      if not still then
        log.error('a2687: link dropped; the session is gone')
        return nil, 'disconnected'
      end
      quiet = quiet + 1
      log.warn(('a2687: connected but no report for %d s'):format(quiet * 5))
      if quiet >= 6 then
        log.error('a2687: connected but silent for 30 s, giving up')
        return nil, 'no reports'
      end
    end
  end
end

return M
