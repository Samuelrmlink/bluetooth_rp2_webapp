// Advertising data (AD structures), parsed in the browser.
//
// The firmware sends each device's raw advertisement and scan response
// (`adv`, `rsp`, hex; fw 1.85+) and a few fields it parsed itself. This reads
// every AD structure from the raw bytes, keeping what the firmware's merge
// loses -- complete vs shortened names, 32- and 128-bit UUIDs, service data,
// complete vs incomplete UUID lists. DOM-free (tests/unit/adparse.test.js).
//
// AD structure: length(1) type(1) data(length-1). Bluetooth Core Supplement
// part A; type numbers from the Assigned Numbers "Common Data Types".

const hexOf = (b) => [...b].map((x) => x.toString(16).padStart(2, '0')).join('');
const bytesOf = (hex) => {
  const h = String(hex || '').replace(/[^0-9a-f]/gi, '');
  const out = new Uint8Array(h.length >> 1);
  for (let i = 0; i < out.length; i++) out[i] = parseInt(h.substr(i * 2, 2), 16);
  return out;
};

// Little-endian UUID bytes -> the usual text form.
function uuid(b) {
  if (b.length === 2) return ((b[1] << 8) | b[0]).toString(16).padStart(4, '0');
  if (b.length === 4) return hexOf([...b].reverse());
  const h = hexOf([...b].reverse());
  return `${h.slice(0, 8)}-${h.slice(8, 12)}-${h.slice(12, 16)}-${h.slice(16, 20)}-${h.slice(20)}`;
}
function uuids(b, size) {
  const out = [];
  for (let i = 0; i + size <= b.length; i += size) out.push(uuid(b.subarray(i, i + size)));
  return out;
}
const text = (b) => {
  try { return new TextDecoder('utf-8', { fatal: false }).decode(b).replace(/\0+$/, ''); } catch { return ''; }
};

// A few company identifiers worth naming (Assigned Numbers, "Company
// Identifiers"); anything else shows as its hex.
export const COMPANIES = {
  0x0006: 'Microsoft', 0x000f: 'Broadcom', 0x004c: 'Apple', 0x0059: 'Nordic Semiconductor',
  0x0075: 'Samsung', 0x0087: 'Garmin', 0x00e0: 'Google', 0x0131: 'Cypress', 0x0157: 'Huami',
  0x02e5: 'Espressif', 0x038f: 'Xiaomi', 0x0822: 'Anker (Fantasia Trading)', 0xffff: 'none (testing)',
};

const FLAG_NAMES = [
  [0x01, 'LE Limited'], [0x02, 'LE General'], [0x04, 'no BR/EDR'],
  [0x08, 'LE+BR/EDR controller'], [0x10, 'LE+BR/EDR host'],
];

// Parse one payload into `out` (shared between adv and rsp so fields merge).
function parseInto(hex, out, where) {
  const b = bytesOf(hex);
  let i = 0;
  while (i < b.length) {
    const len = b[i];
    if (len === 0) break;                          // the rest is padding
    if (i + 1 + len > b.length) { out.malformed = `${where}: AD at byte ${i} runs past the end`; break; }
    const type = b[i + 1];
    const d = b.subarray(i + 2, i + 1 + len);
    switch (type) {
      case 0x01: out.flags = d[0] ?? 0; break;
      case 0x02: out.uuid16.incomplete.push(...uuids(d, 2)); break;
      case 0x03: out.uuid16.complete.push(...uuids(d, 2)); break;
      case 0x04: out.uuid32.incomplete.push(...uuids(d, 4)); break;
      case 0x05: out.uuid32.complete.push(...uuids(d, 4)); break;
      case 0x06: out.uuid128.incomplete.push(...uuids(d, 16)); break;
      case 0x07: out.uuid128.complete.push(...uuids(d, 16)); break;
      case 0x08: out.nameShort = text(d); break;
      case 0x09: out.nameComplete = text(d); break;
      case 0x0a: out.txPower = d.length ? (d[0] << 24) >> 24 : null; break;
      case 0x14: out.solicited.push(...uuids(d, 2)); break;
      case 0x15: out.solicited.push(...uuids(d, 16)); break;
      case 0x1f: out.solicited.push(...uuids(d, 4)); break;
      case 0x16: if (d.length >= 2) out.serviceData.push({ uuid: uuid(d.subarray(0, 2)), data: hexOf(d.subarray(2)) }); break;
      case 0x20: if (d.length >= 4) out.serviceData.push({ uuid: uuid(d.subarray(0, 4)), data: hexOf(d.subarray(4)) }); break;
      case 0x21: if (d.length >= 16) out.serviceData.push({ uuid: uuid(d.subarray(0, 16)), data: hexOf(d.subarray(16)) }); break;
      case 0x19: out.appearance = d.length >= 2 ? d[0] | (d[1] << 8) : null; break;
      case 0xff:
        if (d.length >= 2) {
          const company = d[0] | (d[1] << 8);
          out.mfg = { company, companyName: COMPANIES[company] || null, data: hexOf(d.subarray(2)), raw: hexOf(d) };
        }
        break;
      default: out.other.push({ type, data: hexOf(d) });
    }
    i += 1 + len;
  }
}

export function parseAd(advHex, rspHex) {
  const out = {
    flags: null, nameComplete: '', nameShort: '', txPower: null, appearance: null, mfg: null,
    uuid16: { complete: [], incomplete: [] }, uuid32: { complete: [], incomplete: [] },
    uuid128: { complete: [], incomplete: [] }, solicited: [], serviceData: [], other: [],
    raw: { adv: (advHex || '').toLowerCase(), rsp: (rspHex || '').toLowerCase() }, malformed: '',
  };
  if (advHex) parseInto(advHex, out, 'adv');
  if (rspHex) parseInto(rspHex, out, 'rsp');
  return out;
}

export function flagNames(flags) {
  if (flags == null) return '';
  return FLAG_NAMES.filter(([bit]) => flags & bit).map(([, n]) => n).join(', ') || `0x${flags.toString(16)}`;
}

// Every advertised service UUID, in a stable order, without repeats.
export function allServices(p) {
  return [...new Set([
    ...p.uuid16.complete, ...p.uuid16.incomplete, ...p.uuid32.complete, ...p.uuid32.incomplete,
    ...p.uuid128.complete, ...p.uuid128.incomplete,
  ])];
}

export function companyLabel(mfg) {
  if (!mfg) return '';
  const hex = `0x${mfg.company.toString(16).padStart(4, '0')}`;
  return mfg.companyName ? `${mfg.companyName} (${hex})` : hex;
}
