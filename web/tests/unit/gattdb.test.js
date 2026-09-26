import test from 'node:test';
import assert from 'node:assert/strict';
import { DeviceStore, shortUuid, uuidLabel, hexToAscii } from '../../js/gattdb.js';

const A = 'AA:BB:CC:DD:EE:FF';

test('upsert merges advertisements instead of replacing them', () => {
  const s = new DeviceStore();
  // This is the browser-side twin of the firmware's ADV/SCAN_RSP merge: a
  // later packet carrying fewer fields must not erase the earlier ones.
  s.upsert({ addr: A, rssi: -40, mfg_data: 'ffff01', service_uuids: ['ff09'] });
  s.upsert({ addr: A, rssi: -50, name: 'Widget' });
  const d = s.get(A);
  assert.equal(d.name, 'Widget');
  assert.equal(d.mfg_data, 'ffff01');
  assert.deepEqual(d.service_uuids, ['ff09']);
  assert.equal(d.rssi, -50, 'rssi is current, not merged');
});

test('a disconnect drops the tree, because it belonged to the connection', () => {
  const s = new DeviceStore();
  s.upsert({ addr: A });
  s.setTree(A, { services: [{ id: '0', uuid: '1800', characteristics: [] }] });
  assert.ok(s.get(A).gatt);
  s.setConnected(A, false);
  assert.equal(s.get(A).gatt, null);
  assert.equal(s.get(A).connected, false);
});

test('applyDelta adds, updates and removes', () => {
  const s = new DeviceStore();
  s.upsert({ addr: A });
  s.setTree(A, { services: [
    { id: '0', uuid: '1800', characteristics: [{ id: '0.0', uuid: '2a00', properties: 2 }] },
    { id: '1', uuid: '1801', characteristics: [{ id: '1.0', uuid: '2a05', properties: 32 }] },
  ] });
  s.applyDelta(A, { updated: [{ id: '0.0', properties: 10 }] });
  assert.equal(s.get(A).gatt.services[0].characteristics[0].properties, 10);

  s.applyDelta(A, { added: [{ id: '0.1', uuid: '2a01', properties: 2 }] });
  assert.equal(s.get(A).gatt.services[0].characteristics.length, 2);

  s.applyDelta(A, { removed: ['1.0'] });
  assert.equal(s.get(A).gatt.services[1].characteristics.length, 0);
});

test('applyDelta is a no-op when there is no tree to apply it to', () => {
  const s = new DeviceStore();
  s.upsert({ addr: A });
  assert.equal(s.applyDelta(A, { removed: ['0.0'] }), false);
});

test('setValue records the value and where it came from', () => {
  const s = new DeviceStore();
  s.upsert({ addr: A });
  s.setTree(A, { services: [{ id: '0', uuid: '180f', characteristics: [{ id: '0.0', uuid: '2a19' }] }] });
  s.setValue(A, '0.0', '5a', 'notify');
  const c = s.get(A).gatt.services[0].characteristics[0];
  assert.equal(c.value, '5a');
  assert.equal(c.value_source, 'notify');
});

test('subscribers are told about every change', () => {
  const s = new DeviceStore();
  let n = 0;
  const off = s.subscribe(() => n++);
  s.upsert({ addr: A });
  s.setConnected(A, true);
  assert.equal(n, 2);
  off();
  s.upsert({ addr: A, rssi: -1 });
  assert.equal(n, 2, 'unsubscribed listeners stop being called');
});

test('a throwing subscriber does not stop the others', () => {
  const s = new DeviceStore();
  let ok = 0;
  s.subscribe(() => { throw new Error('boom'); });
  s.subscribe(() => ok++);
  s.upsert({ addr: A });
  assert.equal(ok, 1);
});

test('uuid helpers', () => {
  assert.equal(shortUuid('00001801-0000-1000-8000-00805f9b34fb'), '1801');
  assert.equal(shortUuid('8c850001-0302-41c5-b46e-cf057c562025'),
                         '8c850001-0302-41c5-b46e-cf057c562025');
  assert.match(uuidLabel('180f'), /Battery/);
  assert.equal(hexToAscii('4142'), 'AB');
  assert.equal(hexToAscii('0001'), '..');
});
