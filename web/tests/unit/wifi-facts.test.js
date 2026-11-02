import { test } from 'node:test';
import assert from 'node:assert/strict';
import { wifiFacts } from '../../js/settings/wifi.js';

test('a link-local station address is not a DHCP lease', () => {
  const ll = wifiFacts({ mode: 'sta', ssid: 'Wireless Hotspot', ip: '169.254.241.51' });
  assert.match(ll.state, /no DHCP lease/);
  assert.equal(ll.address, '169.254.241.51');
  assert.match(ll.addressing, /link-local/);
  const leased = wifiFacts({ mode: 'sta', ssid: 'Wireless Hotspot', ip: '192.168.1.230' });
  assert.equal(leased.state, 'joined Wireless Hotspot');
  assert.equal(leased.addressing, 'DHCP lease');
  assert.equal(wifiFacts({ mode: 'ap', ap: true, ip: '' }).state, 'hotspot only');
});
