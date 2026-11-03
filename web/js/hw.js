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
function num(v, d) {
  return typeof v === 'number' && Number.isFinite(v) ? v : d;
}

function bit(map, i) {
  const m = map >>> 0;
  return i === 31 ? m >= 2147483648 : (m & (1 << i)) !== 0;
}

function shapeProg(p) {
  return {
    owner: typeof p.owner === 'string' && p.owner ? p.owner : '?',
    offset: num(p.offset, 0),
    length: num(p.length, 0),
    origin: num(p.origin, -1),
    sm: num(p.sm, 0) >>> 0,
    irq: num(p.irq, 0) >>> 0,
    prot: p.prot === 1 || p.prot === true,
  };
}

function shapeBlock(b, i) {
  const programs = (Array.isArray(b.programs) ? b.programs : []).map(shapeProg);
  const map = num(b.instr_map, 0) >>> 0;
  const cells = [];
  for (let w = 0; w < 32; w++) {
    const taken = bit(map, w);
    const prog = taken ? programs.find((p) => w >= p.offset && w < p.offset + p.length) : null;
    cells.push({ word: w, taken, owner: prog ? prog.owner : (taken ? '?' : ''), prot: !!(prog && prog.prot) });
  }
  const smFree = num(b.sm_free, 0) >>> 0;
  const sms = [0, 1, 2, 3].map((sm) => ({
    sm,
    free: (smFree & (1 << sm)) !== 0,
    owners: programs.filter((p) => (p.sm & (1 << sm)) !== 0).map((p) => p.owner),
  }));
  const irqUsed = num(b.irq_used, 0) >>> 0;
  const irq = [0, 1, 2, 3].filter((f) => (irqUsed & (1 << f)) !== 0).map((flag) => ({
    flag,
    owners: programs.filter((p) => (p.irq & (1 << flag)) !== 0).map((p) => p.owner),
  }));
  return {
    pio: num(b.pio, i),
    gpio_base: num(b.gpio_base, -1),
    instr_used: num(b.instr_used, 0),
    instr_free: num(b.instr_free, 0),
    instr_map: map,
    cells,
    sms,
    irq,
    irq_lines: [0, 1].filter((l) => (num(b.irq_lines, 0) & (1 << l)) !== 0),
    programs,
  };
}

/** pio.status into something the Hardware window can draw. No DOM. */
export function pioView(status) {
  const blocks = (Array.isArray(status?.blocks) ? status.blocks : []).map(shapeBlock);
  let words = 0;
  let machines = 0;
  for (const b of blocks) {
    words += b.instr_used;
    machines += b.sms.filter((s) => !s.free).length;
  }
  const summary = blocks.length
    ? `${blocks.length} block${blocks.length === 1 ? '' : 's'} · ${words} of ${blocks.length * 32} words · ${machines} machine${machines === 1 ? '' : 's'}`
    : 'no PIO status';
  return { blocks, summary };
}

/** One program, as a single line. Origin -1 is the loader's choice. */
export function pioProgLine(p) {
  const sm = [0, 1, 2, 3].filter((i) => p.sm & (1 << i));
  const end = p.offset + Math.max(p.length, 1) - 1;
  return `${p.owner}  words ${p.offset}–${end} (${p.length})  ${sm.length ? `SM ${sm.join(',')}` : 'no SM'}  ${p.origin < 0 ? 'origin auto' : `origin ${p.origin}`}${p.prot ? '  protected' : ''}`;
}

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
