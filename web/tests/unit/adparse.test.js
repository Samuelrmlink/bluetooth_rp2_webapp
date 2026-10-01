import { test } from 'node:test';
import assert from 'node:assert/strict';
import { parseAd, flagNames, allServices, companyLabel } from '../../js/adparse.js';

// AD structure helper: type, data hex -> "len type data"
const ad = (type, hex) => (hex.length / 2 + 1).toString(16).padStart(2, '0') + type + hex;

test('flags, names complete vs shortened, tx power, appearance', () => {
  const p = parseAd(ad('01', '06') + ad('09', '48524d2d39303030') + ad('0a', 'f4') + ad('19', 'c003'),
    ad('08', '48524d'));
  assert.equal(p.flags, 6);
  assert.equal(flagNames(p.flags), 'LE General, no BR/EDR');
  assert.equal(p.nameComplete, 'HRM-9000');
  assert.equal(p.nameShort, 'HRM');
  assert.equal(p.txPower, -12);
  assert.equal(p.appearance, 0x03c0);
});

test('16/32/128-bit UUID lists keep complete vs incomplete, little-endian decoded', () => {
  const u128 = '78563412bcef9a785634123478563412';   // 12345678-3412-5678-...
  const p = parseAd(ad('03', '0d180f18') + ad('02', '0a18') + ad('05', '78563412') + ad('07', u128));
  assert.deepEqual(p.uuid16.complete, ['180d', '180f']);
  assert.deepEqual(p.uuid16.incomplete, ['180a']);
  assert.deepEqual(p.uuid32.complete, ['12345678']);
  assert.equal(p.uuid128.complete[0], '12345678-3412-3456-789a-efbc12345678');
  assert.deepEqual(allServices(p).slice(0, 3), ['180d', '180f', '180a']);
});

test('128-bit UUID byte order is the standard one', () => {
  // Nordic UART service 6e400001-b5a3-f393-e0a9-e50e24dcca9e, as it goes over the air
  const p = parseAd(ad('07', '9ecadc240ee5a9e093f3a3b50100406e'));
  assert.equal(p.uuid128.complete[0], '6e400001-b5a3-f393-e0a9-e50e24dcca9e');
});

test('manufacturer data: company id, name, payload', () => {
  const p = parseAd(ad('ff', '4c0012020003'));
  assert.deepEqual(p.mfg, { company: 0x004c, companyName: 'Apple', data: '12020003', raw: '4c0012020003' });
  assert.equal(companyLabel(p.mfg), 'Apple (0x004c)');
  assert.equal(companyLabel(parseAd(ad('ff', '3412aa')).mfg), '0x1234');
});

test('service data for 16-bit UUIDs, and the raw payloads kept', () => {
  const p = parseAd(ad('16', '0f1857'), '');
  assert.deepEqual(p.serviceData, [{ uuid: '180f', data: '57' }]);
  assert.equal(p.raw.adv, ad('16', '0f1857'));
});

test('a real C1000 advertisement parses', () => {
  // 02 01 06 | 05 02 09 ff (incomplete uuid16 ff09) | 11 ff ... (mfg) -- shape from the bench unit
  const adv = '020106' + ad('03', '09ff') + ad('ff', 'ffff01e8eeccb6897e00b10535595804');
  const rsp = ad('09', '416e6b657220534f4c495820433130303000');
  const p = parseAd(adv, rsp);
  assert.deepEqual(p.uuid16.complete, ['ff09']);
  assert.equal(p.mfg.company, 0xffff);
  assert.equal(p.nameComplete, 'Anker SOLIX C1000', 'trailing NULs trimmed');
});

test('malformed and padded payloads do not throw', () => {
  assert.equal(parseAd('0509ff').malformed, 'adv: AD at byte 0 runs past the end');
  assert.equal(parseAd(ad('09', '41') + '0000').nameComplete, 'A', 'zero length = padding');
  assert.deepEqual(parseAd('', '').uuid16.complete, []);
});
