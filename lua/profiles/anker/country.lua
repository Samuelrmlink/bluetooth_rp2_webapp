--[[
(productCode, sku) -> country/region.

Keyed by PN first, then by the 3-character sku from the advertisement --
skus are not unique on their own, so both halves are needed.

Generated from anker_c1000_protocol/sku-country-table.md (the app's
FirmwareUtil.parseCountryBySku). Two known gaps, neither worth pretending
about: it covers only the statically enumerable set -- more skus come from
the cloud catalogue at runtime -- and only 3-character skus, so the 4-char
v2 skus (our A2687 advertises "DJW6") are simply absent. A nil answer means
"not in this table", not "no such region".

Loaded and unloaded alongside lookup.lua.
]]

local C = {}

C.by_pn = {
  A1722 = { ['0HM']='jp', ['G9A']='jp', ['P2M']='jp', ['SB1']='us', ['SB3']='uk', ['SB5']='eu', ['SB7']='jp', ['SBD']='au', ['SFB']='ca', ['YNF']='gs', ['YNH']='gs' },
  A1723 = { ['SBH']='us', ['SBJ']='uk', ['SBK']='eu', ['SEB']='ca' },
  A1725 = { ['SBV']='us', ['SBY']='jp', ['SF1']='sa', ['SF8']='ca' },
  A1726 = { ['0HP']='jp', ['0LD']='jp', ['136']='ca', ['DAV']='cn', ['UW0']='us' },
  A1727 = { ['0HR']='jp', ['0LF']='jp', ['0QV']='ca', ['WVB']='us' },
  A1728 = { ['ZF8']='us', ['ZG0']='eu', ['ZJ1']='ca' },
  A1729 = { ['RP8']='eu', ['S0U']='us', ['S0X']='uk', ['S18']='jp', ['S1A']='ca', ['ZG8']='us', ['ZJB']='ca' },
  A1753 = { ['5XG']='us', ['5ZA']='uk', ['5ZC']='jp', ['5ZD']='eu', ['5ZE']='au', ['5ZF']='cn', ['6TU']='ca', ['CLC']='jp', ['EAB']='jp' },
  A1754 = { ['5YU']='us', ['5ZG']='eu', ['5ZH']='uk', ['5ZJ']='jp', ['5ZK']='au', ['5ZR']='cn', ['5ZS']='za', ['6U6']='ca', ['EF8']='jp', ['NT1']='jp' },
  A1755 = { ['D7B']='us', ['D81']='uk', ['D82']='jp', ['D84']='au', ['D87']='ca', ['D89']='eu' },
  A1761 = { ['2JR']='jp', ['5YX']='us', ['5ZU']='uk', ['5ZV']='jp', ['5ZW']='za', ['5ZX']='au', ['5ZY']='cn', ['5ZZ']='tw', ['6NX']='eu', ['766']='ca', ['9FH']='jp', ['AA1']='jp', ['CKX']='jp', ['CLA']='jp', ['DQ0']='jp', ['Q62']='jp', ['Q64']='jp' },
  A1762 = { ['05U']='jp' },
  A1770 = { ['3LV']='uk', ['3LW']='au', ['3M6']='us', ['3M7']='eu', ['539']='ca', ['5N7']='cn', ['ZZ2']='jp' },
  A1771 = { ['3NH']='us', ['3NJ']='uk', ['3NK']='eu', ['3NL']='jp', ['3PA']='au', ['4S5']='za', ['5YM']='sa' },
  A1772 = { ['6TZ']='us', ['6UA']='ar', ['6XX']='eu', ['75Z']='jp', ['79W']='uk', ['7CU']='ca', ['7DR']='au', ['7TP']='sa' },
  A1780 = { ['25N']='au', ['4QY']='ca', ['X2Y']='us', ['X5F']='jp', ['ZWR']='uk', ['ZX4']='eu' },
  A1781 = { ['3NM']='us', ['3NN']='uk', ['3NP']='eu', ['3NQ']='jp', ['3PD']='au', ['69G']='ca', ['7NX']='sa', ['7R2']='ar' },
  A1790 = { ['6RT']='uk', ['6S6']='eu', ['73U']='au', ['744']='us', ['746']='jp', ['9PL']='ca', ['C2C']='us', ['C2F']='au', ['SPU']='uk', ['SQ9']='eu', ['T27']='ca', ['T29']='jp' },
  A7320 = { ['5ZA']='uk', ['5ZC']='jp', ['5ZD']='eu', ['5ZE']='au', ['5ZF']='cn', ['6TU']='ca', ['CLC']='jp', ['DKA']='us', ['EAB']='jp' },
  AX1C0 = { ['5ZA']='uk', ['5ZC']='jp', ['5ZE']='au', ['5ZF']='cn', ['6TU']='ca', ['CLC']='jp', ['EAB']='jp' },
}

function C.lookup(product_code, sku)
  if not product_code or not sku then return nil end
  local t = C.by_pn[product_code:upper()]
  return t and t[sku:upper()] or nil
end

return C
