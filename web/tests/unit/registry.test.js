import { test } from 'node:test';
import assert from 'node:assert/strict';
import { pluginFor, deviceKey, GENERIC } from '../../js/devices/registry.js';

test('the C1000 is a PPS, the A2687 a charger, the Pinecil itself', () => {
  assert.equal(pluginFor('profile.anker.telemetry', { product_code: 'A1761' }).id, 'anker-pps');
  assert.equal(pluginFor('profile.anker.telemetry', { product_code: 'A2687' }).id, 'anker-scp');
  assert.equal(pluginFor('profile.pinecil.telemetry', {}).id, 'pinecil');
  assert.equal(pluginFor('profile.ws2812.telemetry', {}).id, 'ws2812');
  assert.equal(pluginFor('profile.ws2812.telemetry', {}).name(), 'WS2812');
});

test('an unknown device or profile still gets the generic telemetry window', () => {
  assert.equal(pluginFor('profile.anker.telemetry', { product_code: 'A9999' }), GENERIC);
  assert.equal(pluginFor('profile.thermo.telemetry', {}), GENERIC);
});

test('the PPS name drops the brand and the (X) suffix', () => {
  const p = pluginFor('profile.anker.telemetry', { product_code: 'A1761' });
  assert.equal(p.name({ model_name: 'Anker SOLIX C1000(X) Portable Power Station' }), 'C1000');
  assert.equal(p.name({ model_name: 'SOLIX C1000(X) Portable Power Station' }), 'C1000', 'as the live C1000 reports it');
});

test('device keys are stable, pane-id-safe addresses', () => {
  assert.equal(deviceKey('profile.anker.telemetry', { addr: 'e8:ee:cc:b6:89:7e' }), 'E8EECCB6897E');
  assert.equal(deviceKey('profile.thermo.telemetry', {}), 'thermo-telemetry');
});
