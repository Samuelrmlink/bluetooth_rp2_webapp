/*
 * Shaping board I/O for display. No DOM, no socket.
 *
 * The one rule worth encoding here: a RESERVED pin is not a claimed pin.
 * GPIO 23, 24, 25 and 29 are the CYW43 interface, and driving one kills WiFi
 * and Bluetooth together -- including the websocket you would use to undo it.
 * The pin broker already refuses them; the display must make it obvious why,
 * rather than showing them as merely busy.
 */

/* Which ADC channel sits on which pin, and which is the internal sensor.
 * Channel 3 is GPIO 29, which on a Pico 2 W is the radio's, so reading it is
 * not free the way 0-2 are. */
export const ADC_CHANNELS = [
  { ch: 0, pin: 26, label: 'ADC0', note: '' },
  { ch: 1, pin: 27, label: 'ADC1', note: '' },
  { ch: 2, pin: 28, label: 'ADC2', note: '' },
  { ch: 3, pin: 29, label: 'ADC3', note: 'shared with the radio' },
  { ch: 4, pin: null, label: 'temp', note: 'internal die sensor' },
];

export function pinClass(p) {
  if (!p) return 'unknown';
  if (p.state === 'reserved') return 'reserved';
  if (p.state === 'free') return 'free';
  return 'claimed';
}

/** A one-line explanation of a pin's state, for a tooltip. */
export function pinTitle(p) {
  if (!p) return '';
  if (p.state === 'reserved') {
    return `GPIO ${p.pin} is reserved by ${p.owner || 'the board'} and can never be claimed`
         + (p.owner === 'cyw43' ? ' — driving it would kill WiFi and Bluetooth' : '');
  }
  if (p.state === 'free') return `GPIO ${p.pin} is free`;
  return `GPIO ${p.pin} claimed by ${p.owner || '?'}`
       + (p.function && p.function !== 'none' ? ` as ${p.function}` : '');
}

export function summarise(pins) {
  const out = { total: 0, free: 0, claimed: 0, reserved: 0 };
  for (const p of pins || []) {
    out.total++;
    if (p.state === 'reserved') out.reserved++;
    else if (p.state === 'free') out.free++;
    else out.claimed++;
  }
  return out;
}

export function fmtVolts(v) {
  return typeof v === 'number' ? `${v.toFixed(3)} V` : '—';
}
export function fmtCelsius(c) {
  return typeof c === 'number' ? `${c.toFixed(1)} °C` : '—';
}

/** i2c.scan returns bare addresses; 7-bit I2C is 0x08..0x77 and anything
 *  outside that is a bug in the scan, not a device. */
export function fmtI2cResults(found) {
  if (!Array.isArray(found)) return { list: [], note: 'no result' };
  const list = found
    .filter((a) => typeof a === 'number' && a >= 0x08 && a <= 0x77)
    .map((a) => ({ addr: a, hex: '0x' + a.toString(16).padStart(2, '0') }));
  return {
    list,
    note: list.length ? `${list.length} device${list.length > 1 ? 's' : ''}`
                      : 'nothing responded',
  };
}
