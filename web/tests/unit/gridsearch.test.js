import { test } from 'node:test';
import assert from 'node:assert/strict';
import { findMatches, matchCells, stepMatch, highlightHTML } from '../../js/controls/gridsearch.js';

const cols = [
  { key: 'a', get: (r) => r.a },
  { key: 'b', get: (r) => r.b, show: (r) => `<${r.b}>` },
  { key: 'x', get: () => 'notify', searchable: false },
];
const rows = [{ a: 'Notify', b: 'x' }, { a: 'read', b: 'notify notify' }, { a: 'write', b: 'y' }];

test('every occurrence, case-insensitive, in what cells show; unsearchable columns skipped', () => {
  const m = findMatches(rows, cols, 'NOTIFY');
  assert.deepEqual(m, [
    { row: 0, col: 0, start: 0, len: 6 },
    { row: 1, col: 1, start: 1, len: 6 },
    { row: 1, col: 1, start: 8, len: 6 },
  ]);
  assert.deepEqual(findMatches(rows, cols, '<y>'), [{ row: 2, col: 1, start: 0, len: 3 }], 'the shown text, not the value');
  assert.deepEqual(findMatches(rows, cols, ''), []);
});

test('matches group into cells, and n / N step through cells, wrapping', () => {
  const cells = matchCells(findMatches(rows, cols, 'notify'));
  assert.equal(cells.length, 2);
  assert.deepEqual(cells[1].ranges, [[1, 6], [8, 6]]);
  assert.equal(stepMatch(cells, null, 1), 0);
  assert.equal(stepMatch(cells, null, -1), 1);
  assert.equal(stepMatch(cells, { row: 0, col: 0 }, 1), 1);
  assert.equal(stepMatch(cells, { row: 1, col: 1 }, 1), 0, 'wraps forward');
  assert.equal(stepMatch(cells, { row: 0, col: 0 }, -1), 1, 'wraps back');
  assert.equal(stepMatch(cells, { row: 0, col: 1 }, 1), 1, 'from a cell that is not a match');
  assert.equal(stepMatch([], null, 1), -1);
});

test('highlighting marks each range and escapes everything', () => {
  assert.equal(highlightHTML('a<b>a', [[0, 1], [4, 1]]), '<mark>a</mark>&lt;b&gt;<mark>a</mark>');
  assert.equal(highlightHTML('x & y', []), 'x &amp; y');
});
