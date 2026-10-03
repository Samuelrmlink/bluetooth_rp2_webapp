// The two windows every device gets, drawn from its plugin's declarations:
//
//   Controls      the plugin's control list -- switches, selects, numbers,
//                 readouts -- built with the control kit
//   Unclassified  every telemetry field no control claims yet: the working
//                 surface for figuring out what an unknown tag means
//
// ctx: { ws, bus (this window's own scope), device, plugin, log }.
// device: { key, name, addr, sample, seen, gone }.

import { buildControl } from '../controls/kit.js';
import { Grid } from '../controls/grid.js';
import { copyWithToast, openCellMenu } from '../controls/cellmenu.js';
import { toast } from '../toast.js';
import { Pending, same, unclassified, displayValue } from '../controls/state.js';

function header(dev) {
  const h = document.createElement('div');
  h.className = 'pane-bar dev-head';
  h.innerHTML = '<span class="dev-name"></span><span class="dim dev-id"></span><span class="spacer"></span><span class="dim dev-age"></span>';
  h.querySelector('.dev-name').textContent = dev.name;
  h.querySelector('.dev-id').textContent = [dev.sample.product_code, dev.addr].filter(Boolean).join(' · ');
  return h;
}

function ageText(dev) {
  if (!dev.seen) return '';
  const s = Math.round((Date.now() - dev.seen) / 1000);
  return dev.gone ? `not heard for ${s} s` : s < 2 ? 'live' : `${s} s ago`;
}

export class ControlsWindow {
  constructor(el, ctx) {
    this.ctx = ctx;
    this.dev = ctx.device;
    this.plugin = ctx.plugin;
    this.pending = new Pending();
    this.controls = [];
    el.appendChild(header(this.dev));
    this._age = el.querySelector('.dev-age');
    const body = document.createElement('div');
    body.className = 'dev-controls';
    el.appendChild(body);

    const writable = typeof this.plugin.write === 'function';
    for (const c of this.plugin.controls || []) {
      const ctl = buildControl(c, (v) => this._write(c, ctl, v));
      if (c.kind !== 'readout') {
        if (!writable) ctl.setEnabled(false, 'no write path yet');
        else if (c.blocked) ctl.setEnabled(false, c.blocked);
        else if (c.verified === false) ctl.setEnabled(false, c.why || 'not yet verified on hardware');
      }
      this.controls.push({ c, ctl });
      body.appendChild(ctl.el);
    }
    if (!this.controls.length) {
      body.innerHTML = '<div class="dim pane-empty">This device\'s plugin declares no controls yet — see Unclassified.</div>';
    }
  }

  async _write(c, ctl, value) {
    if (this.dev.gone) { ctl.flash('device not heard from'); return; }
    // A control may ask before a value that is hard to undo from here
    // (`confirm: { [value]: 'question' }`): AC off can cut the gateway's
    // own network, and then nothing here can switch it back on.
    const ask = c.confirm?.[value];
    if (ask && !window.confirm(ask)) { ctl.setPending(null); return; }
    this.pending.begin(c.field, value, Date.now(), c.tolerance);
    ctl.setPending(value);
    try {
      await this.plugin.write(this.ctx, c, value);
    } catch (e) {
      this.pending.cancel(c.field);
      ctl.setPending(null);
      ctl.flash(`refused: ${e.message}`);
      this.ctx.log('!', `${this.dev.name}: ${c.label || c.field}: ${e.message}`);
    }
  }

  update(sample) {
    const { confirmed, expired } = this.pending.observe(sample);
    for (const { c, ctl } of this.controls) {
      if (!(c.field in sample)) continue;
      if (this.pending.get(c.field)) continue;       // still waiting: keep showing the target
      ctl.set(sample[c.field]);
      if (confirmed.includes(c.field)) ctl.setPending(null);
    }
    for (const x of expired) {
      const hit = this.controls.find((k) => k.c.field === x.field);
      if (!hit) continue;
      hit.ctl.setPending(null);
      hit.ctl.set(x.actual);
      if (!same(x.actual, x.target)) hit.ctl.flash(`device kept ${x.actual ?? 'its value'}`);
    }
  }

  render() {
    for (const { ctl } of this.controls) ctl.tick?.();
    this._age.textContent = ageText(this.dev);
    this._age.classList.toggle('is-gone', !!this.dev.gone);
  }
}

export class UnclassifiedWindow {
  constructor(el, ctx) {
    this.ctx = ctx;
    this.app = ctx.app;
    this.dev = ctx.device;
    this.plugin = ctx.plugin;
    this.changed = new Map();       // field -> time it last changed
    this.prev = {};
    el.appendChild(header(this.dev));
    this._age = el.querySelector('.dev-age');
    const wrap = document.createElement('div');
    wrap.className = 'table-wrap';
    wrap.innerHTML = '<table class="grid dev-fields"><thead></thead><tbody></tbody></table>';
    el.appendChild(wrap);
    this._wrap = wrap;
    this._tbody = wrap.querySelector('tbody');
    this._fields = [];
    // A cell's menu, as in GATT Events and the Scanner: the whole value in a
    // wrapping box that copies on a tap, and Copy row. A click opens it, and
    // in the table (Space, then h j k l) Enter does.
    this._menu = document.createElement('div');
    this._menu.className = 'pane-settings s-menu';
    this._menu.hidden = true;
    el.appendChild(this._menu);
    this._menuCell = null;
    this._tbody.addEventListener('click', (e) => this._onClick(e));
    // Copy all: the table as it is shown (order, columns, sort), as tab-
    // separated text under a line naming the device -- pastes into a report
    // or a spreadsheet.
    const copy = document.createElement('button');
    copy.type = 'button';
    copy.className = 'pane-btn dev-copy';
    copy.textContent = 'Copy all';
    copy.title = 'Copy every field shown to the clipboard';
    copy.addEventListener('click', () => this.copyAll());
    el.querySelector('.dev-head').appendChild(copy);
    // One remembered layout for every device's Unclassified table: the
    // columns mean the same whichever device fills them.
    this._cols = [
      { key: 'field', label: 'Field', width: 200, get: (f) => f },
      { key: 'value', label: 'Value', width: 200, get: (f) => displayValue(this.dev.sample[f]) },
      { key: 'changed', label: 'Changed', width: 90, type: 'num', get: (f) => (this.changed.has(f) ? Date.now() - this.changed.get(f) : null),
        show: (f) => (this.changed.get(f) ? `${Math.round((Date.now() - this.changed.get(f)) / 1000)} s ago` : '') },
    ];
    this.grid = new Grid(wrap.querySelector('table'), {
      id: 'unclassified', columns: this._cols, onChange: () => { this._dirty = true; },
    });
    this._dirty = true;
  }

  attachKeys(keys) {
    keys.nav.registerList(this._wrap, this.grid.controller({
      list: this._wrap,
      count: () => this._fields.length,
      activate: (i, key, td) => this._openMenu(this._fields[i], key, td),
      onChange: () => { this._dirty = true; },
    }));
  }

  _cellText(f, c) {
    return String((c.show ? c.show(f) : c.get(f)) ?? '');
  }

  _onClick(e) {
    const tr = e.target.closest('tr');
    const td = e.target.closest('td');
    if (!tr || !td) return;
    const i = [...this._tbody.rows].indexOf(tr);
    const col = [...tr.cells].indexOf(td);
    if (i < 0 || col < 0 || col >= this.grid.visible.length) return;
    this.grid.cursor = { row: i + 1, col };
    this._openMenu(this._fields[i], this.grid.visible[col].key, td);
  }

  _openMenu(f, key, anchor) {
    if (f == null || !anchor || !this.app) return;
    const col = this._cols.find((c) => c.key === key);
    // the value column: the field's whole value (the cell may be cut short)
    const value = col ? this._cellText(f, col) : '';
    const items = [
      { label: 'Copy row', fn: () => copyWithToast(this.grid.visible.map((c) => this._cellText(f, c)).join('\t'), 'Row copied') },
    ];
    if (key !== 'field') items.push({ label: `Copy the field name (${f})`, fn: () => copyWithToast(f, 'Field name copied') });
    const opened = openCellMenu(this.app, {
      anchor, menuEl: this._menu, paneId: 'unclassified-cell', wrap: this._wrap,
      title: `${this.dev.name} · ${f}`,
      items, value: value === '\u2014' ? '' : value,
      onReturn: () => { this._dirty = true; },
      onClose: () => { this._menuCell = null; this._dirty = true; },
    });
    if (opened) { this._menuCell = { field: f, key }; anchor.classList.add('is-menu-cell'); }
  }

  copyAll() {
    const fields = this.grid.sort(unclassified(this.dev.sample, this.plugin.controls));
    if (!fields.length) { toast('Nothing to copy'); return; }
    const cols = this.grid.visible;
    const id = [this.dev.name, this.dev.sample.product_code, this.dev.addr].filter(Boolean).join(' · ');
    const lines = [`# ${id} — ${new Date().toISOString()}`, cols.map((c) => c.label).join('\t'),
      ...fields.map((f) => cols.map((c) => String((c.show ? c.show(f) : c.get(f)) ?? '')).join('\t'))];
    copyWithToast(lines.join('\n'), `Copied ${fields.length} field${fields.length === 1 ? '' : 's'}`);
  }

  update(sample) {
    const now = Date.now();
    for (const [k, v] of Object.entries(sample)) {
      if (k in this.prev && !same(this.prev[k], v) && String(this.prev[k]) !== String(v)) this.changed.set(k, now);
    }
    this.prev = { ...sample };
    this._dirty = true;
  }

  render() {
    this._age.textContent = ageText(this.dev);
    this._age.classList.toggle('is-gone', !!this.dev.gone);
    if (!this._dirty && !this._flashing) return;
    // Not while a cell's menu is open: telemetry re-draws the rows several
    // times a second, and the menu is anchored to its cell (the value shown
    // in it is the one it was opened on).
    if (this._menuCell) return;
    this._dirty = false;
    const now = Date.now();
    this._flashing = false;
    const fields = this.grid.sort(unclassified(this.dev.sample, this.plugin.controls));
    this._fields = fields;
    this._tbody.replaceChildren(...fields.map((f) => {
      const tr = document.createElement('tr');
      const at = this.changed.get(f);
      if (at && now - at < 1500) { tr.classList.add('just-changed'); this._flashing = true; }
      for (const c of this.grid.visible) {
        const td = document.createElement('td');
        td.textContent = c.show ? c.show(f) : c.get(f);
        tr.appendChild(td);
      }
      this.grid.appendSpacer(tr);
      return tr;
    }));
    this.grid.paintCursor();
  }
}

export const BUILTIN_KINDS = { controls: ControlsWindow, unclassified: UnclassifiedWindow };
