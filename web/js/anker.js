/*
 * Shaping Anker telemetry for display. No DOM, no socket.
 *
 * The firmware sends whatever the Lua profile's field dictionary produced,
 * and that dictionary is the most volatile thing in the project by design --
 * it is the part being reverse-engineered. So this layer assumes as little as
 * possible: every field is optional, an unknown field is shown rather than
 * dropped, and a field whose shape changed must not take the pane down with
 * it. A telemetry pane that goes blank because one tag grew a byte is worse
 * than one that shows a stale number.
 */

/** 1606 -> "v1.6.0.6". Each digit of the zero-padded 4-digit packageVersion
 *  is one component. Confirmed against exactly one unit (firmware v1.6.0.6
 *  reporting 1606), so it is a display convenience, not a fact to rely on. */
export function firmwareLabel(packageVersion) {
  if (typeof packageVersion !== 'number' || !Number.isFinite(packageVersion)) return null;
  const s = String(Math.trunc(packageVersion)).padStart(4, '0');
  return 'v' + s.split('').join('.');
}

export function fmtVolts(mv) {
  return typeof mv === 'number' ? (mv / 1000).toFixed(2) + ' V' : '—';
}
export function fmtAmps(ma) {
  return typeof ma === 'number' ? (ma / 1000).toFixed(2) + ' A' : '—';
}
export function fmtWatts(w) {
  return typeof w === 'number' ? w.toFixed(1) + ' W' : '—';
}

function fmtDuration(sec) {
  if (typeof sec !== 'number' || sec <= 0) return null;
  const m = Math.floor(sec / 60), s = sec % 60;
  return m ? `${m}m${s ? ' ' + s + 's' : ''}` : `${s}s`;
}

/* The three USB-C ports. power and control arrive as separate tags -- A5..A7
 * and AC..AE -- and are joined here so the pane can draw one card per port. */
const PORTS = [
  { id: 'c1', label: 'C1', power: 'c1_power', control: 'c1_control' },
  { id: 'c2', label: 'C2', power: 'c2_power', control: 'c2_control' },
  { id: 'c3', label: 'C3', power: 'c3_power', control: 'c3_control' },
];

/* Tags that are settings rather than measurements, with how to show them.
 * Anything present in the telemetry and NOT named here still gets displayed,
 * as raw JSON under "other" -- see below. */
const SETTINGS = [
  ['backlight_pct',      'backlight',   v => `${v}%`],
  ['screen_off',         'screen off',  v => String(v)],
  ['direction',          'rotation',    v => String(v)],
  ['gyroscope',          'auto-rotate', v => (v ? 'on' : 'off')],
  ['language',           'language',    v => String(v)],
  ['charging_protocol',  'protocol',    v => (v === 0 ? 'default' : String(v))],
  ['fixed_allocation',   'allocation',  v => String(v).replace(/_/g, ' ')],
  ['custom_mode_number', 'custom mode', v => (v === 255 ? 'none' : String(v))],
  ['support_function',   'capabilities',v => String(v)],
  ['ability_info',       'abilities',   v => '0x' + Number(v).toString(16)],
];

const KNOWN = new Set([
  'addr', 'serial', 'truncated', 'package_version', 'error_code',
  ...PORTS.flatMap(p => [p.power, p.control]),
  ...SETTINGS.map(s => s[0]),
]);

/**
 * Turn one telemetry event into what the pane draws.
 * Never throws: a malformed field yields a dash, not an exception.
 */
export function normalizeTelemetry(t, now = Date.now()) {
  t = t || {};
  const ports = PORTS.map(p => {
    const pw = t[p.power] || {};
    const ct = t[p.control] || {};
    return {
      id: p.id,
      label: p.label,
      mv: pw.mv, ma: pw.ma, watts: pw.watts,
      volts: fmtVolts(pw.mv), amps: fmtAmps(pw.ma), power: fmtWatts(pw.watts),
      // "charging" is the device's own status byte; it reads 1 even at 0 mA
      // when a cable is present, so it means "port live", not "current flowing".
      live: pw.charging === true,
      drawing: typeof pw.ma === 'number' && pw.ma > 0,
      on: ct.on === true,
      cable: ct.cable ?? null,
      plugged: ct.cable != null && ct.cable !== 'none',
      countdown: ct.countdown_active ? fmtDuration(ct.countdown_remain_s) : null,
      present: p.power in t || p.control in t,
    };
  });

  const settings = SETTINGS
    .filter(([k]) => t[k] !== undefined && t[k] !== null)
    .map(([k, label, fmt]) => {
      let value;
      try { value = fmt(t[k]); } catch { value = String(t[k]); }
      return { key: k, label, value };
    });

  // Anything the profile started sending that this file does not know about.
  // Showing it is how a newly decoded tag becomes visible without a webapp
  // change -- which is the same argument as putting the dictionary in Lua.
  const other = Object.entries(t)
    .filter(([k]) => !KNOWN.has(k))
    .map(([k, v]) => ({ key: k, value: typeof v === 'object' ? JSON.stringify(v) : String(v) }));

  return {
    addr: t.addr ?? null,
    serial: t.serial ?? null,
    firmware: firmwareLabel(t.package_version),
    error: typeof t.error_code === 'number' ? t.error_code : null,
    truncated: t.truncated === true,
    totalWatts: ports.reduce((a, p) => a + (typeof p.watts === 'number' ? p.watts : 0), 0),
    ports, settings, other,
    at: now,
  };
}

/** How stale the last report is, in the terms the pane shows. The profile
 *  pushes at about 1 Hz, so a couple of seconds of silence is already worth
 *  saying out loud rather than leaving a number that looks live. */
export function freshness(at, now = Date.now()) {
  if (!at) return { state: 'none', text: 'no data' };
  const age = now - at;
  if (age < 2500) return { state: 'live', text: 'streaming' };
  if (age < 15000) return { state: 'stale', text: `${(age / 1000).toFixed(0)}s ago` };
  return { state: 'lost', text: `stalled ${(age / 1000).toFixed(0)}s` };
}
