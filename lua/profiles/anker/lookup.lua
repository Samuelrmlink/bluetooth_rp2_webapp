--[[
productType -> productCode and a human name.

The 2-byte productType in the advertisement is the model discriminator; the
productCode (pnCode) is what everything downstream is keyed on -- the country
table, and the per-model telemetry schema.

Generated from anker_c1000_protocol/producttype-productcode-table.md, whose
source is AKIoTKit's BleDeviceFactory.deviceModels. Entries marked inferred
are unconfirmed in that table and are flagged rather than dropped, because a
device we can name badly is still better than one we cannot name at all.

LOAD THIS, READ IT, UNLOAD IT. It is only needed between the handshake and
choosing a schema, and the whole point of the staged loading is that nothing
avoidable stays resident.
]]

local L = {}

-- [productType] = { code, name, inferred }
L.models = {
  ['B004'] = { 'A17C0', 'SOLIX Solarbank E1600 (Gen 1)' },
  ['B005'] = { 'A17B1', 'SOLIX Home Power Panel (for F3800, non-EU)' },
  ['B006'] = { 'A17C1', 'SOLIX Solarbank 2 E1600 Pro' },
  ['B007'] = { 'A17C2', 'SOLIX Solarbank 2 E1600 AC' },
  ['B008'] = { 'A17C3', 'SOLIX Solarbank 2 E1600 Plus' },
  ['B00A'] = { 'A17C5', 'SOLIX Solarbank 3 E2700' },
  ['B00B'] = { 'AE100', 'SOLIX Power Dock for Solarbank Multisystems' },
  ['B101'] = { 'A1781', 'SOLIX F2600 Portable Power Station' },
  ['B102'] = { 'A1772', 'SOLIX F1500 Portable Power Station' },
  ['B103'] = { 'A1753', 'SOLIX C800(P/X) Portable Power Station' },
  ['B104'] = { 'A1754', 'SOLIX C800(P/X) Portable Power Station' },
  ['B105'] = { 'A1761', 'SOLIX C1000(X) Portable Power Station' },
  ['B106'] = { 'A1790', 'SOLIX F3800 Portable Power Station' },
  ['B107'] = { 'A17Y0', 'SOLIX Solarbank E1600 0W Output Switch' },
  ['B108'] = { 'A1755', 'SOLIX C800(P/X) Portable Power Station' },
  ['B109'] = { 'A1722', 'SOLIX C300(X) AC Portable Power Station' },
  ['B10A'] = { 'A1780P', 'SOLIX F2000(P) Portable Power Station (Plus)' },
  ['B10B'] = { 'A1723', 'SOLIX C300(X) AC Portable Power Station' },
  ['B10D'] = { 'A1725', 'SOLIX C200(X)(DC) Portable Power Station' },
  ['B10E'] = { 'A1726', 'SOLIX C300(X) DC Portable Power Station' },
  ['B10F'] = { 'A1727', 'SOLIX C200(X)(DC) Portable Power Station' },
  ['B110'] = { 'A1782', 'SOLIX F3000 Portable Power Station' },
  ['B111'] = { 'A1762', 'SOLIX C1000 X Portable Power Station' },
  ['B112'] = { 'A1790P', 'SOLIX F3800 Plus Portable Power Station' },
  ['B113'] = { 'A1728', 'SOLIX C300(X) DC Portable Power Station' },
  ['B114'] = { 'A1729', 'SOLIX C200(X)(DC) Portable Power Station' },
  ['B115'] = { 'A17A3', 'SOLIX Everfrost 2 23L Powered Cooler' },
  ['B116'] = { 'A17A4', 'SOLIX Everfrost 2 40L Powered Cooler' },
  ['B117'] = { 'A17A5', 'SOLIX Everfrost 2 58L Powered Cooler' },
  ['B118'] = { 'A1763', 'SOLIX C1000(X) Gen 2 Portable Power Station' },
  ['B119'] = { 'A1765', 'SOLIX C1000(X) Gen 2 Portable Power Station' },
  ['B11A'] = { 'A1783', 'SOLIX C2000(X) Gen 2 Portable Power Station' },
  ['B11B'] = { 'A1785', 'SOLIX C2000(X) Gen 2 Portable Power Station' },
  ['B11C'] = { 'AX1C0', 'SOLIX X1 component (unconfirmed)', true },
  ['B11D'] = { 'AS200', 'SOLIX Alternator Charger' },
  ['B11E'] = { 'AS100', 'SOLIX C1000 Gen 2 LE Portable Power Station' },
  ['B201'] = { 'A17X7', 'Anker 3-Phase WiFi Smart Meter' },
  ['B202'] = { 'A17X8', 'Anker Smart Plug 2500 W' },
  ['B203'] = { 'A17X7US', 'Anker Smart Meter (US grid)' },
  ['B204'] = { 'AE1R0', 'SOLIX P1 Meter' },
  ['B301'] = { 'A5101', 'SOLIX X1 P6K (US) Power Module' },
  ['B302'] = { 'A5102', 'SOLIX X1 Energy Module 1P H(3.68-6)K' },
  ['B303'] = { 'A5103', 'SOLIX X1 Energy Module 3P H(5-12)K' },
  ['B304'] = { 'A5341', 'SOLIX X1 Backup Controller' },
  ['B310'] = { 'A5190', 'SOLIX V1 Smart EV Charger (variant)', true },
  ['B311'] = { 'A5191', 'SOLIX V1 Smart EV Charger' },
  ['B312'] = { 'A17E1', 'SOLIX E10 (US market)' },
  ['B313'] = { 'A7320', 'SOLIX Smart Generator 5500' },
  ['B314'] = { 'AX170', 'Power Dock for SOLIX E10 (US market)' },
  ['B401'] = { 'A91B2', 'Anker Prime Charging Station (8-in-1, 240W)' },
  ['B402'] = { 'A2345', 'Anker 250W Prime Charger' },
  ['B403'] = { 'A1341', 'Anker Prime Power Bank 50K (300W, retractable USB-C)' },
  ['B404'] = { 'A110A', 'Anker Prime Power Bank 26K (300W)' },
  ['B405'] = { 'A2687', 'Anker Prime 160W 3-Port GaN Charger (smart display)' },
  ['B406'] = { 'A110B', 'Anker Prime Power Bank 20K' },
  ['B407'] = { 'A1903', 'Anker 150W Charging Base' },
  ['B408'] = { 'A25X7', 'Anker Prime Wireless Charger' },
  ['B409'] = { 'A2693', 'Anker charger (unconfirmed)', true },
  ['B40A'] = { 'A110G', 'Anker Prime Power Bank 20K' },
}

-- Returns productCode, name, inferred -- or nil when the type is unknown,
-- which is expected: the table is a snapshot of one SDK build and Anker ships
-- product types that postdate it.
function L.lookup(product_type)
  local e = L.models[product_type and product_type:upper()]
  if not e then return nil end
  return e[1], e[2], e[3] == true
end

return L
