/*
 * The Hardware pane: pin broker, ADC, I2C scan, GPIO.
 *
 * Phase 7 exposed these through the module registry and nothing in a browser
 * ever called them. They are reachable from a script and from ws_client.py,
 * which is the design working -- but bring-up is exactly the task where you
 * want to see the whole board at once.
 *
 * NOTE there is no serial monitor. The Kconfig has IO_UART, but io_ops.c
 * registers no uart ops at all, so there is nothing behind it yet; a pane
 * with buttons that cannot work is worse than an honest absence.
 */
import { ADC_CHANNELS, pinClass, pinTitle, summarise, fmtVolts, fmtCelsius, fmtI2cResults }
  from '../hw.js';

const $ = (id) => document.getElementById(id);

export function mountHwPane({ api, log }) {
  let pins = [];
  let selected = null;

  const el = {
    grid: $('h-pins'), sum: $('h-sum'), detail: $('h-pin-detail'),
    adc: $('h-adc'), i2c: $('h-i2c'), i2cNote: $('h-i2c-note'),
  };

  /* ---- pins ------------------------------------------------------------- */
  async function refreshPins() {
    try {
      const r = await api.call('pins.map');
      pins = r.pins || [];
    } catch (e) { log('!', `pins.map: ${e.message}`); return; }

    const s = summarise(pins);
    el.sum.textContent = `${s.total} pins · ${s.free} free · ${s.claimed} claimed · ${s.reserved} reserved`;

    el.grid.replaceChildren(...pins.map((p) => {
      const b = document.createElement('button');
      b.className = 'pin';
      b.dataset.state = pinClass(p);
      b.title = pinTitle(p);
      b.textContent = p.pin;
      // A reserved pin is not selectable: there is nothing to do with it and
      // offering a control implies otherwise.
      if (p.state === 'reserved') b.disabled = true;
      else b.onclick = () => { selected = p.pin; renderDetail(); };
      return b;
    }));
    renderDetail();
  }

  function renderDetail() {
    const p = pins.find((x) => x.pin === selected);
    if (!p) { el.detail.textContent = 'select a pin'; return; }
    el.detail.replaceChildren();
    const line = document.createElement('span');
    line.textContent = pinTitle(p) + '  ';
    el.detail.appendChild(line);
    for (const [label, fn] of [
      ['read', async () => {
        const r = await api.call('gpio.get', { pin: p.pin });
        line.textContent = `GPIO ${p.pin} reads ${r.value ? 'high' : 'low'}  `;
      }],
      ['set high', () => api.call('gpio.set', { pin: p.pin, value: true })],
      ['set low', () => api.call('gpio.set', { pin: p.pin, value: false })],
      ['release', () => api.call('pins.release', { pin: p.pin }).then(refreshPins)],
    ]) {
      const b = document.createElement('button');
      b.className = 'btn'; b.textContent = label;
      b.onclick = () => Promise.resolve(fn()).catch((e) => {
        line.textContent = `GPIO ${p.pin}: ${e.message}  `;
      });
      el.detail.appendChild(b);
    }
  }

  /* ---- adc -------------------------------------------------------------- */
  async function refreshAdc() {
    const rows = document.createDocumentFragment();
    for (const c of ADC_CHANNELS) {
      const row = document.createElement('div');
      row.className = 'kv';
      const k = document.createElement('div');
      k.className = 'k';
      k.textContent = c.pin !== null ? `${c.label} (GPIO ${c.pin})` : c.label;
      const v = document.createElement('div');
      v.className = 'v';
      try {
        const r = await api.call('adc.read', { channel: c.ch });
        v.textContent = c.ch === 4
          ? `${fmtCelsius(r.celsius)}   ${fmtVolts(r.volts)}`
          : `${fmtVolts(r.volts)}   raw ${r.raw}`;
      } catch (e) {
        v.textContent = e.message;
      }
      if (c.note) {
        const n = document.createElement('span');
        n.className = 'muted'; n.textContent = `  ${c.note}`;
        v.appendChild(n);
      }
      row.append(k, v);
      rows.appendChild(row);
    }
    el.adc.replaceChildren(rows);
  }

  /* ---- i2c -------------------------------------------------------------- */
  async function scanI2c() {
    const bus = Number($('h-i2c-bus').value);
    const sda = Number($('h-i2c-sda').value);
    const scl = Number($('h-i2c-scl').value);
    el.i2cNote.textContent = 'scanning…';
    try {
      const r = await api.call('i2c.scan', { id: bus, sda, scl });
      const { list, note } = fmtI2cResults(r.found);
      el.i2cNote.textContent = `bus ${r.bus} · sda ${r.sda} · scl ${r.scl} — ${note}`;
      el.i2c.replaceChildren(...list.map((d) => {
        const s = document.createElement('span');
        s.className = 'addr'; s.textContent = d.hex;
        return s;
      }));
    } catch (e) {
      el.i2cNote.textContent = e.message;
      el.i2c.replaceChildren();
    }
  }

  $('h-refresh').onclick = () => { refreshPins(); refreshAdc(); };
  $('h-i2c-scan').onclick = scanI2c;

  api.on('_open', () => { refreshPins(); refreshAdc(); });
  // The die temperature drifts and a claim can change under us, so keep the
  // board view current -- but slowly: this is a bring-up view, not a scope.
  setInterval(() => { if (api.connected) { refreshPins(); refreshAdc(); } }, 5000);

  return { refreshPins, refreshAdc };
}
