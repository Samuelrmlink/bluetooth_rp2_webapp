import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { manifestFor, manifestPath, deployDriverManifest } from '../../js/editor/drivermanifests.js';

const ankerSource = readFileSync(new URL('../../../pex/modules/drivers/anker.driver.json', import.meta.url), 'utf8');
const pinecilSource = readFileSync(new URL('../../../pex/modules/drivers/pinecil.driver.json', import.meta.url), 'utf8');

test('the anker manifest matches pex/modules/drivers/anker.driver.json', () => {
  assert.deepEqual(manifestFor('anker'), JSON.parse(ankerSource));
  assert.equal(manifestPath(manifestFor('anker')), 'pex/anker.driver.json');
});

test('the pinecil manifest matches pex/modules/drivers/pinecil.driver.json', () => {
  assert.deepEqual(manifestFor('pinecil'), JSON.parse(pinecilSource));
  assert.equal(manifestPath(manifestFor('pinecil')), 'pex/pinecil.driver.json');
});

test('deploy writes pex/anker.driver.json and skips an unknown module', async () => {
  const writes = [];
  const r = await deployDriverManifest(async (path, body) => {
    writes.push({ path, body });
    return { ok: true };
  }, 'anker');
  assert.equal(r.ok, true);
  assert.equal(r.path, 'pex/anker.driver.json');
  assert.equal(writes[0].path, 'pex/anker.driver.json');
  assert.equal(writes[0].body, ankerSource);
  assert.deepEqual(JSON.parse(writes[0].body), manifestFor('anker'));

  let called = false;
  const skipped = await deployDriverManifest(async () => { called = true; return { ok: true }; }, 'blink');
  assert.deepEqual(skipped, { ok: true, skipped: true });
  assert.equal(called, false);
});

test('a failed manifest write is reported and does not throw', async () => {
  const r = await deployDriverManifest(async () => ({ ok: false, error: 'write failed' }), 'anker');
  assert.equal(r.ok, false);
  assert.equal(r.path, 'pex/anker.driver.json');
  assert.match(r.error, /write failed/);
});
