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
        else if (c.verified === false) ctl.setEnabled(false, 'not yet verified on hardware');
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
    this.pending.begin(c.field, value);
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
    this._age.textContent = ageText(this.dev);
    this._age.classList.toggle('is-gone', !!this.dev.gone);
  }
}

export class UnclassifiedWindow {
  constructor(el, ctx) {
    this.ctx = ctx;
    this.dev = ctx.device;
    this.plugin = ctx.plugin;
    this.changed = new Map();       // field -> time it last changed
    this.prev = {};
    el.appendChild(header(this.dev));
    this._age = el.querySelector('.dev-age');
    const wrap = document.createElement('div');
    wrap.className = 'table-wrap';
    wrap.innerHTML = '<table class="grid dev-fields"><thead><tr><th>Field</th><th>Value</th><th>Changed</th></tr></thead><tbody></tbody></table>';
    el.appendChild(wrap);
    this._tbody = wrap.querySelector('tbody');
    this._dirty = true;
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
    this._dirty = false;
    const now = Date.now();
    this._flashing = false;
    const fields = unclassified(this.dev.sample, this.plugin.controls);
    this._tbody.replaceChildren(...fields.map((f) => {
      const tr = document.createElement('tr');
      const at = this.changed.get(f);
      if (at && now - at < 1500) { tr.classList.add('just-changed'); this._flashing = true; }
      const v = this.dev.sample[f];
      for (const text of [f, displayValue(v),
        at ? `${Math.round((now - at) / 1000)} s ago` : '']) {
        const td = document.createElement('td');
        td.textContent = text;
        tr.appendChild(td);
      }
      return tr;
    }));
  }
}

export const BUILTIN_KINDS = { controls: ControlsWindow, unclassified: UnclassifiedWindow };
