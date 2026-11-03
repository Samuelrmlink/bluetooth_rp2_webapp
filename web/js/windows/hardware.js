// The Hardware window: the pin broker's map, the ADC channels and an I2C
// bus scan -- the gateway's own GPIO, as opposed to the devices it talks to.
// Logic (pin states, formatting) is hw.js, unit-tested; this renders.
//
// It polls, because the pin map and the ADC have no events. Only while the
// window is on screen: the old Hardware page polled every five seconds
// whether or not anyone was looking, which is twelve pointless round trips
// a minute over a link that may be a Starlink tunnel.

import { ADC_CHANNELS, pinClass, pinTitle, summarise, fmtVolts, fmtCelsius, fmtI2cResults, pioView, pioProgLine } from '../hw.js';

const POLL_MS = 5000;
const PIO_COLORS = ['#4fd07f', '#7eb6ff', '#e0b45c', '#d07adf', '#5ec8c8', '#e07a62'];

export class HardwareWindow {
  constructor(el, ctx) {
    this.app = ctx.app;
    this.ws = ctx.ws;
    this.pins = [];
    this.selected = null;
    this._timer = 0;

    el.innerHTML = `
      <div class="pane-bar">
        <span class="dim h-sum"></span>
        <span class="spacer"></span>
        <button type="button" class="pane-btn h-refresh">Refresh</button>
      </div>
      <div class="hw-body">
        <section><h4>Pins</h4><div class="hw-pins"></div><div class="hw-detail dim">Select a pin.</div></section>
        <section><h4>PIO</h4><div class="hw-pio dim">…</div></section>
        <section><h4>ADC</h4><div class="hw-adc kv-list"></div></section>
        <section><h4>I2C scan</h4>
          <form class="hw-i2c-form">
            <label>bus <input class="h-bus" type="number" value="0" min="0" max="1"></label>
            <label>sda <input class="h-sda" type="number" value="4" min="0" max="29"></label>
            <label>scl <input class="h-scl" type="number" value="5" min="0" max="29"></label>
            <button type="submit" class="pane-btn">Scan</button>
          </form>
          <div class="dim hw-i2c-note"></div><div class="hw-i2c"></div>
        </section>
      </div>`;
    this._sum = el.querySelector('.h-sum');
    this._grid = el.querySelector('.hw-pins');
    this._detail = el.querySelector('.hw-detail');
    this._pio = el.querySelector('.hw-pio');
    this._adc = el.querySelector('.hw-adc');
    this._i2c = el.querySelector('.hw-i2c');
    this._i2cNote = el.querySelector('.hw-i2c-note');
    this._f = { bus: el.querySelector('.h-bus'), sda: el.querySelector('.h-sda'), scl: el.querySelector('.h-scl') };
    el.querySelector('.h-refresh').addEventListener('click', () => this.refresh());
    el.querySelector('.hw-i2c-form').addEventListener('submit', (e) => { e.preventDefault(); this.scanI2c(); });
  }

  onShow() {
    this.refresh();
    clearInterval(this._timer);
    this._timer = setInterval(() => this.refresh(), POLL_MS);
  }

  onHide() {
    clearInterval(this._timer);
    this._timer = 0;
  }

  onConnect() { if (this._timer) this.refresh(); }

  refresh() {
    if (!this.ws.connected) return;
    this.refreshPins();
    this.refreshAdc();
    this.refreshPio();
  }

  async refreshPins() {
    try {
      this.pins = (await this.ws.call('pins.map')).pins || [];
    } catch (e) {
      this._sum.textContent = `pins.map: ${e.message}`;
      return;
    }
    const s = summarise(this.pins);
    this._sum.textContent = `${s.total} pins · ${s.free} free · ${s.claimed} claimed · ${s.reserved} reserved`;
    this._grid.replaceChildren(...this.pins.map((p) => {
      const b = document.createElement('button');
      b.type = 'button';
      b.className = 'pin';
      b.dataset.state = pinClass(p);
      b.title = pinTitle(p);
      b.textContent = p.pin;
      if (p.pin === this.selected) b.setAttribute('aria-pressed', 'true');
      if (p.state === 'reserved') b.disabled = true;
      else b.addEventListener('click', () => { this.selected = p.pin; this.refreshPins(); });
      return b;
    }));
    this._renderDetail();
  }

  _renderDetail() {
    const p = this.pins.find((x) => x.pin === this.selected);
    if (!p) { this._detail.textContent = 'Select a pin.'; return; }
    const line = document.createElement('span');
    line.textContent = `${pinTitle(p)}  `;
    const acts = [
      ['Read', async () => {
        const r = await this.ws.call('gpio.get', { pin: p.pin });
        line.textContent = `GPIO ${p.pin} reads ${r.value ? 'high' : 'low'}  `;
      }],
      ['Set high', () => this.ws.call('gpio.set', { pin: p.pin, value: true })],
      ['Set low', () => this.ws.call('gpio.set', { pin: p.pin, value: false })],
      ['Release', () => this.ws.call('pins.release', { pin: p.pin }).then(() => this.refreshPins())],
    ];
    this._detail.replaceChildren(line, ...acts.map(([label, fn]) => {
      const b = document.createElement('button');
      b.type = 'button';
      b.className = 'pane-btn';
      b.textContent = label;
      b.addEventListener('click', () => Promise.resolve(fn()).catch((e) => { line.textContent = `GPIO ${p.pin}: ${e.message}  `; }));
      return b;
    }));
  }

  async refreshPio() {
    let view;
    try {
      view = pioView(await this.ws.call('pio.status'));
    } catch (e) {
      this._pio.textContent = `pio.status: ${e.message}`;
      return;
    }
    const owners = [];
    for (const b of view.blocks) {
      for (const p of b.programs) if (!owners.includes(p.owner)) owners.push(p.owner);
    }
    const head = document.createElement('div');
    head.textContent = view.summary;
    const kids = [head];
    for (const b of view.blocks) kids.push(this._pioBlock(b, owners));
    this._pio.replaceChildren(...kids);
  }

  _pioBlock(b, owners) {
    const box = document.createElement('div');
    box.className = 'hw-pio-block';
    const title = document.createElement('div');
    const base = b.gpio_base < 0 ? 'base unset' : `base ${b.gpio_base}`;
    title.textContent = `PIO ${b.pio} · ${base} · ${b.instr_used} used · ${b.instr_free} free · map 0x${b.instr_map.toString(16).padStart(8, '0')}`;
    const map = document.createElement('div');
    map.className = 'hw-pio-map';
    b.cells.forEach((c, i) => {
      if (i && i % 8 === 0) {
        const gap = document.createElement('span');
        gap.className = 'hw-pio-gap';
        map.appendChild(gap);
      }
      const cell = document.createElement('span');
      cell.className = 'hw-pio-cell' + (c.prot ? ' prot' : '');
      cell.title = c.taken ? `word ${c.word} · ${c.owner}${c.prot ? ' · protected' : ''}` : `word ${c.word} free`;
      if (c.taken) {
        const slot = owners.indexOf(c.owner);
        cell.style.background = PIO_COLORS[(slot < 0 ? 0 : slot) % PIO_COLORS.length];
      }
      map.appendChild(cell);
    });
    const sms = document.createElement('div');
    sms.textContent = b.sms.map((s) => (s.free ? `SM ${s.sm} free` : `SM ${s.sm} ${s.owners.join('+') || 'taken'}`)).join(' · ');
    const irq = document.createElement('div');
    const flags = b.irq.length
      ? b.irq.map((f) => (f.owners.length ? `IRQ ${f.flag} ${f.owners.join('+')}` : `IRQ ${f.flag}`)).join(', ')
      : 'IRQ flags free';
    const cpu = b.irq_lines.length ? b.irq_lines.map((l) => `CPU IRQ ${l}`).join(', ') : 'CPU IRQ lines free';
    irq.textContent = `${flags} · ${cpu}`;
    const progs = document.createElement('div');
    progs.className = 'hw-pio-progs';
    if (!b.programs.length) {
      const empty = document.createElement('div');
      empty.textContent = 'no program loaded';
      progs.appendChild(empty);
    } else {
      for (const p of b.programs) {
        const line = document.createElement('div');
        line.className = 'hw-pio-prog';
        line.textContent = pioProgLine(p);
        progs.appendChild(line);
      }
    }
    box.append(title, map, sms, irq, progs);
    return box;
  }

  async refreshAdc() {
    const frag = document.createDocumentFragment();
    for (const c of ADC_CHANNELS) {
      const k = document.createElement('div');
      k.className = 'k';
      k.textContent = c.pin !== null ? `${c.label} (GPIO ${c.pin})` : c.label;
      const v = document.createElement('div');
      v.className = 'v';
      try {
        const r = await this.ws.call('adc.read', { channel: c.ch });
        v.textContent = c.ch === 4 ? `${fmtCelsius(r.celsius)}   ${fmtVolts(r.volts)}` : `${fmtVolts(r.volts)}   raw ${r.raw}`;
      } catch (e) {
        v.textContent = e.message;
      }
      if (c.note) {
        const n = document.createElement('span');
        n.className = 'dim';
        n.textContent = `  ${c.note}`;
        v.appendChild(n);
      }
      frag.append(k, v);
    }
    this._adc.replaceChildren(frag);
  }

  async scanI2c() {
    this._i2cNote.textContent = 'scanning…';
    try {
      const r = await this.ws.call('i2c.scan', {
        id: Number(this._f.bus.value), sda: Number(this._f.sda.value), scl: Number(this._f.scl.value),
      });
      const { list, note } = fmtI2cResults(r.found);
      this._i2cNote.textContent = `bus ${r.bus} · sda ${r.sda} · scl ${r.scl} — ${note}`;
      this._i2c.replaceChildren(...list.map((d) => {
        const s = document.createElement('span');
        s.className = 'addr';
        s.textContent = d.hex;
        return s;
      }));
    } catch (e) {
      this._i2cNote.textContent = e.message;
      this._i2c.replaceChildren();
    }
  }
}
