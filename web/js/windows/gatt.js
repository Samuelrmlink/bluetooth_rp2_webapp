// The GATT window: a connected device's services and characteristics, each
// characteristic with a panel -- collapsed until opened -- holding what can be
// done to it and nothing it does not permit: Read, Notify, Indicate, Write,
// in the format chosen for it (hex, decimal bytes, or a string), with its
// last value and its permissions.
//
// The values themselves are in the app-wide log (js/gattlog.js), which the
// GATT Events window shows; in tabbed mode, where one window at a time would
// hide it, the events can be split into this window instead.
//
// The tree lives in the shared DeviceStore. Discovery (gatt.discovered) only
// SUMMARISES each service -- a characteristic count, not the characteristics
// -- because a whole tree can exceed the 4 KB websocket frame. So the detail
// is fetched here one service at a time with gatt.tree, which also covers a
// page opened after the device connected, whose tree it never saw at all.
//
// Keys: the tree is a list -- j/k move, Enter/Space open or close, l into an
// open panel's controls, Esc back to its row, h closes / goes to the service.

import { uuidDisplay, FORMATS, formatValue, parseInput, byteLength, defaultFormat, formatKey,
  rememberFormat, permissions, actionsFor } from '../gattfmt.js';
import { GattEventsView } from './gattevents.js';
import { makeSplitter, clamp } from '../splitter.js';
import { copyText } from '../clipboard.js';
import { isTextField } from '../nav.js';

const OP_DATA_MAX = 244;            // the firmware's per-op limit (src/bt/le/central.c)
const BADGES = [['read', 'R'], ['write', 'W'], ['write_no_response', 'w'], ['notify', 'N'], ['indicate', 'I']];

const ago = (t) => {
  const s = Math.round((Date.now() - t) / 1000);
  return s < 60 ? `${s} s ago` : s < 3600 ? `${Math.round(s / 60)} min ago` : `${Math.round(s / 3600)} h ago`;
};

export class GattWindow {
  constructor(el, ctx) {
    this.app = ctx.app;
    this.ws = ctx.ws;
    this.ctx = ctx;
    this.store = ctx.app.devices;
    this.log = ctx.app.gattLog;
    this.el = el;
    this.addr = '';                 // device shown
    this.sel = '';                  // selected row: 'svc:<id>' | 'chr:<id>'
    this.open = new Set();          // expanded characteristics, by row key
    this.shut = new Set();          // collapsed services, by row key
    this.subs = new Map();          // `${addr}|${id}` -> 'notify' | 'indicate'
    this._panels = new Map();       // `${addr}|${id}` -> panel parts
    this._fetching = new Set();     // addr:service in flight
    this._struct = true;            // rows need rebuilding
    this._dirty = true;

    el.innerHTML = `
      <div class="pane-bar g-bar">
        <select class="g-dev" aria-label="Device"></select>
        <button type="button" class="pane-btn g-disconnect">Disconnect</button>
        <span class="dim g-note"></span>
        <span class="spacer"></span>
        <button type="button" class="pane-btn g-u128" aria-pressed="false" title="Show 128-bit UUIDs in full">UUID-128</button>
        <button type="button" class="pane-btn g-mtu" title="The ATT MTU: this connection's, and what new connections ask for">MTU</button>
        <button type="button" class="pane-btn g-ev" aria-pressed="false" title="Show GATT events beside the tree (tabbed view)" hidden>Events</button>
        <button type="button" class="pane-btn g-axis" title="Side by side / stacked" hidden></button>
      </div>
      <div class="gatt-body">
        <div class="gatt-tree" role="tree" aria-label="Services and characteristics"></div>
        <div class="splitter g-split" hidden></div>
        <div class="gatt-events-host" hidden></div>
      </div>
      <div class="pane-settings g-uuid-menu" hidden>
        <div class="g-uuid-full"></div>
        <button type="button" class="pane-btn g-uuid-copy">Copy</button>
      </div>
      <div class="pane-settings g-mtu-menu" hidden>
        <div class="g-mtu-now dim"></div>
        <label>Ask new connections for
          <input class="g-mtu-in" type="number" min="23" step="1" inputmode="numeric"></label>
        <div class="g-mtu-actions">
          <button type="button" class="pane-btn g-mtu-save">Save</button>
          <button type="button" class="pane-btn g-mtu-reconnect">Reconnect now</button>
        </div>
        <div class="g-mtu-msg dim"></div>
      </div>`;
    const q = (s) => el.querySelector(s);
    this._dev = q('.g-dev');
    this._note = q('.g-note');
    this._tree = q('.gatt-tree');
    this._body = q('.gatt-body');
    this._split = q('.g-split');
    this._host = q('.gatt-events-host');
    this._u128 = q('.g-u128');
    this._mtuBtn = q('.g-mtu');
    this._evBtn = q('.g-ev');
    this._axisBtn = q('.g-axis');
    this._uuidMenu = q('.g-uuid-menu');
    this._mtuMenu = q('.g-mtu-menu');

    this._dev.addEventListener('change', () => this.showDevice(this._dev.value));
    q('.g-disconnect').addEventListener('click', () => {
      if (this.addr) this.ws.call('dev.disconnect', { device: this.addr }).catch((e) => this.app.log('!', e.message));
    });
    this._u128.addEventListener('click', () => this.app.setPref('gattUuid128', !this.app.prefs.gattUuid128));
    this._mtuBtn.addEventListener('click', () => this._openMtu());
    this._evBtn.addEventListener('click', () => this._setSplit({ on: !this._splitPref().on }));
    this._axisBtn.addEventListener('click', () => this._setSplit({ axis: this._splitPref().axis === 'x' ? 'y' : 'x' }));
    q('.g-uuid-copy').addEventListener('click', async () => {
      const ok = await copyText(q('.g-uuid-full').textContent);
      this._note.textContent = ok ? 'UUID copied' : 'Could not copy';
      this.app.popover.close();
    });
    q('.g-mtu-save').addEventListener('click', () => this._saveMtu());
    q('.g-mtu-reconnect').addEventListener('click', () => this._reconnect());

    this._tree.addEventListener('click', (e) => this._onClick(e));

    // Every advertisement changes the store; only a change to the SHOWN
    // device's tree means new rows (render compares _treeSig). Rebuilding on
    // every one would re-attach the open panels, and moving a focused element
    // drops its focus -- typing in a write box would be impossible while
    // scanning.
    this.store.subscribe(() => { this._dirty = true; });
    this.log.subscribe(() => { this._dirty = true; });
    ctx.bus.on('gatt.discovered', (d) => this._fill(d.addr));
    ctx.bus.on('dev.disconnected', (d) => {
      for (const k of [...this.subs.keys()]) if (k.startsWith(`${d.addr}|`)) this.subs.delete(k);
    });
  }

  // --- keys --------------------------------------------------------------------

  attachKeys(keys) {
    this.keys = keys;
    keys.nav.registerList(this._tree, {
      enter: () => { if (!this.sel) this._step(1); return true; },
      move: (d) => this._step(d),
      first: () => this._stepTo(0),
      last: () => this._stepTo(this._rows().length - 1),
      count: () => this._rows().length,
      activate: () => this._toggle(this.sel),
      right: () => this._right(),
      left: () => this._left(),
    });
  }

  // While an open panel has the keyboard, the banner says how to get back.
  context() {
    return document.activeElement?.closest?.('.t-panel') ? 'gatt-panel' : 'gatt';
  }

  // --- device and tree ---------------------------------------------------------

  // From the scanner (double-click, "Show in GATT") or the app.
  showDevice(addr) {
    this.addr = addr || '';
    this.sel = '';
    this._struct = true;
    this._dirty = true;
    this._fill(this.addr);
  }

  // Fetch whatever part of a device's tree the store does not have yet.
  async _fill(addr) {
    const d = addr && this.store.get(addr);
    if (!d || !d.connected || !this.ws.connected) return;
    try {
      if (!d.gatt) {
        const sum = await this.ws.call('gatt.tree', { device: addr });
        this.store.setTree(addr, sum);
      }
      const g = this.store.get(addr).gatt;
      for (const s of g.services || []) {
        if (Array.isArray(s.characteristics)) continue;   // already detailed
        const key = `${addr}:${s.id}`;
        if (this._fetching.has(key)) continue;
        this._fetching.add(key);
        try {
          const det = await this.ws.call('gatt.tree', { device: addr, service: s.id });
          Object.assign(s, det);
          this._struct = true;
          this._dirty = true;
        } finally {
          this._fetching.delete(key);
        }
      }
    } catch (e) {
      if (e.code !== 'discovering') this.app.log('!', `gatt.tree ${addr}: ${e.message}`);
    }
  }

  _gatt() { return this.store.get(this.addr)?.gatt || null; }

  _services() { return this._gatt()?.services || []; }

  // The rows as shown, in order: services, and the characteristics of open ones.
  _rows() {
    const out = [];
    for (const s of this._services()) {
      out.push({ key: `svc:${s.id}`, svc: s });
      if (this.shut.has(`svc:${s.id}`) || !Array.isArray(s.characteristics)) continue;
      for (const c of s.characteristics) out.push({ key: `chr:${c.id}`, svc: s, chr: c });
    }
    return out;
  }

  _row(key) { return this._rows().find((r) => r.key === key) || null; }

  _select(key) {
    if (!key) return false;
    this.sel = key;
    this._dirty = true;
    this._scrollSel = true;
    return true;
  }

  _step(d) {
    const rows = this._rows();
    if (!rows.length) return false;
    const i = rows.findIndex((r) => r.key === this.sel);
    return this._select(rows[clamp((i < 0 ? -1 : i) + d, 0, rows.length - 1)].key);
  }

  _stepTo(i) {
    const rows = this._rows();
    return rows.length ? this._select(rows[clamp(i, 0, rows.length - 1)].key) : false;
  }

  _toggle(key) {
    if (!key) return;
    const set = key.startsWith('svc:') ? this.shut : this.open;
    if (set.has(key)) set.delete(key); else set.add(key);
    this._struct = true;
    this._dirty = true;
  }

  // l: open a closed row; on an open characteristic, into its controls.
  _right() {
    const r = this._row(this.sel);
    if (!r) return;
    if (r.chr) {
      if (!this.open.has(r.key)) { this._toggle(r.key); return; }
      const p = this._panels.get(`${this.addr}|${r.chr.id}`);
      const first = p?.el.querySelector('select, input, button:not([disabled])');
      if (first) { this._tree.removeAttribute('data-nav-active'); first.focus({ preventScroll: true }); }
    } else if (this.shut.has(r.key)) {
      this._toggle(r.key);
    }
  }

  // h: close an open row, or step out to its service.
  _left() {
    const r = this._row(this.sel);
    if (!r) return;
    if (r.chr && this.open.has(r.key)) this._toggle(r.key);
    else if (r.chr) this._select(`svc:${r.svc.id}`);
    else if (!this.shut.has(r.key)) this._toggle(r.key);
  }

  // Back from a panel's controls to its row, list mode on.
  _backToRow() {
    this._tree.focus({ preventScroll: true });
    this._tree.setAttribute('data-nav-active', '');
    this._scrollSel = true;
    this._dirty = true;
  }

  _onClick(e) {
    if (e.target.closest('.t-panel')) return;          // the panel's own controls
    const rowEl = e.target.closest('[data-key]');
    if (!rowEl) return;
    const key = rowEl.dataset.key;
    const uuidEl = e.target.closest('.t-uuid[data-full]');
    this._select(key);
    if (uuidEl) { this._uuidMenuFor(uuidEl); return; }  // a 128-bit UUID: show it whole
    this._toggle(key);
  }

  _uuidMenuFor(anchor) {
    this._uuidMenu.querySelector('.g-uuid-full').textContent = anchor.dataset.full;
    this.app.popover.toggle(anchor, this._uuidMenu, 'gatt-uuid');
  }

  // --- formats -------------------------------------------------------------------

  _fmt(svc, chr) {
    return (this.app.prefs.gattFormats || {})[formatKey(svc.uuid, chr.uuid)] || defaultFormat(chr.uuid);
  }

  _setFmt(svc, chr, fmt) {
    this.app.setPref('gattFormats', rememberFormat(this.app.prefs.gattFormats, formatKey(svc.uuid, chr.uuid), fmt));
  }

  onPrefs(prefs) {
    // Only UUID-128 changes the rows themselves (every label). A format
    // change is a repaint: rebuilding would re-attach the open panels, which
    // drops focus and can swallow a click in flight.
    const full = !!prefs?.gattUuid128;
    if (full !== this._fullShown) { this._fullShown = full; this._struct = true; }
    this._dirty = true;
    this._events?.onPrefs();
  }

  // --- a characteristic's panel ----------------------------------------------------

  _panel(svc, chr) {
    const key = `${this.addr}|${chr.id}`;
    const have = this._panels.get(key);
    if (have && have.uuid === chr.uuid) return have;
    const addr = this.addr;
    const acts = actionsFor(chr.flags);
    const el = document.createElement('div');
    el.className = 't-panel';
    el.innerHTML = `
      <div class="tp-line"><span class="tp-k">Permissions</span><span class="tp-perms"></span></div>
      <div class="tp-line"><label class="tp-k" for="">Format</label><select class="tp-fmt"></select></div>
      <div class="tp-line"><span class="tp-k">Last value</span>
        <button type="button" class="tp-val" title="Copy"></button><span class="tp-meta dim"></span></div>
      <div class="tp-actions"></div>
      <form class="tp-write" hidden>
        <input class="tp-in" type="text" spellcheck="false" autocomplete="off" aria-label="Value to write">
        <label class="tp-resp dim" hidden><input type="checkbox" class="tp-resp-cb" checked> with response</label>
        <button type="submit" class="pane-btn">Write</button>
        <span class="tp-err"></span>
      </form>`;
    const q = (s) => el.querySelector(s);
    q('.tp-perms').textContent = permissions(chr.flags).join(' · ') || '—';
    const sel = q('.tp-fmt');
    for (const [v, t] of FORMATS) { const o = document.createElement('option'); o.value = v; o.textContent = t; sel.appendChild(o); }
    sel.addEventListener('change', () => this._setFmt(svc, chr, sel.value));

    const actions = q('.tp-actions');
    const button = (label, fn) => {
      const b = document.createElement('button');
      b.type = 'button';
      b.className = 'pane-btn';
      b.textContent = label;
      b.addEventListener('click', fn);
      actions.appendChild(b);
      return b;
    };
    const parts = { el, uuid: chr.uuid, sel, val: q('.tp-val'), meta: q('.tp-meta'), input: q('.tp-in'), err: q('.tp-err') };
    if (acts.read) parts.read = button('Read', () => this._read(addr, chr));
    if (acts.notify) parts.notify = button('Notify', () => this._subscribe(addr, chr, 'notify'));
    if (acts.indicate) parts.indicate = button('Indicate', () => this._subscribe(addr, chr, 'indicate'));
    if (acts.write) {
      const form = q('.tp-write');
      form.hidden = false;
      if (acts.chooseResponse) q('.tp-resp').hidden = false;
      form.addEventListener('submit', (e) => {
        e.preventDefault();
        const resp = acts.chooseResponse ? q('.tp-resp-cb').checked : acts.response;
        this._write(addr, svc, chr, parts, resp);
      });
    }
    parts.val.addEventListener('click', async () => {
      const ok = await copyText(parts.val.textContent);
      parts.meta.textContent = ok ? 'copied' : 'could not copy';
    });
    // Esc from a control goes back to the row (after a text field's own Esc
    // has left the field, which the key layer does first).
    el.addEventListener('keydown', (e) => {
      if (e.key === 'Escape' && !e.defaultPrevented && !isTextField(e.target)) { e.preventDefault(); this._backToRow(); }
    });
    this.keys?.ownEscape(parts.input, () => { this._backToRow(); return true; });
    this._panels.set(key, parts);
    return parts;
  }

  _paintPanel(svc, chr, p) {
    const fmt = this._fmt(svc, chr);
    if (p.sel.value !== fmt) p.sel.value = fmt;
    p.input.placeholder = { hex: 'hex, e.g. 01 a0', dec: 'bytes, e.g. 1 160', str: 'text (\\xNN for a byte)' }[fmt];
    const last = this.log.last(this.addr, chr.id);
    p.val.textContent = last ? (formatValue(last.hex, fmt) || '(empty)') : '—';
    p.val.disabled = !last;
    if (last && !/copied|copy/.test(p.meta.textContent)) p.meta.textContent = `${last.src} · ${last.len} B · ${ago(last.t)}`;
    else if (!last) p.meta.textContent = chr.flags?.includes('read') ? 'not read yet' : '';
    const sub = this.subs.get(`${this.addr}|${chr.id}`);
    for (const m of ['notify', 'indicate']) {
      if (!p[m]) continue;
      const on = sub === m;
      p[m].classList.toggle('on', on);
      p[m].setAttribute('aria-pressed', String(on));
      p[m].textContent = `${m === 'notify' ? 'Notify' : 'Indicate'}: ${on ? 'on' : 'off'}`;
    }
  }

  // --- operations (each result goes to the log) -------------------------------------

  async _read(addr, chr) {
    try {
      await this.ws.call('gatt.read', { device: addr, id: chr.id });
    } catch (e) {
      this.log.add({ addr, id: chr.id, uuid: chr.uuid, src: 'error', note: `read: ${e.message}` });
    }
  }

  async _subscribe(addr, chr, mode) {
    const k = `${addr}|${chr.id}`;
    const want = this.subs.get(k) === mode ? 'off' : mode;
    try {
      await this.ws.call('gatt.subscribe', { device: addr, id: chr.id, mode: want });
      if (want === 'off') this.subs.delete(k); else this.subs.set(k, want);
    } catch (e) {
      this.log.add({ addr, id: chr.id, uuid: chr.uuid, src: 'error', note: `${mode}: ${e.message}` });
    }
    this._dirty = true;
  }

  async _write(addr, svc, chr, p, response) {
    const fmt = this._fmt(svc, chr);
    const r = parseInput(p.input.value, fmt);
    p.err.textContent = '';
    if (!r.ok) { p.err.textContent = r.error; return; }
    const n = byteLength(r.hex);
    const mtu = this._gatt()?.mtu || 23;
    const max = Math.min(mtu - 3, OP_DATA_MAX);
    if (n > max) { p.err.textContent = `${n} bytes: more than the ${max} this link takes in one write`; return; }
    try {
      await this.ws.call('gatt.write', { device: addr, id: chr.id, value: r.hex, response });
      this.log.add({ addr, id: chr.id, uuid: chr.uuid, src: 'write', hex: r.hex, note: response ? 'with response' : 'without response' });
    } catch (e) {
      p.err.textContent = e.message;
      this.log.add({ addr, id: chr.id, uuid: chr.uuid, src: 'error', note: `write: ${e.message}` });
    }
  }

  // --- MTU ---------------------------------------------------------------------------

  async _openMtu() {
    const m = this._mtuMenu;
    const now = this._gatt()?.mtu;
    m.querySelector('.g-mtu-now').textContent = now ? `This connection: ${now} (negotiated when it connected)` : 'No device connected';
    m.querySelector('.g-mtu-msg').textContent = '';
    m.querySelector('.g-mtu-reconnect').hidden = !this.addr;
    this.app.popover.toggle(this._mtuBtn, m, 'gatt-mtu');
    try {
      const r = await this.ws.call('bt.mtu');
      const inp = m.querySelector('.g-mtu-in');
      inp.max = String(r.ceiling);
      inp.value = String(r.max_mtu);
      this._ceiling = r.ceiling;
      m.querySelector('.g-mtu-msg').textContent = `23–${r.ceiling} on this firmware. A new value applies to the next connection.`;
    } catch (e) {
      m.querySelector('.g-mtu-msg').textContent = e.code === 'unknown_op' ? 'This firmware cannot change the MTU (1.84 or later can).' : e.message;
    }
  }

  async _saveMtu() {
    const m = this._mtuMenu;
    const v = Number(m.querySelector('.g-mtu-in').value);
    const msg = m.querySelector('.g-mtu-msg');
    try {
      const r = await this.ws.call('bt.mtu', { max_mtu: v });
      msg.textContent = r.reboot_needed
        ? `Saved ${v}. Raising it back to the maximum takes a reboot of the gateway.`
        : `Saved: new connections ask for ${r.max_mtu}.${this.addr ? ' Reconnect to use it now.' : ''}`;
    } catch (e) {
      msg.textContent = e.message;
    }
  }

  async _reconnect() {
    const addr = this.addr;
    const msg = this._mtuMenu.querySelector('.g-mtu-msg');
    if (!addr) return;
    msg.textContent = 'Reconnecting…';
    try {
      await this.ws.call('dev.disconnect', { device: addr });
      for (let i = 0; i < 50 && this.store.get(addr)?.connected; i++) await new Promise((r) => setTimeout(r, 100));
      await this.ws.call('dev.connect', { device: addr });
      msg.textContent = 'Reconnecting — the MTU shows here once it is up.';
    } catch (e) {
      msg.textContent = e.message;
    }
  }

  // --- the events split (tabbed view only) ---------------------------------------------

  _splitPref() {
    const p = this.app.prefs.gattSplit || {};
    return { on: !!p.on, axis: p.axis === 'y' ? 'y' : 'x', ratio: Number.isFinite(p.ratio) ? clamp(p.ratio, 0.15, 0.85) : 0.55 };
  }

  _setSplit(patch) {
    this.app.setPref('gattSplit', { ...this._splitPref(), ...patch });
    this._layoutSplit();
  }

  // Shown only in tabbed view, where the GATT Events window cannot sit beside
  // this one; in tiled view, use that window.
  _layoutSplit() {
    const tabs = !!this.app.tabsMode;
    const p = this._splitPref();
    const on = tabs && p.on;
    this._evBtn.hidden = !tabs;
    this._evBtn.setAttribute('aria-pressed', String(on));
    this._evBtn.classList.toggle('on', on);
    this._axisBtn.hidden = !on;
    this._axisBtn.textContent = p.axis === 'x' ? '⇅ Stack' : '⇄ Side by side';
    this._split.hidden = !on;
    this._host.hidden = !on;
    this._body.dataset.axis = p.axis;
    this._body.classList.toggle('is-split', on);
    this._tree.style.flexBasis = on ? `${(p.ratio * 100).toFixed(1)}%` : '';
    if (on && !this._events) {
      this._events = new GattEventsView(this._host, this.ctx, { embedded: true });
      if (this.keys) this._events.attachKeys(this.keys);
    }
    if (on && this._splitAxis !== p.axis) {
      this._splitter?.destroy();
      this._splitAxis = p.axis;
      this._split.dataset.axis = p.axis;
      let start = 0, total = 1;
      this._splitter = makeSplitter(this._split, {
        axis: p.axis,
        onStart: () => {
          const r = this._body.getBoundingClientRect();
          const t = this._tree.getBoundingClientRect();
          total = p.axis === 'x' ? r.width : r.height;
          start = p.axis === 'x' ? t.width : t.height;
        },
        onDrag: (d) => { this._tree.style.flexBasis = `${(clamp((start + d) / total, 0.15, 0.85) * 100).toFixed(1)}%`; },
        onEnd: () => {
          const t = this._tree.getBoundingClientRect();
          this._setSplit({ ratio: clamp((p.axis === 'x' ? t.width : t.height) / total, 0.15, 0.85) });
        },
      });
    }
  }

  // --- rendering ------------------------------------------------------------------------

  onShow() { this._struct = true; this._dirty = true; this._fill(this.addr); }

  onResize() { this._dirty = true; }

  render() {
    const tabs = !!this.app.tabsMode;
    if (tabs !== this._tabs) { this._tabs = tabs; this._layoutSplit(); }
    if (this._events && !this._host.hidden) { this._events.follow(this.addr); this._events.render(); }
    if (!this._dirty) return;
    this._dirty = false;

    const conn = this.store.connected();
    if (!this.addr || !conn.some((d) => d.addr === this.addr)) {
      const next = conn[0]?.addr || '';
      if (next !== this.addr) { this.addr = next; this._struct = true; this._fill(next); }
    }
    if (this.addr !== this._filled) { this._filled = this.addr; this._fill(this.addr); }
    const sig = this._treeSig();
    if (sig !== this._sig) { this._sig = sig; this._struct = true; }
    this._renderBar(conn);
    if (this._struct) this._rebuild();
    this._paint();
  }

  _treeSig() {
    return `${this.addr}#${this._services().map((s) => `${s.id}:${s.uuid}:${Array.isArray(s.characteristics)
      ? s.characteristics.map((c) => `${c.id}=${c.uuid}`).join(',') : '?'}`).join(';')}`;
  }

  _renderBar(conn) {
    const keep = this.addr;
    const opts = conn.map((d) => [d.addr, d.name ? `${d.name} · ${d.addr}` : d.addr]);
    if (this._dev.options.length !== opts.length || [...this._dev.options].some((o, i) => o.value !== opts[i][0] || o.textContent !== opts[i][1])) {
      this._dev.replaceChildren(...opts.map(([v, t]) => { const o = document.createElement('option'); o.value = v; o.textContent = t; return o; }));
    }
    this._dev.value = keep;
    const g = this._gatt();
    this._note.textContent = !conn.length ? 'no device connected'
      : !g ? 'discovering…' : g.truncated ? 'TRUNCATED — raise the GATT_MAX_* caps' : '';
    this._mtuBtn.textContent = g?.mtu ? `MTU ${g.mtu}` : 'MTU';
    const full = !!this.app.prefs.gattUuid128;
    this._u128.setAttribute('aria-pressed', String(full));
    this._u128.classList.toggle('on', full);
  }

  _label(uuid) {
    const s = document.createElement('span');
    s.className = 't-uuid';
    const u = uuidDisplay(uuid, !!this.app.prefs.gattUuid128);
    s.textContent = u.text;
    s.title = u.title;
    if (u.is128 && u.text !== u.title) s.dataset.full = u.title;   // tap for the whole UUID
    return s;
  }

  _rebuild() {
    this._struct = false;
    this._fullShown = !!this.app.prefs.gattUuid128;
    const frag = document.createDocumentFragment();
    const live = new Set();
    for (const s of this._services()) {
      const sk = `svc:${s.id}`;
      const sr = document.createElement('div');
      sr.className = 't-row t-svc';
      sr.dataset.key = sk;
      sr.setAttribute('aria-expanded', String(!this.shut.has(sk)));
      const caret = document.createElement('span');
      caret.className = 't-caret';
      caret.textContent = this.shut.has(sk) ? '▸' : '▾';
      const n = document.createElement('span');
      n.className = 't-count dim';
      n.textContent = Array.isArray(s.characteristics) ? `${s.characteristics.length}` : 'loading…';
      sr.append(caret, this._label(s.uuid), n);
      frag.appendChild(sr);
      if (this.shut.has(sk) || !Array.isArray(s.characteristics)) continue;
      for (const c of s.characteristics) {
        const ck = `chr:${c.id}`;
        const cr = document.createElement('div');
        cr.className = 't-row t-chr';
        cr.dataset.key = ck;
        cr.dataset.id = c.id;
        const isOpen = this.open.has(ck);
        cr.setAttribute('aria-expanded', String(isOpen));
        const cc = document.createElement('span');
        cc.className = 't-caret';
        cc.textContent = isOpen ? '▾' : '▸';
        const badges = document.createElement('span');
        badges.className = 't-flags';
        const f = new Set(c.flags || []);
        badges.textContent = BADGES.filter(([k]) => f.has(k)).map(([, b]) => b).join(' ');
        badges.title = permissions(c.flags).join(', ');
        const v = document.createElement('span');
        v.className = 't-val';
        cr.append(cc, this._label(c.uuid), badges, v);
        frag.appendChild(cr);
        if (isOpen) {
          const p = this._panel(s, c);
          live.add(`${this.addr}|${c.id}`);
          frag.appendChild(p.el);
        }
      }
    }
    // A panel whose characteristic is gone (another device, a new tree) goes too.
    for (const k of [...this._panels.keys()]) if (!k.startsWith(`${this.addr}|`)) this._panels.delete(k);
    // Re-attaching a panel moves its elements, which drops focus: put it back.
    const ae = document.activeElement;
    const inTree = ae && this._tree.contains(ae);
    this._tree.replaceChildren(frag);
    if (inTree && ae.isConnected && document.activeElement !== ae) ae.focus({ preventScroll: true });
  }

  _paint() {
    for (const rowEl of this._tree.querySelectorAll('.t-row')) {
      const on = rowEl.dataset.key === this.sel;
      if (on) rowEl.setAttribute('aria-selected', 'true'); else rowEl.removeAttribute('aria-selected');
    }
    for (const r of this._rows()) {
      if (!r.chr) continue;
      const rowEl = this._tree.querySelector(`.t-chr[data-id="${CSS.escape(r.chr.id)}"]`);
      const last = this.log.last(this.addr, r.chr.id);
      const v = rowEl?.querySelector('.t-val');
      if (v) v.textContent = last ? formatValue(last.hex, this._fmt(r.svc, r.chr)).slice(0, 40) : '';
      if (this.open.has(r.key)) {
        const p = this._panels.get(`${this.addr}|${r.chr.id}`);
        if (p) this._paintPanel(r.svc, r.chr, p);
      }
    }
    if (this._scrollSel) {
      this._scrollSel = false;
      this._tree.querySelector('.t-row[aria-selected]')?.scrollIntoView({ block: 'nearest' });
    }
    // Ages tick over without any event, so keep repainting while panels are open.
    if (this.open.size) this._dirty = true;
  }
}
