import { test } from 'node:test';
import assert from 'node:assert/strict';
import { badSdPath, resolveBuildChoice } from '../../js/editor/pexbuilddlg.js';

test('build choice picks the card, internal storage, and install', () => {
  const card = resolveBuildChoice({
    name: 'anker', arch: 'm33', sd: true, sdDir: 'sd/pex/', internal: false, install: false,
  });
  assert.equal(card.error, null);
  assert.equal(card.file, 'anker.m33.pex');
  assert.equal(card.sdPath, 'sd/pex/anker.m33.pex');
  assert.equal(card.internalPath, null);
  assert.equal(card.install, false);

  const both = resolveBuildChoice({
    name: 'browser', arch: 'm33', sd: true, sdDir: 'sd/out', internal: true, install: false,
  });
  assert.equal(both.sdPath, 'sd/out/browser.m33.pex');
  assert.equal(both.internalPath, 'pex/browser.m33.pex');

  const installed = resolveBuildChoice({
    name: 'anker', arch: 'm33', sd: false, sdDir: 'sd/pex', internal: false, install: true,
  });
  assert.equal(installed.internalPath, 'pex/anker.m33.pex');
  assert.equal(installed.sdPath, null);
  assert.equal(installed.install, true);

  const none = resolveBuildChoice({
    name: 'anker', arch: 'm33', sd: false, sdDir: 'sd/pex', internal: false, install: false,
  });
  assert.match(none.error, /Choose/);

  assert.match(badSdPath('pex/anker.m33.pex'), /sd\//);
  assert.match(resolveBuildChoice({
    name: 'anker', arch: 'm33', sd: true, sdDir: 'sd/a/../b', internal: false, install: false,
  }).error, /\.\./);
  const long = 'sd/' + 'a'.repeat(97);
  assert.match(resolveBuildChoice({
    name: 'n', arch: 'm33', sd: true, sdDir: long, internal: true, install: false,
  }).error, /96/);
});
