// The GATT window: a connected device's services and characteristics, and
// what can be done to one -- read, write, notify, indicate -- with a log of
// every value that arrives.
//
// The tree lives in the shared DeviceStore. Discovery (gatt.discovered) only
// SUMMARISES each service -- a characteristic count, not the characteristics
// -- because a whole tree can exceed the 4 KB websocket frame. So the detail
// is fetched here one service at a time with gatt.tree, which also covers a
// page opened after the device connected, whose tree it never saw at all.
//
// The tree is a keyboard list: j/k walk characteristics, Enter reads the
// selected one.

import { shortUuid, uuidLabel, hexToAscii } from '../gattdb.js';

const LOG_MAX = 500;

export class GattWindow {
  constructor(el, ctx) {
    this.app = ctx.app;
    this.ws = ctx.ws;
    this.store = ctx.app.devices;
    this.addr = '';                 // device shown
    this.sel = null;                // {id, uuid, flags}
    this.values = [];               // char.value log for the shown device
    this._fetching = new Set();     // addr:service in flight
    this._dirty = true;

    el.innerHTML = `
      <div class="pane-bar">
        <select class="g-dev" aria-label="Device"></select>
        <span class="dim g-note"></span>
        <span class="spacer"></span>
        <button type="button" class="pane-btn g-disconnect">Disconnect</button>
      </div>
      <div class="gatt-body">
        <div class="gatt-tree" role="tree"></div>
        <div class="gatt-char">
          <div class="g-target dim">Select a characteristic.</div>
          <div class="g-actions">
            <button type="button" class="pane-btn g-read">Read</button>
            <button type="button" class="pane-btn g-notify">Notify</button>
            <button type="button" class="pane-btn g-indicate">Indicate</button>
            <button type="button" class="pane-btn g-off">Unsubscribe</button>
          </div>
          <form class="g-write">
            <input class="g-hex" type="text" placeholder="hex to write" spellcheck="false" autocomplete="off">
            <label class="dim"><input type="checkbox" class="g-resp" checked> with response</label>
            <button type="submit" class="pane-btn">Write</button>
          </form>
          <div class="table-wrap"><table class="grid g-log"><thead><tr>
            <th>Time</th><th>Source</th><th>Len</th><th>Hex</th><th>ASCII</th></tr></thead><tbody></tbody></table></div>
        </div>
      </div>`;
    this._dev = el.querySelector('.g-dev');
    this._note = el.querySelector('.g-note');
    this._tree = el.querySelector('.gatt-tree');
    this._target = el.querySelector('.g-target');
    this._logBody = el.querySelector('.g-log tbody');
    this._dev.addEventListener('change', () => this.showDevice(this._dev.value));
    el.querySelector('.g-disconnect').addEventListener('click', () => {
      if (this.addr) this.ws.call('dev.disconnect', { device: this.addr }).catch((e) => this.app.log('!', e.message));
    });
    el.querySelector('.g-read').addEventListener('click', () => this._op('gatt.read'));
    el.querySelector('.g-notify').addEventListener('click', () => this._op('gatt.subscribe', { mode: 'notify' }));
    el.querySelector('.g-indicate').addEventListener('click', () => this._op('gatt.subscribe', { mode: 'indicate' }));
    el.querySelector('.g-off').addEventListener('click', () => this._op('gatt.subscribe', { mode: 'off' }));
    el.querySelector('.g-write').addEventListener('submit', (e) => {
      e.preventDefault();
      const hex = el.querySelector('.g-hex').value.replace(/\s+/g, '');
      if (!/^([0-9a-fA-F]{2})+$/.test(hex)) { this.app.log('!', 'write: not whole bytes of hex'); return; }
      this._op('gatt.write', { value: hex, response: el.querySelector('.g-resp').checked });
    });
    this._tree.addEventListener('click', (e) => {
      const row = e.target.closest('[data-id]');
      if (row) { this._select(row.dataset.id); }
    });

    this.store.subscribe(() => { this._dirty = true; });
    ctx.bus.on('char.value', (d) => {
      this.values.push({ t: new Date(), addr: d.addr, id: d.id, src: d.source, len: d.len, hex: d.value || '' });
      if (this.values.length > LOG_MAX) this.values.splice(0, this.values.length - LOG_MAX);
      this._dirty = true;
    });
    ctx.bus.on('gatt.discovered', (d) => this._fill(d.addr));
  }

  attachKeys(keys) {
    keys.nav.registerList(this._tree, {
      enter: () => { if (!this.sel) this._step(1); return true; },
      move: (dlt) => this._step(dlt),
      first: () => this._stepTo(0),
      last: () => this._stepTo(this._chars().length - 1),
      count: () => this._chars().length,
      activate: () => this._op('gatt.read'),
    });
  }

  // From the scanner (l / double-click) or the app.
  showDevice(addr) {
    this.addr = addr || '';
    this.sel = null;
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
          this._dirty = true;
        } finally {
          this._fetching.delete(key);
        }
      }
    } catch (e) {
      if (e.code !== 'discovering') this.app.log('!', `gatt.tree ${addr}: ${e.message}`);
    }
  }

  _chars() {
    const g = this.store.get(this.addr)?.gatt;
    const out = [];
    for (const s of g?.services || []) for (const c of (Array.isArray(s.characteristics) ? s.characteristics : [])) out.push(c);
    return out;
  }

  _select(id) {
    const c = this._chars().find((x) => x.id === id);
    if (!c) return false;
    this.sel = { id: c.id, uuid: c.uuid, flags: c.flags || [] };
    this._dirty = true;
    this._scrollSel = true;
    return true;
  }

  _step(delta) {
    const cs = this._chars();
    if (!cs.length) return false;
    const i = cs.findIndex((c) => c.id === this.sel?.id);
    return this._select(cs[Math.max(0, Math.min(cs.length - 1, (i < 0 ? -1 : i) + delta))].id);
  }

  _stepTo(i) {
    const cs = this._chars();
    return cs.length ? this._select(cs[Math.max(0, Math.min(cs.length - 1, i))].id) : false;
  }

  async _op(op, extra = {}) {
    if (!this.addr || !this.sel) { this.app.log('!', 'select a characteristic first'); return; }
    try {
      await this.ws.call(op, { device: this.addr, id: this.sel.id, ...extra });
    } catch (e) {
      this.app.log('!', `${op}: ${e.code || ''} ${e.message}`);
    }
  }

  onShow() { this._dirty = true; this._fill(this.addr); }

  render() {
    if (!this._dirty) return;
    this._dirty = false;

    const conn = this.store.connected();
    if (!this.addr || !conn.some((d) => d.addr === this.addr)) this.addr = conn[0]?.addr || '';
    // The device is often chosen HERE, on first render, rather than by
    // showDevice() -- so fetching only from onShow()/showDevice() missed it:
    // onShow runs before the first render, with no device picked yet, and
    // the tree was never asked for. Fetch whenever the choice settles on a
    // different device.
    if (this.addr !== this._filled) {
      this._filled = this.addr;
      this._fill(this.addr);
    }
    const keep = this.addr;
    this._dev.replaceChildren(...conn.map((d) => {
      const o = document.createElement('option');
      o.value = d.addr;
      o.textContent = d.name ? `${d.name} · ${d.addr}` : d.addr;
      return o;
    }));
    this._dev.value = keep;

    const d = this.store.get(this.addr);
    const g = d?.gatt;
    this._note.textContent = !conn.length ? 'no device connected'
      : !g ? 'discovering…'
        : `mtu ${g.mtu}${g.truncated ? ' · TRUNCATED — raise the GATT_MAX_* caps' : ''}`;

    const frag = document.createDocumentFragment();
    for (const s of g?.services || []) {
      const sd = document.createElement('div');
      sd.className = 't-svc';
      sd.textContent = uuidLabel(s.uuid);
      frag.appendChild(sd);
      if (!Array.isArray(s.characteristics)) {
        const w = document.createElement('div');
        w.className = 't-chr dim';
        w.textContent = 'loading…';
        frag.appendChild(w);
        continue;
      }
      for (const c of s.characteristics) {
        const cd = document.createElement('div');
        cd.className = 't-chr';
        cd.dataset.id = c.id;
        if (this.sel?.id === c.id) cd.setAttribute('aria-selected', 'true');
        const name = document.createElement('span');
        name.textContent = uuidLabel(c.uuid);
        const fl = document.createElement('span');
        fl.className = 't-flags';
        fl.textContent = (c.flags || []).join(' ');
        cd.append(name, fl);
        if (c.value) {
          const v = document.createElement('span');
          v.className = 't-val';
          v.textContent = c.value.slice(0, 32);
          cd.appendChild(v);
        }
        frag.appendChild(cd);
      }
    }
    this._tree.replaceChildren(frag);
    if (this._scrollSel) {
      this._scrollSel = false;
      this._tree.querySelector('[aria-selected]')?.scrollIntoView({ block: 'nearest' });
    }

    this._target.textContent = this.sel
      ? `${this.sel.id}  ${shortUuid(this.sel.uuid)}  [${this.sel.flags.join(', ')}]`
      : 'Select a characteristic.';

    const rows = this.values.filter((v) => v.addr === this.addr).slice(-200).reverse();
    this._logBody.replaceChildren(...rows.map((r) => {
      const tr = document.createElement('tr');
      for (const v of [r.t.toTimeString().slice(0, 8), `${r.id} ${r.src}`, r.len, r.hex, hexToAscii(r.hex)]) {
        const td = document.createElement('td');
        td.textContent = v;
        tr.appendChild(td);
      }
      return tr;
    }));
  }
}
