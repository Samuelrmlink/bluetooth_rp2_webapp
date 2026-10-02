import { test } from 'node:test';
import assert from 'node:assert/strict';
import { LongPress, LONG_PRESS_MS, LONG_PRESS_SLOP } from '../../js/editor/longpress.js';

// A fake clock: one timer at a time is all LongPress ever holds.
function rig() {
  const fired = [];
  let timer = null;
  const lp = new LongPress({
    onFire: (k) => fired.push(k),
    setTimer: (fn, ms) => { timer = { fn, ms }; return timer; },
    clearTimer: (t) => { if (timer === t) timer = null; },
  });
  const elapse = () => { const t = timer; timer = null; t?.fn(); };
  return { lp, fired, elapse, pending: () => timer };
}
const touch = (over = {}) => ({ id: 1, x: 100, y: 100, type: 'touch', key: 'lua/anker.lua', ...over });

test('defaults: about half a second, about 8 px', () => {
  assert.equal(LONG_PRESS_MS, 500);
  assert.equal(LONG_PRESS_SLOP, 8);
});

test('a held finger fires once, with its key; the click after it is eaten, once', () => {
  const { lp, fired, elapse, pending } = rig();
  assert.equal(lp.down(touch()), true);
  assert.equal(pending().ms, 500);
  assert.equal(lp.active, true);
  elapse();
  assert.deepEqual(fired, ['lua/anker.lua']);
  assert.equal(lp.state, 'fired');
  lp.up({ id: 1 });
  assert.equal(lp.active, false);
  assert.equal(lp.takeClick(), true);
  assert.equal(lp.takeClick(), false);
});

test('a short tap does not fire and its click goes through', () => {
  const { lp, fired, pending } = rig();
  lp.down(touch());
  lp.up({ id: 1 });
  assert.equal(pending(), null);
  assert.deepEqual(fired, []);
  assert.equal(lp.takeClick(), false);
});

test('moving past the slop cancels (a scroll); jitter within it does not', () => {
  const { lp, fired, elapse, pending } = rig();
  lp.down(touch());
  lp.move({ id: 1, x: 105, y: 105 });           // ~7 px
  assert.equal(lp.state, 'pending');
  lp.move({ id: 1, x: 100, y: 109 });           // 9 px
  assert.equal(lp.state, 'idle');
  assert.equal(pending(), null);
  elapse();
  assert.deepEqual(fired, []);
});

test('pointercancel (the browser took the gesture) cancels', () => {
  const { lp, fired, pending } = rig();
  lp.down(touch());
  lp.up({ id: 1 });                              // editor.js routes pointercancel here
  assert.equal(pending(), null);
  assert.deepEqual(fired, []);
});

test('a mouse, or a second finger, starts nothing; pen does', () => {
  const { lp, pending } = rig();
  assert.equal(lp.down(touch({ type: 'mouse' })), false);
  assert.equal(pending(), null);
  assert.equal(lp.down(touch({ primary: false })), false);
  assert.equal(lp.down(touch({ type: 'pen' })), true);
});

test('another pointer\'s moves and lifts are ignored', () => {
  const { lp } = rig();
  lp.down(touch());
  lp.move({ id: 2, x: 300, y: 300 });
  lp.up({ id: 2 });
  assert.equal(lp.state, 'pending');
});

test('the browser\'s long-press contextmenu fires it early -- and only once', () => {
  const { lp, fired, elapse, pending } = rig();
  lp.down(touch());
  assert.equal(lp.fire(), true);                 // contextmenu before the timer
  assert.equal(pending(), null);
  elapse();
  assert.equal(lp.fire(), false);                // contextmenu after the timer
  assert.deepEqual(fired, ['lua/anker.lua']);
});

test('a new press forgets an uneaten click (no click came after the long press)', () => {
  const { lp, elapse } = rig();
  lp.down(touch());
  elapse();
  lp.up({ id: 1 });
  lp.down(touch({ id: 2 }));
  lp.up({ id: 2 });
  assert.equal(lp.takeClick(), false);           // the real tap is not eaten
});

test('so does a pointerdown off the rows (a tap on a menu item), starting nothing', () => {
  const { lp, elapse, pending } = rig();
  lp.down(touch());
  elapse();
  lp.up({ id: 1 });
  assert.equal(lp.down(touch({ id: 2, key: null })), false);
  assert.equal(pending(), null);
  assert.equal(lp.takeClick(), false);
});
