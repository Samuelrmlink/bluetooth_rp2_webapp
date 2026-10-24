import { test } from 'node:test';
import assert from 'node:assert/strict';
import { uplinkLine, uplinkLabel, policyOptions, switchText } from '../../js/settings/uplinks.js';

test('an uplink label carries the address, mask and gateway when they exist', () => {
  assert.equal(uplinkLabel({ name: 'b0', kind: 'nap' }), 'b0 (nap)');
  assert.equal(uplinkLabel({ name: 'b0', kind: 'nap', ip: '0.0.0.0', mask: '0.0.0.0', gw: '0.0.0.0' }), 'b0 (nap)');
  assert.equal(uplinkLabel({
    name: 'b0', kind: 'nap', ip: '192.168.44.15', mask: '255.255.255.0', gw: '192.168.44.1',
  }), 'b0 (nap) 192.168.44.15 / 255.255.255.0 gw 192.168.44.1');
});


test('one line per uplink: active marker, state, rtt ± jitter, loss, metric', () => {
  assert.equal(uplinkLine({ active: true, state: 'ok', rtt_ms: 21, jitter_ms: 3, loss_pct: 0, metric: 27 }),
    '● ok · 21 ms ±3 · 0% loss · metric 27');
  assert.equal(uplinkLine({ active: false, state: 'measuring', rtt_ms: -1, jitter_ms: -1, loss_pct: 0, metric: -1 }),
    'measuring… · 0% loss');
  assert.equal(uplinkLine({ active: false, state: 'no_gateway', rtt_ms: -1, loss_pct: 0, metric: -1 }),
    'no gateway (not checked)');
  assert.match(uplinkLine({ active: true, state: 'failed', rtt_ms: 40, jitter_ms: 2, loss_pct: 15, metric: 194 }), /^● FAILED/);
});

test('the policy menu offers fixed:<iface> for each uplink the build has', () => {
  const v = policyOptions([{ name: 'w0', kind: 'wifi' }, { name: 'e0', kind: 'ethernet' }]).map(([k]) => k);
  assert.deepEqual(v, ['best', 'failover', 'fixed:w0', 'fixed:e0']);
});

test('switch history', () => {
  assert.equal(switchText({ switches: 0 }), 'none yet');
  assert.equal(switchText({ switches: 3, last_switch_ms_ago: 4500, last_switch: { from: 'w0', to: 'e0', reason: 'active failed' } }),
    '3 · last w0 → e0 (active failed) 5 s ago');
});
