// Press and hold with a finger: the touch screen's right-click. DOM-free --
// the window feeds it pointer events (editor.js) and it says when the hold
// has lasted long enough; tests drive it with a fake clock.
//
// Only a touch or pen pointer starts one: a mouse has a right button, and a
// mouse held still on a row is someone reading, not asking for a menu.
// Moving more than `slop` px cancels it, so dragging the list to scroll it
// never opens anything; so does lifting early, or the browser taking the
// gesture over (pointercancel).
//
// After it fires, the finger lifts and the browser may follow with a click
// -- on the row, or on whatever menu item now lies under the finger. That
// click must do nothing. takeClick() says to eat it -- once, and the next
// pointerdown anywhere forgets it, because some browsers send no click
// after a long press and a stale flag would eat the next real tap.

export const LONG_PRESS_MS = 500;
export const LONG_PRESS_SLOP = 8;

export class LongPress {
  // onFire(key): the hold lasted. key is what down() was given (a path).
  constructor({ onFire, delay = LONG_PRESS_MS, slop = LONG_PRESS_SLOP,
    setTimer = (fn, ms) => setTimeout(fn, ms), clearTimer = (t) => clearTimeout(t) }) {
    this.onFire = onFire;
    this.delay = delay;
    this.slop = slop;
    this._set = setTimer;
    this._clear = clearTimer;
    this.state = 'idle';          // idle | pending | fired
    this._id = null;
    this._timer = null;
    this._eatClick = false;
  }

  // A pointer went down -- anywhere: every tap starts here, so a click owed
  // to an earlier long press is forgotten. False if it starts no press
  // (not touch or pen, or no key: not on a row).
  down({ id, x, y, type, key, primary = true }) {
    this._stop();
    this._eatClick = false;
    this.state = 'idle';
    if (!primary || key == null || (type !== 'touch' && type !== 'pen')) return false;
    this.state = 'pending';
    this._id = id;
    this._x = x;
    this._y = y;
    this.key = key;
    this._timer = this._set(() => this.fire(), this.delay);
    return true;
  }

  move({ id, x, y }) {
    if (this.state !== 'pending' || id !== this._id) return;
    if (Math.hypot(x - this._x, y - this._y) > this.slop) this.cancel();
  }

  // The pointer lifted (or was cancelled): a pending press is abandoned, a
  // fired one is over -- but its click is still eaten.
  up({ id } = {}) {
    if (this.state === 'idle' || (id !== undefined && id !== this._id)) return;
    this._stop();
    this.state = 'idle';
  }

  cancel() {
    this._stop();
    if (this.state === 'pending') this.state = 'idle';
  }

  // Fire now if a press is pending -- the timer, or the browser's own
  // long-press contextmenu arriving first. True if this call fired it.
  fire() {
    if (this.state !== 'pending') return false;
    this._stop();
    this.state = 'fired';
    this._eatClick = true;
    this.onFire?.(this.key);
    return true;
  }

  // A finger is down (held, or the hold has fired): a contextmenu now is
  // the browser's own long press, not a right-click.
  get active() {
    return this.state !== 'idle';
  }

  takeClick() {
    const eat = this._eatClick;
    this._eatClick = false;
    return eat;
  }

  _stop() {
    if (this._timer !== null) this._clear(this._timer);
    this._timer = null;
  }
}
