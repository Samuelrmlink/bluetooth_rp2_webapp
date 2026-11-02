import { test } from 'node:test';
import assert from 'node:assert/strict';
import { luaShown, sectionShown } from '../../js/settings/caps.js';

test('before sys.info, only the page sections are shown', () => {
  assert.equal(sectionShown('sect-ui', null), true);
  assert.equal(sectionShown('sect-system', null), true);
  assert.equal(sectionShown('sect-wifi', null), false);
  assert.equal(sectionShown('sect-firmware', null), false);
});

test('a Pico image and a C5 image keep different sections', () => {
  const pico = {
    modules: ['wifi', 'wg', 'eth', 'sd', 'ota', 'launch', 'bt', 'uplink', 'script'],
    transports: ['le', 'classic'],
  };
  assert.equal(sectionShown('sect-wifi', pico), true);
  assert.equal(sectionShown('sect-wireguard', pico), true);
  assert.equal(sectionShown('sect-ethernet', pico), true);
  assert.equal(sectionShown('sect-sd', pico), true);
  assert.equal(luaShown(pico), true);
  assert.equal(sectionShown('sect-bluetooth', pico), true);
  assert.equal(sectionShown('sect-uplinks', pico), true);
  assert.equal(sectionShown('sect-firmware', pico), true);
  assert.equal(sectionShown('sect-startup', pico), true);
  assert.equal(sectionShown('sect-nap', pico), false);

  const c5 = { modules: ['fs', 'wifi', 'pex', 'ota', 'launch'], transports: ['wifi'] };
  assert.equal(sectionShown('sect-wifi', c5), true);
  assert.equal(sectionShown('sect-firmware', c5), true);
  assert.equal(sectionShown('sect-startup', c5), true);
  assert.equal(sectionShown('sect-system', c5), true);
  assert.equal(sectionShown('sect-ethernet', c5), false);
  assert.equal(sectionShown('sect-sd', c5), false);
  assert.equal(luaShown(c5), false);
  assert.equal(luaShown({ modules: ['script'] }), true);
  assert.equal(luaShown({ modules: ['lua'] }), true);
  assert.equal(luaShown(null), false);
  assert.equal(sectionShown('sect-bluetooth', c5), false);
  assert.equal(sectionShown('sect-wireguard', c5), false);
  assert.equal(sectionShown('sect-uplinks', c5), false);
  assert.equal(sectionShown('sect-nap', c5), false);
});
