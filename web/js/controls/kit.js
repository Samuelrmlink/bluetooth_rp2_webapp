// The control kit: one builder per kind a device plugin can declare.
//
//   switch    on/off                         { on: 1, off: 0 }
//   select    one of a list                  { options: [[value, label], ...] }
//   number    a value you type               { min, max, step, unit }
//   slider    a value you drag               { min, max, step, unit }
//   readout   a value you only read          { unit, digits, format(v) }
//   button    an action                      { value }
//
// Every one is an ordinary focusable form element, so hjkl moves between
// them, Space presses, a <select> opens the keyboard popup, and a range
// steps with h/l -- nav.js already knows all of it. Each returns
//   { el, set(value), setPending(target|null), setEnabled(ok, why) }
// and calls onWrite(value) when the user asks for a change; what a write
// means, and whether it is allowed yet, is the device window's business.

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
      set = (v) => {
        if (v == null) return;
        if (![...input.options].some((o) => o.value === String(v))) {
          // The device reported a value the plugin did not list: show it
          // rather than silently displaying the wrong option.
          const o = document.createElement('option');
          o.value = String(v);
          o.textContent = `${v} (reported)`;
          input.appendChild(o);
        }
        if (document.activeElement !== input) input.value = String(v);
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
    default: {                                   // readout
      input = document.createElement('span');
      input.className = 'ctl-readout';
      set = (v) => { input.textContent = fmt(c, v); };
    }
  }

  const { r, note } = row(c.label || c.field, input, unit.textContent || c.kind === 'slider' ? unit : null);
  if (c.kind === 'readout') r.classList.add('is-readout');
  let why = '';

  return {
    el: r,
    set(v) { current = v; set(v); },
    setPending(target) {
      r.classList.toggle('is-pending', target != null);
      if (target != null) set(target);
      else set(current);
    },
    setEnabled(ok, reason = '') {
      if (c.kind === 'readout') return;
      input.disabled = !ok;
      why = reason;
      note.textContent = why;
    },
    flash(text) {
      note.textContent = text;
      clearTimeout(this._t);
      this._t = setTimeout(() => { note.textContent = why; }, 4000);
    },
  };
}
