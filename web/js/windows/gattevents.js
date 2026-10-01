// GATT Events: every value read, notified or indicated, every write, and
// every failure, from the app-wide log (js/gattlog.js).
//
// The same view serves twice: as its own window ("GATT Events", tiled next
// to GATT like any window), and embedded inside the GATT window in tabbed
// mode, where one window at a time would otherwise mean never seeing the
// events while working the characteristics. A value is shown in the format
// chosen for its characteristic in the GATT window.

import { Grid } from '../controls/grid.js';
import { formatValue, defaultFormat, formatKey, uuidDisplay } from '../gattfmt.js';
import { copyText } from '../clipboard.js';

const time = (t) => new Date(t).toTimeString().slice(0, 8);

export class GattEventsView {
  // opts.embedded: inside the GATT window (follows its device; no window chrome)
  constructor(el, ctx, opts = {}) {
    this.app = ctx.app;
    this.log = ctx.app.gattLog;
    this.store = ctx.app.devices;
    this.embedded = !!opts.embedded;
    this.device = '';               // '' = every device
    this._dirty = true;

    el.classList.add('gatt-events');
    el.innerHTML = `
      ${this.embedded ? '' : `<div class="pane-bar ge-bar">
        <select class="ge-dev" aria-label="Device"><option value="">All devices</option></select>
        <span class="spacer"></span>
        <span class="dim ge-count"></span>
        <button type="button" class="pane-btn ge-clear">Clear</button>
      </div>`}
      <div class="table-wrap ge-wrap"><table class="grid ge-table"><thead></thead><tbody></tbody></table></div>`;
    this._tbody = el.querySelector('tbody');
    this._count = el.querySelector('.ge-count');
    this._dev = el.querySelector('.ge-dev');
    this._dev?.addEventListener('change', () => { this.device = this._dev.value; this._dirty = true; });
    el.querySelector('.ge-clear')?.addEventListener('click', () => this.log.clear());

    const fmtOf = (e) => this._format(e);
    this.columns = [
      { key: 'time', label: 'Time', width: 70, type: 'num', get: (e) => e.t, show: (e) => time(e.t) },
      { key: 'dev', label: 'Device', width: 120, get: (e) => this._devName(e.addr) },
      { key: 'chr', label: 'Characteristic', width: 170, get: (e) => this._chrName(e) },
      { key: 'src', label: 'Event', width: 64, get: (e) => e.src },
      { key: 'len', label: 'Len', width: 40, type: 'num', get: (e) => (e.src === 'error' ? null : e.len) },
      { key: 'value', label: 'Value', width: 260, get: (e) => (e.src === 'error' ? e.note : formatValue(e.hex, fmtOf(e))) },
    ];
    this.grid = new Grid(el.querySelector('.ge-table'), {
      id: 'gatt-events', columns: this.columns, defaultSort: { key: 'time', dir: 'desc' },
      onChange: () => { this._dirty = true; },
    });
    this._unsub = this.log.subscribe(() => { this._dirty = true; });
    this.store.subscribe(() => { this._dirty = true; });
  }

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
    const rows = this.grid.sort(this.log.list(this.device));
    if (this._count) this._count.textContent = `${rows.length} event${rows.length === 1 ? '' : 's'}`;
    this._rows = rows.slice(0, 300);
    const vis = this.grid.visible;
    this._tbody.replaceChildren(...this._rows.map((e) => {
      const tr = document.createElement('tr');
      if (e.src === 'error') tr.classList.add('is-error');
      for (const c of vis) {
        const td = document.createElement('td');
        const v = c.show ? c.show(e) : c.get(e);
        td.textContent = v ?? '';
        if (c.key === 'value' || c.key === 'chr') td.title = td.textContent;
        tr.appendChild(td);
      }
      return this.grid.appendSpacer(tr);
    }));
    this.grid.paintCursor();
  }

  // The table is a keyboard list like every grid: h/j/k/l between cells, a/d
  // to sort, Shift+H/L to move or resize a column; Enter copies the cell.
  attachKeys(keys) {
    const wrap = this.grid.table.closest('.table-wrap');
    keys.nav.registerList(wrap, this.grid.controller({
      list: wrap,
      count: () => (this._rows || []).length,
      activate: (i, key, td) => { if (td) copyText(td.textContent); },
      onChange: () => { this._dirty = true; },
    }));
  }

  destroy() { this._unsub?.(); }
}

// The window: the view, standing alone.
export class GattEventsWindow {
  constructor(el, ctx) {
    this.view = new GattEventsView(el, ctx);
  }
  attachKeys(keys) { this.view.attachKeys(keys); }
  onPrefs() { this.view.onPrefs(); }
  onShow() { this.view.invalidate(); }
  render() { this.view.render(); }
}
