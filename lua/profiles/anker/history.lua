--[[
The A2687's output history (0x020C), as the Anker app's "Detailed Data" screen
reads it (captured 2026-10-03, anker_c1000_protocol a2687-re/captures/
20261003-181234). Pure: the request and the reply parse, so the host test can
hold them against the capture; run.lua does the sending.

  request   a1 01 21 · a2 02 01 <0 start | 1 more> · fe 04 <epoch>   (FT 0x0F)
  reply     0x0A0C, pushed one packet after another:
            a2 u8 last (1 on the final packet) · a3 u8 records in it ·
            a4 u16 span in minutes (360 = 6 h) · a5 bytes: 10 record slots
  record    12 bytes: c1 mV u16, c1 mA u16, c2 ..., c3 ... (LE);
            ffff ffff = no reading for that port
Records are 5 minutes apart, oldest first, the last one now: 73 for 6 h.
The app sent "more" (a2 = 1) whenever the stream paused; so does run.lua.
]]

local setter = require 'profiles.anker.setter'

local H = {}

H.OPCODE = 0x020C
H.REPLY = 0x0A0C
H.RECORD = 12
H.INTERVAL_S = 300

local REQ = { items = { { tag = 0xa2, enc = 'u8' } }, fe = 'bare' }

-- more: false for the first request of a read, true to ask for the rest
function H.request(more, epoch)
  return setter.payload(REQ, more and 1 or 0, epoch)
end

-- A reply body (status byte already off) -> { last, n, span_min, records }
-- where records is the n*12 record bytes; or nil and why.
function H.parse(body)
  local last, n, span, data
  for tag, rawv, value in tlv.each(body) do
    if tag == 'a2' then last = value
    elseif tag == 'a3' then n = value
    elseif tag == 'a4' then span = value
    elseif tag == 'a5' then data = rawv:sub(2)   -- drop the type byte
    end
  end
  if not (n and data) then return nil, 'not a history packet' end
  n = math.min(n, #data // H.RECORD)
  return { last = last == 1, n = n, span_min = span, records = data:sub(1, n * H.RECORD) }
end

return H
