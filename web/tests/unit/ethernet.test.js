import { test } from 'node:test';
import assert from 'node:assert/strict';
import { ipv4ok, maskOk, ethFormError, ethRows } from '../../js/settings/ethernet.js';

test('IPv4 addresses', () => {
  for (const s of ['192.168.1.1', '0.0.0.0', '255.255.255.255', ' 10.0.0.1 ']) assert.ok(ipv4ok(s), s);
  for (const s of ['', '1.2.3', '1.2.3.4.5', '256.1.1.1', 'a.b.c.d', '1..2.3', null]) assert.ok(!ipv4ok(s), String(s));
});

test('subnet masks must be contiguous and non-zero', () => {
  for (const s of ['255.255.255.0', '255.255.0.0', '255.255.255.252', '255.255.255.255', '128.0.0.0']) assert.ok(maskOk(s), s);
  for (const s of ['0.0.0.0', '255.0.255.0', '255.255.255.1', '1.2.3.4', 'x']) assert.ok(!maskOk(s), s);
});

test('the form is checked the way the firmware checks it', () => {
  assert.equal(ethFormError({ mode: 'dhcp', ip: 'junk' }), null, 'DHCP ignores the manual fields');
  assert.equal(ethFormError({ mode: 'manual', ip: '192.168.50.2', mask: '255.255.255.0', gw: '', dns: '' }), null);
  assert.equal(ethFormError({ mode: 'manual', ip: '192.168.50.2', mask: '255.255.255.0', gw: '192.168.50.1', dns: '1.1.1.1' }), null);
  assert.match(ethFormError({ mode: 'manual', ip: '', mask: '255.255.255.0' }), /IP/);
  assert.match(ethFormError({ mode: 'manual', ip: '0.0.0.0', mask: '255.255.255.0' }), /IP/);
  assert.match(ethFormError({ mode: 'manual', ip: '10.0.0.2', mask: '255.0.255.0' }), /mask/);
  assert.match(ethFormError({ mode: 'manual', ip: '10.0.0.2', mask: '255.255.255.0', gw: '10.0.1.1' }), /subnet/);
  assert.match(ethFormError({ mode: 'manual', ip: '10.0.0.2', mask: '255.255.255.0', dns: '1.1.1' }), /DNS/);
  assert.match(ethFormError({ mode: 'static' }), /mode/);
  assert.equal(ethFormError({ mode: 'server', ip: '', mask: '' }), null, 'server with the defaults');
  assert.equal(ethFormError({ mode: 'server', ip: '10.9.0.1', mask: '255.255.0.0' }), null);
  assert.match(ethFormError({ mode: 'server', ip: '192.168.9.17', mask: '' }), /lease range/);
  assert.match(ethFormError({ mode: 'server', ip: '192.168.9.1', mask: '255.255.255.240' }), /\/24/);
});

test('status rows', () => {
  assert.equal(Object.fromEntries(ethRows({ present: false })).State, 'not present (no W5500 answered on SPI)');
  const up = Object.fromEntries(ethRows({ present: true, link: true, speed: 100, duplex: 'full', mode: 'dhcp',
    ip: '192.168.50.23', mask: '255.255.255.0', gw: '192.168.50.1', rx_frames: 5, tx_frames: 4 }));
  assert.equal(up.State, 'up · 192.168.50.23');
  assert.equal(up.Link, '100 Mb/s full duplex');
  assert.equal(up.Addressing, 'DHCP client');
  const waiting = Object.fromEntries(ethRows({ present: true, link: true, speed: 100, duplex: 'full', mode: 'dhcp', ip: '0.0.0.0' }));
  assert.equal(waiting.State, 'link up, waiting for DHCP…');
  assert.equal(Object.fromEntries(ethRows({ present: true, link: false, mode: 'manual' })).State, 'no link (cable?)');
  const ll = Object.fromEntries(ethRows({ present: true, link: true, speed: 100, duplex: 'full', mode: 'dhcp', ip: '169.254.7.9', mask: '255.255.0.0', addr_source: 'link-local' }));
  assert.match(ll['Address from'], /^link-local/);
  const srv = Object.fromEntries(ethRows({ present: true, link: true, speed: 100, duplex: 'full', mode: 'server', ip: '192.168.9.1', mask: '255.255.255.0', addr_source: 'server', dhcp_router: false }));
  assert.equal(srv.Addressing, 'DHCP server');
});
