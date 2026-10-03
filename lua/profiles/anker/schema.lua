--[[
Turn a TLV report into named fields, using a per-model table.

The wire carries no names and there is no downloadable schema -- not from the
cloud, not from the app binary, where the names exist only as compiled-in
widget bindings. So every schema here is empirical, and this module exists to
make adding one a data change rather than a code change.

Two rules that are easy to get wrong and expensive to get wrong:

  REPORTS USE THE REPORT DIALECT. tlv.parse(body, false). The negotiation
  dialect differs in whether a type byte is present, so parsing a report as
  negotiation shifts every scalar by one byte -- min_soc 60 reads as 15361,
  ac_enabled 1 as 257. Plausible numbers, which is the dangerous kind of
  wrong; a frozen vector locks it.

  WIDTH COMES FROM THE BYTES, NOT THE MAP. tlv.parse hands back a number for
  u8/u16/u32 and a string for everything else, and which type byte a given
  firmware picks for a given tag is not worth betting on. it.raw always
  exists and includes the type byte, so decoding reads the width actually
  present and the map only says what the field MEANS.
]]

local S = {}

local function raw(it) return (it and it.raw) and it.raw:sub(2) or '' end

local function num(it)
  if type(it.value) == 'number' then return it.value end
  local b = raw(it)
  if #b == 1 then return b:byte(1) end
  if #b == 2 then return b:byte(1) | (b:byte(2) << 8) end
  if #b == 4 then
    return b:byte(1) | (b:byte(2) << 8) | (b:byte(3) << 16) | (b:byte(4) << 24)
  end
  return nil
end

local function snum(it)
  local v = num(it)
  if v == nil then return nil end
  local w = #raw(it)
  if w == 2 and v >= 0x8000 then return v - 0x10000 end
  if w == 1 and v >= 0x80   then return v - 0x100 end
  return v
end

local function str(it)
  local b = raw(it)
  if b == '' and type(it.value) == 'string' then b = it.value end
  return (b:gsub('%z.*$', ''))
end

local READERS = { u8 = num, u16 = num, u32 = num, s8 = snum, s16 = snum, str = str }

--[[
decode(model, body, want_unknown) -> fields | nil, err

`model.fields[tag] = { name, type, scale }`, tag lowercase hex. Fields are
matched to names by their TAG, never by position.

Unknown tags are reported as hex, but only when asked -- the set does not
change between reports, and hexing twenty of them every few seconds churned
the arena hard enough to exhaust it after about ten reports on a device that
was otherwise fine. Once is informative; every time is a leak with extra
steps.
]]
function S.decode(model, body, want_unknown)
  if type(body) ~= 'string' then return nil, 'tlv parse failed' end

  -- Field by field (tlv.each), not tlv.parse: parse built a table per item,
  -- ~22 KB for a C1000 report, every poll, read once and dropped. ONE item
  -- table is refilled per field instead, carrying exactly what readers use --
  -- it.raw and it.value -- so the readers, the model files and the tag->name
  -- lookup below are unchanged. A reader must not keep `it`: it is reused.
  local out, unknown, n = {}, {}, 0
  local it = {}
  for tag, rawv, value in tlv.each(body) do
    n = n + 1
    it.hex, it.raw, it.value = tag, rawv, value
    local f = model.fields and model.fields[tag]
    if f then
      -- A reader is a type name for a scalar, or a function for a field
      -- that is a structure. The SCP chargers need the latter for their
      -- per-port objects; the power stations never do.
      local reader = f[2]
      if type(reader) == 'string' then reader = READERS[reader] or num end
      -- One bad field must not cost the whole report: a firmware revision
      -- that widens a struct would otherwise blank the telemetry pane.
      local ok, v = pcall(reader, it)
      if not ok then
        -- `log` is absent in the host harness, which opens only the
        -- protocol bindings. A decoder must not depend on its logger.
        if log and log.warn then
          log.warn(('anker: tag %s did not decode: %s'):format(tag, tostring(v)))
        end
        v = nil
      end
      if v and f[3] then v = v * f[3] end
      out[f[1]] = v
    elseif want_unknown and not (model.ignore and model.ignore[tag]) then
      unknown[tag] = bytes.hex(raw(it))
    end
  end

  if want_unknown and next(unknown) then out.unknown = unknown end
  out.tlv_count = n
  -- a model's flat fields derived from the structured ones (A2687: c1_on ...)
  if model.derive then pcall(model.derive, out) end
  return out
end

return S
