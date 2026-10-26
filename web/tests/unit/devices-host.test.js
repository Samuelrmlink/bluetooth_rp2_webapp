import { test } from 'node:test';
import assert from 'node:assert/strict';
import { DeviceHost } from '../../js/devices/host.js';

function fakeApp() {
  const removed = [];
  const app = {
    removed,
    log() {},
    bus: { owner: () => ({ on() {} }) },
    addWindow(id) {
      return { id, mounted: true };
    },
    removeWindow(id) { removed.push(id); },
  };
  return app;
}

test('profile.anker.gone removes that device even while a window is open', () => {
  const app = fakeApp();
  const host = new DeviceHost(app);
  host._onEvent('profile.anker.telemetry', {
    addr: 'E8:EE:CC:B6:89:7E',
    product_code: 'A1761',
    model_name: 'SOLIX C1000',
  });
  host._onEvent('profile.anker.telemetry', {
    addr: '7C:E9:13:60:1F:D8',
    product_code: 'A2687',
  });
  assert.equal(host.devices.size, 2);

  host._onEvent('profile.anker.gone', { addr: 'e8:ee:cc:b6:89:7e', product_code: 'A1761' });

  assert.equal(host.devices.size, 1);
  assert.ok(host.devices.has('7CE913601FD8'));
  assert.deepEqual(app.removed, [
    'anker-pps.controls@E8EECCB6897E',
    'anker-pps.unclassified@E8EECCB6897E',
  ]);
});

test('profile.anker.gone with no address or an unknown device leaves the rest', () => {
  const app = fakeApp();
  const host = new DeviceHost(app);
  host._onEvent('profile.anker.telemetry', { addr: '7C:E9:13:60:1F:D8', product_code: 'A2687' });
  host._onEvent('profile.anker.gone', {});
  host._onEvent('profile.anker.gone', { addr: '11:22:33:44:55:66' });
  assert.equal(host.devices.size, 1);
  assert.equal(app.removed.length, 0);
});
