import { test } from 'node:test';
import assert from 'node:assert/strict';
import { draftChunks, ScriptIO, DRAFT_CHUNK, mergeScripts, isStartable }
  from '../../js/scriptio.js';

/* A fake transport. Records every call so a test can assert on the SHAPE of
 * the conversation, not just its result -- the failure mode being guarded
 * against here is silent truncation, which a result-only check would miss. */
function fakeApi(handler) {
  const calls = [];
  return {
    calls,
    call(op, params) {
      calls.push({ op, params });
      return Promise.resolve(handler(op, params, calls.length));
    },
  };
}

test('draftChunks splits on the chunk boundary and marks only the last final', () => {
  const src = 'x'.repeat(DRAFT_CHUNK * 2 + 7);
  const cs = draftChunks(src);
  assert.equal(cs.length, 3);
  assert.deepEqual(cs.map(c => c.offset), [0, DRAFT_CHUNK, DRAFT_CHUNK * 2]);
  assert.deepEqual(cs.map(c => c.final), [false, false, true]);
  assert.equal(cs.map(c => c.source).join(''), src);
});

test('draftChunks marks a single short chunk final', () => {
  const cs = draftChunks('print(1)');
  assert.equal(cs.length, 1);
  assert.equal(cs[0].final, true);
  assert.equal(cs[0].offset, 0);
});

test('an empty draft is still one chunk, so a script can be blanked', () => {
  const cs = draftChunks('');
  assert.equal(cs.length, 1);
  assert.equal(cs[0].source, '');
  assert.equal(cs[0].final, true);
});

test('draft() runs only on the final chunk', async () => {
  const api = fakeApi(() => ({ draft: true, bytes: 0, flash_written: false }));
  await new ScriptIO(api).draft('p', 'y'.repeat(DRAFT_CHUNK + 1), { run: true });
  assert.equal(api.calls.length, 2);
  // Running a half-uploaded script is the bug this guards.
  assert.equal(api.calls[0].params.run, false);
  assert.equal(api.calls[1].params.run, true);
});

test('draft() never claims a flash write', async () => {
  const api = fakeApi(() => ({ draft: true, bytes: 8, flash_written: false }));
  const r = await new ScriptIO(api).draft('p', 'print(1)');
  assert.equal(r.flash_written, false);
});

test('source() reassembles pages in order and stops at eof', async () => {
  const whole = 'local a = 1\nlocal b = 2\nreturn a + b\n';
  const page = 10;
  const api = fakeApi((op, p) => {
    const chunk = whole.slice(p.offset, p.offset + page);
    return {
      script: p.script, tier: 'committed', length: whole.length,
      offset: p.offset, bytes: chunk.length, chunk,
      eof: p.offset + chunk.length >= whole.length,
    };
  });
  const r = await new ScriptIO(api).source('p');
  assert.equal(r.source, whole);
  assert.equal(r.tier, 'committed');
  assert.deepEqual(api.calls.map(c => c.params.offset), [0, 10, 20, 30]);
});

test('source() stops when a page returns no bytes, rather than spinning', async () => {
  const api = fakeApi((op, p) => ({
    tier: 'draft', length: 99, offset: p.offset, bytes: 0, chunk: '', eof: false,
  }));
  const r = await new ScriptIO(api).source('p');
  assert.equal(r.source, '');
  assert.equal(api.calls.length, 1);
});

test('source() gives up rather than looping forever on a device that never sets eof', async () => {
  // bytes > 0 and eof never true: without the guard this never returns, and
  // the browser tab locks up with no clue why.
  const api = fakeApi((op, p) => ({
    tier: 'draft', length: 1e9, offset: p.offset, bytes: 1, chunk: 'z', eof: false,
  }));
  await assert.rejects(new ScriptIO(api).source('p'), /did not terminate/);
});

test('source() reports progress against the declared total', async () => {
  const whole = 'abcdefghij';
  const api = fakeApi((op, p) => {
    const chunk = whole.slice(p.offset, p.offset + 4);
    return { tier: 'staged', length: whole.length, offset: p.offset,
             bytes: chunk.length, chunk, eof: p.offset + chunk.length >= whole.length };
  });
  const seen = [];
  await new ScriptIO(api).source('p', { onProgress: (got, total) => seen.push([got, total]) });
  assert.deepEqual(seen, [[4, 10], [8, 10], [10, 10]]);
});

/* ---- merging the filesystem with the runtime ---------------------------- */

/* Exactly what the device returns, captured from it. */
const LIVE = {
  files: [
    { name: 'anker', kind: 'script', tier: 'committed', bytes: 3116 },
    { name: 'demo', kind: 'script', tier: 'committed', bytes: 156 },
    { name: 'pinecil', kind: 'script', tier: 'committed', bytes: 1011 },
    { name: 'profiles/anker_a2687', kind: 'module', tier: 'committed', bytes: 28891 },
    { name: 'profiles/pinecil', kind: 'module', tier: 'committed', bytes: 6691 },
    { name: 'big', kind: 'staged', tier: 'staged', bytes: 5212 },
    { name: 'demo', kind: 'staged', tier: 'staged', bytes: 156 },
    { name: 'rb', kind: 'staged', tier: 'staged', bytes: 3464 },
  ],
  scripts: [{ name: 'pinecil', running: true, dead: false, draft: true, resumes: 99, error: '' }],
};

test('every file on the device appears, not just the running ones', () => {
  const names = mergeScripts(LIVE).map((e) => e.name);
  // The bug this replaces showed one of these eight.
  assert.ok(names.includes('anker'), 'the committed anker script must be listed');
  assert.ok(names.includes('profiles/anker_a2687'), 'modules must be listed');
  assert.ok(names.includes('big'), 'staged-only files must be listed');
});

test('a name that is both committed and staged appears once, as staged', () => {
  // staged/demo.lua is what would actually run, so that is the tier shown.
  const demo = mergeScripts(LIVE).filter((e) => e.name === 'demo');
  assert.equal(demo.length, 1);
  assert.equal(demo[0].tier, 'staged');
});

test('a live draft outranks anything on flash', () => {
  // Showing "committed" while a draft is in effect would mean the editor
  // loads one thing and the board runs another.
  const p = mergeScripts(LIVE).find((e) => e.name === 'pinecil');
  assert.equal(p.tier, 'draft');
  assert.equal(p.running, true);
  assert.equal(p.resumes, 99);
});

test('a draft with no file yet is still listed', () => {
  const r = mergeScripts({ files: [], scripts: [{ name: 'scratch', draft: true, running: true }] });
  assert.equal(r.length, 1);
  assert.equal(r[0].name, 'scratch');
  assert.equal(r[0].tier, 'draft');
});

test('scripts sort before modules', () => {
  const kinds = mergeScripts(LIVE).map((e) => e.kind);
  assert.equal(kinds.indexOf('module'), kinds.lastIndexOf('script') + 1);
});

test('only a script is startable', () => {
  const byName = Object.fromEntries(mergeScripts(LIVE).map((e) => [e.name, e]));
  assert.equal(isStartable(byName['anker']), true);
  assert.equal(isStartable(byName['profiles/anker_a2687']), false);
  assert.equal(isStartable(undefined), false);
});

test('an empty or malformed reply does not throw', () => {
  assert.deepEqual(mergeScripts({}), []);
  assert.deepEqual(mergeScripts(null), []);
  assert.deepEqual(mergeScripts({ files: null, scripts: null }), []);
});
