// The editor's own list of keys: everything in keys.js's 'Editor' group,
// by section (Tabs and split, Files, Vim ...), so the main list (`?`) is
// not where thirty editor keys pile up. Built from the binding table, so it
// cannot drift from what the keys do; a filter box narrows it as in the
// main list. Opened by the ? button in the editor's toolbar or Alt+/ (and
// `?` outside the text) while the editor is the active window.

import { Dialog } from '../modals/dialog.js';
import { BINDINGS, EDITOR_SECTIONS } from '../keys.js';
import { keyNodes } from '../modals/help.js';

export class EditorHelp {
  constructor(app) {
    this.app = app;
    const body = document.createElement('div');
    body.className = 'eh-body';
    body.innerHTML = `
      <div class="eh-top">
        <input type="search" class="eh-filter help-filter" placeholder="Type to filter…" aria-label="Filter the editor's keys" autocomplete="off" spellcheck="false">
        <button type="button" class="pane-btn eh-all">All shortcuts…</button>
      </div>
      <div class="eh-list"></div>
      <p class="eh-empty dim" hidden>No editor key matches that.</p>`;
    this.dlg = app.registerDialog(new Dialog({ id: 'editor-help-dlg', title: 'Editor keys', body }));
    this.q = (s) => body.querySelector(s);
    this.filter = this.q('.eh-filter');
    this.filter.addEventListener('input', () => this._filter());
    this.filter.addEventListener('keydown', (e) => {
      if (e.key === '?') { e.preventDefault(); this.close(); }          // ? opened it, ? closes it
    });
    this.q('.eh-all').addEventListener('click', () => { this.close(); app.helpModal?.open(); });
    this._build();
  }

  get isOpen() { return this.dlg.isOpen; }

  open() {
    // Esc in the filter: clear it, then close -- not merely "leave the box"
    // (which is what Esc in any other text field means).
    if (!this._esc && this.app.keys) {
      this._esc = true;
      this.app.keys.ownEscape(this.filter, () => {
        if (this.filter.value) { this.filter.value = ''; this._filter(); } else this.close();
        return true;
      });
    }
    this.filter.value = '';
    this._filter();
    this.dlg.open();
    setTimeout(() => this.filter.focus({ preventScroll: true }), 0);
  }

  close() { this.dlg.close(); }
  toggle() { if (this.isOpen) this.close(); else this.open(); }

  _build() {
    const list = this.q('.eh-list');
    const frag = document.createDocumentFragment();
    this._rows = [];
    this._sections = [];
    const editor = BINDINGS.filter((b) => b.group === 'Editor');
    const sections = [...EDITOR_SECTIONS, ...new Set(editor.map((b) => b.section).filter((s) => !EDITOR_SECTIONS.includes(s)))];
    for (const section of sections) {
      const rows = editor.filter((b) => (b.section || 'Other') === section);
      if (!rows.length) continue;
      const h = document.createElement('h3');
      h.textContent = section;
      const table = document.createElement('table');
      table.className = 'keys';
      for (const b of rows) {
        const tr = document.createElement('tr');
        tr.dataset.id = b.id;
        const k = document.createElement('td');
        k.append(...keyNodes(b.keys));
        const d = document.createElement('td');
        d.textContent = b.label;
        tr.append(k, d);
        tr.dataset.find = `${section} ${b.keys} ${b.label}`.toLowerCase();
        this._rows.push(tr);
        table.appendChild(tr);
      }
      frag.append(h, table);
      this._sections.push({ h, table });
    }
    list.replaceChildren(frag);
  }

  _filter() {
    const terms = this.filter.value.toLowerCase().split(/\s+/).filter(Boolean);
    let hits = 0;
    for (const tr of this._rows) {
      const show = terms.every((t) => tr.dataset.find.includes(t));
      tr.hidden = !show;
      if (show) hits++;
    }
    for (const s of this._sections) {
      const any = [...s.table.rows].some((r) => !r.hidden);
      s.h.hidden = !any;
      s.table.hidden = !any;
    }
    this.q('.eh-empty').hidden = hits > 0;
  }
}
