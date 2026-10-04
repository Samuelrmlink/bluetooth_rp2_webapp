// Anker small chargers (the A2687 family). Field names from
// lua/profiles/anker/models/A2687.lua; the protocol in docs/a2687-protocol.md.
// Windows: Ports (./ports.js: the ports and the power settings), Controls
// (the screen and display settings), All Data, Screensaver (picture upload).

import { upload, crc32, defaultId, packetLength, SIDE, MAX_PACKET } from './screensaver.js';
import { encodeJpeg } from './jpeg.js';
import { PortsWindow } from './ports.js';

// Every write: the driver's anker.set. A refusal comes back as a result
// with `error` and is thrown, so the control shows why.
export async function write(ctx, control, value) {
  let field = control.field;
  // Custom from the mode select: the charger's stored custom profile, applied
  if (field === 'charging_protocol' && Number(value) === 2) {
    const cp = ctx.device.sample?.custom_power || {};
    const mode = Number(ctx.device.sample?.custom_mode_number);
    field = 'custom_mode';
    value = { mode: mode >= 1 && mode <= 254 ? mode : 1, auto_exit: cp.auto_exit ? 1 : 0,
              c1_w: cp.c1_w ?? 0, c2_w: cp.c2_w ?? 0, c3_w: cp.c3_w ?? 0 };
  }
  const r = await ctx.ws.call('anker.set', { device: ctx.device.addr, field, value });
  if (r && r.error) throw new Error(r.message || r.error);
  return r;
}

export const css = 'plugin.css';

// A port's power is { mv, ma, watts, charging }; its control is { on, cable,
// countdown_s, countdown_remain_s, countdown_active, vid }.
export function portPower(v) {
  if (!v || typeof v !== 'object') return '\u2014';
  if (!v.charging && !v.mv) return 'idle';
  return `${(v.mv / 1000).toFixed(2)} V \u00b7 ${(v.ma / 1000).toFixed(2)} A \u00b7 ${Number(v.watts).toFixed(1)} W`;
}

export function portControl(v) {
  if (!v || typeof v !== 'object') return '\u2014';
  const parts = [v.on ? 'on' : 'off'];
  if (v.cable && v.cable !== 'none') parts.push(`${v.cable} cable`);
  if (v.countdown_active) parts.push(`off in ${Math.round(v.countdown_remain_s / 60)} min`);
  return parts.join(' \u00b7 ');
}

// The Controls window: a summary of the ports (their controls are in Ports)
// and the screen settings, every one verified on the bench (2026-10-03).
export const controls = [
  { kind: 'readout', field: 'c1_power', label: 'USB-C1', format: portPower },
  { kind: 'readout', field: 'c2_power', label: 'USB-C2', format: portPower },
  { kind: 'readout', field: 'c3_power', label: 'USB-C3', format: portPower },
  { kind: 'readout', field: 'c1_control', label: 'USB-C1 port', format: portControl },
  { kind: 'readout', field: 'c2_control', label: 'USB-C2 port', format: portControl },
  { kind: 'readout', field: 'c3_control', label: 'USB-C3 port', format: portControl },
  { kind: 'slider', field: 'backlight_pct', label: 'Brightness', min: 25, max: 100, step: 5, unit: '%', verified: true },
  { kind: 'select', field: 'screen_off', label: 'Screen off after', verified: true,
    options: [['30s', '30 s'], ['1min', '1 min'], ['5min', '5 min'], ['30min', '30 min'], ['12h', '12 h']] },
  { kind: 'select', field: 'direction', label: 'Screen rotation', verified: true,
    options: [['up', 'Up'], ['left', 'Left'], ['down', 'Down'], ['right', 'Right']] },
  { kind: 'switch', field: 'gyroscope', label: 'Auto-rotate', verified: true },
  { kind: 'select', field: 'language', label: 'Language', verified: true,
    options: [['en', 'English'], ['zh', 'Chinese'], ['ja', 'Japanese'], ['de', 'German']] },
  { kind: 'switch', field: 'screensaver_enabled', label: 'Screensaver', verified: true },
  { kind: 'readout', field: 'error_code', label: 'Error code' },
];

// --- Screensaver window --------------------------------------------------------
// Pick or drop a picture; it is centre-cropped and scaled to the charger's
// 240x240 screen and encoded as a JPEG here, then sent through the driver's
// anker.screensaver op one step at a time (./screensaver.js runs the loop).


const MAX_JPEG = 32 * 1024;            // the app's own uploads were 17.7 and 30.7 KB

// The picture as the app sends it: centre-cropped and scaled by the canvas,
// then encoded by ./jpeg.js -- baseline, 4:4:4, quality 90, standard tables,
// JFIF only. (canvas.toBlob would subsample chroma 4:2:0 and add an ICC
// profile, which the charger has never been shown to accept.) EXIF rotation
// is applied, so a phone photo is upright; transparency becomes black.
async function toJpeg(file) {
  const bmp = await createImageBitmap(file, { imageOrientation: 'from-image' });
  const c = document.createElement('canvas');
  c.width = c.height = SIDE;
  const g = c.getContext('2d');
  g.fillStyle = '#000';
  g.fillRect(0, 0, SIDE, SIDE);
  const s = Math.min(bmp.width, bmp.height);
  g.drawImage(bmp, (bmp.width - s) / 2, (bmp.height - s) / 2, s, s, 0, 0, SIDE, SIDE);
  bmp.close?.();
  const pixels = g.getImageData(0, 0, SIDE, SIDE);
  let bytes, quality;
  for (quality of [90, 85, 80, 70, 60, 50, 40]) {
    bytes = encodeJpeg(pixels, quality);
    if (bytes.length <= MAX_JPEG) break;
  }
  return { bytes, canvas: c, quality };
}

export class ScreensaverWindow {
  constructor(el, ctx) {
    this.ctx = ctx;
    this.dev = ctx.device;
    this.waiters = new Set();
    this.jpeg = null;
    el.innerHTML = `
      <div class="pane-bar dev-head"><span class="dev-name"></span><span class="dim dev-id"></span></div>
      <div class="ss">
        <div class="ss-pick">
          <canvas class="ss-preview" width="${SIDE}" height="${SIDE}"></canvas>
          <div class="ss-side">
            <p class="dim">Drop a picture on the preview, or</p>
            <button type="button" class="pane-btn ss-choose">Choose a picture…</button>
            <input type="file" class="ss-file" accept="image/*" hidden>
            <div class="ss-info dim"></div>
            <label class="ss-idrow">Image id <input type="number" class="ss-id" min="1" max="4294967295"></label>
            <div class="ss-actions">
              <button type="button" class="pane-btn ss-send" disabled>Send to charger</button>
              <button type="button" class="pane-btn ss-cancel" hidden>Cancel</button>
            </div>
            <progress class="ss-bar" max="1" value="0" hidden></progress>
            <div class="ss-status"></div>
          </div>
        </div>
        <p class="dim ss-note">Centre-cropped to ${SIDE}×${SIDE}. The charger keeps pictures by id: sending one it already has just selects it.</p>
      </div>`;
    el.querySelector('.dev-name').textContent = this.dev.name;
    el.querySelector('.dev-id').textContent = this.dev.addr || '';
    this.$ = (s) => el.querySelector(s);
    const file = this.$('.ss-file');
    this.$('.ss-choose').addEventListener('click', () => file.click());
    file.addEventListener('change', () => file.files[0] && this.load(file.files[0]));
    const drop = this.$('.ss-preview');
    drop.addEventListener('dragover', (e) => { e.preventDefault(); drop.classList.add('is-over'); });
    drop.addEventListener('dragleave', () => drop.classList.remove('is-over'));
    drop.addEventListener('drop', (e) => {
      e.preventDefault();
      drop.classList.remove('is-over');
      const f = e.dataTransfer?.files?.[0];
      if (f) this.load(f);
    });
    this.$('.ss-send').addEventListener('click', () => this.send());
    this.$('.ss-cancel').addEventListener('click', () => this.abort?.abort());
    ctx.bus.on('profile.anker.screensaver', (ev) => {
      if (String(ev?.addr || '').toUpperCase() !== String(this.dev.addr || '').toUpperCase()) return;
      for (const w of [...this.waiters]) if (w.match(ev)) { this.waiters.delete(w); w.resolve(ev); }
    });
  }

  status(text, cls = '') {
    const s = this.$('.ss-status');
    s.textContent = text;
    s.className = `ss-status ${cls}`;
  }

  async load(file) {
    try {
      const { bytes, canvas, quality } = await toJpeg(file);
      this.jpeg = bytes;
      this.$('.ss-preview').getContext('2d').drawImage(canvas, 0, 0);
      const crc = crc32(bytes);
      this.$('.ss-id').value = String(defaultId(crc));
      this.$('.ss-info').textContent = `${bytes.length} B JPEG · quality ${quality} · CRC32 ${crc.toString(16).padStart(8, '0')}`;
      this.$('.ss-send').disabled = false;
      this.status('');
    } catch (e) {
      this.status(`Could not read that picture: ${e.message}`, 'is-error');
    }
  }

  wait(match, ms) {
    return new Promise((resolve, reject) => {
      const w = { match, resolve };
      this.waiters.add(w);
      setTimeout(() => { if (this.waiters.delete(w)) reject(new Error('no answer from the charger')); }, ms);
    });
  }

  async send() {
    if (!this.jpeg || this.busy) return;
    const id = Number(this.$('.ss-id').value);
    if (!Number.isInteger(id) || id < 1 || id > 0xFFFFFFFF) { this.status('The id must be a whole number above 0.', 'is-error'); return; }
    this.busy = true;
    this.abort = new AbortController();
    const bar = this.$('.ss-bar');
    bar.hidden = false;
    bar.value = 0;
    this.$('.ss-send').disabled = true;
    this.$('.ss-cancel').hidden = false;
    const started = Date.now();
    try {
      let plen = MAX_PACKET;
      try {
        const t = await this.ctx.ws.call('gatt.tree', { device: this.dev.addr });
        if (t?.mtu) plen = packetLength(t.mtu);
      } catch { /* not knowable: use the app's packet length */ }
      if (plen < 16) throw new Error(`the link's MTU is too small for screensaver packets`);
      const io = {
        call: (p) => this.ctx.ws.call('anker.screensaver', { device: this.dev.addr, ...p }),
        wait: (m, ms) => this.wait(m, ms),
      };
      const how = await upload({ jpeg: this.jpeg, id, plen }, io, {
        signal: this.abort.signal,
        onProgress: (p) => {
          bar.value = p.count ? p.sent / p.count : 0;
          if (p.phase === 'select') this.status('Asking the charger…');
          else if (p.phase === 'data') this.status(`Sent ${p.sent} of ${p.count} packets`);
          else if (p.phase === 'retry') this.status(`Resending from packet ${p.sent} (${p.why})`);
        },
      });
      const s = ((Date.now() - started) / 1000).toFixed(1);
      this.status(how === 'cached' ? 'The charger already had this picture: selected it.' : `Sent in ${s} s.`, 'is-ok');
      this.ctx.log('device', `${this.dev.name}: screensaver ${id} ${how === 'cached' ? 'selected' : `sent (${this.jpeg.length} B, ${s} s)`}`);
    } catch (e) {
      this.status(`Failed: ${e.message}`, 'is-error');
      this.ctx.log('!', `${this.dev.name}: screensaver: ${e.message}`);
    } finally {
      this.busy = false;
      this.$('.ss-send').disabled = !this.jpeg;
      this.$('.ss-cancel').hidden = true;
    }
  }

  unmount() {
    this.abort?.abort();
    this.waiters.clear();
  }
}

export const windows = { screensaver: ScreensaverWindow, ports: PortsWindow };
