import { test } from 'node:test';
import assert from 'node:assert/strict';
import { parseDuration, formatDuration } from '../../js/controls/duration.js';
import { Pending, near } from '../../js/controls/state.js';

test('parseDuration: the forms a timer box takes', () => {
  assert.equal(parseDuration('1:30'), 5400);
  assert.equal(parseDuration('1:30:15'), 5415);
  assert.equal(parseDuration('90m'), 5400);
  assert.equal(parseDuration('2h'), 7200);
  assert.equal(parseDuration('1h30m'), 5400);
  assert.equal(parseDuration('1h 30m'), 5400);
  assert.equal(parseDuration('45s'), 45);
  assert.equal(parseDuration('45'), 2700, 'a bare number is minutes');
  assert.equal(parseDuration('0'), 0);
  assert.equal(parseDuration('1.5h'), 5400);
  for (const bad of ['', 'soon', '1:75', '-5', 'h', '1x']) assert.equal(parseDuration(bad), null, bad);
});

test('formatDuration: h:mm:ss, round trips', () => {
  assert.equal(formatDuration(0), '0:00:00');
  assert.equal(formatDuration(3599), '0:59:59');
  assert.equal(formatDuration(86400), '24:00:00');
  assert.equal(formatDuration(-3), '0:00:00');
  assert.equal(parseDuration(formatDuration(5415)), 5415);
});

test('a countdown write confirms near its target, not only on it', () => {
  assert.ok(near(3590, 3600, 20));
  assert.ok(!near(3500, 3600, 20));
  assert.ok(!near(null, 0, 20));
  assert.ok(near(1, true), 'no tolerance: the loose equality as before');
  const p = new Pending(8000);
  p.begin('ac_countdown_s', 3600, 0, 20);
  assert.deepEqual(p.observe({ ac_countdown_s: 0 }, 1000).confirmed, [], 'the old value is not it');
  assert.deepEqual(p.observe({ ac_countdown_s: 3597 }, 2000).confirmed, ['ac_countdown_s']);
});
