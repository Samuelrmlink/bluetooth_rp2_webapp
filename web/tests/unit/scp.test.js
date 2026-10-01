import { test } from 'node:test';
import assert from 'node:assert/strict';
import { portPower, portControl } from '../../js/devices/anker-scp/plugin.js';

test('A2687 port power reads as volts, amps and watts, or idle', () => {
  assert.equal(portPower({ mv: 20080, ma: 2980, watts: 59.8, charging: true }), '20.08 V · 2.98 A · 59.8 W');
  assert.equal(portPower({ mv: 0, ma: 0, watts: 0, charging: false }), 'idle');
  assert.equal(portPower(null), '—');
});

test('A2687 port control: on/off, the cable, a running countdown', () => {
  assert.equal(portControl({ on: true, cable: '60W', countdown_active: false }), 'on · 60W cable');
  assert.equal(portControl({ on: false, cable: 'none' }), 'off');
  assert.equal(portControl({ on: true, cable: '100W', countdown_active: true, countdown_remain_s: 290 }), 'on · 100W cable · off in 5 min');
});
