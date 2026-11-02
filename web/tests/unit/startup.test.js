import { test } from 'node:test';
import assert from 'node:assert/strict';
import { bootSummary, whenNote, moveEntry, addEntry, removeEntry, startupChoices } from '../../js/settings/startup.js';

test('boot summary names the skip and the gates', () => {
  assert.match(bootSummary(null), /no startup list/);
  assert.match(bootSummary({ phase: 'skipped', reason: 'watchdog' }), /watchdog/);
  assert.match(bootSummary({ phase: 'skipped', reason: 'safe' }), /safe mode/);
  assert.match(bootSummary({ phase: 'waiting', when: 'wifi' }), /Wi-Fi/);
  assert.match(bootSummary({ phase: 'waiting', when: 'wireguard' }), /uplink ping/);
  assert.match(bootSummary({ phase: 'waiting', when: 'meshtastic' }), /not implemented/);
  assert.match(bootSummary({ phase: 'running', index: 1, count: 3 }), /Started 1 of 3/);
  assert.match(bootSummary({ phase: 'running', index: 1, count: 3 }), /negotiation/);
  assert.match(bootSummary({ phase: 'running', index: 1, count: 3 }), /about a minute/);
  assert.equal(bootSummary({ phase: 'done', count: 0 }), 'The list is empty.');
});

test('meshtastic is stored as a gate that is not met', () => {
  assert.match(whenNote('meshtastic'), /not implemented/);
  assert.match(whenNote('wireguard'), /does not time out/);
  assert.match(whenNote('wifi'), /grace window/);
});

test('a board without Lua is not offered another board\'s scripts', () => {
  const files = [{ name: 'pinecil.lua' }, { name: 'note.lua.gz' }, { name: 'skip.txt' }, { dir: true, name: 'profiles' }];
  const installed = [{ name: 'anker' }];
  const c5 = { modules: ['fs', 'wifi', 'pex', 'ota', 'launch'] };
  assert.deepEqual(startupChoices(c5, installed, files), [{ kind: 'pex', name: 'anker' }]);
  assert.deepEqual(startupChoices(c5, [], files), []);
  const pico = { modules: ['script', 'pex'] };
  assert.deepEqual(startupChoices(pico, installed, files), [
    { kind: 'pex', name: 'anker' },
    { kind: 'lua', name: 'pinecil' },
    { kind: 'lua', name: 'note' },
  ]);
});

test('the list moves, refuses a second copy of a name, and drops a row', () => {
  const rows = [
    { kind: 'pex', name: 'sample', enabled: true },
    { kind: 'lua', name: 'pinecil', enabled: true },
  ];
  assert.deepEqual(moveEntry(rows, 1, -1).map((e) => e.name), ['pinecil', 'sample']);
  assert.equal(moveEntry(rows, 0, -1), rows);
  const added = addEntry(rows, 'lua', 'pinecil');
  assert.match(added.error, /already/);
  const ok = addEntry(rows, 'pex', 'demo');
  assert.equal(ok.entries.length, 3);
  assert.equal(ok.entries[2].enabled, true);
  assert.match(addEntry(rows, 'pex', '../x').error, /letters/);
  assert.deepEqual(removeEntry(rows, 0).map((e) => e.name), ['pinecil']);
});
