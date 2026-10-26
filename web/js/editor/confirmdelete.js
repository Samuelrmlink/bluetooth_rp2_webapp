// The confirmation before deleting a folder with everything in it: an
// in-app dialog rather than the browser's confirm(), because it has to show
// WHAT goes (every file, with its size), and because a browser may silently
// suppress its own dialogs ("prevent this page from creating additional
// dialogs"). Cancel is the default -- Enter and Esc both keep the files --
// and the gateway's own folders (cfg/, lua/, js/ ...) need their name typed
// before Delete does anything.

import { Dialog } from '../modals/dialog.js';
import { bytes } from './filemenu.js';

const esc = (s) => String(s).replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));
const SHOW_MAX = 300;

export class ConfirmDelete {
  constructor(app) {
    const body = document.createElement('div');
    body.className = 'cd-body';
    body.innerHTML = `
      <p class="cd-summary"></p>
      <div class="cd-list"></div>
      <p class="cd-tip dim">To keep a copy you can restore, cancel and use the folder's <b>Download as .tar</b> first.</p>
      <label class="cd-type" hidden><span class="cd-type-label"></span>
        <input class="cd-name" autocomplete="off" autocapitalize="off" spellcheck="false"></label>
      <div class="cd-actions">
        <button type="button" class="pane-btn cd-cancel">Cancel</button>
        <button type="button" class="pane-btn cd-delete">Delete</button>
      </div>`;
    this.dlg = app.registerDialog(new Dialog({ id: 'confirm-delete-dlg', title: 'Delete folder', body }));
    this.q = (s) => body.querySelector(s);
    this.q('.cd-cancel').addEventListener('click', () => this._end(false));
    this.q('.cd-delete').addEventListener('click', () => { if (!this.q('.cd-delete').disabled) this._end(true); });
    this.q('.cd-name').addEventListener('input', () => this._check());
    // Enter in the name box deletes only once the name matches; anywhere
    // else Enter presses the focused button, which starts as Cancel.
    this.q('.cd-name').addEventListener('keydown', (e) => {
      if (e.key === 'Enter') { e.preventDefault(); if (!this.q('.cd-delete').disabled) this._end(true); }
    });
    this.dlg.onClose = () => this._end(false);
  }

  // { path, files: [{ path, size }], dirs: [paths, the folder last], vital }
  // -> Promise<true when confirmed>
  ask({ path, files, dirs, vital }) {
    this._end(false);                                   // a dialog still open: that one is a no
    this.path = path;
    this.vital = vital;
    const sub = dirs.length - 1;
    const size = files.reduce((n, f) => n + f.size, 0);
    const empty = !files.length && !sub;
    this.dlg._overlay.querySelector('.dlg-title').textContent = `Delete ${path}/`;
    this.q('.cd-summary').innerHTML = empty
      ? `Delete the empty folder <b>${esc(path)}/</b> from the device?`
      : `Delete <b>${esc(path)}/</b> and everything in it: <b>${files.length} file${files.length === 1 ? '' : 's'}</b>`
        + `${sub ? ` in ${sub + 1} folders` : ''}, ${esc(bytes(size))}. <b class="cd-warn">This cannot be undone.</b>`;
    const rows = files.slice(0, SHOW_MAX).map((f) => `<li>${esc(f.path)} <span class="dim">${esc(bytes(f.size))}</span></li>`);
    if (files.length > SHOW_MAX) rows.push(`<li class="dim">… and ${files.length - SHOW_MAX} more</li>`);
    this.q('.cd-list').innerHTML = empty ? '' : `<ul>${rows.join('')}</ul>`;
    this.q('.cd-tip').hidden = empty;
    this.q('.cd-type').hidden = !vital;
    this.q('.cd-type-label').innerHTML = `<b>${esc(path)}/</b> is one of the gateway's own folders. Type <b>${esc(path)}</b> to delete it:`;
    this.q('.cd-name').value = '';
    this.q('.cd-delete').textContent = empty ? 'Delete folder' : `Delete ${files.length} file${files.length === 1 ? '' : 's'}`;
    this._check();
    return new Promise((resolve) => {
      this._resolve = resolve;
      this.dlg.open();
      setTimeout(() => (vital ? this.q('.cd-name') : this.q('.cd-cancel')).focus(), 0);
    });
  }

  // A picked set: one confirm for every path. `roots` are what was picked,
  // `files` and `dirs` are the expansion (a folder listed with its contents).
  // `vitals` are gateway folders in that expansion; the first name has to
  // be typed. Past eight roots the summary says how many more.
  askList({ roots, files, dirs, vitals = [] }) {
    this._end(false);
    const names = roots || [];
    const shown = names.slice(0, 8).map(esc);
    const more = names.length - shown.length;
    const who = shown.join(', ') + (more > 0 ? ` and ${more} more` : '');
    const size = (files || []).reduce((n, f) => n + (f.size || 0), 0);
    const nFiles = (files || []).length;
    const nDirs = (dirs || []).length;
    this.vital = vitals.length > 0;
    this.path = vitals[0] || '';
    this.dlg._overlay.querySelector('.dlg-title').textContent = names.length > 1 ? `Delete ${names.length} items` : `Delete ${names[0] || ''}`;
    const bits = [];
    if (nFiles) bits.push(`<b>${nFiles} file${nFiles === 1 ? '' : 's'}</b>`);
    if (nDirs) bits.push(`<b>${nDirs} folder${nDirs === 1 ? '' : 's'}</b>`);
    this.q('.cd-summary').innerHTML = `Delete ${who || 'the selection'} from the device`
      + (bits.length ? `: ${bits.join(' and ')}, ${esc(bytes(size))}` : '')
      + '. <b class="cd-warn">This cannot be undone.</b>';
    const rows = (files || []).slice(0, SHOW_MAX).map((f) => `<li>${esc(f.path)} <span class="dim">${esc(bytes(f.size || 0))}</span></li>`);
    if (nFiles > SHOW_MAX) rows.push(`<li class="dim">… and ${nFiles - SHOW_MAX} more</li>`);
    this.q('.cd-list').innerHTML = rows.length ? `<ul>${rows.join('')}</ul>` : '';
    this.q('.cd-tip').hidden = !nFiles;
    this.q('.cd-type').hidden = !this.vital;
    const named = vitals.map(esc).join(', ');
    this.q('.cd-type-label').innerHTML = vitals.length > 1
      ? `This includes the gateway's own folders <b>${named}</b>. Type <b>${esc(vitals[0])}</b> to delete them:`
      : `<b>${esc(vitals[0] || '')}/</b> is one of the gateway's own folders. Type <b>${esc(vitals[0] || '')}</b> to delete it:`;
    this.q('.cd-name').value = '';
    const noun = nFiles ? `${nFiles} file${nFiles === 1 ? '' : 's'}` : `${nDirs} folder${nDirs === 1 ? '' : 's'}`;
    this.q('.cd-delete').textContent = `Delete ${noun}`;
    this._check();
    return new Promise((resolve) => {
      this._resolve = resolve;
      this.dlg.open();
      setTimeout(() => (this.vital ? this.q('.cd-name') : this.q('.cd-cancel')).focus(), 0);
    });
  }

  _check() {
    this.q('.cd-delete').disabled = this.vital && this.q('.cd-name').value.trim() !== this.path;
  }

  _end(ok) {
    const r = this._resolve;
    if (!r) return;
    this._resolve = null;
    if (this.dlg.isOpen) this.dlg.close();
    r(ok);
  }
}
