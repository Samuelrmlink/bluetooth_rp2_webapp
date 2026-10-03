// The editing surface: a real <textarea> laid exactly over a highlighted
// <pre>, with a line-number gutter beside them.
//
// Why a textarea and not contenteditable or a library: a textarea gives
// native caret, selection, IME, spellcheck-off, copy/paste and -- above all
// -- a native undo history, for free and correctly on every browser. Its
// text is transparent (only the caret shows), and the <pre> beneath it draws
// the same text in colour. The one thing that must never happen is the two
// disagreeing about where a character is, so both use one font, one size,
// one line height, one tab width, no wrapping, and the same padding; and the
// highlighter is tested to reproduce its input character for character.
//
// Layout: the textarea and pre share ONE grid cell, so the cell grows to fit
// the longer of the two and the textarea never scrolls by itself -- the
// outer .ed-scroll does, carrying both, and the gutter is sticky to its left.
// Typing at the edge still reveals the caret, because the browser scrolls
// the nearest scrollable ancestor to keep a focused caret in view.
//
// Highlighting runs at most once a frame, after the text changes.

import { highlight, esc } from './highlight.js';
import { History } from './history.js';

export class EditorCore {
  constructor(host) {
    host.innerHTML = `
      <div class="ed-scroll">
        <div class="ed-gutter" aria-hidden="true"></div>
        <div class="ed-code">
          <pre class="ed-find" aria-hidden="true"></pre>
          <pre class="ed-hl" aria-hidden="true"></pre>
          <textarea class="ed-input" wrap="off" spellcheck="false" autocomplete="off"
                    autocapitalize="off" autocorrect="off" aria-label="File contents"></textarea>
        </div>
      </div>`;
    this.scroll = host.querySelector('.ed-scroll');
    this.gutter = host.querySelector('.ed-gutter');
    this.pre = host.querySelector('.ed-hl');
    // Search matches: a third layer UNDER the coloured text, same font and
    // padding, its text transparent -- only the <mark> backgrounds show.
    this.findPre = host.querySelector('.ed-find');
    this._find = null;             // { at: [offsets], len, cur } or null
    this._findDirty = false;
    this.ta = host.querySelector('.ed-input');
    this.lang = 'text';
    // Our own undo history: Chrome's cannot survive a live highlight layer
    // (see editor/history.js), so every change is recorded here.
    this.history = new History();
    this._prev = '';               // the text before the change in flight
    this._selBefore = [0, 0];
    this.onChange = null;          // (text) => void, after every edit
    this.onCursor = null;          // (line, col) => void
    this._dirty = true;
    this._lines = 0;

    this.ta.addEventListener('beforeinput', (e) => {
      // The context menu's Undo / Redo arrive here; route them to ours.
      if (e.inputType === 'historyUndo' || e.inputType === 'historyRedo') {
        e.preventDefault();
        if (e.inputType === 'historyUndo') this.undo(); else this.redo();
        return;
      }
      this._selBefore = [this.ta.selectionStart, this.ta.selectionEnd];
    });
    this.ta.addEventListener('input', () => {
      const now = this.ta.value;
      this.history.record(this._prev, now, this._selBefore, [this.ta.selectionStart, this.ta.selectionEnd]);
      this._prev = now;
      this._changed();
    });
    for (const ev of ['keyup', 'click', 'select']) this.ta.addEventListener(ev, () => this._cursor());
    // Tab inserts two spaces rather than leaving the field -- inside an
    // editor, Tab is text. insertText keeps it on the native undo stack.
    this.ta.addEventListener('keydown', (e) => {
      if (e.key === 'Tab' && !e.ctrlKey && !e.altKey && !e.metaKey && !this.readOnly) {
        e.preventDefault();
        this.insert('  ');
        return;
      }
      if ((e.ctrlKey || e.metaKey) && !e.altKey) {
        const k = e.key.toLowerCase();
        if (k === 'z' && !e.shiftKey) { e.preventDefault(); this.undo(); }
        else if ((k === 'z' && e.shiftKey) || k === 'y') { e.preventDefault(); this.redo(); }
      }
    });
  }

  get value() { return this.ta.value; }

  // Replacing the whole text (opening a file) is not an edit: it clears the
  // undo history rather than letting Ctrl+Z walk back into another file.
  setValue(text, lang) {
    this.ta.value = text;
    this._prev = text;
    // a NEW history, not a reset: the old one may be kept by a tab (saveView)
    this.history = new History();
    this.lang = lang || this.lang;
    this._dirty = true;
    this.ta.setSelectionRange(0, 0);
    this.scroll.scrollTop = 0;
    this.scroll.scrollLeft = 0;
    this._cursor();
  }

  // A tab's view, kept while another file shows: undo history, cursor and
  // scroll, so coming back to a tab is coming back to where you were.
  saveView() {
    return { history: this.history, sel: [this.ta.selectionStart, this.ta.selectionEnd],
             top: this.scroll.scrollTop, left: this.scroll.scrollLeft };
  }

  restoreView(v) {
    if (!v) return;
    this.history = v.history;
    const n = this.ta.value.length;
    this.ta.setSelectionRange(Math.min(v.sel[0], n), Math.min(v.sel[1], n));
    this.render();                    // the text's height first, or the scroll is clamped
    this.scroll.scrollTop = v.top;
    this.scroll.scrollLeft = v.left;
    this._cursor();
  }

  // The same file edited in the other half of a split: take its text
  // without making it an edit here. Undo history goes (it described the old
  // text); the cursor stays where it was, as near as the new text allows.
  adopt(text) {
    if (text === this.ta.value) return;
    const [a, b] = [this.ta.selectionStart, this.ta.selectionEnd];
    const top = this.scroll.scrollTop;
    this.ta.value = text;
    this._prev = text;
    this.history.reset();
    this.ta.setSelectionRange(Math.min(a, text.length), Math.min(b, text.length));
    this._dirty = true;
    this.scroll.scrollTop = top;
  }

  set readOnly(v) { this.ta.readOnly = !!v; }
  get readOnly() { return this.ta.readOnly; }

  focus() { this.ta.focus({ preventScroll: true }); }

  // Insert at the selection, as one undo step.
  insert(text) {
    this.replaceRange(this.ta.selectionStart, this.ta.selectionEnd, text);
  }

  _changed() {
    this._dirty = true;
    this.onChange?.(this.ta.value);
    this._cursor();
  }

  undo() { this._restore(this.history.undo(this.ta.value)); }
  redo() { this._restore(this.history.redo(this.ta.value)); }

  _restore(r) {
    if (!r || this.readOnly) return;
    this.ta.value = r.text;
    this._prev = r.text;
    this.ta.setSelectionRange(r.sel[0], r.sel[1]);
    this._changed();
  }

  // Replace [from, to) as ONE undo step -- the vim layer's way of changing
  // text, and Tab's. setRangeText is programmatic (no input event), so the
  // change is recorded here rather than by the input listener.
  replaceRange(from, to, text) {
    if (this.readOnly) return;
    const before = this.ta.value;
    const selBefore = [this.ta.selectionStart, this.ta.selectionEnd];
    this.ta.setRangeText(text, from, to, 'end');
    this.history.seal();
    this.history.record(before, this.ta.value, selBefore, [this.ta.selectionStart, this.ta.selectionEnd]);
    this.history.seal();
    this._prev = this.ta.value;
    this._changed();
  }

  // Highlight these matches (offsets of `len` characters; `cur` is the one
  // the cursor is on), or none. Cheap to call every keystroke: it only marks
  // the layer for the next frame, and only when something changed.
  setFind(at, len, cur = -1) {
    const next = at && at.length && len ? { at, len, cur } : null;
    const same = (a, b) => (!a && !b) || (a && b && a.len === b.len && a.cur === b.cur &&
      a.at.length === b.at.length && a.at.every((v, i) => v === b.at[i]));
    if (same(next, this._find)) return;
    this._find = next;
    this._findDirty = true;
  }

  // Line/column of the caret, 1-based, for the status bar.
  _cursor() {
    const pos = this.ta.selectionStart;
    const before = this.ta.value.slice(0, pos);
    const line = before.split('\n').length;
    const col = pos - before.lastIndexOf('\n');
    this.onCursor?.(line, col);
  }

  _renderFind() {
    this._findDirty = false;
    const f = this._find;
    if (!f) { this.findPre.textContent = ''; return; }
    const t = this.ta.value;
    let html = '';
    let k = 0;
    for (const a of f.at) {
      if (a < k) continue;                       // overlapping: the first wins
      html += esc(t.slice(k, a)) + `<mark${a === f.cur ? ' class="cur"' : ''}>${esc(t.slice(a, a + f.len))}</mark>`;
      k = a + f.len;
    }
    // Only up to the last mark: what follows has nothing to show.
    this.findPre.innerHTML = html;
  }

  // Called from the app's frame loop while the window is visible.
  render() {
    if (this._findDirty || (this._dirty && this._find)) this._renderFind();
    if (!this._dirty) return;
    this._dirty = false;
    const text = this.ta.value;
    // A trailing newline in a <pre> collapses; the textarea keeps the empty
    // last line. The extra space keeps the two the same height.
    this.pre.innerHTML = highlight(text, this.lang) + '\n ';
    const lines = text.split('\n').length;
    if (lines !== this._lines) {
      this._lines = lines;
      let s = '';
      for (let i = 1; i <= lines; i++) s += i + '\n';
      this.gutter.textContent = s;
    }
  }
}
