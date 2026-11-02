import { test } from 'node:test';
import assert from 'node:assert/strict';
import {
  bleSupported, hardwareSupported, localToolTitle, peerRoles, roleTitle, scopeOf, toolGroup,
} from '../../js/windows/peercaps.js';

const pico = {
  modules: ['sys', 'fs', 'bt', 'script', 'io', 'wifi', 'ota'],
  transports: ['le', 'classic'],
};
const c5 = { modules: ['fs', 'wifi', 'pex', 'ota', 'launch'], transports: ['wifi'] };

test('a board with no info yet gets the editor and a console', () => {
  assert.deepEqual(peerRoles(null).map((r) => r.id), ['editor', 'console']);
  assert.equal(bleSupported(null), false);
  assert.equal(hardwareSupported(null), false);
});

test('the C5 image gets a console and not the LE or pin windows', () => {
  assert.deepEqual(peerRoles(c5).map((r) => r.id), ['editor', 'console']);
  assert.equal(bleSupported(c5), false);
});

test('an LE board gets the scanner, GATT, and GATT events', () => {
  assert.deepEqual(peerRoles(pico).map((r) => r.id), [
    'editor', 'console', 'scanner', 'gatt', 'gatt-events', 'hardware',
  ]);
});

test('classic without LE does not get the scanner', () => {
  const classic = { modules: ['bt', 'io'], transports: ['classic'] };
  assert.equal(bleSupported(classic), false);
  assert.deepEqual(peerRoles(classic).map((r) => r.id), ['editor', 'console', 'hardware']);
});

test('a bt module with no transport list still gets the LE windows', () => {
  assert.equal(bleSupported({ modules: ['bt'] }), true);
  assert.ok(peerRoles({ modules: ['bt'] }).some((r) => r.id === 'scanner'));
  assert.equal(hardwareSupported({ modules: ['bt'] }), false);
});

test('an LE transport is enough on its own', () => {
  assert.equal(bleSupported({ transports: ['le'] }), true);
});

test('names and the per-board heading', () => {
  assert.equal(toolGroup('ESP32C5-3844be0a31f0'), 'Tools [ESP32C5-3844be0a31f0]');
  assert.equal(roleTitle('Console', 'ESP32C5-3844be0a31f0'), 'Console [ESP32C5-3844be0a31f0]');
  assert.equal(localToolTitle('Scanner', false), 'Scanner');
  assert.equal(localToolTitle('Scanner', true), 'Scanner [Local]');
  assert.equal(scopeOf('ESP32C5-3844be0a31f0'), '@ESP32C5-3844be0a31f0');
  assert.equal(scopeOf('a b/c'), '@a_b_c');
});
