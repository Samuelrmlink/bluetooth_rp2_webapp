// Settings -> System: reboot the gateway, restart the Lua VM on its own, and
// see where the Lua arena's memory is (script.heap) -- with a download of the
// whole arena for offline analysis (tools/lua_heapdump.py).
import { makeSection, kvRows, button, PolledSection } from './common.js';

const kb = (n) => (n == null ? '—' : `${(n / 1024).toFixed(1)} KB`);

// The census as rows. Pure; tested.
export function heapRows(h) {
  if (!h?.arena) return [];
  const a = h.arena;
  const o = h.objects || {};
  const types = Object.entries(o).filter(([k, v]) => k !== 'total' && v?.bytes)
    .sort((x, y) => y[1].bytes - x[1].bytes)
    .map(([k, v]) => `${k.replace(/_/g, ' ')} ${kb(v.bytes)} (${v.n})`);
  const tables = (h.tables || []).slice(0, 5)
    .map((t) => `${t.name || 'table'} ${kb(t.bytes)}`);
  return [
    ['Used', `${kb(a.used)} of ${kb(a.bytes)} (peak ${kb(a.peak)})`],
    ['Largest free block', `${kb(a.largest_free)} of ${kb(a.free_bytes)} free, in ${a.free_blocks} pieces`],
    ['Failed allocations', a.alloc_fails ? `${a.alloc_fails} (last ${a.alloc_fail_last} B)` : 'none'],
    ['Objects', types.join(', ') || '—'],
    ['Largest tables', tables.join(', ') || '—'],
    ['Coroutines', (h.threads || []).length ? h.threads.map(kb).join(', ') : 'none'],
  ];
}

// The raw arena, fetched 1 KB at a time (an op reply is capped at ~4 KB).
export async function fetchArena(ws, onProgress = () => {}) {
  let size = Infinity, base = 0, header = 0;
  const parts = [];
  for (let off = 0; off < size; off += 1024) {
    const r = await ws.call('script.arena', { offset: off, len: 1024 });
    if (r.error) throw new Error(r.error);
    ({ size, base, header } = r);
    parts.push(r.hex);
    onProgress(Math.min(1, (off + 1024) / size));
  }
  return { base, header, size, hex: parts.join('') };
}

export class SystemSection extends PolledSection {
  constructor(app, settings) {
    super(app, settings, 'sect-system', 5000);
    const d = makeSection(this.id, 'System', `
      <div class="row sys-actions"></div>
      <div class="dim sys-note"></div>
      <div class="row"><strong>Lua memory</strong></div>
      <div class="kv-list sys-heap"></div>
      <div class="row sys-dump"></div>`);
    this._heap = d.querySelector('.sys-heap');
    this._note = d.querySelector('.sys-note');
    const acts = d.querySelector('.sys-actions');
    acts.append(
      button('Reboot gateway', () => this.reboot()),
      button('Restart Lua VM', () => this.restartVm()),
    );
    this._dumpBtn = button('Download heap dump', () => this.dump());
    d.querySelector('.sys-dump').append(this._dumpBtn);
  }

  async reboot() {
    if (!window.confirm('Reboot the gateway?\n\nEvery connection drops: WiFi, WireGuard, Bluetooth devices and this page. Scripts do not restart by themselves.')) return;
    try {
      await this.ws.call('sys.reboot');
      this._note.textContent = 'Rebooting… this page reconnects when it is back.';
    } catch (e) { this._note.textContent = e.message; }
  }

  async restartVm() {
    if (!window.confirm('Restart the Lua VM?\n\nEvery script stops and the Lua arena starts empty. The network and the board keep running.')) return;
    try {
      const r = await this.ws.call('script.vm_restart');
      this._note.textContent = `Lua restarted: ${r.stopped} script(s) stopped, arena ${kb(r.before)} → ${kb(r.after)}.`;
      this.app.log('info', `Lua VM restarted (${r.stopped} script(s) stopped)`);
    } catch (e) { this._note.textContent = e.message; }
    this.soon();
  }

  async dump() {
    this._dumpBtn.disabled = true;
    try {
      const heap = await this.ws.call('script.heap');
      const arena = await fetchArena(this.ws, (f) => { this._dumpBtn.textContent = `Downloading… ${Math.round(f * 100)}%`; });
      const blob = new Blob([JSON.stringify({ taken: new Date().toISOString(), heap, arena })], { type: 'application/json' });
      const a = document.createElement('a');
      a.href = URL.createObjectURL(blob);
      a.download = `lua-heap-${new Date().toISOString().replace(/[:.]/g, '-')}.json`;
      a.click();
      setTimeout(() => URL.revokeObjectURL(a.href), 5000);
      this._note.textContent = 'Saved. Compare two dumps with tools/lua_heapdump.py --diff.';
    } catch (e) {
      this._note.textContent = `Heap dump failed: ${e.message}`;
    } finally {
      this._dumpBtn.disabled = false;
      this._dumpBtn.textContent = 'Download heap dump';
    }
  }

  async poll() {
    const h = await this.ws.call('script.heap').catch(() => null);
    if (h) kvRows(this._heap, heapRows(h));
    else kvRows(this._heap, [['Lua', 'this firmware has no heap census (1.90 or later)']]);
  }
}
