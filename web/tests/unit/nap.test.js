import { test } from 'node:test';
import assert from 'node:assert/strict';
import { bdAddrOk, napRows } from '../../js/settings/nap.js';

test('a Classic address is six hex bytes', () => {
  assert.equal(bdAddrOk('aa:bb:cc:dd:ee:ff'), true);
  assert.equal(bdAddrOk('AA:BB:CC:DD:EE:FF'), true);
  assert.equal(bdAddrOk(' aa:bb:cc:dd:ee:ff '), true);
  assert.equal(bdAddrOk('aa-bb-cc-dd-ee-ff'), false);
  assert.equal(bdAddrOk('aabbccddeeff'), false);
  assert.equal(bdAddrOk(''), false);
});

test('the phone link shows its address once DHCP answers', () => {
  const down = Object.fromEntries(napRows({ state: 'down', addr: '', ip: '0.0.0.0', mask: '0.0.0.0', gw: '0.0.0.0' }));
  assert.equal(down.Link, 'down');
  assert.equal(down.Address, null);
  const wait = Object.fromEntries(napRows({ state: 'up', addr: 'aa:bb:cc:dd:ee:ff', ip: '0.0.0.0', mask: '0.0.0.0', gw: '0.0.0.0' }));
  assert.equal(wait.Link, 'up, waiting for an address');
  assert.equal(wait.Phone, 'aa:bb:cc:dd:ee:ff');
  const up = Object.fromEntries(napRows({
    state: 'up', addr: 'aa:bb:cc:dd:ee:ff', ip: '192.168.44.15', mask: '255.255.255.0', gw: '192.168.44.1',
  }));
  assert.equal(up.Link, 'up · 192.168.44.15');
  assert.equal(up.Address, '192.168.44.15 / 255.255.255.0');
  assert.equal(up.Gateway, '192.168.44.1');
  assert.equal(Object.fromEntries(napRows({ state: 'connecting' })).Link, 'connecting…');
});
