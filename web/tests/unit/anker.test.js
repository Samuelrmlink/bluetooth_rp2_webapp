import { test } from 'node:test';
import assert from 'node:assert/strict';
import { normalizeTelemetry, freshness, firmwareLabel,
         fmtVolts, fmtAmps, fmtWatts } from '../../js/anker.js';

/* A real report, as captured off the bench unit. */
const REAL = {
  addr: '7C:E9:13:60:1F:D8', serial: 'ASHDJW61F45100348',
  package_version: 1606, error_code: 0,
  c1_power: { mv: 5072, ma: 0, watts: 0.0, charging: true },
  c2_power: { mv: 5040, ma: 100, watts: 0.5, charging: true },
  c3_power: { mv: 19888, ma: 0, watts: 0.0, charging: true },
  c1_control: { on: true, cable: '60W', countdown_active: false, countdown_remain_s: 300 },
  c2_control: { on: true, cable: '60W', countdown_active: false, countdown_remain_s: 300 },
  c3_control: { on: true, cable: '240W', countdown_active: false, countdown_remain_s: 300 },
  backlight_pct: 25, screen_off: '12h', direction: 'up', gyroscope: 1,
  language: 'en', charging_protocol: 0, fixed_allocation: 'c1_priority',
  custom_mode_number: 147, support_function: 0, ability_info: 72,
};

test('firmwareLabel turns packageVersion into the printed version', () => {
  assert.equal(firmwareLabel(1606), 'v1.6.0.6');
  assert.equal(firmwareLabel(1), 'v0.0.0.1');
  assert.equal(firmwareLabel(undefined), null);
  assert.equal(firmwareLabel('1606'), null);
});

test('units convert from the wire scale', () => {
  assert.equal(fmtVolts(5072), '5.07 V');
  assert.equal(fmtAmps(100), '0.10 A');
  assert.equal(fmtWatts(0.5), '0.5 W');
  // A missing field must read as absent, not as zero -- those mean
  // different things on a charger.
  assert.equal(fmtVolts(undefined), '—');
  assert.equal(fmtAmps(null), '—');
});

test('a real report yields three ports with joined power and control', () => {
  const n = normalizeTelemetry(REAL);
  assert.equal(n.ports.length, 3);
  assert.equal(n.ports[0].volts, '5.07 V');
  assert.equal(n.ports[1].amps, '0.10 A');
  assert.equal(n.ports[2].cable, '240W');
  assert.equal(n.ports[2].volts, '19.89 V');
  assert.ok(n.ports.every(p => p.present && p.on && p.plugged));
});

test('only a port actually passing current counts as drawing', () => {
  const n = normalizeTelemetry(REAL);
  // charging:true with 0 mA means "port live", not "current flowing" --
  // colouring all three as active would make the pane useless.
  assert.equal(n.ports[0].live, true);
  assert.equal(n.ports[0].drawing, false);
  assert.equal(n.ports[1].drawing, true);
});

test('cable "none" reads as nothing plugged', () => {
  const n = normalizeTelemetry({ c1_control: { on: true, cable: 'none' } });
  assert.equal(n.ports[0].plugged, false);
  assert.equal(n.ports[0].cable, 'none');
});

test('total watts sums the ports', () => {
  assert.equal(normalizeTelemetry(REAL).totalWatts, 0.5);
});

test('settings are labelled and formatted', () => {
  const s = Object.fromEntries(normalizeTelemetry(REAL).settings.map(x => [x.key, x.value]));
  assert.equal(s.backlight_pct, '25%');
  assert.equal(s.gyroscope, 'on');
  assert.equal(s.charging_protocol, 'default');
  assert.equal(s.fixed_allocation, 'c1 priority');
  assert.equal(s.ability_info, '0x48');
});

test('custom mode 255 means none', () => {
  const s = normalizeTelemetry({ custom_mode_number: 255 }).settings;
  assert.equal(s.find(x => x.key === 'custom_mode_number').value, 'none');
});

test('a tag this file has never heard of is surfaced, not dropped', () => {
  // The Lua dictionary is the volatile part by design. A newly decoded tag
  // must appear without a webapp change, or the split stops paying off.
  const n = normalizeTelemetry({ ...REAL, newly_decoded_thing: 42 });
  assert.deepEqual(n.other, [{ key: 'newly_decoded_thing', value: '42' }]);
});

test('an empty report does not throw and shows nothing as present', () => {
  const n = normalizeTelemetry({});
  assert.equal(n.ports.length, 3);
  assert.ok(n.ports.every(p => !p.present));
  assert.equal(n.settings.length, 0);
  assert.equal(n.serial, null);
});

test('a field whose shape changed does not take the pane down', () => {
  // e.g. a firmware revision widens a struct and the Lua decoder returns a
  // string where a table used to be.
  const n = normalizeTelemetry({ c1_power: 'unexpected', c1_control: 7, backlight_pct: {} });
  assert.equal(n.ports[0].volts, '—');
  assert.equal(n.ports[0].on, false);
  assert.ok(Array.isArray(n.settings));
});

test('freshness decays from streaming to stalled', () => {
  const t0 = 1_000_000;
  assert.equal(freshness(t0, t0 + 500).state, 'live');
  assert.equal(freshness(t0, t0 + 5000).state, 'stale');
  assert.equal(freshness(t0, t0 + 30000).state, 'lost');
  assert.equal(freshness(0).state, 'none');
});
