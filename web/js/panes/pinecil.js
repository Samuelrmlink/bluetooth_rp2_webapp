/*
 * The Pinecil pane: telemetry and setpoint control.
 *
 * The second device, and the point of having it. The firmware knows nothing
 * about soldering irons either -- this is driven by profile.pinecil.telemetry
 * from a Lua script, exactly as the charger is, and the two devices could not
 * be less alike underneath. The charger pushes encrypted notifications after
 * a five-stage handshake; the iron has no notify characteristic at all and is
 * polled in clear. Neither fact reached any C.
 */
import { normalize, freshness } from '../pinecil.js';

const $ = (id) => document.getElementById(id);

export function mountPinecilPane({ api, log }) {
  let last = null, lastAt = 0, raw = null;
  let pendingSet = null;   // setpoint we asked for, until telemetry confirms

  const el = {
    state: $('p-state'), ident: $('p-ident'),
    tip: $('p-tip'), set: $('p-set'), bar: $('p-bar'), mode: $('p-mode'),
    grid: $('p-grid'), input: $('p-setpoint'), rawBox: $('p-raw'),
  };

  function kv(parent, label, value) {
    const row = document.createElement('div');
    row.className = 'kv';
    const k = document.createElement('div'); k.className = 'k'; k.textContent = label;
    const v = document.createElement('div'); v.className = 'v'; v.textContent = value;
    row.append(k, v);
    parent.appendChild(row);
  }

  function render() {
    const f = freshness(lastAt);
    el.state.textContent = f.text;
    el.state.dataset.state = f.state;

    if (!last) {
      el.ident.textContent = 'no profile running — start the "pinecil" script';
      el.tip.textContent = '—';
      el.set.textContent = '';
      el.mode.textContent = '';
      el.bar.style.width = '0%';
      el.grid.replaceChildren();
      return;
    }

    el.ident.textContent = [last.serial, last.build, last.addr].filter(Boolean).join(' · ');
    el.tip.textContent = last.tipText;
    el.set.textContent = `of ${last.setpointText}`;
    el.bar.style.width = `${(last.progress * 100).toFixed(1)}%`;
    el.bar.parentElement.dataset.state = last.state;
    el.mode.textContent = `${last.mode ?? '?'} · ${last.state}`;
    el.mode.dataset.state = last.state;

    // The setpoint box is an input, so it must not fight the person typing in
    // it. Only follow the device while the box is not focused, and stop
    // showing a pending value once the device agrees.
    if (pendingSet !== null && last.setpoint === pendingSet) pendingSet = null;
    if (document.activeElement !== el.input && pendingSet === null) {
      el.input.value = last.setpoint ?? '';
    }

    const g = document.createDocumentFragment();
    kv(g, 'power', last.powerPct !== null ? `${last.powerPct}%  (${last.wattsText})` : '—');
    kv(g, 'input', `${last.inputText}  ${last.source ?? ''}`.trim());
    kv(g, 'handle', last.handleText);
    kv(g, 'tip resistance', last.tipOhms);
    kv(g, 'max tip temp', last.maxTip !== null ? `${last.maxTip} °C` : '—');
    kv(g, 'uptime', last.uptimeText);
    kv(g, 'idle for', last.idleText);
    if (last.hall !== null) kv(g, 'hall sensor', String(last.hall));
    el.grid.replaceChildren(g);

    if (el.rawBox && !el.rawBox.hidden) el.rawBox.textContent = JSON.stringify(raw, null, 1);
  }

  async function setTemp(celsius) {
    const c = Number(celsius);
    if (!Number.isFinite(c)) return;
    pendingSet = Math.round(c);
    el.state.textContent = `setting ${pendingSet} °C…`;
    try {
      const r = await api.call('pinecil.set_temp', { celsius: pendingSet });
      // The firmware clamps to what the iron accepts, so believe its answer
      // rather than what was typed.
      if (typeof r.celsius === 'number') pendingSet = r.celsius;
      el.input.value = pendingSet;
    } catch (e) {
      pendingSet = null;
      log('!', `pinecil.set_temp: ${e.message}`);
      el.state.textContent = e.message;
    }
  }

  api.on('profile.pinecil.telemetry', d => {
    raw = d;
    lastAt = Date.now();
    try { last = normalize(d, lastAt); }
    catch (e) { log('!', `pinecil telemetry: ${e.message}`); }
    render();
  });

  setInterval(render, 1000);

  $('p-apply').onclick = () => setTemp(el.input.value);
  el.input.addEventListener('keydown', (e) => {
    if (e.key === 'Enter') { e.preventDefault(); setTemp(el.input.value); }
  });
  for (const b of document.querySelectorAll('[data-settemp]')) {
    b.onclick = () => setTemp(b.dataset.settemp);
  }
  $('p-raw-toggle').onclick = (e) => {
    el.rawBox.hidden = !el.rawBox.hidden;
    e.target.setAttribute('aria-pressed', String(!el.rawBox.hidden));
    render();
  };

  render();
  return { render };
}
