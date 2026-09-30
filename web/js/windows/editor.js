// The Editor window: any file on the device, VS Code-shaped.
//
//   titlebar   the open file and its state, and what can be done to it
//   sidebar    the device's filesystem (Alt+B hides it); built as a tab host
//              with one tab, Files, so more tabs can join it later
//   editor     the textarea-over-highlight surface (editor/core.js)
//   status     mode, path, line:col, language, Lua arena use
//
// Saving and running are deliberately different things, as agreed:
//
//   Ctrl+S  writes the buffer to the file on LittleFS -- a real save
//   F5      sends the buffer as a DRAFT and runs it: RAM only, flash untouched,
//           so a script can be iterated on the device freely and saved once
//           it works
//
// Stage / Commit / Revert remain, in the ... menu, for the firmware's staged
// tier. Files are held as buffers keyed by path, so switching files never
// loses an unsaved edit; the sidebar marks the ones that differ.

import * as fs from '../fs.js';
import { fnv1a } from '../fnv.js';
import { ScriptIO } from '../scriptio.js';
import { EditorCore } from '../editor/core.js';
import { FileTree } from '../editor/filetree.js';
import { languageOf } from '../editor/highlight.js';
import { arenaMarks, shownArena, scriptNameOf, shadowedBy } from '../editor/status.js';

const ARENA_POLL_MS = 3000;
const LANG_NAMES = { lua: 'Lua', json: 'JSON', text: 'Plain text' };

const icon = (id) => `<svg aria-hidden="true"><use href="#i-${id}" xlink:href="#i-${id}"/></svg>`;

export class EditorWindow {
  constructor(el, ctx) {
    this.app = ctx.app;
    this.ws = ctx.ws;
    this.io = new ScriptIO(ctx.ws);
    this.buffers = new Map();       // path -> { text, base, lang, binary }
    this.path = null;
    this.marks = new Map();         // path -> { state, tier, hash } (editor/status.js)
    this.runtime = null;            // script.list's runtime block
    this.notice = '';
    this._pollTimer = 0;

    el.innerHTML = `
      <div class="ed-titlebar">
        <button type="button" class="tool-btn ed-b-sidebar" title="Files (Alt+B)" aria-label="Toggle file sidebar">${icon('sidebar')}</button>
        <span class="ed-title"><span class="ed-name">No file open</span><span class="ed-state"></span></span>
        <span class="ed-notice"></span>
        <span class="spacer"></span>
        <button type="button" class="tool-btn ed-b-save" title="Save to the device (Ctrl+S)" aria-label="Save">${icon('save')}</button>
        <button type="button" class="tool-btn ed-b-run" title="Run as a draft — nothing is written to flash (F5)" aria-label="Run">${icon('run')}</button>
        <button type="button" class="tool-btn ed-b-stop" title="Stop the script (Shift+F5)" aria-label="Stop">${icon('stop')}</button>
        <button type="button" class="tool-btn ed-b-new" title="New file" aria-label="New file">${icon('new')}</button>
        <button type="button" class="tool-btn ed-b-upload" title="Upload files from this computer" aria-label="Upload">${icon('upload')}</button>
        <button type="button" class="tool-btn ed-b-download" title="Download this file" aria-label="Download">${icon('download')}</button>
        <button type="button" class="tool-btn ed-b-more" title="More" aria-label="More actions">${icon('more')}</button>
        <input type="file" class="ed-upload-input" multiple hidden>
      </div>
      <div class="ed-main">
        <aside class="ed-sidebar">
          <div class="ed-tabs" role="tablist"><button type="button" class="ed-tab" role="tab" aria-selected="true">Files</button></div>
          <div class="ed-files" role="tree" aria-label="Files on the device"></div>
        </aside>
        <div class="ed-surface"></div>
      </div>
      <div class="ed-status">
        <span class="ed-s-mode"></span>
        <span class="ed-s-path dim"></span>
        <span class="spacer"></span>
        <span class="ed-s-pos"></span>
        <span class="ed-s-lang dim"></span>
        <span class="ed-s-arena dim"></span>
      </div>
      <div class="ed-more-menu pane-settings" hidden>
        <button type="button" class="pane-btn" data-act="stage">Stage (to staged/)</button>
        <button type="button" class="pane-btn" data-act="commit">Commit staged or draft to lua/</button>
        <button type="button" class="pane-btn" data-act="revert">Revert: drop the draft or staged copy</button>
        <hr>
        <button type="button" class="pane-btn" data-act="rename">Rename / move…</button>
        <button type="button" class="pane-btn" data-act="delete">Delete…</button>
        <button type="button" class="pane-btn" data-act="reload">Reload from the device</button>
      </div>`;
    const q = (s) => el.querySelector(s);
    this.el = el;
    this._name = q('.ed-name');
    this._state = q('.ed-state');
    this._notice = q('.ed-notice');
    this._sidebar = q('.ed-sidebar');
    this._files = q('.ed-files');
    this._sMode = q('.ed-s-mode');
    this._sPath = q('.ed-s-path');
    this._sPos = q('.ed-s-pos');
    this._sLang = q('.ed-s-lang');
    this._sArena = q('.ed-s-arena');
    this._more = q('.ed-more-menu');
    this._moreBtn = q('.ed-b-more');
    this._upload = q('.ed-upload-input');

    this.core = new EditorCore(q('.ed-surface'));
    this.core.onChange = (text) => this._edited(text);
    this.core.onCursor = (l, c) => { this._sPos.textContent = `Ln ${l}, Col ${c}`; };

    this.tree = new FileTree(this._files, {
      onOpen: (p) => this.open(p),
      mark: (p) => this._mark(p),
      onError: (m) => this.app.log('!', m),
    });
    this._files.addEventListener('click', (e) => {
      const row = e.target.closest('.ft-row');
      if (!row) return;
      this.tree.sel = row.dataset.path;
      const n = this.tree.find(row.dataset.path);
      if (n?.dir) this.tree.toggle(n);
      else this.open(row.dataset.path);
      this.tree.invalidate();
    });

    q('.ed-b-sidebar').addEventListener('click', () => this.toggleSidebar());
    q('.ed-b-save').addEventListener('click', () => this.save());
    q('.ed-b-run').addEventListener('click', () => this.run());
    q('.ed-b-stop').addEventListener('click', () => this.stop());
    q('.ed-b-new').addEventListener('click', () => this.newFile());
    q('.ed-b-upload').addEventListener('click', () => this._upload.click());
    q('.ed-b-download').addEventListener('click', () => this.download());
    this._upload.addEventListener('change', () => this.upload([...this._upload.files]));
    this._moreBtn.addEventListener('click', () => this.app.popover.toggle(this._moreBtn, this._more, 'editor-more'));
    this._more.addEventListener('click', (e) => {
      const act = e.target.closest('[data-act]')?.dataset.act;
      if (!act) return;
      this.app.popover.close();
      this[`_act_${act}`]?.();
    });

    this._applySidebar(this.app.prefs.sidebar);
    this._renderTitle();
    // Unsaved work is not lost to a stray reload.
    window.addEventListener('beforeunload', (e) => {
      if ([...this.buffers.values()].some((b) => this._isDirty(b))) e.preventDefault();
    });
    ctx.app.openFile = (p) => { ctx.app.show('editor'); return this.open(p); };
  }

  // --- keys --------------------------------------------------------------------

  attachKeys(keys) {
    this.keys = keys;
    keys.nav.registerList(this._files, this.tree.listController());
  }

  // Active editor window -> the 'editor' context: the workspace bindings
  // plus the editor's own (Ctrl+S, F5, Alt+B).
  context() { return 'editor'; }

  action(id) {
    if (id === 'editor-save') return this.save();
    if (id === 'editor-run') return this.run();
    if (id === 'editor-stop') return this.stop();
    if (id === 'editor-sidebar') return this.toggleSidebar();
    return undefined;
  }

  // --- visibility and polling ------------------------------------------------------

  onShow() {
    if (!this.tree.root.children) this.tree.load();
    this._poll();
    clearInterval(this._pollTimer);
    this._pollTimer = setInterval(() => this._poll(), ARENA_POLL_MS);
  }

  onHide() {
    clearInterval(this._pollTimer);
    this._pollTimer = 0;
  }

  onConnect() {
    if (this._pollTimer) { this.tree.refresh(); this._poll(); }
  }

  async _poll() {
    if (!this.ws.connected) return;
    try {
      const [loaded, list] = await Promise.all([this.ws.call('script.loaded'), this.ws.call('script.list')]);
      this.marks = arenaMarks(loaded.units);
      this.list = list;
      this.runtime = list.runtime || null;
      this.tree.invalidate();
      this._renderTitle();
    } catch { /* offline or an older firmware: markers simply stay as they were */ }
  }

  // --- buffers ---------------------------------------------------------------

  _isDirty(b) { return !b.binary && fnv1a(b.text) !== b.base; }

  _mark(path) {
    const b = this.buffers.get(path);
    return { dirty: !!b && this._isDirty(b), arena: shownArena(this.marks.get(path), b?.base) };
  }

  async open(path) {
    let b = this.buffers.get(path);
    if (!b) {
      if (/\.(gz|png|ico|bin)$/i.test(path)) {
        b = { text: '', base: fnv1a(''), lang: 'text', binary: true };
      } else {
        const r = await fs.readText(path);
        if (!r.ok) { this._say(`Could not open ${path}: ${r.error}`, 'err'); return false; }
        b = { text: r.text, base: fnv1a(r.text), lang: languageOf(path), binary: r.text.includes('\0') };
      }
      this.buffers.set(path, b);
    }
    this.path = path;
    this.core.setValue(b.binary ? '' : b.text, b.lang);
    this.core.readOnly = b.binary;
    this._say(b.binary ? 'Binary file — download it to inspect it.' : '');
    this.tree.reveal(path);
    this._renderTitle();
    this.core.focus();
    return true;
  }

  _edited(text) {
    const b = this.buffers.get(this.path);
    if (!b) return;
    b.text = text;
    this.tree.invalidate();
    this._renderTitle();
  }

  // --- the verbs ----------------------------------------------------------------

  async save() {
    const b = this.buffers.get(this.path);
    if (!b || b.binary) return;
    const text = this.core.value;
    const r = await fs.write(this.path, text);
    if (!r.ok) { this._say(`Save failed: ${r.error}`, 'err'); return; }
    b.text = text;
    b.base = fnv1a(text);
    this._say(`Saved ${this.path}`);
    await this._poll();
    // Saved, but is it what will run? draft > staged > committed.
    const by = shadowedBy(this.path, this.list);
    if (by) this._say(`Saved — but a ${by} copy runs instead of this file. Revert it from ⋯ to use the saved file.`, 'warn');
    // Refresh re-lists only directories already expanded; a first save into
    // a closed one (every new file, in a fresh session) would not appear.
    await this.tree.refresh();
    this.tree.reveal(this.path);
  }

  async run() {
    const name = this.path && scriptNameOf(this.path);
    if (!name) {
      this._say(this.path?.endsWith('.lua')
        ? 'This is a module: run the script that requires it.'
        : 'Only a script in lua/ or staged/ can be run.', 'warn');
      return;
    }
    const text = this.core.value;
    if (new TextEncoder().encode(text).length > 32 * 1024) {
      this._say('Too large to run as a draft (32 KB limit) — save it and start it instead.', 'err');
      return;
    }
    try {
      await this.io.draft(name, text, { run: true });
      this._say(`Running ${name} from the buffer (draft — flash untouched)`);
    } catch (e) {
      this._say(`Run failed: ${e.message}`, 'err');
    }
    this._poll();
  }

  async stop() {
    const name = this.path && scriptNameOf(this.path);
    if (!name) return;
    try {
      await this.ws.call('script.stop', { script: name });
      this._say(`Stopped ${name}`);
    } catch (e) {
      this._say(`Stop: ${e.message}`, 'err');
    }
    this._poll();
  }

  async _tier(op, done) {
    const name = this.path && scriptNameOf(this.path);
    if (!name) { this._say('Only a top-level script has a staged tier.', 'warn'); return; }
    try {
      if (op === 'script.stage') await this.io.draft(name, this.core.value, { run: false });
      await this.ws.call(op, { script: name });
      this._say(done(name));
    } catch (e) {
      this._say(`${op}: ${e.message}`, 'err');
    }
    await this._poll();
    this.tree.refresh();
  }

  _act_stage() { return this._tier('script.stage', (n) => `Staged ${n} to staged/${n}.lua`); }
  _act_commit() { return this._tier('script.commit', (n) => `Committed ${n} to lua/${n}.lua`); }
  _act_revert() { return this._tier('script.revert', (n) => `Reverted ${n}: the next tier down runs now`); }

  async _act_reload() {
    if (!this.path) return;
    const b = this.buffers.get(this.path);
    if (b && this._isDirty(b) && !confirm(`Discard unsaved changes to ${this.path}?`)) return;
    this.buffers.delete(this.path);
    this.open(this.path);
  }

  async _act_rename() {
    if (!this.path) return;
    const to = prompt('Rename or move to:', this.path)?.trim();
    if (!to || to === this.path) return;
    const r = await fs.rename(this.path, to);
    if (!r.ok) { this._say(`Rename failed: ${r.error}`, 'err'); return; }
    const b = this.buffers.get(this.path);
    this.buffers.delete(this.path);
    if (b) b.lang = languageOf(to);
    if (b) this.buffers.set(to, b);
    this.path = to;
    await this.tree.refresh();
    this.tree.reveal(to);
    this._renderTitle();
  }

  async _act_delete() {
    if (!this.path || !confirm(`Delete ${this.path} from the device? This cannot be undone.`)) return;
    const r = await fs.remove(this.path);
    if (!r.ok) { this._say(`Delete failed: ${r.error}`, 'err'); return; }
    this.buffers.delete(this.path);
    this._say(`Deleted ${this.path}`);
    this.path = null;
    this.core.setValue('', 'text');
    this.tree.refresh();
    this._renderTitle();
  }

  async newFile() {
    const name = prompt('New file (path on the device):', `${this.tree.currentDir}/`)?.trim();
    if (!name || name.endsWith('/')) return;
    // Not written until saved -- a new file is an unsaved buffer, like any
    // editor's. Its base is a hash no text has, so it shows as modified.
    this.buffers.set(name, { text: '', base: '', lang: languageOf(name), binary: false });
    this.path = name;
    this.core.setValue('', languageOf(name));
    this.core.readOnly = false;
    this._say(`New file — Ctrl+S writes ${name} to the device`);
    this.tree.invalidate();
    this._renderTitle();
    this.core.focus();
  }

  async upload(files) {
    const dir = this.tree.currentDir;
    for (const f of files) {
      const dest = dir ? `${dir}/${f.name}` : f.name;
      const r = await fs.write(dest, f);
      this._say(r.ok ? `Uploaded ${dest}` : `Upload of ${f.name} failed: ${r.error}`, r.ok ? '' : 'err');
    }
    this._upload.value = '';
    this.tree.refresh();
  }

  async download() {
    if (!this.path) return;
    const b = this.buffers.get(this.path);
    // The buffer if it is text (what you see is what you get, saved or not);
    // the device's bytes if it is binary.
    let blob;
    if (b && !b.binary) {
      blob = new Blob([this.core.value], { type: 'text/plain' });
    } else {
      const r = await fs.readBlob(this.path);
      if (!r.ok) { this._say(`Download failed: ${r.error}`, 'err'); return; }
      blob = r.blob;
    }
    const a = document.createElement('a');
    a.href = URL.createObjectURL(blob);
    a.download = this.path.split('/').pop();
    a.click();
    setTimeout(() => URL.revokeObjectURL(a.href), 1000);
  }

  toggleSidebar() {
    const open = this._sidebar.hidden;
    this._applySidebar(open);
    this.app.setPref('sidebar', open);
  }

  _applySidebar(open) {
    this._sidebar.hidden = !open;
    this.el.classList.toggle('ed-no-sidebar', !open);
  }

  // --- rendering -------------------------------------------------------------------

  _say(text, kind = '') {
    this.notice = text;
    this._notice.textContent = text;
    this._notice.className = `ed-notice${kind ? ' ' + kind : ''}`;
  }

  _renderTitle() {
    const b = this.path && this.buffers.get(this.path);
    this._name.textContent = this.path || 'No file open';
    const m = this.path ? this._mark(this.path) : {};
    const bits = [];
    if (m.dirty) bits.push('● modified');
    if (m.arena === 'loaded') bits.push('loaded');
    if (m.arena === 'stale') bits.push('loaded — changed since');
    this._state.textContent = bits.length ? `  ${bits.join(' · ')}` : '';
    this._state.className = `ed-state${m.arena ? ' ed-' + m.arena : ''}${m.dirty ? ' is-dirty' : ''}`;
    this._sPath.textContent = this.path || '';
    this._sLang.textContent = b ? LANG_NAMES[b.lang] || '' : '';
    this._sArena.textContent = this.runtime
      ? `arena ${Math.round(this.runtime.used / 1024)}/${Math.round(this.runtime.arena_bytes / 1024)} KB` : '';
    this._sMode.textContent = this.modeLabel || '';
  }

  render() {
    this.core.render();
    this.tree.render();
  }
}
