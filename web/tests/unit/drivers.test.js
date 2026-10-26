import { test } from 'node:test';
import assert from 'node:assert/strict';
import { parseAd } from '../../js/adparse.js';
import { factsFromAd, matchSet, driverMatches, matchingDrivers, driverStatus, driverLabel, loadDriverManifests, driverWatchPath } from '../../js/drivers.js';

const anker = {
  id: 'anker', title: 'Anker SOLIX / Prime', kind: 'pex', file: 'pex/anker.m33.pex',
  match: [{ all: [{ mfg_company: 'ffff' }, { uuid16: 'ff09' }] }],
};
const pinecil = {
  id: 'pinecil', title: 'Pinecil V2', kind: 'lua', file: 'lua/pinecil.lua',
  match: [
    { all: [{ name_contains: 'Pinecil' }] },
    { all: [{ uuid128: 'd85ef000-168e-4a71-aa55-33e27f9bc533' }] },
  ],
};
const uuidOnly = {
  id: 'ff09-only', title: 'FF09', kind: 'pex', file: 'pex/ff09.m33.pex',
  match: [{ all: [{ uuid16: 'ff09' }] }],
};

const charger = { name: 'ASHDJW61F45100348', company: 'ffff', uuid16: ['ff09'], uuid128: [] };
const c1000 = { name: 'Anker SOLIX C1000', company: 'ffff', uuid16: ['ff09'], uuid128: [] };
const iron = { name: 'Pinecil-ABC', company: '', uuid16: [], uuid128: ['d85ef000-168e-4a71-aa55-33e27f9bc533'] };

test('an AND set passes only when every rule passes', () => {
  assert.equal(matchSet(anker.match[0], charger), true);
  assert.equal(matchSet(anker.match[0], { ...charger, company: '0822' }), false);
  assert.equal(matchSet(anker.match[0], { ...charger, uuid16: [] }), false);
  assert.equal(matchSet({ all: [] }, charger), false);
  assert.equal(matchSet({ all: [{ unused: true }] }, charger), false);
});

test('the charger serial matches Anker without the word Anker, and two drivers can both match', () => {
  assert.equal(driverMatches(anker, charger), true);
  assert.equal(driverMatches(anker, { ...charger, name: 'nope' }), true);
  const both = matchingDrivers([anker, uuidOnly, pinecil], charger);
  assert.deepEqual(both.map((d) => d.id), ['anker', 'ff09-only']);
  assert.deepEqual(matchingDrivers([anker, pinecil], iron).map((d) => d.id), ['pinecil']);
  assert.deepEqual(matchingDrivers([anker, pinecil], c1000).map((d) => d.id), ['anker']);
});

test('a parsed advertisement feeds the same rules', () => {
  // flags, complete uuid16 ff09, manufacturer data ffff…, complete name
  const adv = '020106030309ff11ffffff01e8eeccb6897e00b105355958041209415348444a573631463435313030333438';
  const facts = factsFromAd(parseAd(adv, ''));
  assert.equal(facts.company, 'ffff');
  assert.deepEqual(facts.uuid16, ['ff09']);
  assert.equal(facts.name, 'ASHDJW61F45100348');
  assert.equal(driverMatches(anker, facts), true);
  assert.equal(driverMatches({ ...anker, match: [{ all: [{ name_contains: 'Anker' }] }] }, facts), false);
});

test('Start is offered when the file is present and the driver is not running', () => {
  const present = {
    installed: new Set(['anker']),
    pexFiles: new Set(['pex/anker.m33.pex']),
    luaFiles: new Set(['lua/pinecil.lua']),
    runningPex: new Set(),
    runningLua: new Set(),
    loadedPex: new Set(),
  };
  assert.deepEqual(driverStatus(anker, present), { running: false, present: true, startName: 'anker' });
  assert.equal(driverStatus(anker, { ...present, runningPex: new Set(['anker']) }).running, true);
  assert.equal(driverStatus(anker, { installed: new Set(), pexFiles: new Set(), loadedPex: new Set() }).present, false);
  assert.equal(driverStatus(pinecil, present).present, true);
  assert.equal(driverStatus(pinecil, { ...present, runningLua: new Set(['pinecil']) }).running, true);
  assert.match(driverLabel([anker], present), /^Anker SOLIX \/ Prime$/);
  assert.match(driverLabel([anker], { ...present, runningPex: new Set(['anker']) }), /running$/);
  assert.match(driverLabel([pinecil], { luaFiles: new Set() }), /not on device$/);
});

test('manifests are read from pex/ and lua/profiles, and one id is kept once', async () => {
  const files = {
    pex: [{ name: 'anker.driver.json' }, { name: 'anker.m33.pex' }, { name: 'sample.m33.pex' }],
    'lua/profiles': [{ name: 'anker', dir: true }, { name: 'pinecil', dir: true }, { name: 'pinecil.lua' }],
    lua: [{ name: 'pinecil.lua' }, { name: 'anker.lua' }],
  };
  const text = {
    'pex/anker.driver.json': JSON.stringify(anker),
    'lua/profiles/anker/driver.json': JSON.stringify({ ...anker, title: 'from profile' }),
    'lua/profiles/pinecil/driver.json': JSON.stringify(pinecil),
  };
  const fs = {
    list: async (dir) => (files[dir] ? { ok: true, files: files[dir] } : { ok: false, error: 'missing' }),
    readText: async (path) => (text[path] ? { ok: true, text: text[path] } : { ok: false }),
  };
  const r = await loadDriverManifests(fs);
  assert.deepEqual(r.manifests.map((m) => m.id), ['anker', 'pinecil']);
  assert.equal(r.manifests[0].title, 'Anker SOLIX / Prime');
  assert.ok(r.pexFiles.has('pex/anker.m33.pex') && r.pexFiles.has('pex/sample.m33.pex'));
  assert.ok(r.luaFiles.has('lua/pinecil.lua'));
});

test('a change under pex or lua/profiles reloads drivers, and other trees do not', () => {
  for (const p of [
    'pex',
    'pex/anker.driver.json',
    'pex/anker.m33.pex',
    '/pex/anker.driver.json',
    'sd/pex',
    'sd/pex/anker.driver.json',
    'lua/profiles',
    'lua/profiles/anker/driver.json',
    'lua/profiles/anker/main.lua',
    'sd/lua/profiles',
    'sd/lua/profiles/pinecil/driver.json',
  ]) {
    assert.equal(driverWatchPath(p), true, p);
  }
  for (const p of [
    '',
    'lua/pinecil.lua',
    'lua/anker.lua',
    'sd/lua/pinecil.lua',
    'cfg/nap.json',
    'pexfoo/x',
    'lua/profiles-old/x',
    'sd/pextra/x',
    'not/pex/anker.driver.json',
  ]) {
    assert.equal(driverWatchPath(p), false, p);
  }
  assert.equal(driverWatchPath(null), false);
  assert.equal(driverWatchPath(undefined), false);
});
