// The Scanner window: what the radio hears, and a way to connect to it.
//
// Rows come from the shared DeviceStore (gattdb.js), which merges
// advertisements -- a later ad that omits the name does not erase it -- and
// which the GATT window reads too, so the two can never disagree about a
// device. The table is a keyboard list: Space/Enter enters it, j/k move,
// Enter connects or disconnects, l shows the device in the GATT window.
//
// The filter boxes filter what has been HEARD, in the browser, as you type:
// they work with the scan stopped, and clearing one brings back everything
// it hid. (They used to be scan.start parameters, filtering on the radio:
// only applied when a scan started, and what they dropped was never heard.)
// Alt+N / Alt+Shift+N step between the boxes while one has focus; `/` jumps
// to the first.

import { shortUuid } from '../gattdb.js';
import { Grid } from '../controls/grid.js';

const lc = (s) => String(s || '').toLowerCase();

// The filter as a predicate over device records. Empty boxes match all.
//   name     part of the name or the address, any case
//   mfg      manufacturer data starting with these hex bytes (spaces ignored)
//   service  part of any advertised service UUID (short or full form)
//   rssi     RSSI at least this (dBm)
export function scanFilter({ name = '', mfg = '', service = '', rssi = '' } = {}) {
  const n = lc(name).trim();
  const m = lc(mfg).replace(/[\s:]/g, '');
  const u = lc(service).trim();
  const r = String(rssi).trim() === '' ? null : Number(rssi);
  return (d) => {
    if (n && !lc(d.name).includes(n) && !lc(d.addr).includes(n)) return false;
    if (m && !lc(d.mfg_data).startsWith(m)) return false;
    if (u && !(d.service_uuids || []).some((x) => lc(x).includes(u) || shortUuid(x).includes(u))) return false;
    if (r != null && Number.isFinite(r) && !((d.rssi ?? -127) >= r)) return false;
    return true;
  };
}

const COLUMNS = [
  { key: 'action', label: '', width: 112, sortable: false },
  { key: 'addr', label: 'Address', width: 150, get: (d) => d.addr },
  { key: 'name', label: 'Name', width: 170, get: (d) => d.name },
  { key: 'rssi', label: 'RSSI', width: 64, type: 'num', get: (d) => d.rssi },
  { key: 'services', label: 'Services', width: 140, get: (d) => (d.service_uuids || []).map(shortUuid).join(' ') },
  { key: 'mfg', label: 'Manufacturer data', width: 280, get: (d) => d.mfg_data },
];

export class ScannerWindow {
  constructor(el, ctx) {
    this.app = ctx.app;
    this.ws = ctx.ws;
    this.store = ctx.app.devices;
    this.scanning = false;
    this.sel = null;              // selected address -- survives re-sorting
    this._dirty = true;

    el.innerHTML = `
      <div class="pane-bar scan-bar">
        <button type="button" class="pane-btn s-toggle">Scan</button>
        <input class="s-filter s-name" type="search" placeholder="name / address" title="Filter: part of the name or address" spellcheck="false" autocomplete="off" size="12">
        <input class="s-filter s-mfg" type="search" placeholder="mfg hex" title="Filter: manufacturer data starting with these bytes" spellcheck="false" autocomplete="off" size="8">
        <input class="s-filter s-uuid" type="search" placeholder="service" title="Filter: part of a service UUID" spellcheck="false" autocomplete="off" size="6">
        <input class="s-filter s-rssi" type="number" placeholder="rssi ≥" title="Filter: RSSI at least (dBm)" min="-127" max="0" step="1">
        <label class="dim" title="Active scanning asks for scan responses (names, more UUIDs)"><input type="checkbox" class="s-active" checked> active</label>
        <span class="spacer"></span>
        <span class="dim s-count"></span>
      </div>
      <div class="table-wrap s-wrap">
        <table class="grid s-table"><thead></thead><tbody></tbody></table>
      </div>`;
    this._toggle = el.querySelector('.s-toggle');
    this._count = el.querySelector('.s-count');
    this._wrap = el.querySelector('.s-wrap');
    this._tbody = el.querySelector('tbody');
    this._f = {
      name: el.querySelector('.s-name'), mfg: el.querySelector('.s-mfg'),
      uuid: el.querySelector('.s-uuid'), rssi: el.querySelector('.s-rssi'),
      active: el.querySelector('.s-active'),
    };
    this._filters = [...el.querySelectorAll('.s-filter')];
    for (const f of this._filters) f.addEventListener('input', () => { this._dirty = true; });
    this.grid = new Grid(el.querySelector('.s-table'), {
      id: 'scanner', columns: COLUMNS, defaultSort: { key: 'rssi', dir: 'desc' },
      onChange: () => { this._dirty = true; },
    });
    this._toggle.addEventListener('click', () => this.toggleScan());
    this._tbody.addEventListener('click', (e) => this._onClick(e));
    this._tbody.addEventListener('dblclick', (e) => {
      const addr = e.target.closest('tr')?.dataset.addr;
      if (addr) this.app.showDevice(addr);
    });

    this.store.subscribe(() => { this._dirty = true; });
    ctx.bus.on('scan.done', () => { this.scanning = false; this._syncToggle(); });
  }

  attachKeys(keys) {
    keys.nav.registerList(this._wrap, {
      enter: () => { if (!this.sel) this._selectAt(0); return true; },
      move: (d) => this._selectAt(this._index() + d),
      first: () => this._selectAt(0),
      last: () => this._selectAt(this._rows().length - 1),
      count: () => this._rows().length,
      activate: () => this.sel && this._connectToggle(this.sel),
      right: () => this.sel && this.app.showDevice(this.sel),
    });
  }

  // 'scanner-filter' while a filter box has focus (Alt+N steps between them),
  // 'scanner' otherwise; both are the workspace plus these (keys.js).
  context() {
    return this._filters.includes(document.activeElement) ? 'scanner-filter' : 'scanner';
  }

  action(id) {
    const i = this._filters.indexOf(document.activeElement);
    const n = this._filters.length;
    if (id === 'scanner-filter-next') this._focusFilter((i + 1) % n);
    else if (id === 'scanner-filter-prev') this._focusFilter((i - 1 + n) % n);
    else if (id === 'scanner-filter') this._focusFilter(0);
  }

  _focusFilter(i) {
    const f = this._filters[i];
    f.focus();
    f.select?.();
  }

  async onConnect() {
    try {
      const r = await this.ws.call('scan.list');
      for (const d of r.devices || []) this.store.upsert(d);
      this.scanning = !!r.scanning;
      this._syncToggle();
    } catch { /* the next scan.device fills the table anyway */ }
  }

  // The radio hears everything; the boxes filter the table (scanFilter).
  _params() {
    return { duration_ms: 0, active: this._f.active.checked };
  }

  async toggleScan() {
    try {
      if (this.scanning) {
        await this.ws.call('scan.stop');
        this.scanning = false;
      } else {
        await this.ws.call('scan.start', this._params());
        this.scanning = true;
      }
    } catch (e) {
      this.app.log('!', `scan: ${e.message}`);
    }
    this._syncToggle();
  }

  _syncToggle() {
    this._toggle.textContent = this.scanning ? 'Stop' : 'Scan';
    this._toggle.classList.toggle('on', this.scanning);
    this._dirty = true;              // the count says "scanning" too
  }

  async _connectToggle(addr) {
    const d = this.store.get(addr);
    if (!d) return;
    try {
      await this.ws.call(d.connected ? 'dev.disconnect' : 'dev.connect', { device: addr });
    } catch (e) {
      this.app.log('!', `${addr}: ${e.message}`);
    }
  }

  _onClick(e) {
    const tr = e.target.closest('tr');
    if (!tr) return;
    if (e.target.closest('button')) { this._connectToggle(tr.dataset.addr); return; }
    this.sel = tr.dataset.addr;
    this._dirty = true;
  }

  _rows() {
    const f = this._f;
    const keep = scanFilter({ name: f.name.value, mfg: f.mfg.value, service: f.uuid.value, rssi: f.rssi.value });
    return this.grid.sort(this.store.list().filter(keep));
  }

  _index() { return this._rows().findIndex((d) => d.addr === this.sel); }

  _selectAt(i) {
    const rows = this._rows();
    if (!rows.length) return false;
    this.sel = rows[Math.max(0, Math.min(rows.length - 1, i))].addr;
    this._dirty = true;
    this._scrollSel = true;
    return true;
  }

  onShow() { this._dirty = true; }

  render() {
    if (!this._dirty) return;
    this._dirty = false;
    const rows = this._rows();
    const all = this.store.devices.size;
    const shown = rows.length === all ? `${all}` : `${rows.length} of ${all}`;
    this._count.textContent = `${shown} device${all === 1 ? '' : 's'}${this.scanning ? ' · scanning' : ''}`;
    const frag = document.createDocumentFragment();
    for (const d of rows) {
      const tr = document.createElement('tr');
      tr.dataset.addr = d.addr;
      if (d.addr === this.sel) tr.setAttribute('aria-selected', 'true');
      if (d.connected) tr.classList.add('is-connected');
      // The Connect button leads the row: at the end, a long manufacturer
      // field pushed it out of sight.
      const td = document.createElement('td');
      const b = document.createElement('button');
      b.type = 'button';
      b.className = 'pane-btn s-conn';
      b.tabIndex = -1;                 // the list is the stop; Enter connects
      b.textContent = d.connected ? 'Disconnect' : 'Connect';
      b.disabled = !d.connectable && !d.connected;
      if (b.disabled) b.title = 'Not connectable (the advertisement says so)';
      td.appendChild(b);
      tr.appendChild(td);
      for (const c of COLUMNS.slice(1)) {
        const cell = document.createElement('td');
        const v = c.get(d);
        cell.textContent = v ?? '';     // advertised names are anyone's text
        if (c.key === 'mfg' || c.key === 'name') cell.title = v || '';
        tr.appendChild(cell);
      }
      frag.appendChild(tr);
    }
    this._tbody.replaceChildren(frag);
    if (this._scrollSel) {
      this._scrollSel = false;
      this._tbody.querySelector('tr[aria-selected]')?.scrollIntoView({ block: 'nearest' });
    }
  }
}
