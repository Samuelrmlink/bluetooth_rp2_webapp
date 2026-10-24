import { test } from 'node:test';
import assert from 'node:assert/strict';
import { freshnessAction, changeAction, freshTimerActive, nextFreshDelay, FRESH_MS, FRESH_BACKOFF_MAX } from '../../js/editor/freshness.js';

const same = { status: 200, hash: 'abc' };
const other = { status: 200, hash: 'def' };

function act(over) {
  return freshnessAction({
    binary: false, partition: false, isNew: false, pristine: false,
    dirty: false, base: 'abc', kept: undefined, notedGone: false, askedGone: false,
    remote: same,
    ...over,
  });
}

test('a matching hash, a binary, and a partition view do nothing', () => {
  assert.equal(act({}), 'same');
  assert.equal(act({ binary: true, remote: other }), 'skip');
  assert.equal(act({ partition: true, remote: other }), 'skip');
  assert.equal(FRESH_MS, 5000);
});

test('a clean buffer reloads when the hash differs', () => {
  assert.equal(act({ remote: other }), 'reload');
});

test('a dirty buffer asks once per remote hash, then stays', () => {
  assert.equal(act({ dirty: true, remote: other }), 'ask');
  assert.equal(act({ dirty: true, remote: other, kept: 'def' }), 'same');
  assert.equal(act({ dirty: true, remote: { status: 200, hash: 'fff' }, kept: 'def' }), 'ask');
});

test('a missing file asks to close a clean buffer once, and only notes a dirty one', () => {
  const gone = { status: 404 };
  assert.equal(act({ remote: gone }), 'gone');
  assert.equal(act({ remote: gone, askedGone: true }), 'same');
  assert.equal(act({ dirty: true, remote: gone }), 'gone-dirty');
  assert.equal(act({ dirty: true, remote: gone, notedGone: true }), 'same');
  assert.equal(act({ isNew: true, base: '', remote: gone }), 'same');
  assert.equal(act({ pristine: true, isNew: true, base: '', remote: other }), 'reload');
});

test('a busy card backs off; other failures do not reload', () => {
  assert.equal(act({ remote: { status: 503 } }), 'busy');
  assert.equal(act({ remote: { offline: true } }), 'error');
  assert.equal(act({ remote: { status: 500 } }), 'error');
  assert.equal(nextFreshDelay(0), 10000);
  assert.equal(nextFreshDelay(10000), 20000);
  assert.equal(nextFreshDelay(20000), FRESH_BACKOFF_MAX);
  assert.equal(nextFreshDelay(FRESH_BACKOFF_MAX), FRESH_BACKOFF_MAX);
});

test('the poll stops once change events arrive, and a matching hash needs no read', () => {
  assert.equal(freshTimerActive(false), true);
  assert.equal(freshTimerActive(true), false);
  assert.equal(changeAction({ binary: false, gz: false, hash: 'abc', base: 'abc' }), 'same');
  assert.equal(changeAction({ binary: false, gz: false, hash: 'def', base: 'abc' }), 'read');
  assert.equal(changeAction({ binary: false, gz: true, hash: 'abc', base: 'abc' }), 'read');
  assert.equal(changeAction({ binary: false, gz: false, hash: null, base: 'abc' }), 'read');
  assert.equal(changeAction({ binary: false, missing: true }), 'read');
  assert.equal(changeAction({ binary: true, hash: 'def', base: 'abc' }), 'skip');
});
