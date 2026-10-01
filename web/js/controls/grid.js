// Tables whose columns you can sort, resize, reorder and hide -- with the
// mouse, a finger, or the keyboard -- remembered per table in the browser
// like the window arrangement. The rules are in controls/gridmodel.js.
//
// The windows draw their tables from data every frame, so sorting belongs to
// the DATA, not the DOM: a window asks grid.sort(rows) for the order and draws
// grid.visible's columns. The header is a row of buttons (click or Enter
// sorts: ascending, descending, back to the default; drag one sideways to
// move the column) with a resize handle on each right edge (double-click
// resets). A trailing spacer column takes whatever width is left, so every
// other column keeps exactly the width asked for, left-aligned, instead of
// being stretched to fill.
//
// Keyboard (grid.controller -> a nav.js list): h/j/k/l and arrows move a cell
// cursor, row 0 being the header; a / d sort the cursor's column ascending /
// descending; on the header Shift+H/L moves the column, on a row it narrows /
// widens it; Enter/Space sorts (header) or is the window's (a row).
//
// Column spec: { key, label, type: 'num'|'text', width, get(row), show?(row),
//                sortable?, pinned?, fixed?, title? }.

import { readPrefs, writePrefs } from '../prefs.js';
import { sortRows, visibleColumns, columnOrder, moveColumn, placeColumn, setWidth, widthOf, nextSort, hiddenKeys } from './gridmodel.js';

export { compareValues, sortRows } from './gridmodel.js';

const STEP = 8;                  // px per Shift+H / Shift+L
const DRAG_PX = 6;               // a press that moves less than this is a click

function loadState(id) {
  const t = readPrefs().tables;
  const st = (t && typeof t === 'object' && t[id]) || {};
  // hidden stays null until chosen, so a column declared hidden starts hidden.
  return { widths: st.widths || {}, sort: st.sort || null, order: st.order || [], hidden: Array.isArray(st.hidden) ? st.hidden : null };
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
    this.state = loadState(id);
    if (this.state.sort && !columns.some((c) => c.key === this.state.sort.key)) this.state.sort = null;
    this.cursor = { row: -1, col: 0 };
    this._build();
  }

  get visible() { return visibleColumns(this.columns, this.state); }
  get activeSort() { return this.state.sort || this.defaultSort; }
  isHidden(key) { return hiddenKeys(this.columns, this.state).includes(key); }

  sort(rows) {
    const s = this.activeSort;
    const col = s && this.columns.find((c) => c.key === s.key);
    return col ? sortRows(rows, col, s.dir === 'desc' ? -1 : 1) : rows;
  }

  // --- changes (each persisted, the header rebuilt, the window told) ---------

  _commit() {
    saveState(this.id, this.state);
    this._build();
    this.onChange();
  }

  setSort(key, dir) {
    const col = this.columns.find((c) => c.key === key);
    if (!col || col.sortable === false) return;
    this.state.sort = dir ? { key, dir } : null;
    this._commit();
  }

  cycleSort(key) {
    const col = this.columns.find((c) => c.key === key);
    if (!col || col.sortable === false) return;
    this.state.sort = nextSort(this.state.sort, key);
    this._commit();
  }

  move(key, delta) {
    this.state.order = moveColumn(this.columns, this.state, key, delta);
    this._commit();
  }

  place(key, beforeKey) {
    this.state.order = placeColumn(this.columns, this.state, key, beforeKey);
    this._commit();
  }

  resize(key, deltaPx) {
    const col = this.columns.find((c) => c.key === key);
    if (!col) return;
    this.state.widths = setWidth(this.columns, this.state, key, widthOf(col, this.state) + deltaPx);
    this._commit();
  }

  setHidden(key, hidden) {
    const set = new Set(hiddenKeys(this.columns, this.state));
    if (hidden) set.add(key); else set.delete(key);
    this.state.hidden = [...set];
    this.state.order = columnOrder(this.columns, this.state.order);   // keep a hidden one's place
    this._commit();
  }

  reset() {
    this.state = { widths: {}, sort: null, order: [], hidden: null };
    this._commit();
  }

  // A data row's trailing cell, under the spacer: windows append it so the row
  // reads to the edge (borders, hover) like the header does.
  appendSpacer(tr) {
    const td = document.createElement('td');
    td.className = 'col-spacer';
    tr.appendChild(td);
    return tr;
  }

  // --- the header ---------------------------------------------------------------

  _build() {
    const t = this.table;
    t.classList.add('grid-resizable');
    const vis = this.visible;
    t.querySelector('colgroup')?.remove();
    const cg = document.createElement('colgroup');
    this._cols = vis.map((c) => {
      const col = document.createElement('col');
      col.style.width = `${widthOf(c, this.state)}px`;
      cg.appendChild(col);
      return col;
    });
    cg.appendChild(document.createElement('col'));     // the spacer: auto
    let thead = t.querySelector('thead');
    if (!thead) { thead = document.createElement('thead'); t.prepend(thead); }
    t.prepend(cg);

    const tr = document.createElement('tr');
    this._heads = vis.map((c, i) => {
      const th = document.createElement('th');
      th.dataset.key = c.key;
      if (c.title) th.title = c.title;
      if (c.sortable === false) {
        const s = document.createElement('span');
        s.className = 'col-label';
        s.textContent = c.label;
        th.appendChild(s);
      } else {
        const b = document.createElement('button');
        b.type = 'button';
        b.className = 'col-sort';
        b.tabIndex = -1;                         // the table is the keyboard stop
        b.innerHTML = '<span class="col-label"></span><span class="col-arrow" aria-hidden="true"></span>';
        b.querySelector('.col-label').textContent = c.label;
        b.addEventListener('click', () => {
          if (this._suppressClick) { this._suppressClick = false; return; }
          this.cycleSort(c.key);
        });
        if (!c.pinned) b.addEventListener('pointerdown', (e) => this._dragStart(e, c, b));
        th.appendChild(b);
      }
      const h = document.createElement('span');
      h.className = 'col-resize';
      h.title = 'Drag to resize; double-click to reset';
      h.addEventListener('pointerdown', (e) => this._resizeDrag(e, c, i));
      h.addEventListener('dblclick', () => { this.state.widths = setWidth(this.columns, this.state, c.key, c.width); this._commit(); });
      th.appendChild(h);
      tr.appendChild(th);
      return th;
    });
    const spacer = document.createElement('th');
    spacer.className = 'col-spacer';
    tr.appendChild(spacer);
    thead.replaceChildren(tr);
    this._paintSort();
  }

  _paintSort() {
    const s = this.activeSort;
    for (const th of this._heads) {
      const on = s && th.dataset.key === s.key;
      th.setAttribute('aria-sort', on ? (s.dir === 'desc' ? 'descending' : 'ascending') : 'none');
      const a = th.querySelector('.col-arrow');
      if (a) a.textContent = on ? (s.dir === 'desc' ? ' ▾' : ' ▴') : '';
      th.classList.toggle('is-default-sort', !!on && !this.state.sort);
    }
  }

  // Resize by dragging a heading's right edge: live while dragging (just the
  // <col>), committed on release.
  _resizeDrag(e, c, i) {
    e.preventDefault();
    e.stopPropagation();
    const h = e.currentTarget;
    h.setPointerCapture?.(e.pointerId);
    const x0 = e.clientX;
    const w0 = widthOf(c, this.state);
    let w = w0;
    this.table.classList.add('is-resizing');
    const move = (ev) => {
      w = setWidth(this.columns, this.state, c.key, w0 + ev.clientX - x0)[c.key] ?? c.width;
      this._cols[i].style.width = `${w}px`;
    };
    const up = () => {
      h.removeEventListener('pointermove', move);
      h.removeEventListener('pointerup', up);
      h.removeEventListener('pointercancel', up);
      this.table.classList.remove('is-resizing');
      this.state.widths = setWidth(this.columns, this.state, c.key, w);
      this._commit();
    };
    h.addEventListener('pointermove', move);
    h.addEventListener('pointerup', up);
    h.addEventListener('pointercancel', up);
  }

  // Reorder by dragging a heading sideways. Under DRAG_PX it stays a click
  // (a sort); past it, a marker shows where the column will land.
  _dragStart(e, c, b) {
    if (e.button !== 0) return;
    const x0 = e.clientX;
    let dragging = false;
    let before = undefined;
    const marks = () => this._heads.forEach((th) => th.classList.remove('drop-before', 'drop-after'));
    const move = (ev) => {
      if (!dragging && Math.abs(ev.clientX - x0) < DRAG_PX) return;
      if (!dragging) { dragging = true; b.setPointerCapture?.(ev.pointerId); this.table.classList.add('is-dragging-col'); }
      ev.preventDefault();
      marks();
      const cands = this._heads.filter((th) => !this.columns.find((k) => k.key === th.dataset.key)?.pinned);
      const hit = cands.find((th) => { const r = th.getBoundingClientRect(); return ev.clientX < r.left + r.width / 2; });
      before = hit ? hit.dataset.key : null;
      if (hit) hit.classList.add('drop-before'); else cands[cands.length - 1]?.classList.add('drop-after');
    };
    const up = () => {
      window.removeEventListener('pointermove', move, true);
      window.removeEventListener('pointerup', up, true);
      window.removeEventListener('pointercancel', up, true);
      marks();
      this.table.classList.remove('is-dragging-col');
      if (!dragging) return;
      this._suppressClick = true;           // the click that follows a drag is not a sort
      setTimeout(() => { this._suppressClick = false; }, 0);
      if (before !== c.key) this.place(c.key, before);
    };
    window.addEventListener('pointermove', move, true);
    window.addEventListener('pointerup', up, true);
    window.addEventListener('pointercancel', up, true);
  }

  // --- the keyboard cursor --------------------------------------------------------

  // A nav.js list controller over this table. `opts`:
  //   list        the focusable element registered as the list
  //   count()     the number of data rows
  //   activate(i, key, td)   Enter/Space on data row i (0-based), column key
  //   onCursor(i) the cursor moved to data row i (-1: the header)
  controller(opts) {
    this._list = opts.list;
    const n = () => opts.count();
    const set = (row, col) => {
      const vis = this.visible;
      this.cursor = { row: Math.max(0, Math.min(n(), row)), col: Math.max(0, Math.min(vis.length - 1, col)) };
      this._scrollCursor = true;
      opts.onCursor?.(this.cursor.row - 1);
      opts.onChange?.();
      return true;
    };
    const key = () => this.visible[this.cursor.col]?.key;
    return {
      enter: () => (this.cursor.row < 0 ? set(n() ? 1 : 0, this.visible.findIndex((c) => !c.pinned) >= 0 ? 0 : 0) : set(this.cursor.row, this.cursor.col)),
      move: (d) => set(this.cursor.row + d, this.cursor.col),
      first: () => set(n() ? 1 : 0, this.cursor.col),
      last: () => set(n(), this.cursor.col),
      count: () => n() + 1,                  // the header is a row too
      activate: () => {
        if (this.cursor.row === 0) { this.cycleSort(key()); return; }
        const td = this._cursorCell();
        opts.activate?.(this.cursor.row - 1, key(), td);
      },
      key: (e) => {
        if (e.altKey || e.ctrlKey || e.metaKey) return false;
        const dir = { KeyH: -1, ArrowLeft: -1, KeyL: 1, ArrowRight: 1 }[e.code];
        if (dir && !e.shiftKey) return set(this.cursor.row, this.cursor.col + dir);
        if (dir && e.shiftKey) {
          const k = key();
          if (!k) return true;
          if (this.cursor.row === 0) {
            this.move(k, dir);
            set(0, this.visible.findIndex((c) => c.key === k));
          } else {
            this.resize(k, dir * STEP);
          }
          return true;
        }
        if (!e.shiftKey && (e.code === 'KeyA' || e.code === 'KeyD')) {
          this.setSort(key(), e.code === 'KeyA' ? 'asc' : 'desc');
          return true;
        }
        return false;
      },
    };
  }

  _cursorCell() {
    const { row, col } = this.cursor;
    if (row === 0) return this._heads[col] || null;
    const tr = this.table.tBodies[0]?.rows[row - 1];
    return tr?.cells[col] || null;
  }

  // Called by the window after drawing its rows: marks the cursor's cell (only
  // while the table has the keyboard) and keeps it on screen.
  paintCursor() {
    this.table.querySelectorAll('.is-cursor').forEach((el) => el.classList.remove('is-cursor'));
    const active = this._list?.hasAttribute('data-nav-active');
    if (!active || this.cursor.row < 0) return;
    const vis = this.visible;
    if (this.cursor.col >= vis.length) this.cursor.col = vis.length - 1;
    const cell = this._cursorCell();
    if (!cell) return;
    cell.classList.add('is-cursor');
    if (this._scrollCursor) {
      this._scrollCursor = false;
      cell.scrollIntoView({ block: 'nearest', inline: 'nearest' });
    }
  }
}
