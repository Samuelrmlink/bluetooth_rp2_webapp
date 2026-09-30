// The Scanner window: what the radio hears, and a way to connect to it.
//
// Rows come from the shared DeviceStore (gattdb.js), which merges
// advertisements -- a later ad that omits the name does not erase it -- and
// which the GATT window reads too, so the two can never disagree about a
// device. The table is a keyboard list: Space/Enter enters it, j/k move,
// Enter connects or disconnects, l shows the device in the GATT window.

import { shortUuid } from '../gattdb.js';

const COLS = ['Address', 'Name', 'RSSI', 'Services', 'Manufacturer data', ''];

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
        <input class="s-name" type="text" placeholder="name prefix" spellcheck="false" size="10">
        <input class="s-mfg" type="text" placeholder="mfg hex" spellcheck="false" size="8">
        <input class="s-uuid" type="text" placeholder="service" spellcheck="false" size="6">
        <input class="s-rssi" type="number" placeholder="rssi ≥" min="-127" max="0" step="1">
        <label class="dim" title="Active scanning asks for scan responses (names, more UUIDs)"><input type="checkbox" class="s-active" checked> active</label>
        <span class="spacer"></span>
        <span class="dim s-count"></span>
      </div>
      <div class="table-wrap s-wrap">
        <table class="grid s-table"><thead><tr>${COLS.map((c) => `<th>${c}</th>`).join('')}</tr></thead><tbody></tbody></table>
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

  async onConnect() {
    try {
      const r = await this.ws.call('scan.list');
      for (const d of r.devices || []) this.store.upsert(d);
      this.scanning = !!r.scanning;
      this._syncToggle();
    } catch { /* the next scan.device fills the table anyway */ }
  }

  _params() {
    const f = this._f;
    const p = { duration_ms: 0, active: f.active.checked };
    if (f.name.value.trim()) p.name_prefix = f.name.value.trim();
    if (f.mfg.value.trim()) p.mfg_prefix = f.mfg.value.trim();
    if (f.uuid.value.trim()) p.service_uuid = f.uuid.value.trim();
    if (f.rssi.value.trim()) p.rssi_min = parseInt(f.rssi.value, 10);
    return p;
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
    return this.store.list().sort((a, b) => (b.rssi ?? -127) - (a.rssi ?? -127));
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
    this._count.textContent = `${rows.length} device${rows.length === 1 ? '' : 's'}${this.scanning ? ' · scanning' : ''}`;
    const frag = document.createDocumentFragment();
    for (const d of rows) {
      const tr = document.createElement('tr');
      tr.dataset.addr = d.addr;
      if (d.addr === this.sel) tr.setAttribute('aria-selected', 'true');
      if (d.connected) tr.classList.add('is-connected');
      const cells = [d.addr, d.name || '', d.rssi ?? '', (d.service_uuids || []).map(shortUuid).join(' '), d.mfg_data || ''];
      for (const v of cells) {
        const td = document.createElement('td');
        td.textContent = v;            // advertised names are anyone's text
        tr.appendChild(td);
      }
      const td = document.createElement('td');
      const b = document.createElement('button');
      b.type = 'button';
      b.className = 'pane-btn';
      b.tabIndex = -1;                 // the list is the stop; Enter connects
      b.textContent = d.connected ? 'Disconnect' : 'Connect';
      b.disabled = !d.connectable && !d.connected;
      td.appendChild(b);
      tr.appendChild(td);
      frag.appendChild(tr);
    }
    this._tbody.replaceChildren(frag);
    if (this._scrollSel) {
      this._scrollSel = false;
      this._tbody.querySelector('tr[aria-selected]')?.scrollIntoView({ block: 'nearest' });
    }
  }
}
