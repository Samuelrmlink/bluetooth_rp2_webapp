// The editor's undo history.
//
// Why the editor keeps its own rather than using the textarea's: Chrome ends
// a typing undo group whenever the document's text changes between two
// keystrokes -- ANY text, anywhere -- and a live-highlighted editor changes
// the highlight layer on every keystroke by design. Measured: with the
// highlight updating, each typed character became its own undo step even
// with nothing else going on. So every change is recorded here as a small
// diff and undone by this, never by the browser.
//
// Grouping, which is the part that makes undo pleasant:
//  - a run of typing, or of backspacing, merges into one step, broken by a
//    pause, a newline or a jump elsewhere (roughly what VS Code does);
//  - the vim layer squashes a whole insert session (i ... Esc) into one
//    step, and each normal-mode command is one step, as in vim.
//
// DOM-free and tested (tests/unit/history.test.js).

const MERGE_MS = 1000;

// The single changed region between two texts.
export function diff(a, b) {
  let p = 0;
  const max = Math.min(a.length, b.length);
  while (p < max && a[p] === b[p]) p++;
  let s = 0;
  while (s < max - p && a[a.length - 1 - s] === b[b.length - 1 - s]) s++;
  return { at: p, del: a.slice(p, a.length - s), ins: b.slice(p, b.length - s) };
}

export class History {
  constructor(limit = 500) {
    this.limit = limit;
    this.reset();
  }

  reset() {
    this.done = [];
    this.undone = [];
    this._sealed = true;
  }

  get length() { return this.done.length; }

  // The next change starts a new step, whatever it is.
  seal() { this._sealed = true; }

  // A change from `before` to `after`. selBefore/selAfter are [start, end].
  record(before, after, selBefore, selAfter, now = Date.now()) {
    if (before === after) return;
    const op = { ...diff(before, after), selBefore, selAfter, t: now };
    const last = this.done[this.done.length - 1];
    if (!this._sealed && last && mergeable(last, op)) {
      if (op.del === '') { last.ins += op.ins; } else { last.at = op.at; last.del = op.del + last.del; }
      last.selAfter = selAfter;
      last.t = now;
    } else {
      this.done.push(op);
      if (this.done.length > this.limit) this.done.shift();
    }
    this.undone = [];
    // A newline or a jump ends a typing run; so does the start of a paste.
    this._sealed = op.ins.includes('\n') || op.ins.length > 1 || op.del.length > 1;
  }

  // Replace every step since `index` with one: the vim insert session.
  squashSince(index, before, after, selBefore, selAfter) {
    if (index >= this.done.length) return;
    this.done.length = index;
    this._sealed = true;
    this.record(before, after, selBefore, selAfter);
    this._sealed = true;
  }

  undo(text) {
    const op = this.done.pop();
    if (!op) return null;
    this.undone.push(op);
    this._sealed = true;
    return { text: text.slice(0, op.at) + op.del + text.slice(op.at + op.ins.length), sel: op.selBefore };
  }

  redo(text) {
    const op = this.undone.pop();
    if (!op) return null;
    this.done.push(op);
    this._sealed = true;
    return { text: text.slice(0, op.at) + op.ins + text.slice(op.at + op.del.length), sel: op.selAfter };
  }
}

function mergeable(last, op) {
  if (op.t - last.t > MERGE_MS) return false;
  // typing on: inserts only, each starting where the last ended
  if (op.del === '' && last.del === '' && op.ins.length === 1 && op.at === last.at + last.ins.length) {
    return !/\s/.test(op.ins) || /\s$/.test(last.ins);   // a word, or a run of spaces, per step
  }
  // backspacing: deletes only, each ending where the last began
  if (op.ins === '' && last.ins === '' && op.del.length === 1 && op.at + op.del.length === last.at) return true;
  return false;
}
