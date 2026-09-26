import { test } from 'node:test';
import assert from 'node:assert/strict';
import { normalize, heatProgress, thermalState, fmtUptime, fmtTemp, freshness }
  from '../../js/pinecil.js';

/* A real report, read off the bench iron: cold, idle, on a USB-PD supply. */
const COLD = {
  addr: 'A8:17:10:22:EA:09', build: 'v2.23', serial: 'A8171022EA09',
  tip_c: 26, setpoint_c: 310, handle_c: 26.5, input_v: 27.5,
  power_pct: 0, power_source: 'PD', tip_ohms: 8.0,
  uptime_s: 2526, last_move_s: 1881, max_tip_c: 439,
  tip_uv: 797, hall_uT: 0, mode: 'idle', watts: 0.0,
};

test('a cold idle iron reads as cold, not as heating', () => {
  const n = normalize(COLD);
  assert.equal(n.tipText, '26 °C');
  assert.equal(n.setpointText, '310 °C');
  assert.equal(n.state, 'cold');
  assert.equal(n.mode, 'idle');
});

test('heat progress is anchored at room temperature', () => {
  // Dividing by the setpoint alone would show a room-temperature tip as
  // already a fifth of the way to 310, which is nonsense on a bar.
  assert.ok(heatProgress(20, 310) === 0);
  assert.ok(heatProgress(26, 310) < 0.03);
  assert.equal(heatProgress(310, 310), 1);
  assert.equal(heatProgress(400, 310), 1, 'clamped above the setpoint');
  assert.equal(heatProgress(10, 310), 0, 'clamped below ambient');
});

test('heat progress copes with a setpoint at or below ambient', () => {
  assert.equal(heatProgress(25, 20), 0);
  assert.equal(heatProgress(25, 10), 0);
});

test('thermal state distinguishes heating from holding', () => {
  // The operating mode says "soldering" for both; only the power level and
  // the distance to the setpoint tell them apart.
  assert.equal(thermalState({ tip_c: 150, setpoint_c: 310, power_pct: 90 }), 'heating');
  assert.equal(thermalState({ tip_c: 308, setpoint_c: 310, power_pct: 30 }), 'holding');
  assert.equal(thermalState({ tip_c: 200, setpoint_c: 310, power_pct: 0 }), 'cooling');
  assert.equal(thermalState({ tip_c: 26, setpoint_c: 310, power_pct: 0 }), 'cold');
  assert.equal(thermalState({ tip_c: 300, mode: 'sleeping', power_pct: 10 }), 'sleeping');
  assert.equal(thermalState({}), 'unknown');
});

test('uptime is readable at every scale', () => {
  assert.equal(fmtUptime(45), '45s');
  assert.equal(fmtUptime(750), '12m 30s');
  assert.equal(fmtUptime(3840), '1h 04m');
  assert.equal(fmtUptime(undefined), '—');
});

test('idle time is uptime minus last movement', () => {
  const n = normalize(COLD);
  assert.equal(n.idleText, fmtUptime(2526 - 1881));
});

test('a missing temperature reads as absent, not as zero', () => {
  assert.equal(fmtTemp(undefined), '—');
  const n = normalize({});
  assert.equal(n.tipText, '—');
  assert.equal(n.progress, 0);
  assert.equal(n.state, 'unknown');
});

test('a field whose shape changed does not throw', () => {
  const n = normalize({ tip_c: 'hot', handle_c: {}, uptime_s: null });
  assert.equal(n.tipText, '—');
  assert.equal(n.handleText, '—');
  assert.equal(n.uptimeText, '—');
});

test('freshness allows a wider window than a pushing device', () => {
  const t0 = 1_000_000;
  // Polling costs a round trip that can lose a turn on the cooperative loop.
  assert.equal(freshness(t0, t0 + 2500).state, 'live');
  assert.equal(freshness(t0, t0 + 6000).state, 'stale');
  assert.equal(freshness(t0, t0 + 30000).state, 'lost');
});
