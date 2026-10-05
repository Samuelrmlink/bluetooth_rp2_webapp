// The control kit: one builder per kind a device plugin can declare.
//
//   switch    on/off                         { on: 1, off: 0 }
//   select    one of a list                  { options: [[value, label], ...] }
//   number    a value you type               { min, max, step, unit }
//   slider    a value you drag               { min, max, step, unit }
//   readout   a value you only read          { unit, digits, format(v) }
//   button    an action                      { value }
//   timer     a countdown in seconds         { max, steps: [[secs, label], ...] }
//
// Every one is an ordinary focusable form element, so hjkl moves between
// them, Space presses, a <select> opens the keyboard popup, and a range
// steps with h/l -- nav.js already knows all of it. Each returns
//   { el, set(value), setPending(target|null), setEnabled(ok, why) }
// and calls onWrite(value) when the user asks for a change; what a write
// means, and whether it is allowed yet, is the device window's business.
// A timer also has tick(), called every frame: between telemetry samples it
// counts down locally, so the shown time does not stall for a second or two.

import { parseDuration, formatDuration } from './duration.js';

function row(label, input, extra) {
  const r = document.createElement('div');
  r.className = 'ctl';
  const l = document.createElement('label');
  l.className = 'ctl-label';
  l.textContent = label;
  const v = document.createElement('div');
  v.className = 'ctl-value';
  v.appendChild(input);
  if (extra) v.appendChild(extra);
  const note = document.createElement('span');
  note.className = 'ctl-note dim';
  v.appendChild(note);
  r.append(l, v);
  return { r, note };
}

function fmt(c, v) {
  if (v == null || v === '') return '—';
  if (c.format) return c.format(v);
  const n = Number(v);
  const s = Number.isFinite(n) && c.digits != null ? n.toFixed(c.digits) : String(v);
  return c.unit ? `${s} ${c.unit}` : s;
}

export function buildControl(c, onWrite) {
  let input;
  let set;
  let current;
  let tick = null;
  let extraEls = [];
  const unit = document.createElement('span');
  unit.className = 'ctl-unit dim';

  switch (c.kind) {
    case 'switch': {
      input = document.createElement('button');
      input.type = 'button';
      input.className = 'ctl-switch';
      input.setAttribute('role', 'switch');
      const on = c.on ?? 1, off = c.off ?? 0;
      input.addEventListener('click', () => onWrite(input.getAttribute('aria-checked') === 'true' ? off : on));
      set = (v) => {
        const isOn = v != null && !!Number(v === true ? 1 : v);
        input.setAttribute('aria-checked', String(isOn));
        input.textContent = v == null ? '—' : isOn ? 'On' : 'Off';
      };
      break;
    }
    case 'select': {
      input = document.createElement('select');
      for (const [v, label] of c.options || []) {
        const o = document.createElement('option');
        o.value = String(v);
        o.textContent = label;
        input.appendChild(o);
      }
      input.addEventListener('change', () => onWrite(Number.isNaN(Number(input.value)) ? input.value : Number(input.value)));
      // force: put the device's value back even with the select focused (a
      // cancelled or refused write -- otherwise it kept showing the choice
      // that was never made)
      set = (v, force) => {
        if (v == null) return;
        if (![...input.options].some((o) => o.value === String(v))) {
          // The device reported a value the plugin did not list: show it
          // rather than silently displaying the wrong option.
          const o = document.createElement('option');
          o.value = String(v);
          o.textContent = `${v} (reported)`;
          input.appendChild(o);
        }
        if (force || document.activeElement !== input) input.value = String(v);
      };
      break;
    }
    case 'number':
    case 'slider': {
      input = document.createElement('input');
      input.type = c.kind === 'slider' ? 'range' : 'number';
      for (const k of ['min', 'max', 'step']) if (c[k] != null) input[k] = c[k];
      unit.textContent = c.unit || '';
      if (c.kind === 'slider') unit.className = 'ctl-unit';
      // Commit on change (Enter, blur, slider release), never per keystroke.
      input.addEventListener('change', () => onWrite(Number(input.value)));
      set = (v) => {
        if (v == null || document.activeElement === input) return;
        input.value = String(v);
        if (c.kind === 'slider') unit.textContent = fmt(c, v);
      };
      if (c.kind === 'slider') input.addEventListener('input', () => { unit.textContent = fmt(c, input.value); });
      break;
    }
    case 'button': {
      input = document.createElement('button');
      input.type = 'button';
      input.className = 'pane-btn';
      input.textContent = c.action || 'Send';
      input.addEventListener('click', () => onWrite(c.value ?? 1));
      set = () => {};
      break;
    }
    case 'timer': {
      // The time left, as h:mm:ss -- also the box to type a new one into.
      // The steps start from the time left NOW, not the last sample.
      input = document.createElement('input');
      input.type = 'text';
      input.className = 'ctl-timer';
      input.inputMode = 'text';
      input.autocomplete = 'off';
      input.spellcheck = false;
      input.title = 'Type a time: 1:30, 90m, 2h, 1h30m, or minutes. Enter sends it.';
      const max = c.max ?? 86400;
      let left = null, at = 0, shown = '';
      const now = () => (left == null ? null : Math.max(0, left - Math.floor((Date.now() - at) / 1000)));
      const show = () => {
        if (document.activeElement === input) return;
        const t = now();
        const txt = t == null ? '' : t ? formatDuration(t) : 'off';
        if (txt !== shown) { shown = txt; input.value = txt; }
      };
      const send = (secs) => onWrite(Math.max(0, Math.min(max, Math.round(secs))));
      input.addEventListener('focus', () => { if (input.value === 'off') input.value = ''; input.select(); });
      input.addEventListener('keydown', (e) => {
        if (e.key === 'Escape' && input.value !== shown) { e.stopPropagation(); input.value = shown; }
      });
      input.addEventListener('change', () => {
        const v = input.value.trim() === 'off' ? 0 : parseDuration(input.value);
        if (v == null) { shown = ''; input.blur(); show(); api.flash(`"${input.value}" is not a time`); return; }
        if (v > max) { shown = ''; input.blur(); show(); api.flash(`at most ${formatDuration(max)}`); return; }
        send(v);
        input.blur();
      });
      input.addEventListener('blur', () => { shown = ''; show(); });
      const steps = c.steps || [[-900, '\u221215 m'], [900, '+15 m'], [3600, '+1 h'], [0, 'Cancel']];
      for (const [d, label] of steps) {
        const b = document.createElement('button');
        b.type = 'button';
        b.className = 'pane-btn ctl-step';
        b.textContent = label;
        b.dataset.step = String(d);
        // c.absolute: the steps are presets (the A2687 app's Off / 1 / 2 / 3 h),
        // not amounts to add to the time left
        b.addEventListener('click', () => send(d === 0 ? 0 : c.absolute ? d : (now() || 0) + d));
        extraEls.push(b);
      }
      set = (v) => {
        if (v == null) return;
        left = Number(v);
        at = Date.now();
        show();
      };
      tick = show;
      break;
    }
    default: {                                   // readout
      // A button that looks like text: a keyboard stop (hjkl reach it), and
      // Enter / Space / a click open its menu (the device window's: copy
      // the value, as in Telemetry).
      input = document.createElement('button');
      input.type = 'button';
      input.className = 'ctl-readout';
      input.title = 'Copy…';
      set = (v) => { input.textContent = fmt(c, v); };
    }
  }

  let extra = unit.textContent || c.kind === 'slider' ? unit : null;
  if (extraEls.length) {
    extra = document.createElement('span');
    extra.className = 'ctl-steps';
    extra.append(...extraEls);
  }
  const { r, note } = row(c.label || c.field, input, extra);
  if (c.kind === 'readout') r.classList.add('is-readout');
  let why = '';

  const api = {
    el: r,
    set(v) { current = v; set(v); },
    setPending(target) {
      r.classList.toggle('is-pending', target != null);
      if (target != null) set(target);
      else set(current, true);
    },
    setEnabled(ok, reason = '') {
      if (c.kind === 'readout') return;
      input.disabled = !ok;
      for (const b of extraEls) b.disabled = !ok;
      why = reason;
      note.textContent = why;
    },
    flash(text) {
      note.textContent = text;
      clearTimeout(this._t);
      this._t = setTimeout(() => { note.textContent = why; }, 4000);
    },
  };
  if (tick) api.tick = tick;
  return api;
}
