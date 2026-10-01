--[[
A2687 screensaver upload: the charger's side of it, kept small on purpose.

The browser does the heavy lifting -- decoding, cropping and scaling the
picture to 240x240, encoding the JPEG, its CRC32, the image id, cutting it
into packets, the window loop and every retry. This module only turns one
step at a time into a frame and reports what the charger answered, so the
Lua arena never holds more than one window of packets (10 x 156 B).

The exchange, decrypted from the app's own upload (anker_c1000_protocol
a2687-re/captures/20261001-063437, every frame tag-verified):

  0x021F select  a1 21 · a3 u8 mode (3) · a4 id u32 · a5 crc32 · fd "SmallChargingUrl" · fe
                 -> 0x0A1F status 0x11: not cached, send it (0: already has it)
  0x0220 begin   a1 21 · a2 u8 1 · a3 id · a4 crc32 · a5 u32 size · a6 u8 packets per ack
                 · a7 u16 packet length · a8 u16 packet count · fe   -> 0x0A20 status 0
  0x0221 data    a1 21 · a2 u16 packet# (from 0) · a3 <packet length> bytes   (no fe;
                 the last packet is padded to full length; a5 says where it ends)
                 -> 0x0A21 after every window and the last packet:
                    status 0 + a2 u16 = packets received; status 0x10 = complete

Each value carries its type byte (01 u8, 02 u16, 03 u32, 04 bytes, 00 text),
as the charger's own setters do (setter.lua).
]]

local link_m = require 'profiles.anker.link'

local S = {}

S.FT = 0x0F                  -- the charging function type every A2687 command uses
S.SELECT, S.BEGIN, S.DATA = 0x021F, 0x0220, 0x0221

local function le16(v) return string.char(v & 0xff, (v >> 8) & 0xff) end
local le32 = link_m.le32
local function item(tag, value) return { tag = tag, enc = 'bytes', value = value, type_byte = false } end
local function sel() return item(0xa1, '\x21') end
local function fe(epoch) return item(0xfe, '\x03' .. le32(epoch)) end

function S.select_payload(id, crc, mode, epoch)
  return tlv.build{
    sel(),
    item(0xa3, '\x01' .. string.char(mode)),
    item(0xa4, '\x04' .. le32(id)),
    item(0xa5, '\x04' .. le32(crc)),
    item(0xfd, '\x00SmallChargingUrl'),
    fe(epoch),
  }
end

function S.begin_payload(p, epoch)
  return tlv.build{
    sel(),
    item(0xa2, '\x01\x01'),
    item(0xa3, '\x04' .. le32(p.id)),
    item(0xa4, '\x04' .. le32(p.crc)),
    item(0xa5, '\x03' .. le32(p.size)),
    item(0xa6, '\x01' .. string.char(p.window)),
    item(0xa7, '\x02' .. le16(p.plen)),
    item(0xa8, '\x02' .. le16(p.count)),
    fe(epoch),
  }
end

function S.data_payload(n, data)
  return tlv.build{ sel(), item(0xa2, '\x02' .. le16(n)), item(0xa3, '\x04' .. data) }
end

-- The u16 in an ack's a2 (packets received), or nil.
function S.received(body)
  local i = 1
  while i + 1 <= #body do
    local tag, n = body:byte(i), body:byte(i + 1)
    if tag == 0xa2 and n == 3 then return body:byte(i + 3) | (body:byte(i + 4) << 8) end
    i = i + 2 + n
  end
  return nil
end

local function u32(v) return math.tointeger(tonumber(v)) end

--[[
Validate one browser request and turn it into a job. Returns the job, or nil
and why. Steps:
  select {id, crc, mode=3}
  begin  {id, crc, size, count, plen, window=10}
  data   {n, a, b, ack}   a / b: the packet as two hex halves -- an op's string
                          parameter is capped at 255 characters by the
                          firmware's JSON decoder, and a whole 156-byte packet
                          is 312 in hex. ack: wait for the charger's 0x0A21
                          after this one (end of a window, or the last packet).
]]
function S.job(p)
  local step = p.step
  if step == 'select' then
    local id, crc = u32(p.id), u32(p.crc)
    local mode = u32(p.mode or 3)
    if not id or not crc or not mode then return nil, 'select needs id and crc' end
    return { step = step, id = id, crc = crc, mode = mode & 0xff }
  elseif step == 'begin' then
    local j = { step = step, id = u32(p.id), crc = u32(p.crc), size = u32(p.size),
                count = u32(p.count), plen = u32(p.plen or 156), window = u32(p.window or 10) }
    for _, k in ipairs{ 'id', 'crc', 'size', 'count', 'plen', 'window' } do
      if not j[k] or j[k] < 0 then return nil, 'begin needs ' .. k end
    end
    if j.plen < 1 or j.plen > 200 then return nil, 'plen takes 1..200' end
    return j
  elseif step == 'data' then
    local n = u32(p.n)
    local data = bytes.unhex(tostring(p.a or '') .. tostring(p.b or ''))
    if not n or n < 0 or not data or #data == 0 then return nil, 'data needs n and a/b hex' end
    return { step = step, n = n, data = data, ack = p.ack == true }
  end
  return nil, 'step is select, begin or data'
end

local function send(link, opcode, payload)
  return link:secure_send(S.FT, opcode, payload)
end

local function reply(link, opcode, ms)
  local f, dec = link:await(opcode | 0x0800, ms)
  if not f then return nil, tostring(dec) end
  return dec
end

--[[
Run one job on the driver's coroutine. `emit(t)` publishes the charger's
answer as a profile.anker.screensaver event; the browser drives the next step
from it.
]]
function S.run(link, addr, j, emit)
  local ev = { addr = addr, step = j.step }
  if j.step == 'select' then
    local ok, err = send(link, S.SELECT, S.select_payload(j.id, j.crc, j.mode, link_m.now_epoch()))
    if ok then
      local dec; dec, err = reply(link, S.SELECT, 5000)
      if dec then ev.status = dec.status end
    end
    ev.id, ev.error = j.id, err
  elseif j.step == 'begin' then
    local ok, err = send(link, S.BEGIN, S.begin_payload(j, link_m.now_epoch()))
    if ok then
      local dec; dec, err = reply(link, S.BEGIN, 5000)
      if dec then ev.status = dec.status end
    end
    ev.id, ev.error = j.id, err
  else
    local ok, err = send(link, S.DATA, S.data_payload(j.n, j.data))
    ev.n = j.n
    if not ok then
      ev.error = err
    elseif j.ack then
      local dec; dec, err = reply(link, S.DATA, 8000)
      if dec then ev.status, ev.received = dec.status, S.received(dec.body) end
      ev.error = err
    else
      return                  -- mid-window: nothing to say
    end
  end
  emit(ev)
end

return S
