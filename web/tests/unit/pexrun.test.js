import { test } from 'node:test';
import assert from 'node:assert/strict';
import { pexNameOf, isPexFile, pexRunningPaths, startPex, stopPex } from '../../js/editor/pexrun.js';

const fail = (code, message) => Object.assign(new Error(message || code), { code });

function fake(handlers) {
  const calls = [];
  return {
    calls,
    call(op, params) {
      calls.push([op, params]);
      const r = handlers[op];
      if (typeof r === 'function') return Promise.resolve(r(params));
      return Promise.reject(fail('unknown_op', op));
    },
  };
}

test('pexNameOf reads the app name and ignores everything else', () => {
  assert.equal(pexNameOf('pex/anker.m33.pex'), 'anker');
  assert.equal(pexNameOf('pex/pinecil.m0p.pex'), 'pinecil');
  assert.equal(pexNameOf('lua/anker.lua'), null);
  assert.equal(pexNameOf('pex/anker.m33.pex.gz'), null);
  assert.equal(pexNameOf(''), null);
});

test('isPexFile is a .pex or a gzipped .pex, and nothing editable', () => {
  assert.equal(isPexFile('pex/anker.m33.pex'), true);
  assert.equal(isPexFile('pex/anker.m33.pex.gz'), true);
  assert.equal(isPexFile('lua/anker.lua'), false);
  assert.equal(isPexFile('js/app.js.gz'), false);
});

test('pexRunningPaths maps every state, and skips a second instance', () => {
  assert.deepEqual(pexRunningPaths(null), {});
  assert.deepEqual(pexRunningPaths({
    arch: 'm33',
    modules: [
      { name: 'anker', state: 'running' },
      { name: 'pinecil', state: 'stopped' },
      { name: 'sample', state: 'loaded' },
      { name: 'old', state: 'exited' },
      { name: 'bad', state: 'faulted' },
      { name: 'anker#2', state: 'running' },
    ],
  }), {
    'pex/anker.m33.pex': 'running',
    'pex/pinecil.m33.pex': 'stopped',
    'pex/sample.m33.pex': 'loaded',
    'pex/old.m33.pex': 'exited',
    'pex/bad.m33.pex': 'faulted',
  });
});

test('startPex loads, then starts, when the app is not loaded', async () => {
  const api = fake({
    'pex.info': () => { throw fail('not_loaded', 'anker'); },
    'pex.load': () => ({ name: 'anker', state: 'loaded' }),
    'pex.start': () => ({ started: true }),
  });
  assert.deepEqual(await startPex(api, 'anker'), { started: true });
  assert.deepEqual(api.calls.map((c) => c[0]), ['pex.info', 'pex.load', 'pex.start']);
  assert.deepEqual(api.calls[2][1], { name: 'anker' });
});

test('startPex does not load or start an app that is already running', async () => {
  const api = fake({
    'pex.info': () => ({ name: 'anker', state: 'running' }),
  });
  assert.deepEqual(await startPex(api, 'anker'), { started: false, state: 'running' });
  assert.deepEqual(api.calls.map((c) => c[0]), ['pex.info']);
});

test('startPex starts a loaded app that is stopped, without loading it again', async () => {
  const api = fake({
    'pex.info': () => ({ name: 'pinecil', state: 'stopped' }),
    'pex.start': () => ({ started: true }),
  });
  assert.deepEqual(await startPex(api, 'pinecil'), { started: true });
  assert.deepEqual(api.calls.map((c) => c[0]), ['pex.info', 'pex.start']);
});

test('startPex reports a missing file instead of loading', async () => {
  const api = fake({
    'pex.info': () => { throw fail('not_loaded'); },
    'pex.load': () => { throw fail('not_found', 'not installed, and no lfs:/pex/<name>.<arch>.pex'); },
  });
  await assert.rejects(startPex(api, 'anker'), { code: 'not_found' });
});

test('stopPex stops, then unloads', async () => {
  const api = fake({
    'pex.stop': () => ({ stopped: true }),
    'pex.unload': () => ({ unloaded: true }),
  });
  assert.deepEqual(await stopPex(api, 'anker'), { stopped: true, unloaded: true });
  assert.deepEqual(api.calls, [
    ['pex.stop', { name: 'anker' }],
    ['pex.unload', { name: 'anker' }],
  ]);
});

test('stopPex does not unload when stop fails', async () => {
  const api = fake({
    'pex.stop': () => { throw fail('not_loaded', 'anker'); },
    'pex.unload': () => ({ unloaded: true }),
  });
  await assert.rejects(stopPex(api, 'anker'), { code: 'not_loaded' });
  assert.deepEqual(api.calls, [['pex.stop', { name: 'anker' }]]);
});

test('stopPex reports a module that stopped but stayed loaded', async () => {
  const api = fake({
    'pex.stop': () => ({ stopped: true }),
    'pex.unload': () => { throw fail('busy', 'hello has 1 live reference'); },
  });
  assert.deepEqual(await stopPex(api, 'hello'), {
    stopped: true,
    unloaded: false,
    error: 'hello has 1 live reference',
  });
  assert.deepEqual(api.calls.map((c) => c[0]), ['pex.stop', 'pex.unload']);
});
