--[[
Anker BLE advertisement: identify the device before connecting to it.

Everything needed to decide how to talk to an Anker device is broadcast
before you connect -- which model it is, and which session cipher it will
accept. Reading it here means nothing downstream has to be keyed on a model
name, which is how this codebase ended up with two near-identical profiles
whose only real difference was a product code.

Layout, from adv-manufacturer-data-re.md (AKIoTKit's ModernBleAdvertisementData).
`mfg_data` as bt.devices() reports it INCLUDES the 2-byte company identifier,
which is stripped here; every offset below is relative to what remains:

  [0]      versionCode      1 or 2
  [1:7]    bleMacAddress    6 bytes, in order (not reversed)
  [7]      bindType         0 unbound, 2 bound
  [8:10]   productType      2 bytes -> 4 hex chars, e.g. "B105"  <- the model
  [10:13]  sku  (v1)        3 ASCII chars      | the ONLY thing the version
  [10:14]  sku  (v2)        4 ASCII chars      | changes
  [13]     capability (v1)  bitfield
  [14]     capability (v2)  bitfield

READ BYTE 0 FIRST. The version changes the sku length and therefore where the
capability byte sits. Read a v2 device at the v1 offset and you get the last
character of its sku instead of its capability -- for our A2687 that is 0x36,
which even has bit 2 set, so it looks like a plausible answer and is not one.

Bytes [0:10] are version-independent, so a device can be identified even when
the payload is too short to carry a capability.
]]

local A = {}

A.SERVICE_UUID16 = 'ff09'      -- advertised by every current-generation device
A.COMPANY_ID     = 'ffff'      -- the reserved "no company" id, not a real one

--[[
Decode the manufacturer data. Returns a table, or nil plus a reason.

  version        1 | 2
  mac            "E8:EE:CC:B6:89:7E"
  bind           raw byte;  bound = (bind == 2)
  product_type   "B105"
  sku            "5YX" | "DJW6" | nil when the payload is short
  capability     byte, 0 when the payload is too short to carry one
  gcm_hint       (capability & 0x04) ~= 0

gcm_hint is a HINT and is named so deliberately. The app treats it as GCM only
when the bit is set AND a cloud value (conn_confirm_model) is non-zero, which
we have no way to read. Both of our devices set the bit; only one of them
speaks GCM. Use it to choose which cipher to TRY first, never to decide.
]]
function A.decode(mfg_hex)
  if type(mfg_hex) ~= 'string' then return nil, 'no manufacturer data' end
  local m = bytes.unhex(mfg_hex)
  if not m then return nil, 'manufacturer data is not hex' end
  if #m < 2 then return nil, 'manufacturer data too short' end
  if ('%02x%02x'):format(m:byte(1), m:byte(2)) ~= A.COMPANY_ID then
    return nil, 'not an Anker company id'
  end

  local d = m:sub(3)
  if #d < 10 then return nil, 'payload too short to identify' end

  local version = d:byte(1)
  if version ~= 1 and version ~= 2 then
    return nil, ('unknown advertisement version %d'):format(version)
  end

  local sku_len = (version == 2) and 4 or 3
  local cap_at  = 10 + sku_len + 1          -- 1-based index of the capability
  local sku = (#d >= 10 + sku_len) and d:sub(11, 10 + sku_len) or nil
  -- Short payloads report no capability rather than guessing, which is what
  -- getCapability() does upstream.
  local capability = (#d >= cap_at) and d:byte(cap_at) or 0

  return {
    version      = version,
    mac          = ('%02X:%02X:%02X:%02X:%02X:%02X'):format(
                     d:byte(2), d:byte(3), d:byte(4), d:byte(5), d:byte(6), d:byte(7)),
    bind         = d:byte(8),
    bound        = d:byte(8) == 2,
    product_type = ('%02X%02X'):format(d:byte(9), d:byte(10)),
    sku          = sku,
    capability   = capability,
    gcm_hint     = (capability & 0x04) ~= 0,
    length       = #d,
  }
end

-- Is this scan entry an Anker device we can talk to? Returns the decoded
-- advertisement, or nil plus a reason.
--
-- All three checks matter. The service uuid is what marks it as Anker at all;
-- the company id is checked inside decode; and the MAC in the payload is
-- compared against the advertised address, which is the cheap way to notice
-- that we are looking at a layout we have misread.
function A.identify(dev)
  if type(dev) ~= 'table' or not dev.addr then return nil, 'not a device' end

  local has_uuid = false
  for _, u in ipairs(dev.service_uuids or {}) do
    if u:lower() == A.SERVICE_UUID16 then has_uuid = true end
  end
  if not has_uuid then return nil, 'no ff09 service uuid' end

  local a, err = A.decode(dev.mfg_data)
  if not a then return nil, err end
  if a.mac ~= dev.addr:upper() then
    return nil, ('mac %s in the payload is not the advertised %s'):format(a.mac, dev.addr)
  end
  return a
end

return A
