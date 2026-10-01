// The Scanner window: what the radio hears, and a way to connect to it.
//
// Rows come from the shared DeviceStore (gattdb.js), which merges
// advertisements -- a later ad that omits the name does not erase it -- and
// which the GATT window reads too, so the two can never disagree about a
// device. Columns come from the raw advertisement and scan response the
// firmware sends (fw 1.85+), parsed in the browser (adparse.js): any of them
// can be shown, hidden, moved and resized (Columns dialog, controls/grid.js).
//
// The filters (Filter dialog) filter what has been HEARD, in the browser, as
// you type or drag -- scanning or not; clearing one brings back everything
// it hid. The radio hears everything. "Expire" purges devices not heard for a
// while, during a scan.
//
// The table is a keyboard grid: Space/Enter enters it, h/j/k/l move a cell
// cursor, a/d sort, Shift+H/L move (header) or resize (rows) a column, and
// Enter/Space opens the cell's menu -- Connect, Show in GATT, Filter by, Copy.

import { shortUuid } from '../gattdb.js';
import { Grid } from '../controls/grid.js';
import { Dialog } from '../modals/dialog.js';
import { parseAd, flagNames, allServices, companyLabel } from '../adparse.js';
import { uuidDisplay } from '../gattfmt.js';
import { copyText } from '../clipboard.js';
import { toast } from '../toast.js';

const lc = (s) => String(s || '').toLowerCase();

// The filter as a predicate over device records. Empty / null matches all.
//   name     part of the name or the address, any case
//   mfg      manufacturer data starting with these hex bytes (spaces ignored)
//   service  part of any advertised service UUID (short or full form)
//   rssi     RSSI at least this (dBm); null = off
// `servicesOf(d)` lists a device's advertised UUIDs (parsed, when available).
export function scanFilter({ name = '', mfg = '', service = '', rssi = '' } = {}, servicesOf = (d) => d.service_uuids || []) {
  const n = lc(name).trim();
  const m = lc(mfg).replace(/[\s:]/g, '');
  const u = lc(service).trim();
  const r = rssi === null || String(rssi).trim() === '' ? null : Number(rssi);
  return (d) => {
    if (n && !lc(d.name).includes(n) && !lc(d.addr).includes(n)) return false;
    if (m && !lc(d.mfg_data).startsWith(m)) return false;
    if (u && !servicesOf(d).some((x) => lc(x).includes(u) || shortUuid(x).includes(u))) return false;
    if (r != null && Number.isFinite(r) && !((d.rssi ?? -127) >= r)) return false;
    return true;
  };
}

// The expiry slider's stops, in seconds (0 = off).
export const EXPIRE_STOPS = [0, 5, 10, 15, 20, 30, 45, 60, 90, 120, 180, 240, 300];
const RSSI_OFF = -101;              // the RSSI slider's left end

const FILTER_DEFAULTS = { name: '', mfg: '', service: '', rssi: null, expire: 0, active: true };

export function activeFilterCount(f) {
  return ['name', 'mfg', 'service'].filter((k) => String(f[k] || '').trim()).length
    + (f.rssi != null ? 1 : 0) + (f.expire ? 1 : 0);
}

const ADDR_TYPES = ['public', 'random', 'public id', 'random id'];
const list = (a) => (a || []).map((u) => shortUuid(u)).join(' ');

// --- the columns ------------------------------------------------------------------
// `p(d)` is the device's parsed advertisement. `hidden` = off until chosen.
function scannerColumns(p, now) {
  return [
    { key: 'action', label: '', width: 112, pinned: true, sortable: false, group: '' },
    // Device
    { key: 'addr', label: 'Address', group: 'Device', width: 150, get: (d) => d.addr },
    { key: 'addr_type', label: 'Address type', group: 'Device', width: 90, hidden: true, get: (d) => ADDR_TYPES[d.addr_type] ?? d.addr_type },
    { key: 'connectable', label: 'Connectable', group: 'Device', width: 90, hidden: true, get: (d) => (d.connectable ? 'yes' : 'no') },
    { key: 'rssi', label: 'RSSI', group: 'Device', width: 56, type: 'num', get: (d) => d.rssi },
    { key: 'tx', label: 'TX power', group: 'Device', width: 64, type: 'num', hidden: true, get: (d) => p(d).txPower ?? d.tx_power },
    { key: 'seen', label: 'Last seen', group: 'Device', width: 70, type: 'num', hidden: true,
      get: (d) => (d.seen ? Math.round((now() - d.seen) / 1000) : null), show: (d) => (d.seen ? `${Math.round((now() - d.seen) / 1000)} s` : '') },
    // Advertisement
    { key: 'name', label: 'Name', group: 'Advertisement', width: 170, get: (d) => p(d).nameComplete || p(d).nameShort || d.name },
    { key: 'name_complete', label: 'Complete name', group: 'Advertisement', width: 150, hidden: true, get: (d) => p(d).nameComplete },
    { key: 'name_short', label: 'Shortened name', group: 'Advertisement', width: 120, hidden: true, get: (d) => p(d).nameShort },
    { key: 'flags', label: 'Flags', group: 'Advertisement', width: 140, hidden: true, get: (d) => flagNames(p(d).flags) },
    { key: 'appearance', label: 'Appearance', group: 'Advertisement', width: 80, hidden: true,
      get: (d) => (p(d).appearance == null ? '' : `0x${p(d).appearance.toString(16).padStart(4, '0')}`) },
    { key: 'company', label: 'Manufacturer code', group: 'Advertisement', width: 130, hidden: true, get: (d) => companyLabel(p(d).mfg) },
    { key: 'mfg', label: 'Manufacturer data', group: 'Advertisement', width: 240, get: (d) => p(d).mfg?.raw || d.mfg_data },
    { key: 'services', label: 'Services', group: 'Advertisement', width: 130, get: (d) => list(servicesOf(d, p)) },
    { key: 'uuid16c', label: 'UUID16 (complete)', group: 'Advertisement', width: 120, hidden: true, get: (d) => list(p(d).uuid16.complete) },
    { key: 'uuid16i', label: 'UUID16 (incomplete)', group: 'Advertisement', width: 120, hidden: true, get: (d) => list(p(d).uuid16.incomplete) },
    { key: 'uuid32', label: 'UUID32', group: 'Advertisement', width: 120, hidden: true, get: (d) => list([...p(d).uuid32.complete, ...p(d).uuid32.incomplete]) },
    { key: 'uuid128c', label: 'UUID128 (complete)', group: 'Advertisement', width: 280, hidden: true, get: (d) => list(p(d).uuid128.complete) },
    { key: 'uuid128i', label: 'UUID128 (incomplete)', group: 'Advertisement', width: 280, hidden: true, get: (d) => list(p(d).uuid128.incomplete) },
    { key: 'svcdata', label: 'Service data', group: 'Advertisement', width: 200, hidden: true,
      get: (d) => p(d).serviceData.map((s) => `${shortUuid(s.uuid)}: ${s.data}`).join('  ') },
    // Raw
    { key: 'adv', label: 'Advertisement (raw)', group: 'Raw', width: 300, hidden: true, get: (d) => d.adv || '' },
    { key: 'rsp', label: 'Scan response (raw)', group: 'Raw', width: 300, hidden: true, get: (d) => d.rsp || '' },
  ];
}

// Advertised services: parsed from the raw data when there is some, else the
// firmware's own 16-bit list (an older firmware sends only that).
function servicesOf(d, p) {
  const parsed = allServices(p(d));
  return parsed.length ? parsed : (d.service_uuids || []);
}

export class ScannerWindow {
  constructor(el, ctx) {
    this.app = ctx.app;
    this.ws = ctx.ws;
    this.store = ctx.app.devices;
    this.scanning = false;
    this.sel = null;              // the cursor's device -- survives re-sorting
    this._rows = [];
    this._parsed = new Map();     // addr -> { sig, p }
    this._dirty = true;
    this.filters = { ...FILTER_DEFAULTS, ...(this.app.prefs.scanFilters || {}) };

    el.innerHTML = `
      <div class="pane-bar scan-bar">
        <button type="button" class="pane-btn s-toggle">Scan</button>
        <button type="button" class="pane-btn s-filter-btn" title="Filter (/)">Filter</button>
        <button type="button" class="pane-btn s-cols-btn" title="Columns (c)">Columns</button>
        <button type="button" class="pane-btn s-clear" title="Clear the list (Shift+X) — devices come back as they advertise">Clear list</button>
        <span class="spacer"></span>
        <span class="dim s-count"></span>
      </div>
      <div class="table-wrap s-wrap">
        <table class="grid s-table"><thead></thead><tbody></tbody></table>
      </div>
      <div class="pane-settings s-menu" hidden></div>`;
    this._toggle = el.querySelector('.s-toggle');
    this._filterBtn = el.querySelector('.s-filter-btn');
    this._count = el.querySelector('.s-count');
    this._wrap = el.querySelector('.s-wrap');
    this._tbody = el.querySelector('tbody');
    this._menu = el.querySelector('.s-menu');

    const p = (d) => this._parse(d);
    this.columns = scannerColumns(p, () => Date.now());
    this.grid = new Grid(el.querySelector('.s-table'), {
      id: 'scanner', columns: this.columns, defaultSort: { key: 'rssi', dir: 'desc' },
      onChange: () => { this._dirty = true; this._columnsDlg?.sync(); },
    });

    this._toggle.addEventListener('click', () => this.toggleScan());
    this._filterBtn.addEventListener('click', () => this.filterDlg.toggle());
    el.querySelector('.s-cols-btn').addEventListener('click', () => this.columnsDlg.toggle());
    el.querySelector('.s-clear').addEventListener('click', () => this.clearList());
    this._tbody.addEventListener('click', (e) => this._onClick(e));
    this._tbody.addEventListener('dblclick', (e) => {
      const addr = e.target.closest('tr')?.dataset.addr;
      if (addr) { this.app.popover.close(); this.app.showDevice(addr); }
    });

    this._buildFilterDialog();
    this._buildColumnsDialog();

    this.store.subscribe(() => { this._dirty = true; });
    ctx.bus.on('scan.done', () => { this.scanning = false; this._syncToggle(); });
    // Expiry runs whether or not the window is on screen: it is the list's
    // policy, not a view of it.
    this._expiryTimer = setInterval(() => this._expire(), 1000);
  }

  // --- parsing ------------------------------------------------------------------------

  _parse(d) {
    const sig = `${d.adv || ''}|${d.rsp || ''}`;
    const have = this._parsed.get(d.addr);
    if (have && have.sig === sig) return have.p;
    const p = parseAd(d.adv, d.rsp);
    this._parsed.set(d.addr, { sig, p });
    return p;
  }

  // --- keys ---------------------------------------------------------------------------

  attachKeys(keys) {
    this.keys = keys;
    keys.nav.registerList(this._wrap, this.grid.controller({
      list: this._wrap,
      count: () => this._rows.length,
      activate: (i, key, td) => {
        const d = this._rows[i];
        if (!d) return;
        if (key === 'action') this._connectToggle(d.addr);
        else this._openMenu(d, key, td);
      },
      onCursor: (i) => { this.sel = this._rows[i]?.addr || this.sel; },
      onChange: () => { this._dirty = true; },
    }));
  }

  // The table's keys depend on where the cursor is; the hint bar follows.
  context() {
    if (this._wrap.hasAttribute('data-nav-active')) return this.grid.cursor.row === 0 ? 'scanner-head' : 'scanner-table';
    return 'scanner';
  }

  action(id) {
    if (id === 'scanner-filter') { this.filterDlg.open(); this._focusFilter(0); return; }
    if (id === 'scanner-columns') { this.columnsDlg.open(); return; }
    if (id === 'scanner-clear') { this.clearList(); return; }
    const i = this._filters.indexOf(document.activeElement);
    const n = this._filters.length;
    if (id === 'scanner-filter-next') this._focusFilter((i + 1) % n);
    else if (id === 'scanner-filter-prev') this._focusFilter((i - 1 + n) % n);
  }

  _focusFilter(i) {
    const f = this._filters[i];
    f.focus();
    f.select?.();
  }

  // --- the Filter dialog ---------------------------------------------------------------

  _buildFilterDialog() {
    const body = document.createElement('div');
    body.className = 'scan-dlg';
    body.innerHTML = `
      <label class="sd-row"><span>Name or address</span>
        <input class="s-filter sf-name" type="search" spellcheck="false" autocomplete="off" placeholder="part of either"></label>
      <label class="sd-row"><span>Manufacturer data starts with</span>
        <input class="s-filter sf-mfg" type="search" spellcheck="false" autocomplete="off" placeholder="hex, e.g. 4c00"></label>
      <label class="sd-row"><span>Service UUID</span>
        <input class="s-filter sf-svc" type="search" spellcheck="false" autocomplete="off" placeholder="part of one, e.g. 180d"></label>
      <label class="sd-row"><span>Minimum RSSI <b class="sf-rssi-v"></b></span>
        <input class="sf-rssi" type="range" min="${RSSI_OFF}" max="-20" step="1"></label>
      <label class="sd-row"><span>Expire, while scanning <b class="sf-exp-v"></b></span>
        <input class="sf-exp" type="range" min="0" max="${EXPIRE_STOPS.length - 1}" step="1"></label>
      <div class="dim sd-note">Devices not heard for this long are removed from the list. Connected ones stay.</div>
      <label class="sd-check"><input type="checkbox" class="sf-active"> Active scanning (asks for scan responses: names, more UUIDs)</label>
      <div class="sd-actions"><button type="button" class="pane-btn sf-clear">Clear filters</button></div>`;
    const q = (s) => body.querySelector(s);
    this._f = { name: q('.sf-name'), mfg: q('.sf-mfg'), service: q('.sf-svc'), rssi: q('.sf-rssi'), expire: q('.sf-exp'), active: q('.sf-active') };
    this._filters = [this._f.name, this._f.mfg, this._f.service];
    const dlg = new Dialog({ id: 'scanner-filter-dlg', title: 'Filter', body });
    // Alt+N / Alt+Shift+N step between the text boxes while one has focus.
    dlg.context = () => (this._filters.includes(document.activeElement) ? 'scanner-filter' : 'settings');
    dlg.onOpen = () => this._paintFilterDialog();
    this.filterDlg = this.app.registerDialog(dlg);

    const changed = () => {
      const f = this._f;
      const rssi = Number(f.rssi.value);
      this.filters = {
        name: f.name.value, mfg: f.mfg.value, service: f.service.value,
        rssi: rssi <= RSSI_OFF ? null : rssi, expire: EXPIRE_STOPS[Number(f.expire.value)] || 0, active: f.active.checked,
      };
      this.app.setPref('scanFilters', this.filters);
      this._paintFilterDialog(false);
      this._dirty = true;
    };
    for (const k of ['name', 'mfg', 'service', 'rssi', 'expire']) this._f[k].addEventListener('input', changed);
    this._f.active.addEventListener('change', changed);
    q('.sf-clear').addEventListener('click', () => this.clearFilters());
  }

  // Puts the filter state into the dialog (fromState) and labels the sliders.
  _paintFilterDialog(fromState = true) {
    const f = this._f, s = this.filters;
    if (fromState) {
      f.name.value = s.name; f.mfg.value = s.mfg; f.service.value = s.service;
      f.rssi.value = String(s.rssi ?? RSSI_OFF);
      f.expire.value = String(Math.max(0, EXPIRE_STOPS.indexOf(s.expire)));
      f.active.checked = s.active !== false;
    }
    this.filterDlg.body.querySelector('.sf-rssi-v').textContent = s.rssi == null ? 'off' : `≥ ${s.rssi} dBm`;
    this.filterDlg.body.querySelector('.sf-exp-v').textContent = s.expire ? `${s.expire} s` : 'off';
  }

  clearFilters() {
    this.filters = { ...FILTER_DEFAULTS, active: this.filters.active };
    this.app.setPref('scanFilters', this.filters);
    this._paintFilterDialog();
    this._dirty = true;
  }

  // --- the Columns dialog -----------------------------------------------------------------

  _buildColumnsDialog() {
    const body = document.createElement('div');
    body.className = 'scan-dlg cols-dlg';
    const groups = new Map();
    for (const c of this.columns) {
      if (c.pinned) continue;
      if (!groups.has(c.group)) groups.set(c.group, []);
      groups.get(c.group).push(c);
    }
    for (const [g, cols] of groups) {
      const h = document.createElement('div');
      h.className = 'cd-group';
      h.textContent = g;
      body.appendChild(h);
      for (const c of cols) {
        const l = document.createElement('label');
        l.className = 'sd-check';
        l.innerHTML = `<input type="checkbox" data-col="${c.key}"> <span></span>`;
        l.querySelector('span').textContent = c.label;
        l.querySelector('input').addEventListener('change', (e) => this.grid.setHidden(c.key, !e.target.checked));
        body.appendChild(l);
      }
    }
    const act = document.createElement('div');
    act.className = 'sd-actions';
    act.innerHTML = '<button type="button" class="pane-btn cd-reset">Reset columns</button><span class="dim">Drag a heading to move it; drag its edge to resize.</span>';
    act.querySelector('.cd-reset').addEventListener('click', () => this.grid.reset());
    body.appendChild(act);
    const dlg = new Dialog({ id: 'scanner-columns-dlg', title: 'Columns', body });
    dlg.sync = () => {
      for (const cb of body.querySelectorAll('input[data-col]')) cb.checked = !this.grid.isHidden(cb.dataset.col);
    };
    dlg.onOpen = () => dlg.sync();
    this._columnsDlg = dlg;
    this.columnsDlg = this.app.registerDialog(dlg);
  }

  // --- the cell menu ------------------------------------------------------------------------

  _openMenu(d, key, anchor) {
    if (!anchor) return;
    const m = this._menu;
    m.replaceChildren();
    const title = document.createElement('div');
    title.className = 'sm-title';
    title.textContent = d.name ? `${d.name} · ${d.addr}` : d.addr;
    m.appendChild(title);
    const item = (label, fn, cls = '') => {
      const b = document.createElement('button');
      b.type = 'button';
      b.className = `pane-btn sm-item ${cls}`;
      b.textContent = label;
      b.addEventListener('click', () => { this.app.popover.close(); fn(); });
      m.appendChild(b);
      return b;
    };
    const filterBy = (k, v, what) => {
      this.filters = { ...this.filters, [k]: v };
      this.app.setPref('scanFilters', this.filters);
      this._paintFilterDialog();
      this._dirty = true;
      toast(`Filtered by ${what}`);
    };
    const copy = async (text, what = 'Copied') => toast((await copyText(text)) ? what : 'Could not copy');
    const col = this.columns.find((c) => c.key === key);
    const value = col?.get ? String(col.get(d) ?? '') : '';

    if (d.connected || d.connectable) item(d.connected ? 'Disconnect' : 'Connect', () => this._connectToggle(d.addr));
    if (d.connected) item('Show in GATT', () => this.app.showDevice(d.addr));
    const p = this._parse(d);
    switch (key) {
      case 'addr':
        item('Copy address', () => copy(d.addr));
        item('Filter by this address', () => filterBy('name', d.addr, d.addr));
        break;
      case 'name': case 'name_complete': case 'name_short':
        if (value) { item('Copy name', () => copy(value)); item('Filter by this name', () => filterBy('name', value, `"${value}"`)); }
        break;
      case 'services': case 'uuid16c': case 'uuid16i': case 'uuid32': case 'uuid128c': case 'uuid128i': {
        const all = key === 'services' ? servicesOf(d, (x) => this._parse(x)) : value.split(' ').filter(Boolean);
        for (const u of all) item(`Filter by ${uuidDisplay(u).text}`, () => filterBy('service', shortUuid(u), uuidDisplay(u).text));
        break;
      }
      case 'company':
        if (p.mfg) {
          const le = p.mfg.raw.slice(0, 4);
          item(`Filter by ${companyLabel(p.mfg)}`, () => filterBy('mfg', le, companyLabel(p.mfg)));
        }
        break;
      default:
        break;
    }
    // The cell's whole value, wrapped; tap to copy. For the long hex fields
    // (manufacturer data, raw) this is where they can be read in full.
    if (value && key !== 'action') {
      const v = item(value, () => copy(value), 'sm-value');
      v.title = 'Copy';
      const hint = document.createElement('div');
      hint.className = 'dim sm-hint';
      hint.textContent = 'Tap the value to copy it';
      m.appendChild(hint);
    }
    if (m.children.length <= 1) return;
    // Opened from the keyboard, the table was in its selection mode -- which
    // ends when focus moves into the menu. Closing the menu (Esc included)
    // must only close the menu: put the table back as it was.
    const fromTable = this._wrap.hasAttribute('data-nav-active');
    this.app.popover.open(anchor, m, 'scanner-cell', {
      onClose: () => {
        if (!fromTable || !this._wrap.isConnected) return;
        // An item that moved the keyboard elsewhere (Show in GATT) wins.
        const ae = document.activeElement;
        if (ae && ae !== document.body && !this._wrap.contains(ae) && !anchor.contains(ae)) return;
        this._wrap.focus({ preventScroll: true });
        this._wrap.setAttribute('data-nav-active', '');
        this._dirty = true;
      },
    });
  }

  // --- data -----------------------------------------------------------------------------

  async onConnect() {
    // Paged (fw 1.85+): one reply is one 4 KB frame. An older firmware has no
    // `next`, and its one page is all it has.
    try {
      let offset = 0;
      for (let page = 0; page < 20; page++) {
        const r = await this.ws.call('scan.list', { offset, limit: 100 });
        for (const d of r.devices || []) this.store.upsert(d);
        this.scanning = !!r.scanning;
        if (r.next == null) break;
        offset = r.next;
      }
      this._syncToggle();
    } catch { /* the next scan.device fills the table anyway */ }
  }

  _params() {
    return { duration_ms: 0, active: this.filters.active !== false };
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

  _expire() {
    const s = this.filters.expire;
    if (!s || !this.scanning) return;
    const cutoff = Date.now() - s * 1000;
    for (const d of this.store.list()) {
      if (!d.connected && d.seen && d.seen < cutoff) this.store.remove(d.addr);
    }
  }

  clearList() {
    for (const d of this.store.list()) if (!d.connected) this.store.remove(d.addr);
    this._parsed.clear();
    toast('List cleared — devices come back as they advertise');
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
    const addr = tr.dataset.addr;
    const i = this._rows.findIndex((d) => d.addr === addr);
    const td = e.target.closest('td');
    const col = td ? [...tr.cells].indexOf(td) : -1;
    if (i >= 0 && col >= 0 && col < this.grid.visible.length) this.grid.cursor = { row: i + 1, col };
    this.sel = addr;
    this._dirty = true;
    if (e.target.closest('button')) { this._connectToggle(addr); return; }
    const key = this.grid.visible[col]?.key;
    if (key && key !== 'action' && this._rows[i]) this._openMenu(this._rows[i], key, td);
  }

  _rowsNow() {
    const keep = scanFilter(this.filters, (d) => servicesOf(d, (x) => this._parse(x)));
    return this.grid.sort(this.store.list().filter(keep));
  }

  onShow() { this._dirty = true; }

  render() {
    if (!this._dirty && !this._ticking) return;
    this._dirty = false;
    const rows = this._rowsNow();
    this._rows = rows;
    const all = this.store.devices.size;
    const shown = rows.length === all ? `${all}` : `${rows.length} of ${all}`;
    this._count.textContent = `${shown} device${all === 1 ? '' : 's'}${this.scanning ? ' · scanning' : ''}`;
    const nf = activeFilterCount(this.filters);
    this._filterBtn.textContent = nf ? `Filter (${nf})` : 'Filter';
    this._filterBtn.classList.toggle('on', nf > 0);

    const vis = this.grid.visible;
    // "Last seen" counts up by itself: keep redrawing while it is shown.
    this._ticking = vis.some((c) => c.key === 'seen');
    const frag = document.createDocumentFragment();
    for (const d of rows) {
      const tr = document.createElement('tr');
      tr.dataset.addr = d.addr;
      if (d.addr === this.sel) tr.setAttribute('aria-selected', 'true');
      if (d.connected) tr.classList.add('is-connected');
      for (const c of vis) {
        const td = document.createElement('td');
        if (c.key === 'action') {
          // The Connect button leads the row: at the end, a long manufacturer
          // field pushed it out of sight.
          const b = document.createElement('button');
          b.type = 'button';
          b.className = 'pane-btn s-conn';
          b.tabIndex = -1;                 // the table is the stop; Enter connects
          b.textContent = d.connected ? 'Disconnect' : 'Connect';
          b.disabled = !d.connectable && !d.connected;
          if (b.disabled) b.title = 'Not connectable (the advertisement says so)';
          td.appendChild(b);
        } else {
          const v = c.show ? c.show(d) : c.get(d);
          td.textContent = v ?? '';         // advertised names are anyone's text
          if (v && String(v).length > 12) td.title = String(v);
        }
        tr.appendChild(td);
      }
      frag.appendChild(this.grid.appendSpacer(tr));
    }
    this._tbody.replaceChildren(frag);
    this.grid.paintCursor();
  }
}
