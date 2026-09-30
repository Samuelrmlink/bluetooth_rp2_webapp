// Table headers you can sort and resize, remembered per table in the browser
// like the window arrangement.
//
// The windows draw their tables from data every frame, so sorting belongs to
// the DATA, not the DOM: a window asks grid.sort(rows) for the order and
// draws that. The header is a row of buttons (click or Enter sorts: ascending,
// then descending, then back to the window's default) with a drag handle on
// each column's right edge (double-click it to restore the default width).
//
// Column spec: { key, label, type: 'num' | 'text', width, get(row), sortable }.
// State lives in prefs under `tables[<id>]`: { widths: {key: px}, sort: {key, dir} }.

import { readPrefs, writePrefs } from '../prefs.js';

const MIN_W = 36;

// Nulls and empty strings sort last whichever way, so "unknown" never crowds
// out the values you sorted to see. Numbers compare as numbers, text
// case-insensitively and naturally ("dev2" before "dev10").
export function compareValues(a, b, type) {
  const blank = (v) => v == null || v === '';
  if (blank(a) || blank(b)) return blank(a) === blank(b) ? 0 : blank(a) ? 1 : -1;
  if (type === 'num') return Number(a) - Number(b);
  return String(a).localeCompare(String(b), undefined, { numeric: true, sensitivity: 'base' });
}

// A stable sort of `rows` by column `col` in direction `dir` (1 or -1),
// blanks last in both directions.
export function sortRows(rows, col, dir) {
  const blank = (v) => v == null || v === '';
  return rows
    .map((r, i) => [r, i, col.get(r)])
    .sort(([, ia, va], [, ib, vb]) => {
      if (blank(va) || blank(vb)) return (blank(va) === blank(vb) ? 0 : blank(va) ? 1 : -1) || ia - ib;
      return dir * compareValues(va, vb, col.type) || ia - ib;
    })
    .map(([r]) => r);
}

function loadState(id) {
  const t = readPrefs().tables;
  return (t && typeof t === 'object' && t[id]) || {};
}

function saveState(id, state) {
  const all = { ...(readPrefs().tables || {}) };
  all[id] = state;
  writePrefs({ tables: all });
}

export class Grid {
  // defaultSort: { key, dir } -- what "no sort chosen" means for this table.
  constructor(table, { id, columns, defaultSort = null, onChange = () => {} }) {
    this.table = table;
    this.id = id;
    this.columns = columns;
    this.defaultSort = defaultSort;
    this.onChange = onChange;
    const st = loadState(id);
    this.widths = { ...(st.widths || {}) };
    this.sortState = st.sort && columns.some((c) => c.key === st.sort.key) ? st.sort : null;
    this._build();
  }

  get activeSort() { return this.sortState || this.defaultSort; }

  sort(rows) {
    const s = this.activeSort;
    const col = s && this.columns.find((c) => c.key === s.key);
    return col ? sortRows(rows, col, s.dir === 'desc' ? -1 : 1) : rows;
  }

  _persist() {
    saveState(this.id, { widths: this.widths, sort: this.sortState });
  }

  _build() {
    const t = this.table;
    t.classList.add('grid-resizable');
    t.querySelector('colgroup')?.remove();
    const cg = document.createElement('colgroup');
    this._cols = this.columns.map((c) => {
      const col = document.createElement('col');
      col.style.width = `${this.widths[c.key] || c.width}px`;
      cg.appendChild(col);
      return col;
    });
    t.prepend(cg);

    let thead = t.querySelector('thead');
    if (!thead) { thead = document.createElement('thead'); t.prepend(thead); t.prepend(cg); }
    const tr = document.createElement('tr');
    this._heads = this.columns.map((c, i) => {
      const th = document.createElement('th');
      th.dataset.key = c.key;
      if (c.sortable === false) {
        th.textContent = c.label;
      } else {
        const b = document.createElement('button');
        b.type = 'button';
        b.className = 'col-sort';
        b.innerHTML = '<span class="col-label"></span><span class="col-arrow" aria-hidden="true"></span>';
        b.querySelector('.col-label').textContent = c.label;
        b.addEventListener('click', () => this._cycle(c));
        th.appendChild(b);
      }
      const h = document.createElement('span');
      h.className = 'col-resize';
      h.title = 'Drag to resize; double-click to reset';
      h.addEventListener('pointerdown', (e) => this._drag(e, c, i));
      h.addEventListener('dblclick', () => this._setWidth(c, i, c.width, true));
      th.appendChild(h);
      tr.appendChild(th);
      return th;
    });
    thead.replaceChildren(tr);
    this._paint();
    this._fitWidth();
  }

  // ascending -> descending -> the table's default order
  _cycle(c) {
    const s = this.sortState;
    if (!s || s.key !== c.key) this.sortState = { key: c.key, dir: 'asc' };
    else if (s.dir === 'asc') this.sortState = { key: c.key, dir: 'desc' };
    else this.sortState = null;
    this._persist();
    this._paint();
    this.onChange();
  }

  _paint() {
    const s = this.activeSort;
    for (const th of this._heads) {
      const on = s && th.dataset.key === s.key;
      th.setAttribute('aria-sort', on ? (s.dir === 'desc' ? 'descending' : 'ascending') : 'none');
      const a = th.querySelector('.col-arrow');
      if (a) a.textContent = on ? (s.dir === 'desc' ? ' ▾' : ' ▴') : '';
      th.classList.toggle('is-default-sort', !!on && !this.sortState);
    }
  }

  // A fixed-layout table must be told its width, or the columns squeeze to
  // fit the window instead of keeping the widths asked for.
  _fitWidth() {
    const total = this.columns.reduce((n, c) => n + (this.widths[c.key] || c.width), 0);
    this.table.style.width = `${total}px`;
    this.table.style.minWidth = '100%';
  }

  _setWidth(c, i, w, persist) {
    const px = Math.max(MIN_W, Math.round(w));
    if (px === c.width) delete this.widths[c.key]; else this.widths[c.key] = px;
    this._cols[i].style.width = `${px}px`;
    this._fitWidth();
    if (persist) this._persist();
  }

  _drag(e, c, i) {
    e.preventDefault();
    e.stopPropagation();
    const h = e.currentTarget;
    h.setPointerCapture(e.pointerId);
    const x0 = e.clientX;
    const w0 = this.widths[c.key] || c.width;
    this.table.classList.add('is-resizing');
    const move = (ev) => this._setWidth(c, i, w0 + ev.clientX - x0, false);
    const up = () => {
      h.removeEventListener('pointermove', move);
      h.removeEventListener('pointerup', up);
      h.removeEventListener('pointercancel', up);
      this.table.classList.remove('is-resizing');
      this._persist();
    };
    h.addEventListener('pointermove', move);
    h.addEventListener('pointerup', up);
    h.addEventListener('pointercancel', up);
  }
}
