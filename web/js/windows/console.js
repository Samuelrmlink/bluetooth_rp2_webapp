// The Console window: what the device says, and a line to ask it things.
//
// Everything worth reading that is not a telemetry sample -- the firmware's
// log events, Lua errors, a script woken by a link dropping -- plus a
// script.eval line, which is the fastest way to poke a running script. Events
// arrive whether or not the window is on screen, and are kept (capped) so
// opening it later shows what happened; drawing waits until it is visible.

const MAX_LINES = 1000;

import { restartText } from '../settings/system.js';
import { copyWithToast } from '../controls/cellmenu.js';
import { toast } from '../toast.js';
export class ConsoleWindow {
  constructor(el, ctx) {
    this.ws = ctx.ws;
    this.lines = [];
    this._drawn = 0;
    this._dirty = false;

    el.innerHTML = `
      <div class="pane-bar">
        <label class="dim"><input type="checkbox" class="c-info" checked> info</label>
        <label class="dim"><input type="checkbox" class="c-lua" checked> Lua</label>
        <label class="dim"><input type="checkbox" class="c-bt"> Bluetooth</label>
        <span class="spacer"></span>
        <button type="button" class="pane-btn c-copy" title="Copy every line shown (the filters apply) to the clipboard">Copy all</button>
        <button type="button" class="pane-btn c-clear" title="Clear">Clear</button>
      </div>
      <div class="console-log" tabindex="0" role="log" aria-live="off"></div>
      <form class="console-eval">
        <select class="c-target" aria-label="Evaluate in"><option value="">global</option></select>
        <input class="c-chunk" type="text" spellcheck="false" autocomplete="off"
               enterkeyhint="send" placeholder="Lua expression or statement — Enter to run">
        <button type="submit" class="pane-btn c-send" title="Run it (Enter)">Send</button>
      </form>`;
    this._log = el.querySelector('.console-log');
    this._filters = {
      info: el.querySelector('.c-info'),
      lua: el.querySelector('.c-lua'),
      bt: el.querySelector('.c-bt'),
    };
    for (const cb of Object.values(this._filters)) cb.addEventListener('change', () => this._redraw());
    el.querySelector('.c-clear').addEventListener('click', () => { this.lines = []; this._redraw(); });
    el.querySelector('.c-copy').addEventListener('click', () => this.copyAll());
    this._target = el.querySelector('.c-target');
    this._chunk = el.querySelector('.c-chunk');
    el.querySelector('.console-eval').addEventListener('submit', (e) => { e.preventDefault(); this._eval(); });
    // Pressing Send must not take focus from the box: on a phone that would
    // close the keyboard after every line.
    el.querySelector('.c-send').addEventListener('pointerdown', (e) => {
      if (document.activeElement === this._chunk) e.preventDefault();
    });
    // Stay on the newest line while the log shrinks (a keyboard opening) or
    // grows, unless the user has scrolled back to read something.
    // Only the USER scrolling decides: the scroll event from our own jump to
    // the bottom arrives a frame late, and if the log has shrunk in between (a
    // keyboard animating open) it measured as "scrolled up" and unpinned it.
    this._stick = true;
    this._ownTop = -1;
    this._log.addEventListener('scroll', () => {
      const l = this._log;
      if (Math.abs(l.scrollTop - this._ownTop) < 2) return;       // ours
      this._stick = l.scrollTop + l.clientHeight >= l.scrollHeight - 4;
    });
    new ResizeObserver(() => { if (this._stick) this._toBottom(); }).observe(this._log);

    const bus = ctx.bus;
    bus.on('log', (d) => this.add('info', d?.src || 'device', d?.message ?? JSON.stringify(d), d?.level));
    bus.on('script.error', (d) => this.add('lua', d?.script || 'lua', d?.error || 'error', 'error'));
    // A short wait timing out is a script polling in slices (the Anker
    // loop's 250 ms waits): routine, not news -- firmware 1.97+ no longer
    // sends those; this keeps an older one from filling the Console 4x a second.
    bus.on('script.wake', (d) => {
      if (d?.reason === 'timeout' && Number(d.waited_ms) < 1000) return;
      this.add('lua', d?.script || 'lua', `woke: ${d?.reason} after ${d?.waited_ms} ms`);
    });
    // A rebuilt Lua VM: by hand, or by itself after the last script stopped with the arena in pieces.
    bus.on('script.vm_restarted', (d) => this.add('lua', 'lua', restartText(d || {})));
    for (const t of ['dev.connected', 'dev.disconnected', 'security.failed', 'security.bonded']) {
      bus.on(t, (d) => this.add('bt', t, d?.addr || JSON.stringify(d)));
    }
  }

  onConnect() {
    this.add('info', 'app', 'connected to the gateway');
    this._refreshTargets();
  }

  onDisconnect() { this.add('info', 'app', 'connection lost', 'warn'); }

  add(kind, src, text, level) {
    this.lines.push({ t: new Date(), kind, src, text: String(text), level });
    if (this.lines.length > MAX_LINES) this.lines.splice(0, this.lines.length - MAX_LINES);
    this._dirty = true;
  }

  async _refreshTargets() {
    try {
      const r = await this.ws.call('script.list');
      const keep = this._target.value;
      this._target.length = 1;
      for (const s of r.scripts || []) {
        if (!s.running) continue;
        const o = document.createElement('option');
        o.value = o.textContent = s.name;
        this._target.appendChild(o);
      }
      this._target.value = keep;
    } catch { /* offline: keep what is there */ }
  }

  async _eval() {
    const chunk = this._chunk.value.trim();
    if (!chunk) return;
    this.add('lua', '>', chunk);
    try {
      const params = { chunk };
      if (this._target.value) params.script = this._target.value;
      const r = await this.ws.call('script.eval', params);
      this.add('lua', r.ok ? '=' : '!', r.value ?? '', r.ok ? undefined : 'error');
    } catch (e) {
      this.add('lua', '!', e.message, 'error');
    }
    this._chunk.select();
  }

  // Every line shown (the info / Lua / Bluetooth filters apply), as text:
  // "HH:MM:SS source message", one per line -- for pasting into a bug report.
  copyAll() {
    const shown = this.lines.filter((l) => this._shown(l));
    if (!shown.length) { toast('Nothing to copy'); return; }
    const text = shown.map((l) => `${l.t.toTimeString().slice(0, 8)} ${l.src} ${l.text}`).join('\n');
    copyWithToast(text, `Copied ${shown.length} line${shown.length === 1 ? '' : 's'}`);
  }

  onShow() { this._redraw(); }

  render() {
    if (this._dirty) this._redraw();
  }

  _toBottom() {
    this._log.scrollTop = this._log.scrollHeight;
    this._ownTop = this._log.scrollTop;
  }

  _shown(l) { return this._filters[l.kind]?.checked !== false; }

  _redraw() {
    this._dirty = false;
    const atBottom = this._stick !== false;
    const frag = document.createDocumentFragment();
    for (const l of this.lines) {
      if (!this._shown(l)) continue;
      const row = document.createElement('div');
      row.className = `console-line${l.level ? ' lvl-' + l.level : ''}`;
      const t = document.createElement('span');
      t.className = 'dim';
      t.textContent = l.t.toTimeString().slice(0, 8) + ' ';
      const s = document.createElement('span');
      s.className = 'console-src';
      s.textContent = l.src + ' ';
      const m = document.createElement('span');
      m.textContent = l.text;          // device text: never innerHTML
      row.append(t, s, m);
      frag.appendChild(row);
    }
    this._log.replaceChildren(frag);
    if (atBottom) this._toBottom();
  }
}
