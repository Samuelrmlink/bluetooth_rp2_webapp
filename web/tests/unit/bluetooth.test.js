import { test } from 'node:test';
import assert from 'node:assert/strict';
import { bondEntries } from '../../js/settings/bluetooth.js';

test('empty LE slots are not bonded devices', () => {
  const rows = bondEntries([
    { index: 0, addr: '00:00:00:00:00:00', addr_type: 254 },
    { index: 1, addr: '00:00:00:00:00:00', addr_type: 254 },
    { index: 2, addr: 'AA:BB:CC:DD:EE:FF', addr_type: 0, transport: 'le' },
  ]);
  assert.deepEqual(rows, [{ addr: 'AA:BB:CC:DD:EE:FF', kind: 'LE' }]);
});

test('a Classic link key is labeled Classic', () => {
  const rows = bondEntries([
    { index: 0, addr: '24:24:B7:25:B0:E0', addr_type: 0, transport: 'classic' },
  ]);
  assert.deepEqual(rows, [{ addr: '24:24:B7:25:B0:E0', kind: 'Classic' }]);
});

test('an older list without a transport is LE', () => {
  assert.deepEqual(bondEntries([{ addr: '11:22:33:44:55:66', addr_type: 1 }]), [
    { addr: '11:22:33:44:55:66', kind: 'LE' },
  ]);
  assert.deepEqual(bondEntries([]), []);
  assert.deepEqual(bondEntries(null), []);
});
