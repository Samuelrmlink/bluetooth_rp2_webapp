// GATT Events: every value read, notified or indicated, every write, and
// every failure, from the app-wide log (js/gattlog.js).
//
// The same view serves twice: as its own window ("GATT Events", tiled next
// to GATT like any window), and embedded inside the GATT window in tabbed
// mode. A value is shown in the format chosen for its characteristic in the
// GATT window.
//
// The table behaves exactly as the Scanner's -- the same grid
// (controls/grid.js), Columns dialog (controls/columnsdialog.js) and cell
// menu (controls/cellmenu.js) -- plus a search: `/` opens it, matches are
// marked, n / Shift+N step through them, and "Show only matching rows"
// turns it into a filter.

import { Grid } from '../controls/grid.js';
import { Dialog } from '../modals/dialog.js';
import { columnsDialog } from '../controls/columnsdialog.js';
import { openCellMenu, copyWithToast } from '../controls/cellmenu.js';
import { findMatches, matchCells, stepMatch, highlightHTML, cellText } from '../controls/gridsearch.js';
import { formatValue, defaultFormat, formatKey, uuidDisplay } from '../gattfmt.js';
import { toast } from '../toast.js';

const time = (t) => new Date(t).toTimeString().slice(0, 8);
const ROWS_MAX = 300;
let instances = 0;

export class GattEventsView {
  // opts.embedded: inside the GATT window (follows its device)
  constructor(el, ctx, opts = {}) {
    this.app = ctx.app;
    this.log = ctx.app.gattLog;
    this.store = ctx.app.devices;
    this.embedded = !!opts.embedded;
    this.device = '';               // '' = every device
    this.search = { q: '', only: false };
    this._cur = null;               // the current match: { row, col } in the rows shown
    this._rows = [];
    this._dirty = true;
    const n = ++instances;

    el.classList.add('gatt-events');
    el.innerHTML = `
      <div class="pane-bar ge-bar">
        ${this.embedded ? '' : '<select class="ge-dev" aria-label="Device"><option value="">All devices</option></select>'}
        <button type="button" class="pane-btn ge-search-btn" title="Search (/)">Search</button>
        <button type="button" class="pane-btn ge-cols-btn" title="Columns (c)">Columns</button>
        <span class="spacer"></span>
        <span class="dim ge-count"></span>
        ${this.embedded ? '' : '<button type="button" class="pane-btn ge-clear">Clear</button>'}
      </div>
      <div class="table-wrap ge-wrap"><table class="grid ge-table"><thead></thead><tbody></tbody></table></div>
      <div class="pane-settings s-menu" hidden></div>`;
    this._tbody = el.querySelector('tbody');
    this._wrap = el.querySelector('.ge-wrap');
    this._count = el.querySelector('.ge-count');
    this._dev = el.querySelector('.ge-dev');
    this._searchBtn = el.querySelector('.ge-search-btn');
    this._menu = el.querySelector('.s-menu');
    this._dev?.addEventListener('change', () => { this.device = this._dev.value; this._dirty = true; });
    el.querySelector('.ge-clear')?.addEventListener('click', () => this.log.clear());
    this._searchBtn.addEventListener('click', () => this.openSearch());
    el.querySelector('.ge-cols-btn').addEventListener('click', () => this.columnsDlg.toggle());
    this._tbody.addEventListener('click', (e) => this._onClick(e));

    const fmtOf = (e) => this._format(e);
    this.columns = [
      { key: 'time', label: 'Time', width: 70, type: 'num', get: (e) => e.t, show: (e) => time(e.t) },
      { key: 'dev', label: 'Device', width: 120, get: (e) => this._devName(e.addr) },
      { key: 'addr', label: 'Address', width: 140, hidden: true, get: (e) => e.addr || '' },
      { key: 'svc', label: 'Service', width: 150, hidden: true, get: (e) => { const s = this._svcOf(e); return s ? uuidDisplay(s.uuid, !!this.app.prefs.gattUuid128).text : ''; } },
      { key: 'chr', label: 'Characteristic', width: 170, get: (e) => this._chrName(e) },
      { key: 'uuid', label: 'Characteristic UUID', width: 280, hidden: true, get: (e) => e.uuid || '' },
      { key: 'id', label: 'Handle path', width: 70, hidden: true, get: (e) => e.id || '' },
      { key: 'src', label: 'Event', width: 64, get: (e) => e.src },
      { key: 'len', label: 'Len', width: 40, type: 'num', get: (e) => (e.src === 'error' ? null : e.len) },
      { key: 'value', label: 'Value', width: 260, get: (e) => (e.src === 'error' ? e.note : formatValue(e.hex, fmtOf(e))) },
      { key: 'hex', label: 'Raw hex', width: 220, hidden: true, get: (e) => e.hex || '' },
      { key: 'note', label: 'Note', width: 160, hidden: true, get: (e) => e.note || '' },
    ];
    this.grid = new Grid(el.querySelector('.ge-table'), {
      id: 'gatt-events', columns: this.columns, defaultSort: { key: 'time', dir: 'desc' },
      onChange: () => { this._dirty = true; this.columnsDlg?.sync(); },
    });
    this.columnsDlg = columnsDialog(this.app, this.grid, { id: `gatt-events-columns-${n}` });
    this._buildSearch(n);
    this._unsub = this.log.subscribe(() => { this._dirty = true; });
    this.store.subscribe(() => { this._dirty = true; });
  }

  // --- the search ---------------------------------------------------------------------

  _buildSearch(n) {
    const body = document.createElement('div');
    body.className = 'scan-dlg ge-search';
    body.innerHTML = `
      <label class="sd-row"><span>Find in the events <b class="gs-count"></b></span>
        <input class="gs-q" type="search" spellcheck="false" autocomplete="off" placeholder="text in any column shown"></label>
      <div class="sd-actions">
        <button type="button" class="pane-btn gs-prev" title="Previous match (Shift+N, or Shift+Enter in the box)">◀ Prev</button>
        <button type="button" class="pane-btn gs-next" title="Next match (n, or Enter in the box)">Next ▶</button>
        <button type="button" class="pane-btn gs-clear">Clear</button>
      </div>
      <label class="sd-check"><input type="checkbox" class="gs-only"> Show only matching rows</label>`;
    const q = (s) => body.querySelector(s);
    this._q = q('.gs-q');
    this._only = q('.gs-only');
    this._sCount = q('.gs-count');
    this._q.addEventListener('input', () => { this.search.q = this._q.value; this._cur = null; this._jump = true; this._dirty = true; });
    this._q.addEventListener('keydown', (e) => {
      if (e.key === 'Enter') { e.preventDefault(); this.step(e.shiftKey ? -1 : 1); }
    });
    this._only.addEventListener('change', () => { this.search.only = this._only.checked; this._cur = null; this._dirty = true; });
    q('.gs-prev').addEventListener('click', () => this.step(-1));
    q('.gs-next').addEventListener('click', () => this.step(1));
    q('.gs-clear').addEventListener('click', () => {
      this.search = { q: '', only: false };
      this._q.value = '';
      this._only.checked = false;
      this._cur = null;
      this._dirty = true;
      this._q.focus();
    });
    this.searchDlg = this.app.registerDialog(new Dialog({ id: `gatt-events-search-${n}`, title: 'Search events', body }));
  }

  openSearch() {
    this.searchDlg.open();
    this._q.focus({ preventScroll: true });
    this._q.select();
  }

  // n / Shift+N, the dialog's buttons, Enter in the box: the next or previous
  // matching cell, wrapping. In the table, the cell cursor goes there too.
  step(dir) {
    if (!this.search.q) { toast('No search — press / to search'); return; }
    const cells = this._cells || [];
    if (!cells.length) { toast(`Not found: ${this.search.q}`); return; }
    const inTable = this._wrap.hasAttribute('data-nav-active') && this.grid.cursor.row > 0;
    const from = inTable ? { row: this.grid.cursor.row - 1, col: this.grid.cursor.col } : this._cur;
    const i = stepMatch(cells, from, dir);
    const c = cells[i];
    this._cur = { row: c.row, col: c.col };
    if (this._wrap.hasAttribute('data-nav-active')) {
      this.grid.cursor = { row: c.row + 1, col: c.col };
      this.grid._scrollCursor = true;
    }
    this._scrollMatch = true;
    this._dirty = true;
  }

  // --- keys ---------------------------------------------------------------------------

  attachKeys(keys) {
    keys.nav.registerList(this._wrap, this.grid.controller({
      list: this._wrap,
      count: () => this._rows.length,
      activate: (i, key, td) => this._openMenu(this._rows[i], key, td),
      onChange: () => { this._dirty = true; },
    }));
  }

  // The banner follows the cursor, as in the Scanner.
  context() {
    if (this._wrap.hasAttribute('data-nav-active')) return this.grid.cursor.row === 0 ? 'events-head' : 'events-table';
    return 'events';
  }

  action(id) {
    if (id === 'events-search') { this.openSearch(); return true; }
    if (id === 'events-columns') { this.columnsDlg.open(); return true; }
    if (id === 'events-next') { this.step(1); return true; }
    if (id === 'events-prev') { this.step(-1); return true; }
    return false;
  }

  // --- the cell menu -------------------------------------------------------------------

  _onClick(e) {
    const tr = e.target.closest('tr');
    const td = e.target.closest('td');
    if (!tr || !td) return;
    const i = [...this._tbody.rows].indexOf(tr);
    const col = [...tr.cells].indexOf(td);
    if (i < 0 || col < 0 || col >= this.grid.visible.length) return;
    this.grid.cursor = { row: i + 1, col };
    this._dirty = true;
    this._openMenu(this._rows[i], this.grid.visible[col].key, td);
  }

  _openMenu(e, key, anchor) {
    if (!e || !anchor) return;
    const col = this.columns.find((c) => c.key === key);
    const value = col ? cellText(e, col) : '';
    const items = [
      { label: 'Copy row', fn: () => copyWithToast(this.grid.visible.map((c) => cellText(e, c)).join('\t'), 'Row copied') },
    ];
    if (value) {
      items.push({
        label: 'Search for this', fn: () => {
          this.search.q = value;
          this._q.value = value;
          this._cur = null;
          this._jump = true;
          this._dirty = true;
          toast(`Searching for "${value.length > 30 ? `${value.slice(0, 30)}…` : value}"`);
        },
      });
    }
    if (e.addr && this.store.get(e.addr)?.connected) items.push({ label: 'Show in GATT', fn: () => this.app.showDevice(e.addr) });
    const opened = openCellMenu(this.app, {
      anchor, menuEl: this._menu, paneId: 'events-cell', wrap: this._wrap,
      title: `${time(e.t)} · ${this._chrName(e) || e.addr || ''}`,
      items, value, onReturn: () => { this._dirty = true; },
      onClose: () => { this._menuCell = null; this._dirty = true; },
    });
    // The cell the menu is for stays marked while it is open.
    if (opened) { this._menuCell = { entry: e, key }; anchor.classList.add('is-menu-cell'); this._dirty = true; }
  }

  // --- data ----------------------------------------------------------------------------

  // From the GATT window: which device the embedded view follows.
  follow(addr) {
    if (this.embedded && addr !== this.device) { this.device = addr; this._dirty = true; }
  }

  // Formats changed in the GATT window.
  onPrefs() { this._dirty = true; }
  invalidate() { this._dirty = true; }

  _devName(addr) {
    const d = this.store.get(addr);
    return d?.name ? `${d.name}` : addr || '';
  }

  // Service and characteristic of a log entry, from the device's tree: the
  // events carry the characteristic's path id ("2.0") and UUID only.
  _svcOf(e) {
    const g = this.store.get(e.addr)?.gatt;
    const sid = String(e.id || '').split('.')[0];
    return g?.services?.find((s) => s.id === sid) || null;
  }

  _chrName(e) {
    if (!e.uuid) return e.id ? `chr ${e.id}` : '';
    return uuidDisplay(e.uuid, !!this.app.prefs.gattUuid128).text;
  }

  _format(e) {
    const svc = this._svcOf(e);
    const map = this.app.prefs.gattFormats || {};
    return (svc && e.uuid && map[formatKey(svc.uuid, e.uuid)]) || defaultFormat(e.uuid || '');
  }

  render() {
    if (!this._dirty) return;
    this._dirty = false;
    if (this._dev) {
      const keep = this.device;
      const opts = [['', 'All devices'], ...this.store.list().filter((d) => d.connected || this.log.items.some((i) => i.addr === d.addr))
        .map((d) => [d.addr, d.name ? `${d.name} · ${d.addr}` : d.addr])];
      if (this._dev.options.length !== opts.length || [...this._dev.options].some((o, i) => o.value !== opts[i][0])) {
        this._dev.replaceChildren(...opts.map(([v, t]) => { const o = document.createElement('option'); o.value = v; o.textContent = t; return o; }));
      }
      this._dev.value = keep;
    }
    const vis = this.grid.visible;
    let rows = this.grid.sort(this.log.list(this.device)).slice(0, ROWS_MAX);
    // The search: matches over what the visible columns show. "Only matching"
    // drops the rest before the cells are numbered.
    if (this.search.q && this.search.only) {
      const hit = new Set(findMatches(rows, vis, this.search.q).map((m) => m.row));
      rows = rows.filter((_, i) => hit.has(i));
    }
    this._rows = rows;
    const cells = this.search.q ? matchCells(findMatches(rows, vis, this.search.q)) : [];
    this._cells = cells;
    if (this._jump && cells.length) { this._jump = false; this._cur = { row: cells[0].row, col: cells[0].col }; this._scrollMatch = true; }
    const byCell = new Map(cells.map((c) => [`${c.row}:${c.col}`, c.ranges]));
    const curIdx = this._cur ? cells.findIndex((c) => c.row === this._cur.row && c.col === this._cur.col) : -1;

    const total = this.log.list(this.device).length;
    this._count.textContent = `${rows.length === total ? total : `${rows.length} of ${total}`} event${total === 1 ? '' : 's'}`;
    this._searchBtn.textContent = this.search.q ? `Search (${curIdx >= 0 ? curIdx + 1 : 0}/${cells.length})` : 'Search';
    this._searchBtn.classList.toggle('on', !!this.search.q);
    if (this._sCount) this._sCount.textContent = this.search.q ? `${curIdx >= 0 ? curIdx + 1 : 0} of ${cells.length}` : '';

    let curTd = null;
    this._tbody.replaceChildren(...rows.map((e, i) => {
      const tr = document.createElement('tr');
      if (e.src === 'error') tr.classList.add('is-error');
      vis.forEach((c, j) => {
        const td = document.createElement('td');
        const text = cellText(e, c);
        const ranges = byCell.get(`${i}:${j}`);
        if (ranges) {
          td.innerHTML = highlightHTML(text, ranges);
          td.classList.add('is-match');
          if (this._cur && this._cur.row === i && this._cur.col === j) { td.classList.add('is-match-cur'); curTd = td; }
        } else {
          td.textContent = text;
        }
        if (c.key === 'value' || c.key === 'chr' || c.key === 'uuid' || c.key === 'hex') td.title = text;
        if (this._menuCell && this._menuCell.entry === e && this._menuCell.key === c.key) td.classList.add('is-menu-cell');
        tr.appendChild(td);
      });
      return this.grid.appendSpacer(tr);
    }));
    this.grid.paintCursor();
    if (this._scrollMatch && curTd) { this._scrollMatch = false; curTd.scrollIntoView({ block: 'nearest', inline: 'nearest' }); }
  }

  destroy() { this._unsub?.(); }
}

// The window: the view, standing alone.
export class GattEventsWindow {
  constructor(el, ctx) {
    this.view = new GattEventsView(el, ctx);
  }
  attachKeys(keys) { this.view.attachKeys(keys); }
  context() { return this.view.context(); }
  action(id) { return this.view.action(id); }
  onPrefs() { this.view.onPrefs(); }
  onShow() { this.view.invalidate(); }
  render() { this.view.render(); }
}
