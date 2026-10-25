import { test } from 'node:test';
import assert from 'node:assert/strict';
import { bdAddrOk, forwardIfaces, napRows } from '../../js/settings/nap.js';

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

test('a failed try stays on the link line and the next attempt is not blocked', () => {
  const failed = Object.fromEntries(napRows({
    state: 'down', addr: 'aa:bb:cc:dd:ee:ff', fail: 'timed out',
    ip: '0.0.0.0', mask: '0.0.0.0', gw: '0.0.0.0',
  }));
  assert.equal(failed.Link, 'down · last try failed (timed out)');
  assert.equal(failed.Phone, 'aa:bb:cc:dd:ee:ff');
  const dialing = Object.fromEntries(napRows({ state: 'connecting', fail: 'timed out' }));
  assert.equal(dialing.Link, 'connecting…');
  const up = Object.fromEntries(napRows({
    state: 'up', fail: 'timed out', addr: 'aa:bb:cc:dd:ee:ff',
    ip: '192.168.44.15', mask: '255.255.255.0', gw: '192.168.44.1',
  }));
  assert.equal(up.Link, 'up · 192.168.44.15');
  const plain = Object.fromEntries(napRows({ state: 'down', fail: '' }));
  assert.equal(plain.Link, 'down');
});

test('the side row says who placed the call', () => {
  const panu = Object.fromEntries(napRows({
    state: 'up', role: 'panu', addr: 'aa:bb:cc:dd:ee:ff',
    ip: '192.168.44.15', mask: '255.255.255.0', gw: '192.168.44.1',
  }));
  assert.equal(panu.Side, 'board started it');
  assert.equal(panu.Gateway, '192.168.44.1');
  const nap = Object.fromEntries(napRows({
    state: 'up', role: 'nap', addr: 'aa:bb:cc:dd:ee:ff',
    ip: '192.168.44.1', mask: '255.255.255.0', gw: '0.0.0.0',
  }));
  assert.equal(nap.Side, 'phone started it');
  assert.equal(nap.Address, '192.168.44.1 / 255.255.255.0');
  assert.equal(nap.Gateway, null);
  const down = Object.fromEntries(napRows({ state: 'down', role: '' }));
  assert.equal(down.Side, null);
});

test('share internet is off until the switch is on, and b0 is not an exit', () => {
  const off = Object.fromEntries(napRows({ state: 'down', forward: false }));
  assert.equal(off['Share internet'], 'off');
  const on = Object.fromEntries(napRows({
    state: 'up', role: 'nap', forward: true, forward_iface: 'w0',
    ip: '192.168.44.1', mask: '255.255.255.0', gw: '0.0.0.0',
  }));
  assert.equal(on['Share internet'], 'on, out w0');
  const stuck = Object.fromEntries(napRows({
    state: 'up', forward: true, forward_iface: 'e0', forward_why: 'down',
  }));
  assert.equal(stuck['Share internet'], 'on, out e0 · down');
  assert.deepEqual(forwardIfaces([
    { name: 'w0', kind: 'wifi' },
    { name: 'b0', kind: 'nap' },
    { name: 'e0', kind: 'ethernet' },
  ]).map((u) => u.name), ['w0', 'e0']);
  assert.deepEqual(forwardIfaces([
    { name: 'w0', kind: 'wifi' },
    { name: 'b0', kind: 'nap' },
  ], [
    { name: 'wg', kind: 'wireguard' },
    { name: 'b0', kind: 'nap' },
    { name: 'wg', kind: 'wireguard' },
  ]).map((u) => u.name), ['w0', 'wg']);
});
