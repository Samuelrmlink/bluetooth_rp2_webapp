// What a device window shows about a control, kept DOM-free and tested.
//
// A control shows what the DEVICE reports, never what was last clicked. A
// write marks the field pending (drawn dimmed, at the value asked for) until
// a telemetry sample says so -- confirmed -- or the time runs out and the
// device's own value is put back, with a notice. A write the device refuses
// outright is reverted at once. Without this a switch can sit "on" over an
// outlet that is off, which is worse than no switch at all.

// Loose equality for device values: 1 == true == "1", 60 == "60".
export function same(a, b) {
  if (a === b) return true;
  if (a == null || b == null) return false;
  if (typeof a === 'boolean' || typeof b === 'boolean') return !!Number(a) === !!Number(b);
  return Number(a) === Number(b) && !Number.isNaN(Number(a));
}

// A reported value close enough to the target to confirm a write. A
// countdown keeps ticking between the write and the next sample, so it can
// never equal what was sent: `tolerance` is how far off it may be.
export function near(actual, target, tolerance = 0) {
  if (!tolerance) return same(actual, target);
  const a = Number(actual), t = Number(target);
  return actual != null && Number.isFinite(a) && Math.abs(a - t) <= tolerance;
}

export class Pending {
  constructor(timeoutMs = 8000) {
    this.timeoutMs = timeoutMs;
    this.map = new Map();   // field -> { target, since, tolerance }
  }

  begin(field, target, now = Date.now(), tolerance = 0) { this.map.set(field, { target, since: now, tolerance }); }
  cancel(field) { this.map.delete(field); }
  get(field) { return this.map.get(field); }
  get size() { return this.map.size; }

  // A new telemetry sample: which pending writes it confirms, and which have
  // waited too long -- the device reported something else, or nothing.
  observe(sample, now = Date.now()) {
    const confirmed = [];
    const expired = [];
    for (const [field, p] of this.map) {
      if (field in sample && near(sample[field], p.target, p.tolerance)) {
        confirmed.push(field);
        this.map.delete(field);
      } else if (now - p.since > this.timeoutMs) {
        expired.push({ field, target: p.target, actual: sample[field] });
        this.map.delete(field);
      }
    }
    return { confirmed, expired };
  }
}

// Fields every driver adds to say WHICH device a sample is from. Shown in a
// window's header, never as data to classify.
export const IDENTITY = new Set(['addr', 'product_type', 'product_code', 'model_name', 'region',
  'cipher', 'serial', 'serial_number', 'build', 'body_hex']);

// The fields no control has claimed yet -- the Unclassified window's rows.
// It shrinks as fields are promoted into controls.
export function unclassified(sample, controls) {
  const claimed = new Set((controls || []).map((c) => c.field).filter(Boolean));
  return Object.keys(sample || {}).filter((k) => !claimed.has(k) && !IDENTITY.has(k)).sort();
}

// Every field of a sample, for the All Data window, and what already shows
// each one: a control's label, 'device identity', or '' (nothing yet -- the
// fields the "Unclassified only" view keeps).
export function shownAs(controls) {
  const m = new Map();
  for (const c of controls || []) if (c.field && !m.has(c.field)) m.set(c.field, c.label || c.field);
  return (field) => m.get(field) || (IDENTITY.has(field) ? 'device identity' : '');
}

export function allFields(sample) {
  return Object.keys(sample || {}).sort();
}

// How the Unclassified window shows a value. Some schema fields are raw
// bytes decoded as a string (capability_block, memory_port_bits); printed
// as text they are unreadable boxes, and this window is where unknown fields
// get worked out -- so anything with control characters is shown as hex.
export function displayValue(v) {
  if (v == null) return '\u2014';
  if (typeof v === 'object') return JSON.stringify(v);
  const s = String(v);
  // eslint-disable-next-line no-control-regex
  if (typeof v === 'string' && /[\x00-\x08\x0b-\x1f\x7f-\x9f\ufffd]/.test(s)) {
    return [...s].map((c) => c.charCodeAt(0).toString(16).padStart(2, '0')).join(' ') + '  (hex)';
  }
  return s;
}
