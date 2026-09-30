import { test } from 'node:test';
import assert from 'node:assert/strict';
import { same, Pending, unclassified, IDENTITY, displayValue } from '../../js/controls/state.js';

test('same: device values compare loosely', () => {
  assert.ok(same(1, true));
  assert.ok(same(0, false));
  assert.ok(same('60', 60));
  assert.ok(!same(50, 60));
  assert.ok(!same(null, 0));
  assert.ok(!same(undefined, false));
});

test('a write is pending until telemetry confirms it', () => {
  const p = new Pending(8000);
  p.begin('ac_output_enabled', 0, 1000);
  let r = p.observe({ ac_output_enabled: 1 }, 2000);
  assert.deepEqual(r, { confirmed: [], expired: [] }, 'not yet: the device still says 1');
  assert.ok(p.get('ac_output_enabled'));
  r = p.observe({ ac_output_enabled: 0 }, 3000);
  assert.deepEqual(r.confirmed, ['ac_output_enabled']);
  assert.equal(p.size, 0);
});

test('a write the device never takes expires, reporting what it said instead', () => {
  const p = new Pending(8000);
  p.begin('ac_frequency_hz', 50, 0);
  const r = p.observe({ ac_frequency_hz: 60 }, 9000);
  assert.deepEqual(r.expired, [{ field: 'ac_frequency_hz', target: 50, actual: 60 }]);
  assert.equal(p.size, 0);
});

test('a sample without the field does not confirm it', () => {
  const p = new Pending(8000);
  p.begin('x', 1, 0);
  assert.deepEqual(p.observe({ y: 1 }, 100), { confirmed: [], expired: [] });
});

test('unclassified: every field no control claims, minus identity', () => {
  const sample = { addr: 'E8', product_code: 'A1761', battery_pct: 27, ac_output_power_w: 22, unknown_b1: 3 };
  const controls = [{ kind: 'readout', field: 'battery_pct' }, { kind: 'switch', field: 'ac_output_enabled' }];
  assert.deepEqual(unclassified(sample, controls), ['ac_output_power_w', 'unknown_b1']);
  assert.ok(IDENTITY.has('addr'));
});

test('displayValue: binary strings as hex, everything else as itself', () => {
  assert.equal(displayValue('\x01\x00\xff'), '01 00 ff  (hex)');
  assert.equal(displayValue('SOLIX C1000'), 'SOLIX C1000');
  assert.equal(displayValue(27), '27');
  assert.equal(displayValue({ a: 1 }), '{"a":1}');
  assert.equal(displayValue(null), '\u2014');
});
