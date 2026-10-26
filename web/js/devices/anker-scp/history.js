// The A2687's History window: the last six hours of each port's output, read
// from the charger itself (0x020C through the driver's anker.history op --
// docs/a2687-protocol.md). 73 records, five
// minutes apart, oldest first, the last one "now": per port mV and mA, or no
// reading (an empty port).
//
// One chart, one axis: Total (the ports summed), or Power, Current or Voltage
// per port. Lines are 2 px, a missing reading is a gap, never a fake zero;
// hover (or the keyboard: h / l, Shift+H / L an hour) puts a crosshair on a
// point with every port's value; Enter opens the copy menu for it; the
// numbers are also in a table. Port colours: the dataviz dark categorical
// slots 1-3, validated on this surface (#111113): CVD dE >= 9.4, >= 3:1.

import { copyWithToast, openCellMenu } from '../../controls/cellmenu.js';

export const PORT_COLORS = ['#3987e5', '#d95926', '#199e70'];
const RECORD = 12;

// --- pure (tests/unit/scp.test.js) ---------------------------------------------

// A packet's hex -> records [{ c1: {mv, ma} | null, c2, c3 }]
export function recordsFromHex(hex, n) {
  const out = [];
  for (let i = 0; i < n; i++) {
    const rec = {};
    for (let p = 0; p < 3; p++) {
      const at = (i * RECORD + p * 4) * 2;
      const u16 = (o) => parseInt(hex.slice(at + o + 2, at + o + 4) + hex.slice(at + o, at + o + 2), 16);
      const mv = u16(0), ma = u16(4);
      rec[`c${p + 1}`] = Number.isNaN(mv) || (mv === 0xffff && ma === 0xffff) ? null : { mv, ma };
    }
    out.push(rec);
  }
  return out;
}

export const METRICS = {
  total: { label: 'Total', unit: 'W', digits: 1, of: (r) => r && (r.mv * r.ma) / 1e6 },
  w: { label: 'Power', unit: 'W', digits: 1, of: (r) => r && (r.mv * r.ma) / 1e6 },
  a: { label: 'Current', unit: 'A', digits: 2, of: (r) => r && r.ma / 1000 },
  v: { label: 'Voltage', unit: 'V', digits: 2, of: (r) => r && r.mv / 1000 },
};

// The lines to draw: [{ name, color, values: [number | null] }]
export function seriesOf(records, metric) {
  const m = METRICS[metric];
  if (metric === 'total') {
    return [{ name: 'Total', color: PORT_COLORS[0], values: records.map((rec) => {
      const parts = [1, 2, 3].map((p) => m.of(rec[`c${p}`])).filter((x) => x != null);
      return parts.length ? parts.reduce((a, b) => a + b, 0) : null;
    }) }];
  }
  return [1, 2, 3].map((p) => ({ name: `C${p}`, color: PORT_COLORS[p - 1], values: records.map((rec) => m.of(rec[`c${p}`]) ?? null) }));
}

// A clean axis top and its ticks: 0 .. top in 4 steps of 1/2/2.5/5 x 10^n.
export function axis(max) {
  if (!(max > 0)) return { top: 1, ticks: [0, 0.25, 0.5, 0.75, 1] };
  const raw = max / 4;
  const p = 10 ** Math.floor(Math.log10(raw));
  const step = [1, 2, 2.5, 5, 10].map((k) => k * p).find((s) => s >= raw);
  const top = step * Math.ceil(max / step);
  const ticks = [];
  for (let v = 0; v <= top + 1e-9; v += step) ticks.push(Math.round(v * 1e6) / 1e6);
  return { top, ticks };
}

// The time of record i of n, the last being `endMs`, `intervalS` apart.
export const timeOf = (i, n, endMs, intervalS = 300) => endMs - (n - 1 - i) * intervalS * 1000;
const hhmm = (ms) => new Date(ms).toTimeString().slice(0, 5);

// --- the window --------------------------------------------------------------------

const NS = 'http://www.w3.org/2000/svg';
const el = (tag, attrs = {}) => {
  const e = document.createElementNS(NS, tag);
  for (const [k, v] of Object.entries(attrs)) e.setAttribute(k, v);
  return e;
};

export class HistoryWindow {
  constructor(root, ctx) {
    this.ctx = ctx;
    this.app = ctx.app;
    this.dev = ctx.device;
    this.metric = 'total';
    this.records = [];
    this.cursor = -1;
    this._read = null;                       // { id, records, intervalS }
    root.classList.add('scp-history-pane');
    root.innerHTML = `
      <div class="pane-bar dev-head"><span class="dev-name"></span><span class="dim dev-id"></span></div>
      <div class="hs">
        <div class="hs-bar">
          <span class="hs-seg" role="group" aria-label="What to show">
            ${Object.entries(METRICS).map(([k, m]) => `<button type="button" class="pane-btn hs-metric" data-metric="${k}" aria-pressed="${k === 'total'}">${m.label}</button>`).join('')}
          </span>
          <button type="button" class="pane-btn hs-refresh">Read again</button>
          <span class="hs-status dim"></span>
        </div>
        <div class="hs-legend"></div>
        <div class="hs-plot" tabindex="0" aria-label="Output history chart: h and l move along it, Enter copies a point">
          <svg class="hs-svg" role="img"></svg>
          <div class="hs-tip" hidden></div>
        </div>
        <details class="hs-table-wrap"><summary>Table</summary><table class="hs-table"></table></details>
      </div>
      <div class="pane-settings s-menu" hidden></div>`;
    const q = (s) => root.querySelector(s);
    q('.dev-name').textContent = this.dev.name;
    q('.dev-id').textContent = [this.dev.sample?.product_code, this.dev.addr].filter(Boolean).join(' · ');
    this.q = q;
    this.svg = q('.hs-svg');
    this.plot = q('.hs-plot');
    this.tip = q('.hs-tip');
    this.menu = q('.s-menu');
    root.querySelectorAll('.hs-metric').forEach((b) => b.addEventListener('click', () => {
      this.metric = b.dataset.metric;
      root.querySelectorAll('.hs-metric').forEach((x) => x.setAttribute('aria-pressed', String(x === b)));
      this.draw();
    }));
    q('.hs-refresh').addEventListener('click', () => this.read());
    this.plot.addEventListener('pointermove', (e) => this._hover(e));
    this.plot.addEventListener('pointerleave', () => { if (document.activeElement !== this.plot) this._setCursor(-1); });
    this.plot.addEventListener('click', (e) => { this._hover(e); this._openMenu(); });
    new ResizeObserver(() => this.draw()).observe(this.plot);
    ctx.bus.on('profile.anker.history', (ev) => this._event(ev));
    this.read();
  }

  // --- reading -------------------------------------------------------------------

  async read() {
    this._status('Reading the last 6 h from the charger…');
    this.q('.hs-refresh').disabled = true;
    try {
      const r = await this.ctx.ws.call('anker.history', { device: this.dev.addr });
      if (r?.error) throw new Error(r.message || r.error);
    } catch (e) {
      this._status(`Could not read it: ${e.message}`, 'is-error');
      this.q('.hs-refresh').disabled = false;
    }
  }

  _event(ev) {
    if (!ev || String(ev.addr).toUpperCase() !== String(this.dev.addr).toUpperCase()) return;
    if (ev.hex) {
      if (!this._read || this._read.id !== ev.id || ev.seq === 1) this._read = { id: ev.id, records: [], intervalS: ev.interval_s || 300 };
      this._read.records.push(...recordsFromHex(ev.hex, ev.n));
      this._status(`Reading… ${this._read.records.length} points`);
    }
    if (ev.done) {
      this.q('.hs-refresh').disabled = false;
      if (ev.error) { this._status(`The read stopped: ${ev.error}`, 'is-error'); return; }
      if (!this._read) return;
      this.records = this._read.records;
      this.intervalS = this._read.intervalS;
      this.endMs = Date.now();
      this.cursor = -1;
      this._status(`Read at ${hhmm(this.endMs)} · ${this.records.length} points, ${this.intervalS / 60} min apart`);
      this.draw();
      this._table();
    }
  }

  _status(text, kind = '') {
    const s = this.q('.hs-status');
    s.textContent = text;
    s.className = `hs-status ${kind || 'dim'}`;
  }

  // --- drawing -------------------------------------------------------------------

  draw() {
    const svg = this.svg;
    svg.replaceChildren();
    const W = Math.max(280, this.plot.clientWidth), H = Math.max(180, Math.min(360, Math.round(W * 0.42)));
    svg.setAttribute('viewBox', `0 0 ${W} ${H}`);
    svg.setAttribute('width', W);
    svg.setAttribute('height', H);
    const n = this.records.length;
    const legend = this.q('.hs-legend');
    if (!n) { legend.replaceChildren(); svg.setAttribute('aria-label', 'No history read yet'); return; }
    const m = METRICS[this.metric];
    const series = seriesOf(this.records, this.metric);
    this.series = series;
    const max = Math.max(0, ...series.flatMap((s) => s.values.filter((v) => v != null)));
    const { top, ticks } = axis(max);
    const pad = { l: 44, r: 58, t: 10, b: 24 };
    const x = (i) => pad.l + (n === 1 ? 0 : (i / (n - 1)) * (W - pad.l - pad.r));
    const y = (v) => pad.t + (1 - v / top) * (H - pad.t - pad.b);
    this.geom = { x, y, pad, W, H, n };
    // grid and y ticks: hairline, recessive
    for (const t of ticks) {
      svg.append(el('line', { x1: pad.l, x2: W - pad.r, y1: y(t), y2: y(t), class: 'hs-grid' }));
      const lab = el('text', { x: pad.l - 6, y: y(t) + 4, class: 'hs-tick', 'text-anchor': 'end' });
      lab.textContent = `${t}`;
      svg.append(lab);
    }
    const unit = el('text', { x: 4, y: pad.t + 4, class: 'hs-tick' });
    unit.textContent = m.unit;
    svg.append(unit);
    // x ticks: every hour from the end
    const perHour = 3600 / this.intervalS;
    for (let i = n - 1; i >= 0; i -= perHour) {
      const lab = el('text', { x: x(i), y: H - 6, class: 'hs-tick', 'text-anchor': 'middle' });
      lab.textContent = hhmm(timeOf(i, n, this.endMs, this.intervalS));
      svg.append(lab);
    }
    // the lines: a gap where a port had no reading
    const ends = [];
    for (const s of series) {
      let d = '', pen = false;
      s.values.forEach((v, i) => {
        if (v == null) { pen = false; return; }
        d += `${pen ? 'L' : 'M'}${x(i).toFixed(1)},${y(v).toFixed(1)}`;
        pen = true;
      });
      if (this.metric === 'total' && d) {
        // the one series gets a wash under it (~10 %)
        const first = s.values.findIndex((v) => v != null);
        const last = s.values.length - 1 - [...s.values].reverse().findIndex((v) => v != null);
        svg.append(el('path', { d: `${d}L${x(last).toFixed(1)},${y(0)}L${x(first).toFixed(1)},${y(0)}Z`, fill: s.color, 'fill-opacity': 0.1, stroke: 'none' }));
      }
      svg.append(el('path', { d, fill: 'none', stroke: s.color, 'stroke-width': 2, 'stroke-linejoin': 'round', 'stroke-linecap': 'round', class: 'hs-line' }));
      // the end: a dot, and its value as a direct label (the latest reading)
      const li = s.values.length - 1 - [...s.values].reverse().findIndex((v) => v != null);
      if (li < s.values.length && s.values[li] != null) {
        svg.append(el('circle', { cx: x(li), cy: y(s.values[li]), r: 4, fill: s.color, class: 'hs-dot' }));
        ends.push({ x: x(li) + 8, y: y(s.values[li]) + 4,
                    text: `${series.length > 1 ? `${s.name} ` : ''}${s.values[li].toFixed(m.digits)}` });
      }
    }
    // end labels never overprint: pushed apart to a line height, kept on the plot
    ends.sort((a, b) => a.y - b.y);
    for (let k = 1; k < ends.length; k++) ends[k].y = Math.max(ends[k].y, ends[k - 1].y + 13);
    const over = ends.length ? ends[ends.length - 1].y - (H - pad.b + 4) : 0;
    if (over > 0) ends.forEach((e) => { e.y -= over; });
    for (const e of ends) {
      const lab = el('text', { x: e.x, y: e.y, class: 'hs-end' });
      lab.textContent = e.text;
      svg.append(lab);
    }
    svg.setAttribute('aria-label', `${m.label} over the last ${Math.round((n - 1) * this.intervalS / 3600)} h`);
    // legend: always for more than one series; a single one is named by the toggle
    legend.innerHTML = series.length > 1
      ? series.map((s) => `<span class="hs-key"><i style="background:${s.color}"></i>${s.name}</span>`).join('') : '';
    this._cursorLayer = el('g', { class: 'hs-cursor' });
    svg.append(this._cursorLayer);
    this._setCursor(this.cursor);
  }

  // --- the crosshair, by pointer or keyboard ----------------------------------------

  _hover(e) {
    if (!this.geom) return;
    const r = this.svg.getBoundingClientRect();
    const px = ((e.clientX - r.left) / r.width) * this.geom.W;
    const { pad, W, n } = this.geom;
    const i = Math.round(((px - pad.l) / (W - pad.l - pad.r)) * (n - 1));
    this._setCursor(Math.max(0, Math.min(n - 1, i)));
  }

  _setCursor(i) {
    this.cursor = i;
    const g = this._cursorLayer;
    if (!g) return;
    g.replaceChildren();
    if (i < 0 || !this.geom || i >= this.geom.n) { this.tip.hidden = true; return; }
    const { x, y, pad, H } = this.geom;
    g.append(el('line', { x1: x(i), x2: x(i), y1: pad.t, y2: H - pad.b, class: 'hs-crosshair' }));
    for (const s of this.series) {
      const v = s.values[i];
      if (v != null) g.append(el('circle', { cx: x(i), cy: y(v), r: 4, fill: s.color, class: 'hs-dot' }));
    }
    const m = METRICS[this.metric];
    const t = hhmm(timeOf(i, this.geom.n, this.endMs, this.intervalS));
    this.tip.innerHTML = `<b>${t}</b>${this.series.map((s) => `<span><i style="background:${s.color}"></i>${s.name} ${s.values[i] == null ? '—' : `${s.values[i].toFixed(m.digits)} ${m.unit}`}</span>`).join('')}`;
    this.tip.hidden = false;
    const r = this.svg.getBoundingClientRect(), pr = this.plot.getBoundingClientRect();
    const left = (x(i) / this.geom.W) * r.width + (r.left - pr.left);
    this.tip.style.left = `${Math.min(left + 12, pr.width - this.tip.offsetWidth - 4)}px`;
    this.tip.style.top = '8px';
  }

  _pointText(i) {
    const m = METRICS[this.metric];
    const t = hhmm(timeOf(i, this.records.length, this.endMs, this.intervalS));
    return `${t}  ${this.series.map((s) => `${s.name} ${s.values[i] == null ? '—' : `${s.values[i].toFixed(m.digits)} ${m.unit}`}`).join('  ')}`;
  }

  _openMenu() {
    if (this.cursor < 0 || !this.app) return;
    const value = this._pointText(this.cursor);
    openCellMenu(this.app, {
      anchor: this.plot, menuEl: this.menu, paneId: 'history-point', wrap: this.plot,
      title: `${this.dev.name} · ${METRICS[this.metric].label}`, value,
      items: [{ label: 'Copy the whole table (tab-separated)', fn: () => copyWithToast(this._tsv(), 'Table copied') }],
    });
  }

  attachKeys(keys) {
    const step = (d) => {
      if (!this.records.length) return true;
      const i = this.cursor < 0 ? this.records.length - 1 : this.cursor + d;
      this._setCursor(Math.max(0, Math.min(this.records.length - 1, i)));
      return true;
    };
    keys.nav.registerList(this.plot, {
      enter: () => step(0),
      move: (d) => step(d),
      first: () => { this._setCursor(0); return true; },
      last: () => { this._setCursor(this.records.length - 1); return true; },
      count: () => this.records.length,
      activate: () => this._openMenu(),
      key: (e) => {
        if (e.altKey || e.ctrlKey || e.metaKey) return false;
        const dir = { KeyH: -1, ArrowLeft: -1, KeyL: 1, ArrowRight: 1 }[e.code];
        if (!dir) return false;
        return step(dir * (e.shiftKey ? 3600 / (this.intervalS || 300) : 1));
      },
    });
  }

  // --- the table view ------------------------------------------------------------------

  _rows() {
    const n = this.records.length;
    const f = (r, k) => (r ? k(r) : null);
    return this.records.map((rec, i) => {
      const ws = [1, 2, 3].map((p) => f(rec[`c${p}`], METRICS.w.of));
      const total = ws.filter((v) => v != null).reduce((a, b) => a + b, 0);
      return [hhmm(timeOf(i, n, this.endMs, this.intervalS)),
        ...[1, 2, 3].map((p) => (rec[`c${p}`] ? `${(rec[`c${p}`].mv / 1000).toFixed(2)} V ${(rec[`c${p}`].ma / 1000).toFixed(2)} A` : '—')),
        total.toFixed(2)];
    });
  }

  _tsv() {
    return [['time', 'C1', 'C2', 'C3', 'total W'], ...this._rows()].map((r) => r.join('\t')).join('\n');
  }

  _table() {
    const t = this.q('.hs-table');
    t.innerHTML = `<thead><tr><th>Time</th><th>C1</th><th>C2</th><th>C3</th><th>Total W</th></tr></thead>
      <tbody>${this._rows().reverse().map((r) => `<tr>${r.map((c) => `<td>${c}</td>`).join('')}</tr>`).join('')}</tbody>`;
  }

  update() {}
  render() {}
}
