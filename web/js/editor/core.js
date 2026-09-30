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

import { highlight } from './highlight.js';

export class EditorCore {
  constructor(host) {
    host.innerHTML = `
      <div class="ed-scroll">
        <div class="ed-gutter" aria-hidden="true"></div>
        <div class="ed-code">
          <pre class="ed-hl" aria-hidden="true"></pre>
          <textarea class="ed-input" wrap="off" spellcheck="false" autocomplete="off"
                    autocapitalize="off" autocorrect="off" aria-label="File contents"></textarea>
        </div>
      </div>`;
    this.scroll = host.querySelector('.ed-scroll');
    this.gutter = host.querySelector('.ed-gutter');
    this.pre = host.querySelector('.ed-hl');
    this.ta = host.querySelector('.ed-input');
    this.lang = 'text';
    this.onChange = null;          // (text) => void, after every edit
    this.onCursor = null;          // (line, col) => void
    this._dirty = true;
    this._lines = 0;

    this.ta.addEventListener('input', () => {
      this._dirty = true;
      this.onChange?.(this.ta.value);
      this._cursor();
    });
    for (const ev of ['keyup', 'click', 'select']) this.ta.addEventListener(ev, () => this._cursor());
    // Tab inserts two spaces rather than leaving the field -- inside an
    // editor, Tab is text. insertText keeps it on the native undo stack.
    this.ta.addEventListener('keydown', (e) => {
      if (e.key === 'Tab' && !e.ctrlKey && !e.altKey && !e.metaKey && !this.readOnly) {
        e.preventDefault();
        this.insert('  ');
      }
    });
  }

  get value() { return this.ta.value; }

  // Replacing the whole text (opening a file) is not an edit: it clears the
  // undo history rather than letting Ctrl+Z walk back into another file.
  setValue(text, lang) {
    this.ta.value = text;
    this.lang = lang || this.lang;
    this._dirty = true;
    this.ta.setSelectionRange(0, 0);
    this.scroll.scrollTop = 0;
    this.scroll.scrollLeft = 0;
    this._cursor();
  }

  set readOnly(v) { this.ta.readOnly = !!v; }
  get readOnly() { return this.ta.readOnly; }

  focus() { this.ta.focus({ preventScroll: true }); }

  // Insert at the selection as if typed, so it is one undo step.
  insert(text) {
    this.ta.focus({ preventScroll: true });
    if (!document.execCommand('insertText', false, text)) {
      // Fallback where insertText is refused: correct, but not undoable.
      const { selectionStart: a, selectionEnd: b, value } = this.ta;
      this.ta.value = value.slice(0, a) + text + value.slice(b);
      this.ta.setSelectionRange(a + text.length, a + text.length);
      this.ta.dispatchEvent(new Event('input'));
    }
  }

  // Line/column of the caret, 1-based, for the status bar.
  _cursor() {
    const pos = this.ta.selectionStart;
    const before = this.ta.value.slice(0, pos);
    const line = before.split('\n').length;
    const col = pos - before.lastIndexOf('\n');
    this.onCursor?.(line, col);
  }

  // Called from the app's frame loop while the window is visible.
  render() {
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
