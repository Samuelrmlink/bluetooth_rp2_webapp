import { test } from 'node:test';
import assert from 'node:assert/strict';
import { compareValues, sortRows } from '../../js/controls/grid.js';
import { scanFilter } from '../../js/windows/scanner.js';
import { bindingsFor, matches } from '../../js/keys.js';

const col = (key, type) => ({ key, type, get: (r) => r[key] });

test('numbers sort as numbers, both ways, blanks last either way', () => {
  const rows = [{ v: -70 }, { v: null }, { v: -5 }, { v: -100 }, { v: '' }];
  assert.deepEqual(sortRows(rows, col('v', 'num'), 1).map((r) => r.v), [-100, -70, -5, null, '']);
  assert.deepEqual(sortRows(rows, col('v', 'num'), -1).map((r) => r.v), [-5, -70, -100, null, '']);
});

test('text sorts naturally and ignores case', () => {
  const rows = ['dev10', 'Dev2', 'alpha', 'dev1'].map((v) => ({ v }));
  assert.deepEqual(sortRows(rows, col('v'), 1).map((r) => r.v), ['alpha', 'dev1', 'Dev2', 'dev10']);
  assert.equal(compareValues('ABC', 'abc'), 0);
});

test('sorting is stable: equal keys keep their order', () => {
  const rows = [{ v: 1, i: 0 }, { v: 0, i: 1 }, { v: 1, i: 2 }, { v: 0, i: 3 }];
  assert.deepEqual(sortRows(rows, col('v', 'num'), 1).map((r) => r.i), [1, 3, 0, 2]);
  assert.deepEqual(sortRows(rows, col('v', 'num'), -1).map((r) => r.i), [0, 2, 1, 3]);
});

const DEVS = [
  { addr: 'E8:EE:CC:B6:89:7E', name: 'ASHDJW61F45100348', rssi: -60, mfg_data: 'ffff027c', service_uuids: ['ff09'] },
  { addr: '11:22:33:44:55:66', name: 'HRM-9000', rssi: -80, service_uuids: ['0000180d-0000-1000-8000-00805f9b34fb'] },
  { addr: 'AA:BB:CC:DD:EE:FF', rssi: -95, mfg_data: '4c0012020003' },
];
const pick = (f) => DEVS.filter(scanFilter(f)).map((d) => d.addr.slice(0, 2));

test('scan filter: empty matches everything', () => {
  assert.deepEqual(pick({}), ['E8', '11', 'AA']);
  assert.deepEqual(pick({ name: ' ', mfg: '', service: '', rssi: '' }), ['E8', '11', 'AA']);
});

test('scan filter: name matches part of the name or the address, any case', () => {
  assert.deepEqual(pick({ name: 'hrm' }), ['11']);
  assert.deepEqual(pick({ name: 'dd:ee' }), ['AA']);
});

test('scan filter: mfg is a hex prefix, spaces and colons ignored', () => {
  assert.deepEqual(pick({ mfg: '4C 00' }), ['AA']);
  assert.deepEqual(pick({ mfg: '0012' }), []);
});

test('scan filter: service matches short or full UUIDs', () => {
  assert.deepEqual(pick({ service: '180d' }), ['11']);
  assert.deepEqual(pick({ service: 'FF09' }), ['E8']);
});

test('scan filter: rssi is a floor; a device with no RSSI counts as -127', () => {
  assert.deepEqual(pick({ rssi: '-80' }), ['E8', '11']);
  assert.deepEqual(DEVS.concat({ addr: 'X' }).filter(scanFilter({ rssi: -100 })).length, 3);
});

const altN = { code: 'KeyN', altKey: true, shiftKey: false, ctrlKey: false, metaKey: false };
const first = (ctx, e) => bindingsFor(ctx).find((b) => b.combos.some((c) => matches(e, c)))?.id;

test('Alt+N steps filter boxes in a Scanner filter box, windows everywhere else', () => {
  assert.equal(first('scanner-filter', altN), 'scanner-filter-next');
  assert.equal(first('scanner-filter', { ...altN, shiftKey: true }), 'scanner-filter-prev');
  assert.equal(first('scanner', altN), 'cycle-next');
  assert.equal(first('workspace', altN), 'cycle-next');
  assert.equal(first('editor', altN), 'cycle-next');
});

test('the Scanner keeps every workspace binding', () => {
  const ws = bindingsFor('workspace').map((b) => b.id);
  const sc = new Set(bindingsFor('scanner').map((b) => b.id));
  for (const id of ws) assert.ok(sc.has(id), id);
});
