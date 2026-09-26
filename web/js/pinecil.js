/*
 * Shaping Pinecil telemetry for display. No DOM, no socket.
 *
 * Same contract as anker.js and for the same reason: the Lua field dictionary
 * is the volatile half, so every field is optional and a shape that changed
 * must not take the pane down.
 */

export const MODES = ['idle', 'soldering', 'boost', 'sleeping', 'settings', 'debug'];

export function fmtTemp(c, digits = 0) {
  return typeof c === 'number' ? `${c.toFixed(digits)} °C` : '—';
}
export function fmtVolts(v) {
  return typeof v === 'number' ? `${v.toFixed(1)} V` : '—';
}
export function fmtWatts(w) {
  return typeof w === 'number' ? `${w.toFixed(1)} W` : '—';
}

/** Seconds -> "1h 04m" / "12m 30s" / "45s". Uptime runs to hours. */
export function fmtUptime(sec) {
  if (typeof sec !== 'number' || sec < 0) return '—';
  const s = Math.floor(sec % 60), m = Math.floor(sec / 60) % 60, h = Math.floor(sec / 3600);
  if (h) return `${h}h ${String(m).padStart(2, '0')}m`;
  if (m) return `${m}m ${String(s).padStart(2, '0')}s`;
  return `${s}s`;
}

/**
 * How far the tip is between room temperature and the setpoint, 0..1.
 *
 * Anchored at 20 °C rather than 0: a tip sitting at room temperature should
 * read as an empty bar, not as already a third of the way there, which is
 * what dividing by the setpoint alone would show.
 */
export function heatProgress(tip, setpoint, ambient = 20) {
  if (typeof tip !== 'number' || typeof setpoint !== 'number') return 0;
  if (setpoint <= ambient) return 0;
  const p = (tip - ambient) / (setpoint - ambient);
  return Math.max(0, Math.min(1, p));
}

/** What the tip is doing, which is not the same as the operating mode: an
 *  iron in "soldering" mode at temperature is holding, not heating. */
export function thermalState(t) {
  if (!t || typeof t.tip_c !== 'number') return 'unknown';
  if (t.mode === 'sleeping') return 'sleeping';
  if (typeof t.power_pct === 'number' && t.power_pct > 5) {
    return (typeof t.setpoint_c === 'number' && t.tip_c >= t.setpoint_c - 5)
      ? 'holding' : 'heating';
  }
  if (t.tip_c > 50) return 'cooling';
  return 'cold';
}

export function normalize(t, now = Date.now()) {
  t = t || {};
  const tip = typeof t.tip_c === 'number' ? t.tip_c : null;
  const set = typeof t.setpoint_c === 'number' ? t.setpoint_c : null;
  return {
    addr: t.addr ?? null,
    build: t.build ?? null,
    serial: t.serial ?? null,
    tip, setpoint: set,
    tipText: fmtTemp(tip),
    setpointText: fmtTemp(set),
    handleText: fmtTemp(t.handle_c, 1),
    inputText: fmtVolts(t.input_v),
    wattsText: fmtWatts(t.watts),
    powerPct: typeof t.power_pct === 'number' ? t.power_pct : null,
    progress: heatProgress(tip, set),
    state: thermalState(t),
    mode: t.mode ?? null,
    source: t.power_source ?? null,
    tipOhms: typeof t.tip_ohms === 'number' ? `${t.tip_ohms.toFixed(1)} Ω` : '—',
    maxTip: t.max_tip_c ?? null,
    uptimeText: fmtUptime(t.uptime_s),
    idleText: fmtUptime(typeof t.uptime_s === 'number' && typeof t.last_move_s === 'number'
      ? t.uptime_s - t.last_move_s : undefined),
    hall: t.hall_uT ?? null,
    at: now,
  };
}

export function freshness(at, now = Date.now()) {
  if (!at) return { state: 'none', text: 'no data' };
  const age = now - at;
  // The profile polls at 1 Hz, but a poll is a round trip that can lose a
  // turn to something else on the cooperative loop, so the live window is a
  // little wider than the charger's.
  if (age < 3000) return { state: 'live', text: 'polling' };
  if (age < 15000) return { state: 'stale', text: `${(age / 1000).toFixed(0)}s ago` };
  return { state: 'lost', text: `stalled ${(age / 1000).toFixed(0)}s` };
}
