// Settings -> SD card. Shown only when the firmware was built with one
// (sys.info lists the `sd` module). Status from GET /api/sd (every 2 s, every
// 500 ms while a format runs), the card and its filesystem, Format (with the
// cluster choices the board would accept, what the choice comes to, and a
// confirmation naming the card), and a speed bench. The files themselves are
// in the editor, under its top-level sd/ folder. docs/sd.md.
import { makeSection, kvRows, setNote, PolledSection } from './common.js';
import { sdStatus, sdFormat, sdBench } from '../fs.js';
import { CLUSTERS, defaultFs, defaultCluster, derive, fmtBytes, fmtCluster, fmtDuration } from './sd-format.js';

const STATE_TEXT = {
  ok: 'ready',
  none: 'no card',
  settling: 'card inserted, settling…',
  error: 'card present but not readable — unformatted (format it below), or not answering',
  formatting: 'formatting…',
};

export function sdRows(s) {
  const c = s.card;
  const cid = c?.cid;
  return [
    ['State', STATE_TEXT[s.state] || s.state],
    ['Card', c ? `${c.type} · ${fmtBytes(c.bytes)}` : null],
    ['Made by', cid ? `${cid.oem} "${cid.name}" rev ${cid.rev} · ${cid.date} · serial ${cid.serial}` : null],
    ['Filesystem', s.mounted ? s.fs : null],
    ['Space', s.mounted ? `${fmtBytes(s.free)} free of ${fmtBytes(s.total)}` : null],
    ['Bus', s.bus?.width ? `${s.bus.type.toUpperCase()} ${s.bus.width}-bit · ${(s.bus.clock_khz / 1000).toFixed(1)} MHz` : s.bus?.type?.toUpperCase()],
  ];
}

export class SdCardSection extends PolledSection {
  constructor(app, settings) {
    super(app, settings, 'sect-sd', 2000);
    const d = makeSection(this.id, 'SD card', `
      <div class="kv-list sdc-status"></div>
      <div class="dim">The card's files are in the editor, under <code>sd/</code>.</div>
      <hr>
      <form class="sdc-format">
        <div class="row"><strong>Format</strong></div>
        <div class="row"><label for="sd-fs">Filesystem</label>
          <select id="sd-fs"><option value="fat32">FAT32</option><option value="exfat">exFAT</option></select></div>
        <div class="row"><label for="sd-cluster">Cluster</label><select id="sd-cluster"></select></div>
        <div class="dim sdc-derived"></div>
        <div class="row"><button type="submit" class="pane-btn sdc-format-go">Format…</button></div>
        <progress class="sdc-progress" max="100" value="0" hidden></progress>
      </form>
      <hr>
      <div class="row"><strong>Speed</strong><span class="spacer"></span>
        <button type="button" class="pane-btn sdc-bench-go">Run 10 MB bench</button></div>
      <div class="dim sdc-bench"></div>
      <div class="dim sdc-note"></div>`);
    this._d = d;
    d.hidden = true;                      // until sys.info says the module is there
    const q = (s) => d.querySelector(s);
    Object.assign(this, {
      _status: q('.sdc-status'), _form: q('.sdc-format'), _fs: q('#sd-fs'), _cluster: q('#sd-cluster'),
      _derived: q('.sdc-derived'), _go: q('.sdc-format-go'), _bar: q('.sdc-progress'), _bench: q('.sdc-bench'),
      _benchGo: q('.sdc-bench-go'), _note: q('.sdc-note'),
    });
    this.st = null;
    this._seeded = false;
    this._fs.addEventListener('change', () => { this._fillClusters(); this._showDerived(); });
    this._cluster.addEventListener('change', () => this._showDerived());
    this._form.addEventListener('submit', (e) => { e.preventDefault(); this._format(); });
    this._benchGo.addEventListener('click', () => this._runBench());
  }

  _fillClusters() {
    const fs = this._fs.value;
    const bytes = this.st?.card?.bytes || 0;
    const want = Number(this._cluster.value) || defaultCluster(fs, bytes);
    this._cluster.replaceChildren(...CLUSTERS[fs].map((c) => {
      const o = document.createElement('option');
      o.value = String(c);
      const d = bytes ? derive(fs, c, bytes) : { ok: true };
      o.disabled = !d.ok;
      o.textContent = `${fmtCluster(c)}${c === defaultCluster(fs, bytes) ? ' (default)' : ''}${d.ok ? '' : ' — not on this card'}`;
      return o;
    }));
    const pick = CLUSTERS[fs].includes(want) && !(bytes && !derive(fs, want, bytes).ok) ? want : defaultCluster(fs, bytes);
    this._cluster.value = String(pick);
  }

  _showDerived() {
    const bytes = this.st?.card?.bytes;
    if (!bytes) { this._derived.textContent = 'no card'; return; }
    const d = derive(this._fs.value, Number(this._cluster.value), bytes);
    this._derived.textContent = d.ok
      ? `${d.clusters.toLocaleString()} clusters · FAT ${fmtBytes(d.fatBytes)} · about ${fmtDuration(d.estSeconds)}`
      : d.why;
  }

  async _format() {
    const c = this.st?.card;
    if (!c) { setNote(this._note, 'no card to format', true); return; }
    const fs = this._fs.value;
    const cluster = Number(this._cluster.value);
    const name = c.cid ? `${c.cid.oem} "${c.cid.name}" (${fmtBytes(c.bytes)})` : `the ${fmtBytes(c.bytes)} card`;
    if (!confirm(`Format ${name} as ${fs.toUpperCase()} with ${fmtCluster(cluster)} clusters?\n\nEverything on the card is erased.`)) return;
    const r = await sdFormat(fs, cluster);
    setNote(this._note, r.ok ? 'formatting…' : `format: ${r.error}`, !r.ok);
    this.soon();
  }

  async _runBench() {
    const r = await sdBench(10);
    setNote(this._note, r.ok ? 'bench running…' : `bench: ${r.error}`, !r.ok);
    this.soon();
  }

  async poll() {
    const s = await sdStatus();
    if (!s.ok) {
      setNote(this._note, s.status === 404 ? 'this firmware has no SD card support' : `status: ${s.error}`, true);
      return;
    }
    this.st = s;
    kvRows(this._status, sdRows(s));
    if (!this._seeded && s.card) {
      this._fs.value = s.defaults?.fs || defaultFs(s.card.bytes);
      this._seeded = true;
    }
    if (document.activeElement !== this._cluster && document.activeElement !== this._fs) this._fillClusters();
    this._showDerived();
    const f = s.format || {};
    const running = f.state === 'running';
    this._go.disabled = running || !s.card;
    this._bar.hidden = !running && f.state !== 'done';
    if (f.total) this._bar.value = Math.min(100, Math.round((100 * f.done) / f.total));
    if (running) setNote(this._note, `formatting: ${f.phase}…`);
    else if (f.state === 'done' && this._note.textContent.startsWith('formatting')) setNote(this._note, `formatted as ${f.fs}`);
    else if (f.state === 'error') setNote(this._note, `format failed: ${f.error}`, true);
    this.every = running ? 500 : 2000;
    const b = s.bench || {};
    this._benchGo.disabled = b.state === 'running' || s.state !== 'ok';
    setNote(this._bench, b.state === 'running' ? `running… ${b.done_kb} KB written`
      : b.state === 'done' ? `write ${fmtBytes(b.write_kBps * 1000)}/s · read ${fmtBytes(b.read_kBps * 1000)}/s · longest card busy ${(b.card_busy_max_us / 1000).toFixed(0)} ms`
        : b.state === 'error' ? `bench failed: ${b.error}` : '', b.state === 'error');
  }
}
