// The two windows every device gets, drawn from its plugin's declarations:
//
//   Controls      the plugin's control list -- switches, selects, numbers,
//                 readouts -- built with the control kit
//   Telemetry      every telemetry field, and what shows it -- or (Unclassified
//                 only) the fields no control claims yet: the working
//                 surface for figuring out what an unknown tag means
//
// ctx: { ws, bus (this window's own scope), device, plugin, log }.
// device: { key, name, addr, sample, seen, gone }.

import { buildControl } from '../controls/kit.js';
import { Grid } from '../controls/grid.js';
import { columnsDialog } from '../controls/columnsdialog.js';
import { copyWithToast, openCellMenu } from '../controls/cellmenu.js';
import { toast } from '../toast.js';
import { askConfirm } from '../controls/confirm.js';
import { Pending, same, unclassified, displayValue, shownAs, allFields } from '../controls/state.js';

const ONLY_KEY = 'btgw.alldata.unclassifiedOnly';

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
    this.app = ctx.app;
    this.dev = ctx.device;
    this.plugin = ctx.plugin;
    this.pending = new Pending();
    this.controls = [];
    el.appendChild(header(this.dev));
    this._age = el.querySelector('.dev-age');
    const body = document.createElement('div');
    body.className = 'dev-controls';
    el.appendChild(body);

    this.body = body;
    const writable = typeof this.plugin.write === 'function';
    for (const c of this.controlList()) {
      const ctl = buildControl(c, (v) => this._write(c, ctl, v));
      if (c.kind !== 'readout') {
        if (!writable) ctl.setEnabled(false, 'no write path yet');
        else if (c.blocked) ctl.setEnabled(false, c.blocked);
        else if (c.verified === false) ctl.setEnabled(false, c.why || 'not yet verified on hardware');
      }
      this.controls.push({ c, ctl });
      this.place(c, ctl, body);
    }
    if (!this.controls.length) {
      body.innerHTML = '<div class="dim pane-empty">This device\'s plugin declares no controls yet — see Telemetry.</div>';
    }
    // A readout's menu: its whole value in the wrapping box that copies, as
    // in Telemetry and GATT Events (a click, or Enter / Space on it).
    this._menu = document.createElement('div');
    this._menu.className = 'pane-settings s-menu';
    this._menu.hidden = true;
    el.appendChild(this._menu);
    body.addEventListener('click', (e) => {
      const b = e.target.closest('.ctl-readout');
      if (!b) return;
      const hit = this.controls.find((k) => k.ctl.el.contains(b));
      if (hit) this._openMenu(hit.c, b);
    });
  }

  // What this window shows (a subclass shows another list -- the A2687's Ports).
  controlList() { return this.plugin.controls || []; }

  // Where a control goes (a subclass groups them -- one card per port).
  place(c, ctl, body) { body.appendChild(ctl.el); }

  _openMenu(c, anchor) {
    if (!this.app) return;
    const label = c.label || c.field;
    const value = anchor.textContent.trim();
    const raw = this.dev.sample?.[c.field];
    const items = [{ label: `Copy "${label}: ${value}"`, fn: () => copyWithToast(`${label}: ${value}`, 'Copied') }];
    if (raw != null && String(raw) !== value) items.push({ label: `Copy the raw value (${displayValue(raw)})`, fn: () => copyWithToast(String(raw), 'Copied') });
    items.push({ label: `Copy the field name (${c.field})`, fn: () => copyWithToast(c.field, 'Field name copied') });
    const opened = openCellMenu(this.app, {
      anchor, menuEl: this._menu, paneId: 'controls-readout', title: `${this.dev.name} · ${label}`,
      items, value,
      onClose: () => anchor.classList.remove('is-menu-cell'),
    });
    if (opened) anchor.classList.add('is-menu-cell');
  }

  async _write(c, ctl, value) {
    if (this.dev.gone) { ctl.flash('device not heard from'); return; }
    // A control may ask before a value that is hard to undo from here
    // (`confirm: { [value]: 'question' }`): AC off can cut the gateway's
    // own network, and then nothing here can switch it back on.
    // (a string per value, or a function of the value and the latest sample)
    const ask = typeof c.confirm === 'function' ? c.confirm(value, this.dev.sample || {}) : c.confirm?.[value];
    if (ask && !(await askConfirm(this.app, { title: c.label || c.field, message: ask, ok: c.confirmOk || 'Yes' }))) {
      ctl.setPending(null);
      return;
    }
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
      // `display`: the field shown, when it is not the one a write confirms
      // against (a timer shows the time left, and confirms the time set)
      const shown = c.display || c.field;
      if (!(shown in sample)) continue;
      if (this.pending.get(c.field)) continue;       // still waiting: keep showing the target
      ctl.set(sample[shown]);
      if (confirmed.includes(c.field)) ctl.setPending(null);
    }
    for (const x of expired) {
      const hit = this.controls.find((k) => k.c.field === x.field);
      if (!hit) continue;
      hit.ctl.setPending(null);
      hit.ctl.set(hit.c.display ? this.dev.sample?.[hit.c.display] : x.actual);
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
    // Telemetry shows every field; this narrows it to the ones nothing shows
    // yet (where unknown fields get worked out). Remembered.
    const only = document.createElement('label');
    only.className = 'dev-only';
    only.innerHTML = '<input type="checkbox"> Unclassified only';
    this._onlyBox = only.querySelector('input');
    try { this._onlyBox.checked = localStorage.getItem(ONLY_KEY) === '1'; } catch { /* default: all */ }
    this._onlyBox.addEventListener('change', () => {
      try { localStorage.setItem(ONLY_KEY, this._onlyBox.checked ? '1' : '0'); } catch { /* not remembered */ }
      this._dirty = true;
    });
    this._shownAs = shownAs(this.plugin.controls);
    el.querySelector('.dev-head').append(only, copy);
    // One remembered layout for every device's Unclassified table: the
    // columns mean the same whichever device fills them.
    this._cols = [
      { key: 'field', label: 'Field', width: 200, get: (f) => f },
      { key: 'value', label: 'Value', width: 200, get: (f) => displayValue(this.dev.sample[f]) },
      { key: 'shown', label: 'Shown as', width: 130, get: (f) => this._shownAs(f) },
      { key: 'changed', label: 'Changed', width: 90, type: 'num', get: (f) => (this.changed.has(f) ? Date.now() - this.changed.get(f) : null),
        show: (f) => (this.changed.get(f) ? `${Math.round((Date.now() - this.changed.get(f)) / 1000)} s ago` : '') },
    ];
    this.grid = new Grid(wrap.querySelector('table'), {
      id: 'unclassified', columns: this._cols, onChange: () => { this._dirty = true; },
    });
    // As every table: sort, resize and reorder from the header (mouse, finger
    // or keyboard) and show / hide columns here -- remembered.
    if (this.app) {
      this.columnsDlg = columnsDialog(this.app, this.grid, { id: `alldata-columns-${this.dev.key || this.dev.addr || 'dev'}` });
      const cb = document.createElement('button');
      cb.type = 'button';
      cb.className = 'pane-btn dev-cols';
      cb.textContent = 'Columns';
      cb.title = 'Show or hide columns (drag a header to move one, its edge to resize)';
      cb.addEventListener('click', () => this.columnsDlg.toggle());
      el.querySelector('.dev-head').insertBefore(cb, el.querySelector('.dev-copy'));
    }
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
      // the mark goes with the menu: cells are updated in place now, so
      // nothing else would ever take it off (it stuck, cell after cell)
      onClose: () => { this._menuCell = null; anchor.classList.remove('is-menu-cell'); this._dirty = true; },
    });
    if (opened) { this._menuCell = { field: f, key }; anchor.classList.add('is-menu-cell'); }
  }

  // The rows: every field, or only those nothing shows yet.
  _list() {
    return this.grid.sort(this._onlyBox.checked ? unclassified(this.dev.sample, this.plugin.controls) : allFields(this.dev.sample));
  }

  copyAll() {
    const fields = this._list();
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
    const fields = this._list();
    const cols = this.grid.visible;
    // The same rows in the same order and columns: update the cells in
    // place. Rebuilt every frame (something is always changing when every
    // field is shown), a row could be replaced between a mouse press and
    // its release, and the click was lost.
    const shape = `${cols.map((c) => c.key).join(',')}|${fields.join(',')}`;
    const inPlace = shape === this._shape && this._tbody.rows.length === fields.length;
    this._shape = shape;
    this._fields = fields;
    const fill = (tr, f) => {
      tr.classList.toggle('is-unclassified', !this._shownAs(f));
      const at = this.changed.get(f);
      const flash = !!at && now - at < 1500;
      tr.classList.toggle('just-changed', flash);
      if (flash) this._flashing = true;
      cols.forEach((c, i) => {
        const text = String((c.show ? c.show(f) : c.get(f)) ?? '');
        if (tr.cells[i].textContent !== text) tr.cells[i].textContent = text;
        // marked only while its menu is open
        tr.cells[i].classList.toggle('is-menu-cell', this._menuCell?.field === f && this._menuCell?.key === c.key);
      });
    };
    if (inPlace) {
      fields.forEach((f, i) => fill(this._tbody.rows[i], f));
    } else {
      this._tbody.replaceChildren(...fields.map((f) => {
        const tr = document.createElement('tr');
        for (let i = 0; i < cols.length; i++) tr.appendChild(document.createElement('td'));
        this.grid.appendSpacer(tr);
        fill(tr, f);
        return tr;
      }));
    }
    this.grid.paintCursor();
  }
}

export const BUILTIN_KINDS = { controls: ControlsWindow, unclassified: UnclassifiedWindow };
