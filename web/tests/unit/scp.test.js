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

import { canEnable, customProblem, portState, voltsAmps, cable, device, PROTOCOLS_DEFAULT } from '../../js/devices/anker-scp/ports.js';

test('protocols: the default is everything but PD 12V; a fourth PD bit cannot be switched on', () => {
  assert.equal(PROTOCOLS_DEFAULT, 0x3b);
  assert.equal(canEnable(0x3b, 2), false);          // PD 12V with all three PPS ranges on
  assert.equal(canEnable(0x33, 2), true);           // one PPS range off: PD 12V may go on
  assert.equal(canEnable(0x3b, 0), true);           // UFCS and SCP are not PD bits
  assert.equal(canEnable(0x3b, 3), true);           // a bit that is on can always stay on
});

test('custom mode: each port 0 or 15..140 W, 160 W in all', () => {
  assert.equal(customProblem({ c1_w: 140, c2_w: 20, c3_w: 0 }), null);
  assert.match(customProblem({ c1_w: 10, c2_w: 0, c3_w: 0 }), /C1 takes 0 or 15–140/);
  assert.match(customProblem({ c1_w: 140, c2_w: 40, c3_w: 0 }), /180 W in all/);
  assert.match(customProblem({ c1_w: 141, c2_w: 0, c3_w: 0 }), /C1/);
});

test('a port card: its state word and its lines', () => {
  assert.equal(portState({ mv: 20080, ma: 2980, watts: 59.8 }, 1), 'charging');
  assert.equal(portState({ mv: 5080, ma: 0, watts: 0 }, 1), 'idle');
  assert.equal(portState({ mv: 0, ma: 0, watts: 0 }, 1), 'empty');
  assert.equal(portState({ mv: 5080, ma: 0, watts: 0 }, 0), 'off');
  assert.equal(voltsAmps({ mv: 5104, ma: 773 }), '5.10 V · 0.77 A');
  assert.equal(voltsAmps({ mv: 0, ma: 0 }), '— V · — A');
  assert.equal(cable({ cable: '60W' }), '60W cable');
  assert.equal(cable({ cable: 'none' }), 'no cable');
  assert.equal(device({ c2: 'none' }, 2), 'not identified');
  assert.equal(device({ c1: { brand: 3, model: 17 } }, 1), 'brand 3 · model 17');
});

import { powerSections, modeQuestion, modeNote } from '../../js/devices/anker-scp/ports.js';

test('power section: Standard\'s option only in Standard, the custom editor only in Custom', () => {
  assert.deepEqual(powerSections(0), { standard: false, custom: false });
  assert.deepEqual(powerSections(1), { standard: true, custom: false });
  assert.deepEqual(powerSections(2), { standard: false, custom: true });
  assert.deepEqual(powerSections('2'), { standard: false, custom: true });
  // not reported yet: neither (null is not mode 0's Number(null) === 0)
  assert.deepEqual(powerSections(undefined), { standard: false, custom: false });
  assert.deepEqual(powerSections(null), { standard: false, custom: false });
  assert.deepEqual(powerSections(''), { standard: false, custom: false });
  // a switch the charger has not confirmed yet: neither, whatever it was in
  assert.deepEqual(powerSections(2, true), { standard: false, custom: false });
  assert.deepEqual(powerSections(1, true), { standard: false, custom: false });
  assert.deepEqual(powerSections(7), { standard: false, custom: false });
});

test('a mode change asks first; the question says what the change does', () => {
  const s = { charging_protocol: 2, c3_w: 11.2, custom_power: { c1_w: 90, c2_w: 70, c3_w: 0, auto_exit: true } };
  assert.equal(modeQuestion(2, s), null);                               // no change: nothing to ask
  assert.match(modeQuestion(0, s), /from Custom to AI Mode 2\.0/);
  assert.match(modeQuestion(1, { charging_protocol: 0 }), /from AI Mode 2\.0 to Standard/);
  const q = modeQuestion(2, { ...s, charging_protocol: 0 });
  assert.match(q, /C1 90 W · C2 70 W · C3 0 W/);
  assert.match(q, /C3 is in use and would get no power/);
  assert.match(q, /Automatic deactivation is on/);
  const quiet = modeQuestion(2, { charging_protocol: 1, c3_w: 0, custom_power: { c1_w: 100, c2_w: 30, c3_w: 30, auto_exit: true } });
  assert.doesNotMatch(quiet, /no power|deactivation/);                  // every port powered: nothing to warn of
  assert.match(modeQuestion(1, {}), /from its current mode to Standard/);
});

test('the mode note says what the charger is doing, and why Custom may end by itself', () => {
  assert.equal(modeNote({}), '');
  assert.match(modeNote({ charging_protocol: 0 }), /shares its 160 W/);
  assert.match(modeNote({ charging_protocol: 1 }), /option below/);
  assert.equal(modeNote({ charging_protocol: 2, custom_power: { c1_w: 100, c2_w: 30, c3_w: 30, auto_exit: true } }),
    'Custom: C1 100 W · C2 30 W · C3 30 W.');
  assert.match(modeNote({ charging_protocol: 2, custom_power: { c1_w: 90, c2_w: 70, c3_w: 0, auto_exit: true } }),
    /returns the charger to AI Mode/);
});

import { recordsFromHex, seriesOf, axis, timeOf } from '../../js/devices/anker-scp/history.js';

test('history: records from a packet, the charger\'s own last packet of 2026-10-03', () => {
  const r = recordsFromHex('d8130000f8130303ffffffffd813000010141f03ffffffffd8130000d813be01c0130000', 3);
  assert.deepEqual(r[0], { c1: { mv: 5080, ma: 0 }, c2: { mv: 5112, ma: 771 }, c3: null });
  assert.deepEqual(r[2].c2, { mv: 5080, ma: 446 });
  assert.deepEqual(r[2].c3, { mv: 5056, ma: 0 });
});

test('history: series per metric; Total sums the ports there are; a missing port is null', () => {
  const r = recordsFromHex('d8130000f8130303ffffffff', 1);
  const w = seriesOf(r, 'w');
  assert.deepEqual(w.map((s) => s.name), ['C1', 'C2', 'C3']);
  assert.equal(w[2].values[0], null);
  assert.equal(seriesOf(r, 'total')[0].values[0].toFixed(2), '3.94');
  assert.equal(seriesOf(r, 'v')[1].values[0], 5.112);
  assert.equal(seriesOf(r, 'a')[1].values[0], 0.771);
});

test('history: a clean axis, and the time of each record (the last is now)', () => {
  assert.deepEqual(axis(8.28), { top: 10, ticks: [0, 2.5, 5, 7.5, 10] });
  assert.deepEqual(axis(0.77).ticks, [0, 0.2, 0.4, 0.6, 0.8]);
  assert.equal(axis(0).top, 1);
  assert.equal(timeOf(72, 73, 1_000_000_000, 300), 1_000_000_000);
  assert.equal(timeOf(0, 73, 1_000_000_000, 300), 1_000_000_000 - 72 * 300_000);
});
