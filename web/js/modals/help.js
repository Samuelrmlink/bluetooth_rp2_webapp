// The keyboard shortcut list: `?` opens it, `?` / Esc / the close button /
// the backdrop close it. Built from the binding table in keys.js, so it
// cannot drift from what the keys actually do.
//
// Unlike Settings this one dims and blocks the page: it is read, not worked
// alongside, and a blocking overlay is what Esc-to-close implies.
//
// It is also long enough that reading it beats searching it only for the
// first few entries, so opening it puts the caret in a filter box: type any
// part of a key or of what it does and the list narrows to match. Every row
// carries its own searchable text (`data-find`), including the two prose
// notes at the bottom — someone looking for "drag" should find the paragraph
// that explains dragging, not just the bindings.
//
// Typing reaching the box at all depends on keys.js leaving text fields
// alone, which it does; the one thing it had to be told is that Esc in
// *this* field means clear-then-close rather than "leave the field".

import { BINDINGS, GROUPS, IS_MAC } from '../keys.js';

export class HelpModal {
  constructor() {
    this._overlay = document.getElementById('help-modal');
    this._panel = this._overlay.querySelector('.modal');
    this._body = this._overlay.querySelector('.modal-body');
    this._restoreTo = null;

    this._input = document.getElementById('help-filter');
    this._rows = [];
    this._sections = [];

    this._overlay.querySelector('.modal-close').addEventListener('click', () => this.close());
    this._overlay.addEventListener('click', (e) => {
      if (e.target === this._overlay) this.close();
    });
    this._input?.addEventListener('input', () => this._filter());
    // The body is what scrolls, and a caret in a one-line field has nothing
    // to do with the arrows, so they are borrowed to move the list. j/k still
    // scroll it when the focus is in the list itself (nav.js) — here they are
    // letters, because here you are typing.
    this._input?.addEventListener('keydown', (e) => {
      // `?` opened this and `?` closes it — the habit is stronger than the
      // chance of wanting to search for a question mark, and that row is
      // reachable by typing "help" instead. keys.js never sees this one: it
      // leaves text fields alone, which is what lets the rest be typed.
      if (e.key === '?') {
        e.preventDefault();
        this.close();
        return;
      }
      const by = { ArrowDown: 48, ArrowUp: -48, PageDown: 240, PageUp: -240 }[e.key];
      if (by === undefined) return;
      this._body.scrollTop += by;
      e.preventDefault();
    });
    this._build();
  }

  get isOpen() {
    return !this._overlay.hidden;
  }

  get panel() {
    return this._panel;
  }

  open() {
    if (this.isOpen) return;
    const ae = document.activeElement;
    this._restoreTo = ae && ae !== document.body ? ae : null;
    this._overlay.hidden = false;
    // Always from the top, never from where the last search left it.
    if (this._input) this._input.value = '';
    this._filter();
    this._body.scrollTop = 0;
    (this._input ?? this._panel).focus({ preventScroll: true });
  }

  // Esc, by way of app.js. A filter in hand is the thing to let go of first:
  // closing the list and losing what you had narrowed it to would mean
  // typing it again.
  escape() {
    if (this._input?.value) {
      this._input.value = '';
      this._filter();
      this._input.focus({ preventScroll: true });
      return;
    }
    this.close();
  }

  close() {
    if (!this.isOpen) return;
    // Let go of the filter explicitly. Hiding the overlay does not take the
    // focus with it, and a caret left in a hidden field makes every later key
    // look like typing — swallowing the next `?`, which is the key that would
    // bring this back.
    if (this._input && document.activeElement === this._input) this._input.blur();
    this._overlay.hidden = true;
    const to = this._restoreTo;
    this._restoreTo = null;
    if (to && to.isConnected) to.focus({ preventScroll: true });
  }

  toggle() {
    if (this.isOpen) this.close();
    else this.open();
  }

  _build() {
    const frag = document.createDocumentFragment();
    this._rows = [];
    this._sections = [];
    // What a row is searched by: its group, the keys as written, and the
    // description. Searching "split" finds the splitting bindings; searching
    // "alt+r" finds the one key.
    const searchable = (el, text) => {
      el.dataset.find = text.toLowerCase();
      this._rows.push(el);
      return el;
    };
    for (const group of GROUPS) {
      // The editor has its own list (editor/help.js); here, one row that
      // says where -- and that a search for an editor key still lands on.
      const editorRows = BINDINGS.filter((b) => b.group === 'Editor');
      const rows = group === 'Editor'
        ? [{ keys: 'Alt+/ in the editor', label: 'The editor has its own list of keys: tabs and split, files, save and run, drag and drop, vim. Open it with the ? button in the editor\'s toolbar, or Alt+/ while the editor is the active window.',
             find: editorRows.map((b) => `${b.keys} ${b.label}`).join(' ') }]
        : BINDINGS.filter((b) => b.group === group);
      if (!rows.length) continue;
      const h = document.createElement('h3');
      h.textContent = group;
      frag.appendChild(h);
      const table = document.createElement('table');
      table.className = 'keys';
      for (const b of rows) {
        const tr = document.createElement('tr');
        const td1 = document.createElement('td');
        td1.append(...keyNodes(b.keys));
        const td2 = document.createElement('td');
        td2.textContent = b.label;
        tr.append(td1, td2);
        table.appendChild(searchable(tr, `${group} ${b.keys} ${b.label} ${b.find || ''}`));
      }
      frag.appendChild(table);
      this._sections.push({ head: h, table });
    }
    const note = document.createElement('p');
    note.className = 'dim';
    note.textContent = IS_MAC
      ? 'Alt is the Option key. Shortcuts match the physical key, so they work on any layout.'
      : 'Firefox on Linux and Windows reserves Alt+letter for its menu bar; use the arrow-key forms there (Alt+arrows, Alt+Enter). Shortcuts match the physical key, so they work on any layout.';
    frag.appendChild(searchable(note, note.textContent));
    const drag = document.createElement('p');
    drag.className = 'dim';
    drag.textContent = 'Mouse: drag a tile by the empty part of its header, or Alt+drag anywhere on it, and drop on another window. The pane gives up its place as soon as you start, and is drawn half-faded wherever it would land, so what you see is what you would get: near an edge it takes that half of the window, splitting it; on the line between two windows it takes a band beside both of them; in the middle the two panes swap. Releasing settles it and Esc puts everything back. Drag a floating window by its header, and resize it from any edge or corner.';
    frag.appendChild(searchable(drag, drag.textContent));

    // Shown only when a filter matches nothing, so that an empty list is
    // never mistaken for a broken one.
    this._empty = document.createElement('p');
    this._empty.className = 'dim';
    this._empty.hidden = true;
    this._empty.textContent = 'No shortcut matches that.';
    frag.appendChild(this._empty);

    this._body.replaceChildren(frag);
  }

  // Every term has to appear somewhere in the row, so "alt split" narrows to
  // the splitting bindings that use Alt rather than to everything with either
  // word in it. A heading goes when the last of its rows does.
  _filter() {
    const terms = (this._input?.value ?? '').toLowerCase().split(/\s+/).filter(Boolean);
    let hits = 0;
    for (const el of this._rows) {
      const show = terms.every((t) => el.dataset.find.includes(t));
      el.hidden = !show;
      if (show) hits += 1;
    }
    for (const s of this._sections) {
      const any = [...s.table.rows].some((r) => !r.hidden);
      s.head.hidden = !any;
      s.table.hidden = !any;
    }
    this._empty.hidden = hits > 0;
    this._body.scrollTop = 0;
  }
}

// 'Alt+H / Alt+←' -> <kbd>Alt</kbd>+<kbd>H</kbd> / <kbd>Alt</kbd>+<kbd>←</kbd>
export function keyNodes(display) {
  const out = [];
  display.split(' / ').forEach((alt, i) => {
    if (i) out.push(document.createTextNode(' / '));
    // '+' is also a key ('Alt+=' is fine, but 'Alt+-' splits on the plus).
    const parts = alt.split('+').filter((p) => p !== '');
    parts.forEach((p, j) => {
      if (j) out.push(document.createTextNode('+'));
      const kbd = document.createElement('kbd');
      kbd.textContent = p;
      out.push(kbd);
    });
  });
  return out;
}
