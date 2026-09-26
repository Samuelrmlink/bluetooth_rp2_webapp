/*
 * The Anker pane.
 *
 * Worth being explicit about what this demonstrates: the firmware has no idea
 * this device exists. Everything below is driven by profile.anker.telemetry,
 * an event a LUA script emits, decoded by a field dictionary that lives on
 * the filesystem. Adding a charger needed no C, no reflash and no change to
 * the generic BLE core -- and neither would adding the next one.
 *
 * Renderer only; the shaping is in anker.js, where it is tested.
 */
import { normalizeTelemetry, freshness } from '../anker.js';

const $ = (id) => document.getElementById(id);

export function mountAnkerPane({ api, log }) {
  let last = null;      // normalized
  let lastAt = 0;
  let raw = null;

  const el = {
    state: $('a-state'), ident: $('a-ident'), total: $('a-total'),
    ports: $('a-ports'), settings: $('a-settings'), other: $('a-other'),
    rawBox: $('a-raw'),
  };

  function cell(cls, text) {
    const d = document.createElement('div');
    d.className = cls; d.textContent = text;
    return d;
  }

  function renderPorts(n) {
    const frag = document.createDocumentFragment();
    for (const p of n.ports) {
      const card = document.createElement('div');
      card.className = 'port';
      card.dataset.state = !p.present ? 'absent'
        : !p.plugged ? 'empty'
        : p.drawing ? 'drawing' : 'idle';

      const head = document.createElement('div');
      head.className = 'port-head';
      head.append(cell('port-name', p.label));
      const dot = document.createElement('span');
      dot.className = 'dot';
      dot.title = p.on ? 'port enabled' : 'port disabled';
      head.appendChild(dot);
      card.appendChild(head);

      card.append(
        cell('port-v', p.volts),
        cell('port-a', p.amps),
        cell('port-w', p.power),
      );
      // cableInfo 3 means nothing plugged; anything else is a cable class,
      // which the device reports even with no load.
      card.append(cell('port-cable', p.plugged ? `${p.cable} cable` : 'nothing plugged'));
      if (p.countdown) card.append(cell('port-timer', `timer ${p.countdown}`));
      frag.appendChild(card);
    }
    el.ports.replaceChildren(frag);
  }

  function renderSettings(n) {
    const frag = document.createDocumentFragment();
    for (const s of n.settings) {
      const row = document.createElement('div');
      row.className = 'kv';
      row.append(cell('k', s.label), cell('v', s.value));
      frag.appendChild(row);
    }
    el.settings.replaceChildren(frag);

    // Unknown tags get shown, not dropped: this is how a newly decoded field
    // appears the moment the Lua dictionary learns it, with no webapp change.
    const of = document.createDocumentFragment();
    for (const o of n.other) {
      const row = document.createElement('div');
      row.className = 'kv';
      row.append(cell('k', o.key), cell('v', o.value));
      of.appendChild(row);
    }
    el.other.replaceChildren(of);
    el.other.parentElement.hidden = n.other.length === 0;
  }

  function render() {
    const f = freshness(lastAt);
    el.state.textContent = f.text;
    el.state.dataset.state = f.state;

    if (!last) {
      el.ident.textContent = 'no profile running — start the "anker" script';
      el.total.textContent = '';
      el.ports.replaceChildren();
      el.settings.replaceChildren();
      el.other.parentElement.hidden = true;
      return;
    }
    const bits = [last.serial, last.firmware, last.addr].filter(Boolean);
    el.ident.textContent = bits.join(' · ');
    if (last.error) {
      el.ident.textContent += `  · error ${last.error}`;
    }
    el.total.textContent = last.totalWatts > 0 ? `${last.totalWatts.toFixed(1)} W total` : '';
    renderPorts(last);
    renderSettings(last);
    if (el.rawBox && !el.rawBox.hidden) {
      el.rawBox.textContent = JSON.stringify(raw, null, 1);
    }
  }

  api.on('profile.anker.telemetry', d => {
    raw = d;
    lastAt = Date.now();
    try { last = normalizeTelemetry(d, lastAt); }
    catch (e) { log('!', `anker telemetry: ${e.message}`); }
    render();
  });

  // The device pushes at ~1 Hz. Repaint on a timer too, so "streaming" decays
  // to "stalled" when the charger stops talking instead of freezing on a
  // number that looks current.
  setInterval(render, 1000);

  $('a-refresh').onclick = async () => {
    try { await api.call('anker.a2687.refresh'); }
    catch (e) { log('!', `anker.refresh: ${e.message}`); }
  };
  $('a-raw-toggle').onclick = (e) => {
    el.rawBox.hidden = !el.rawBox.hidden;
    e.target.setAttribute('aria-pressed', String(!el.rawBox.hidden));
    render();
  };

  render();
  return { render };
}
