// The A2687's Ports window: one card per USB-C port, as the Anker app lays
// them out -- the watts large, volts and amps under them, the cable and what
// is plugged in, then the port's switch and its auto-off timer -- and below
// the cards the charger's power settings: charging mode, Standard's option,
// the custom mode's per-port watts, and the fast-charge protocols per port.
//
// Writes go through the driver's anker.set (docs/a2687-protocol.md). The
// cards' controls are the shared control kit, so a write shows as pending
// until the charger's own telemetry confirms it (devices/windows.js); the two
// editors (custom mode, protocols) send several values at once and say so
// in their own status line.

import { ControlsWindow } from '../windows.js';

// --- pure rules (tested in tests/unit/scp.test.js) ------------------------------

export const PROTOCOLS = [
  ['ufcs', 0, 'UFCS'], ['scp', 1, 'SCP'], ['pd12v', 2, 'PD 12V'],
  ['pps11', 3, '5–11V PPS'], ['pps16', 4, '5–16V PPS'], ['pps21', 5, '4.5–21V PPS'],
];
export const PROTOCOLS_DEFAULT = 59;      // 0x3B: everything but PD 12V, as the app ships
const PD_BITS = 0x3c;                    // PD 12V and the three PPS ranges

const bitsOf = (m) => { let n = 0; for (let x = m; x; x >>= 1) n += x & 1; return n; };

// Can this protocol bit be switched ON in this mask? At most three of the
// four PD bits at once -- the app greys the fourth out.
export function canEnable(mask, bit) {
  if (mask & (1 << bit)) return true;
  if (!((1 << bit) & PD_BITS)) return true;
  return bitsOf(mask & PD_BITS) < 3;
}

// The custom mode's watts: each port 0 or 15..140, 160 in all. null when fine.
export function customProblem({ c1_w, c2_w, c3_w }) {
  const ws = [c1_w, c2_w, c3_w].map(Number);
  for (const [i, w] of ws.entries()) {
    if (!Number.isInteger(w) || (w !== 0 && (w < 15 || w > 140))) return `C${i + 1} takes 0 or 15–140 W`;
  }
  const total = ws.reduce((a, b) => a + b, 0);
  if (total > 160) return `${total} W in all — the charger has 160`;
  return null;
}

// What a port is doing, in a word: the card's status chip.
export function portState(power, on) {
  if (on === 0 || on === false) return 'off';
  if (!power || typeof power !== 'object') return '';
  if (power.watts > 0.05 || power.ma > 0) return 'charging';
  return power.mv ? 'idle' : 'empty';
}

export const watts = (w) => (w == null ? '—' : `${Number(w).toFixed(1)} W`);
export function voltsAmps(p) {
  if (!p || typeof p !== 'object' || (!p.mv && !p.ma)) return '— V · — A';
  return `${(p.mv / 1000).toFixed(2)} V · ${(p.ma / 1000).toFixed(2)} A`;
}
export function cable(c) {
  if (!c || typeof c !== 'object') return '—';
  return c.cable === 'none' ? 'no cable' : `${c.cable} cable`;
}
export function device(d, n) {
  const v = d && typeof d === 'object' ? d[`c${n}`] : null;
  if (!v || v === 'none') return 'not identified';
  return `brand ${v.brand} · model ${v.model}`;
}

// --- the window ------------------------------------------------------------------

const PRESETS = [[0, 'Off'], [3600, '1 h'], [7200, '2 h'], [10800, '3 h']];

function portControls(n) {
  return [
    { kind: 'readout', field: `c${n}_w`, label: 'Output', format: watts, port: n, part: 'watts' },
    { kind: 'readout', field: `c${n}_power`, label: 'Voltage · current', format: voltsAmps, port: n },
    { kind: 'readout', field: `c${n}_control`, label: 'Cable', format: cable, port: n },
    { kind: 'readout', field: 'connected_device', label: 'Device', format: (d) => device(d, n), port: n },
    { kind: 'switch', field: `c${n}_on`, label: 'Output on', port: n, verified: true, confirmOk: 'Turn off',
      confirm: (v, s) => (v === 0
        ? `Turn off USB-C${n}?${s[`c${n}_w`] > 0.05 ? `\n\nIt is delivering ${watts(s[`c${n}_w`])} right now.` : ''}\nAnything plugged into it loses power.`
        : null) },
    { kind: 'timer', field: `c${n}_timer_s`, display: `c${n}_timer_remain_s`, label: 'Auto-off',
      port: n, max: 86400, steps: PRESETS, absolute: true, verified: true },
  ];
}

const CHARGER = [
  { kind: 'select', field: 'charging_protocol', label: 'Charging mode', verified: true,
    options: [[0, 'AI Mode 2.0'], [1, 'Standard'], [2, 'Custom (the profile below)']] },
  { kind: 'select', field: 'fixed_allocation', label: 'Standard mode', verified: true,
    options: [['c1_priority', 'C1 port priority'], ['laptop_mode', 'Dual-laptop']] },
];

export class PortsWindow extends ControlsWindow {
  constructor(el, ctx) {
    super(el, ctx);
    el.classList.add('scp-ports-pane');
    this._buildCustom();
    this._buildProtocols();
  }

  controlList() { return [1, 2, 3].flatMap(portControls).concat(CHARGER); }

  place(c, ctl, body) {
    if (!this._cards) {
      this._cards = document.createElement('div');
      this._cards.className = 'scp-cards';
      this._settings = document.createElement('section');
      this._settings.className = 'scp-section scp-power';
      this._settings.innerHTML = '<h3>Power</h3>';
      body.append(this._cards, this._settings);
      this._card = {};
    }
    if (!c.port) { this._settings.appendChild(ctl.el); return; }
    let card = this._card[c.port];
    if (!card) {
      card = document.createElement('section');
      card.className = 'scp-card';
      card.dataset.port = String(c.port);
      card.innerHTML = `<header><span class="scp-port">C${c.port}</span><span class="scp-state dim"></span></header>`;
      this._cards.appendChild(card);
      this._card[c.port] = card;
    }
    if (c.part === 'watts') ctl.el.classList.add('scp-watts');
    card.appendChild(ctl.el);
  }

  update(sample) {
    super.update(sample);
    for (const n of [1, 2, 3]) {
      const card = this._card?.[n];
      if (!card) continue;
      const st = portState(sample[`c${n}_power`], sample[`c${n}_on`]);
      card.dataset.state = st;
      card.querySelector('.scp-state').textContent = st;
    }
    this._fillCustom(sample);
    this._fillProtocols(sample);
    this._watchApplied(sample);
  }

  // Several values at once: the editor's own status line says whether the
  // charger took them, from the telemetry.
  _status(el, text, kind = '') {
    el.textContent = text;
    el.className = `scp-status ${kind}`;
  }

  async _send(field, value, statusEl, check) {
    this._status(statusEl, 'sending…');
    try {
      await this.plugin.write(this.ctx, { field }, value);
      this._waiting = { statusEl, check, until: Date.now() + 8000 };
      this._status(statusEl, 'sent — waiting for the charger…');
    } catch (e) {
      this._status(statusEl, `refused: ${e.message}`, 'is-error');
    }
  }

  _watchApplied(sample) {
    const w = this._waiting;
    if (!w) return;
    if (w.check(sample)) { this._status(w.statusEl, 'applied', 'is-ok'); this._waiting = null; }
    else if (Date.now() > w.until) { this._status(w.statusEl, 'the charger did not confirm it', 'is-error'); this._waiting = null; }
  }

  // --- custom mode: per-port watts within the 160 W budget ---------------------

  _buildCustom() {
    const s = document.createElement('section');
    s.className = 'scp-section scp-custom';
    s.innerHTML = `
      <h3>Custom mode</h3>
      <p class="dim">Each port 0 W (off in this mode) or 15–140 W; 160 W in all. Applying it switches the charger to Custom.</p>
      ${[1, 2, 3].map((n) => `
        <label class="scp-w"><span>C${n}</span>
          <input type="range" min="0" max="140" step="5" data-port="${n}" aria-label="C${n} watts">
          <input type="number" min="0" max="140" step="1" data-port="${n}" class="scp-wn" aria-label="C${n} watts, exact">
          <span class="dim">W</span></label>`).join('')}
      <div class="scp-row"><span class="scp-total"></span>
        <label class="scp-check"><input type="checkbox" class="scp-auto"> Automatic deactivation</label></div>
      <p class="dim scp-hint">Automatic deactivation: a port set to 0 W that gets used ends Custom mode.</p>
      <div class="scp-row"><button type="button" class="pane-btn scp-apply-custom">Apply custom mode</button>
        <span class="scp-status"></span></div>`;
    this.body.appendChild(s);
    this._custom = s;
    this._customDirty = false;
    const sync = (from) => {
      for (const n of [1, 2, 3]) {
        const r = s.querySelector(`input[type=range][data-port="${n}"]`);
        const t = s.querySelector(`.scp-wn[data-port="${n}"]`);
        if (from === 'range') t.value = r.value; else r.value = t.value;
      }
      this._customDirty = true;
      this._customTotal();
    };
    s.querySelectorAll('input[type=range]').forEach((i) => i.addEventListener('input', () => sync('range')));
    s.querySelectorAll('.scp-wn').forEach((i) => i.addEventListener('input', () => sync('number')));
    s.querySelector('.scp-auto').addEventListener('change', () => { this._customDirty = true; });
    s.querySelector('.scp-apply-custom').addEventListener('click', () => this._applyCustom());
  }

  _customValue() {
    const s = this._custom;
    const w = (n) => Number(s.querySelector(`.scp-wn[data-port="${n}"]`).value);
    const mode = Number(this.dev.sample?.custom_mode_number);
    return { mode: mode >= 1 && mode <= 254 ? mode : 1, auto_exit: s.querySelector('.scp-auto').checked ? 1 : 0,
             c1_w: w(1), c2_w: w(2), c3_w: w(3) };
  }

  _customTotal() {
    const v = this._customValue();
    const total = v.c1_w + v.c2_w + v.c3_w;
    const problem = customProblem(v);
    const el = this._custom.querySelector('.scp-total');
    el.textContent = `Total ${total} W / 160 W${problem ? ` — ${problem}` : ''}`;
    el.classList.toggle('is-error', !!problem);
    this._custom.querySelector('.scp-apply-custom').disabled = !!problem;
  }

  _fillCustom(sample) {
    const cp = sample.custom_power;
    if (!cp || typeof cp !== 'object' || this._customDirty) return;
    const s = this._custom;
    for (const n of [1, 2, 3]) {
      const v = cp[`c${n}_w`];
      if (v == null) continue;
      s.querySelector(`input[type=range][data-port="${n}"]`).value = String(v);
      s.querySelector(`.scp-wn[data-port="${n}"]`).value = String(v);
    }
    s.querySelector('.scp-auto').checked = !!cp.auto_exit;
    this._customTotal();
  }

  _applyCustom() {
    const v = this._customValue();
    if (customProblem(v)) return;
    this._customDirty = false;
    this._send('custom_mode', v, this._custom.querySelector('.scp-status'), (s) =>
      s.charging_protocol === 2 && s.custom_power?.c1_w === v.c1_w && s.custom_power?.c2_w === v.c2_w
      && s.custom_power?.c3_w === v.c3_w);
  }

  // --- fast-charge protocols, per port -----------------------------------------

  _buildProtocols() {
    const s = document.createElement('section');
    s.className = 'scp-section scp-protocols';
    s.innerHTML = `
      <h3>Charging protocols</h3>
      <p class="dim scp-proto-note"></p>
      <table class="scp-proto">
        <thead><tr><th></th><th>C1</th><th>C2</th><th>C3</th></tr></thead>
        <tbody>${PROTOCOLS.map(([key, bit, label]) => `
          <tr><th scope="row">${label}</th>${[1, 2, 3].map((n) => `
            <td><input type="checkbox" data-port="${n}" data-bit="${bit}" aria-label="${label} on C${n}"></td>`).join('')}</tr>`).join('')}
        </tbody>
      </table>
      <div class="scp-row"><button type="button" class="pane-btn scp-apply-proto">Save protocols</button>
        <span class="scp-status"></span></div>`;
    this.body.appendChild(s);
    this._proto = s;
    this._protoDirty = false;
    this._masks = { c1: PROTOCOLS_DEFAULT, c2: PROTOCOLS_DEFAULT, c3: PROTOCOLS_DEFAULT };
    s.addEventListener('change', (e) => {
      const box = e.target.closest('input[type=checkbox]');
      if (!box) return;
      const k = `c${box.dataset.port}`;
      const bit = 1 << Number(box.dataset.bit);
      this._masks[k] = box.checked ? this._masks[k] | bit : this._masks[k] & ~bit;
      this._protoDirty = true;
      this._paintProtocols();
    });
    s.querySelector('.scp-apply-proto').addEventListener('click', () => {
      const v = { ...this._masks };
      this._protoDirty = false;
      this._send('protocols', v, s.querySelector('.scp-status'),
        (x) => x.protocols && x.protocols.c1 === v.c1 && x.protocols.c2 === v.c2 && x.protocols.c3 === v.c3);
    });
    this._paintProtocols();
  }

  _fillProtocols(sample) {
    const known = sample.protocols && typeof sample.protocols === 'object';
    this._proto.querySelector('.scp-proto-note').textContent = known
      ? 'As this gateway last set them (the charger does not report them).'
      : 'Not known: the charger does not report them and this gateway has not set them since it connected. Shown: the default; saving sends all three ports.';
    if (known && !this._protoDirty) {
      this._masks = { c1: sample.protocols.c1, c2: sample.protocols.c2, c3: sample.protocols.c3 };
      this._paintProtocols();
    }
  }

  _paintProtocols() {
    for (const box of this._proto.querySelectorAll('input[type=checkbox]')) {
      const m = this._masks[`c${box.dataset.port}`];
      const bit = Number(box.dataset.bit);
      box.checked = !!(m & (1 << bit));
      box.disabled = !canEnable(m, bit);
      box.title = box.disabled ? 'At most three of PD 12V and the PPS ranges' : '';
    }
  }
}
