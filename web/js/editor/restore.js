// Restore a tar archive into a folder (or the root): read it, show what it
// would do, and only then write. Merge, never replace: files in the archive
// are added or overwrite their namesakes, everything else on the device
// stays. The parsing and the plan are tar.js's (pure, tested); this is the
// dialog and the writing.

import * as fs from '../fs.js';
import { Dialog } from '../modals/dialog.js';
import { parseTar, planRestore, foldersOf } from '../tar.js';
import { bytes } from './filemenu.js';

const esc = (s) => String(s).replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));

export class RestoreDialog {
  constructor(app, { onDone, track } = {}) {
    this.app = app;
    this.onDone = onDone;
    this.track = track;
    const body = document.createElement('div');
    body.className = 'rs-body';
    body.innerHTML = `
      <p class="rs-summary"></p>
      <div class="rs-lists"></div>
      <progress class="rs-progress" max="1" value="0" hidden></progress>
      <p class="rs-status dim"></p>
      <div class="rs-actions">
        <button type="button" class="pane-btn rs-cancel">Cancel</button>
        <button type="button" class="pane-btn rs-go">Restore</button>
      </div>`;
    this.dlg = app.registerDialog(new Dialog({ id: 'restore-dlg', title: 'Restore archive', body }));
    this.q = (s) => body.querySelector(s);
    this.q('.rs-cancel').addEventListener('click', () => {
      if (this.busy) { this.stop = true; this._job?.request(); }
      else this.dlg.close();
    });
    this.q('.rs-go').addEventListener('click', () => this.run());
  }

  // file: a File (.tar). target: '' for the root, else a folder path.
  async open(file, target) {
    this.plan = null;
    this.target = target;
    this.q('.rs-summary').textContent = `Reading ${file.name}…`;
    this.q('.rs-lists').innerHTML = '';
    this.q('.rs-status').textContent = '';
    this.q('.rs-progress').hidden = true;
    this.q('.rs-go').hidden = true;
    this.q('.rs-cancel').textContent = 'Close';
    this.dlg._overlay.querySelector('.dlg-title').textContent = `Restore ${file.name}`;
    this.dlg.open();
    let parsed;
    try {
      if (/\.(tgz|gz)$/i.test(file.name)) throw new Error('only plain .tar archives are taken -- unpack the .gz first (gunzip)');
      parsed = parseTar(new Uint8Array(await file.arrayBuffer()));
    } catch (e) {
      this.q('.rs-summary').textContent = `Cannot restore ${file.name}: ${e.message}`;
      return;
    }
    // What is there already, so the preview can say added or overwritten.
    const existing = new Set();
    for (const dir of foldersOf(parsed, target)) {
      const r = await fs.list(dir);
      if (r.offline) { this.q('.rs-summary').textContent = 'The device is unreachable.'; return; }
      if (!r.ok) continue;                          // a folder not there yet: everything in it is new
      for (const f of r.files) {
        if (!f.dir) existing.add(dir ? `${dir}/${f.name}` : f.name);
      }
    }
    const p = planRestore(parsed, target, existing);
    this.plan = p;
    const where = target ? `into ${target}/` : 'at the root of the filesystem';
    const n = p.adds.length + p.overwrites.length;
    this.q('.rs-summary').innerHTML =
      `${esc(n)} file${n === 1 ? '' : 's'} (${esc(bytes(p.bytes))}) ${esc(where)}: ` +
      `<b>${p.adds.length}</b> new, <b class="rs-warn">${p.overwrites.length}</b> overwritten` +
      (p.mkdirs.length ? `, ${p.mkdirs.length} empty folder${p.mkdirs.length === 1 ? '' : 's'}` : '') +
      (p.skipped.length ? `, ${p.skipped.length} skipped` : '') +
      '. Nothing on the device is deleted.';
    const list = (title, rows, open, cls = '') => rows.length ? `
      <details class="rs-list ${cls}"${open ? ' open' : ''}><summary>${esc(title)} (${rows.length})</summary>
        <ul>${rows.map((r) => `<li>${r}</li>`).join('')}</ul></details>` : '';
    this.q('.rs-lists').innerHTML =
      list('Overwritten', p.overwrites.map((w) => `${esc(w.path)} <span class="dim">${esc(bytes(w.data.length))}</span>`), true, 'rs-warn') +
      list('New', p.adds.map((w) => `${esc(w.path)} <span class="dim">${esc(bytes(w.data.length))}</span>`), p.adds.length <= 12) +
      list('Empty folders made', p.mkdirs.map((d) => `${esc(d)}/`), false) +
      list('Skipped', p.skipped.map((s) => `${esc(s.path)} <span class="dim">— ${esc(s.why)}</span>`), true);
    this.q('.rs-go').hidden = !n && !p.mkdirs.length;
    this.q('.rs-go').textContent = `Restore ${n} file${n === 1 ? '' : 's'}`;
    this.q('.rs-cancel').textContent = 'Cancel';
    setTimeout(() => this.q('.rs-go').focus(), 0);
  }

  async run() {
    const p = this.plan;
    if (!p || this.busy) return;
    this.busy = true;
    this.stop = false;
    this._job = this.track?.('Restoring archive') || null;
    this.q('.rs-go').hidden = true;
    this.q('.rs-cancel').textContent = 'Stop';
    const bar = this.q('.rs-progress');
    const status = this.q('.rs-status');
    const writes = [...p.overwrites, ...p.adds].sort((a, b) => a.path.localeCompare(b.path));
    const total = writes.length + p.mkdirs.length || 1;
    bar.hidden = false;
    bar.max = total;
    bar.value = 0;
    let done = 0, failed = null;
    const step = (label) => {
      const n = Math.min(total, done + 1);
      status.textContent = label;
      this._job?.set(`Restoring ${label} · ${n} of ${total}`, n / total);
    };
    for (const d of p.mkdirs) {
      if (this.stop) break;
      step(`${d}/`);
      const r = await fs.mkdir(d);
      if (!r.ok) { failed = `${d}/: ${r.error}`; break; }
      bar.value = ++done;
    }
    let files = 0;
    for (const w of failed ? [] : writes) {
      if (this.stop) break;
      step(w.path);
      const r = await fs.write(w.path, new Blob([w.data]));
      if (!r.ok) { failed = `${w.path}: ${r.error}`; break; }
      files++;
      bar.value = ++done;
    }
    this.busy = false;
    this.q('.rs-cancel').textContent = 'Close';
    if (this.stop) this._job?.aborted();
    else if (failed) this._job?.fail(failed);
    else this._job?.done(`Restored ${files} file${files === 1 ? '' : 's'}`);
    this._job = null;
    const left = writes.length - files;
    status.textContent = failed
      ? `Stopped at ${failed}. ${files} of ${writes.length} files were written; ${left} were not.`
      : this.stop ? `Stopped: ${files} of ${writes.length} files written.`
        : `Restored ${files} file${files === 1 ? '' : 's'}${p.mkdirs.length ? ` and ${p.mkdirs.length} folder${p.mkdirs.length === 1 ? '' : 's'}` : ''}.`;
    status.className = `rs-status${failed ? ' rs-err' : ''}`;
    this.onDone?.({ files, failed, target: this.target });
  }
}
