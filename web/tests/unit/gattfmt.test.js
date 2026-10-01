import { test } from 'node:test';
import assert from 'node:assert/strict';
import { formatValue, parseInput, defaultFormat, formatKey, rememberFormat, permissions, actionsFor, uuidDisplay, byteLength } from '../../js/gattfmt.js';
import { GattLog } from '../../js/gattlog.js';

test('formats show the same bytes three ways', () => {
  assert.equal(formatValue('6400ff', 'hex'), '64 00 ff');
  assert.equal(formatValue('6400ff', 'dec'), '100 0 255');
  assert.equal(formatValue('6d6f636b', 'str'), 'mock');
  assert.equal(formatValue('', 'hex'), '');
});

test('string view escapes what is not printable UTF-8, and keeps what is', () => {
  assert.equal(formatValue('41000a5c', 'str'), 'A\\0\\n\\\\');
  assert.equal(formatValue('ff41', 'str'), '\\xffA');
  assert.equal(formatValue('c3a9', 'str'), 'é');
  assert.equal(formatValue('c3', 'str'), '\\xc3', 'a truncated sequence is shown as its byte');
});

test('typed input parses per format; each format round-trips', () => {
  assert.deepEqual(parseInput('64 00 FF', 'hex'), { ok: true, hex: '6400ff' });
  assert.deepEqual(parseInput('0x64:00', 'hex'), { ok: true, hex: '6400' });
  assert.equal(parseInput('6', 'hex').ok, false);
  assert.equal(parseInput('zz', 'hex').ok, false);
  assert.deepEqual(parseInput('100 0, 255', 'dec'), { ok: true, hex: '6400ff' });
  assert.equal(parseInput('256', 'dec').ok, false);
  assert.equal(parseInput('1.5', 'dec').ok, false);
  assert.deepEqual(parseInput('A\\x00é', 'str'), { ok: true, hex: '4100c3a9' });
  assert.equal(parseInput('\\q', 'str').ok, false);
  for (const hex of ['', '00', '6400ff', '41000a5cff', 'c3a9e282ac']) {
    for (const f of ['hex', 'dec', 'str']) {
      assert.equal(parseInput(formatValue(hex, f), f).hex, hex, `${hex} via ${f}`);
    }
  }
  assert.equal(byteLength('6400ff'), 3);
});

test('known characteristics start readable; anything else starts as hex', () => {
  assert.equal(defaultFormat('2a00'), 'str');
  assert.equal(defaultFormat('00002a29-0000-1000-8000-00805f9b34fb'), 'str');
  assert.equal(defaultFormat('2a19'), 'dec');
  assert.equal(defaultFormat('2a37'), 'hex');
  assert.equal(defaultFormat('8c850003-0302-41c5-b46e-cf057c562025'), 'hex');
});

test('remembered formats are keyed by service/characteristic and capped', () => {
  assert.equal(formatKey('0000180f-0000-1000-8000-00805f9b34fb', '2A19'), '180f/2a19');
  let m = {};
  for (let i = 0; i < 6; i++) m = rememberFormat(m, `k${i}`, 'dec', 4);
  assert.deepEqual(Object.keys(m), ['k2', 'k3', 'k4', 'k5']);
  m = rememberFormat(m, 'k2', 'str', 4);
  assert.deepEqual(Object.keys(m), ['k3', 'k4', 'k5', 'k2'], 're-chosen moves to the end');
});

test('only permitted actions are offered', () => {
  assert.deepEqual(permissions(['read', 'write_no_response']), ['Read', 'Write (no response)']);
  assert.deepEqual(actionsFor(['read']), { read: true, notify: false, indicate: false, write: false, chooseResponse: false, response: false });
  assert.equal(actionsFor(['write_no_response']).write, true);
  assert.equal(actionsFor(['write_no_response']).response, false, 'no-response only: never with response');
  assert.equal(actionsFor(['write', 'write_no_response']).chooseResponse, true);
  assert.equal(actionsFor(['write']).chooseResponse, false);
});

test('UUIDs: 16-bit with names, 128-bit named or abbreviated unless asked in full', () => {
  assert.deepEqual(uuidDisplay('2a19'), { text: '2a19 Battery Level', title: '2a19', is128: false });
  assert.equal(uuidDisplay('00002a19-0000-1000-8000-00805f9b34fb').text, '2a19 Battery Level');
  assert.equal(uuidDisplay('abcd').text, 'abcd');
  const anker = '8c850003-0302-41c5-b46e-cf057c562025';
  assert.equal(uuidDisplay(anker).text, 'Anker telemetry');
  assert.equal(uuidDisplay(anker, true).text, anker);
  const custom = '12345678-1234-5678-9abc-def012345678';
  assert.equal(uuidDisplay(custom).text, '12345678…');
  assert.equal(uuidDisplay(custom).title, custom);
});

test('the GATT log keeps the last value per characteristic, and is capped', () => {
  const log = new GattLog(3);
  const seen = [];
  log.subscribe((e) => seen.push(e && e.src));
  log.add({ addr: 'A', id: '1.0', src: 'read', hex: '01' });
  log.add({ addr: 'A', id: '1.0', src: 'write', hex: '02' });
  log.add({ addr: 'A', id: '1.0', src: 'error', note: 'x' });
  log.add({ addr: 'B', id: '1.0', src: 'notify', hex: '0304' });
  assert.equal(log.items.length, 3);
  assert.equal(log.last('A', '1.0').hex, '01', 'a write or an error is not a value');
  assert.equal(log.last('B', '1.0').len, 2);
  assert.equal(log.list('B').length, 1);
  assert.deepEqual(seen, ['read', 'write', 'error', 'notify']);
});
