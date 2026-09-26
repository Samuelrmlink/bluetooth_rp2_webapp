import { test } from 'node:test';
import assert from 'node:assert/strict';
import { draftChunks, ScriptIO, DRAFT_CHUNK } from '../../js/scriptio.js';

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
