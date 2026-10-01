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
import { initialState, handle as vimHandle, modeLabel, clampNormal, searchMatches } from '../editor/vim.js';
import { copyText } from '../clipboard.js';

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
        <button type="button" class="tool-btn ed-b-keys" aria-pressed="false" title="Vim keys on/off — off, the text behaves like any text box (also Settings → UI)">VIM</button>
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
    // Vim: a pure reducer (editor/vim.js) driven from here. vcur is vim's
    // logical cursor -- in normal mode the textarea shows it as a one-
    // character selection, which is what makes it look like a block.
    this.vimOn = !!this.app.prefs.vim;
    this.vst = initialState();
    this.vcur = 0;
    this._painted = null;           // the selection _vimPaint last set
    this.core.ta.addEventListener('keydown', (e) => this._vimKey(e));
    // A selection made with the mouse or a finger, in normal mode, becomes
    // visual mode -- so y / d / c / p act on it. 'select' and selectionchange
    // also fire for the selection _vimPaint sets itself; _painted tells them apart.
    // Not WHILE a pointer is down: repainting the block cursor mid-drag
    // resets the browser's drag anchor, so a drag could never select more
    // than one character. The release decides.
    this._pointerDown = false;
    this.core.ta.addEventListener('pointerdown', () => { this._pointerDown = true; });
    window.addEventListener('pointerup', () => {
      if (!this._pointerDown) return;
      this._pointerDown = false;
      if (document.activeElement === this.core.ta) this._vimSync();
    }, true);
    window.addEventListener('pointercancel', () => { this._pointerDown = false; }, true);
    for (const ev of ['select', 'focus']) {
      this.core.ta.addEventListener(ev, () => { if (!this._pointerDown) this._vimSync(); });
    }
    document.addEventListener('selectionchange', () => {
      if (!this._pointerDown && document.activeElement === this.core.ta) this._vimSync();
    });
    // A paste in normal or visual mode (Ctrl+V, the phone's Paste) puts the
    // clipboard in as p would -- the only way the system clipboard reaches
    // the editor over http, where pages cannot read it on demand.
    this.core.ta.addEventListener('paste', (e) => this._vimPaste(e));
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
    this._keysBtn = q('.ed-b-keys');
    this._keysBtn.addEventListener('click', () => this.app.setPref('vim', !this.vimOn));
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
    // The Esc ladder: insert -> normal (vim takes it), then normal -> leave
    // the text (vim declines, the key layer blurs), then Alt+hjkl.
    keys.ownEscape(this.core.ta, (e) => this._vimEsc(e));
  }

  // --- vim ---------------------------------------------------------------------

  onPrefs(prefs) {
    if (!!prefs.vim === this.vimOn) return;
    this.vimOn = !!prefs.vim;
    this.vst = initialState();
    const p = this.core.ta.selectionStart;
    this.core.ta.setSelectionRange(p, p);
    this._painted = null;
    this._updateFind();
    this._renderTitle();
  }

  _vimEsc() {
    if (!this.vimOn) return false;
    const r = vimHandle(this.vst, 'Escape', this.core.value, this._vimCursor());
    if (r.action === 'leave') return false;
    this._vimApply(r);
    return true;
  }

  _vimCursor() {
    return this.vst.mode === 'insert' ? this.core.ta.selectionStart : this.vcur;
  }

  _vimKey(e) {
    if (!this.vimOn || this.core.readOnly) return;
    if (e.key === 'Escape' || e.altKey || e.metaKey || e.isComposing) return;
    if (['Shift', 'Control', 'Alt', 'Meta', 'CapsLock'].includes(e.key)) return;
    // Ctrl combos are the browser's (copy, paste, select all) except the
    // ones vim means: Ctrl+R redo.
    if (e.ctrlKey && e.key !== 'r') return;
    const r = vimHandle(this.vst, e.key, this.core.value, this._vimCursor(), e.ctrlKey);
    if (r.action === 'pass') { this.vst = r.state; return; }
    e.preventDefault();
    this._vimApply(r);
  }

  _vimApply(r) {
    const wasInsert = this.vst.mode === 'insert';
    const nowInsert = r.state.mode === 'insert';
    const h = this.core.history;
    // An insert session -- the command that opened it (o, cw, A...) plus
    // everything typed until Esc -- is ONE undo step, as in vim. Snapshot
    // before the opening command's edit; squash on the way out.
    if (!wasInsert && nowInsert) {
      h.seal();
      this._ins = { index: h.length, text: this.core.value, sel: [this.core.ta.selectionStart, this.core.ta.selectionEnd] };
    }
    this.vst = r.state;
    if (r.edit) this.core.replaceRange(r.edit.from, r.edit.to, r.edit.insert);
    if (!nowInsert && this._ins) {
      if (wasInsert) {
        const ta = this.core.ta;
        h.squashSince(this._ins.index, this._ins.text, this.core.value, this._ins.sel, [ta.selectionStart, ta.selectionEnd]);
      }
      this._ins = null;
    }
    this.vcur = r.cursor;
    switch (r.action) {
      case 'undo': this.core.undo(); this.vcur = clampNormal(this.core.value, this.core.ta.selectionStart); break;
      case 'redo': this.core.redo(); this.vcur = clampNormal(this.core.value, this.core.ta.selectionStart); break;
      case 'ex': this._ex(r.ex); break;
      case 'info': this._say(r.message || ''); break;
      case 'unknown': this._say(r.message || 'Unknown command', 'warn'); break;
      default: break;
    }
    if (r.yank !== undefined) this._yanked(r.yank);
    this._vimPaint();
    this._updateFind();
    this._renderTitle();
  }

  // Yanks go to the system clipboard too (vim's clipboard=unnamedplus), so
  // what is yanked here pastes anywhere else.
  async _yanked(text) {
    const ok = await copyText(text);
    this._vimPaint();                 // the copy borrowed the selection
    const n = text.length;
    this._say(ok ? `Yanked ${n} character${n === 1 ? '' : 's'} (copied to the clipboard)` : `Yanked ${n} characters`);
  }

  _vimPaste(e) {
    if (!this.vimOn || this.vst.mode === 'insert' || this.core.readOnly) return;
    const text = e.clipboardData?.getData('text/plain');
    e.preventDefault();
    if (!text) return;
    // As p would, from a register holding the clipboard -- without replacing
    // the editor's own register.
    const keep = this.vst.reg;
    const visual = this.vst.mode === 'visual' || this.vst.mode === 'vline';
    const tmp = { ...this.vst, reg: { text, linewise: text.endsWith('\n') } };
    const r = vimHandle(tmp, visual ? 'P' : 'p', this.core.value, this._vimCursor());
    r.state = { ...r.state, reg: keep };
    this._vimApply(r);
  }

  // The search highlight: what is being typed after / or ?, else the last
  // search while it is on (until :noh). Called after anything that can move
  // the cursor, change the text, or change the search.
  _updateFind() {
    const st = this.vst;
    let pat = '', word = false;
    if (this.vimOn && this.path) {
      if (st.cmd && (st.cmd[0] === '/' || st.cmd[0] === '?') && st.cmd.length > 1) pat = st.cmd.slice(1);
      else if (st.hl && st.search) { pat = st.search; word = st.word; }
    }
    if (!pat) { this.core.setFind(null); return; }
    const at = searchMatches(this.core.value, pat, word);
    this.core.setFind(at, pat.length, at.includes(this.vcur) ? this.vcur : -1);
  }

  // --- : commands ------------------------------------------------------------------

  async _ex({ cmd, arg, force }) {
    const b = this.path && this.buffers.get(this.path);
    const dirty = !!b && this._isDirty(b);
    switch (cmd) {
      case 'w':
        if (arg) return this.writeCopy(arg);
        return this.save();
      case 'q':
        if (!this.path) return undefined;
        if (dirty && !force) { this._say('E37: No write since last change (add ! to override)', 'warn'); return undefined; }
        return this.close();
      case 'wq':
        if (await this.save()) this.close();
        return undefined;
      case 'x':                        // write only if modified, then close
        if (!dirty || await this.save()) this.close();
        return undefined;
      case 'e':
        if (arg) return this.open(arg, { create: true });
        if (!this.path) return undefined;
        if (dirty && !force) { this._say('E37: No write since last change (add ! to override)', 'warn'); return undefined; }
        this.buffers.delete(this.path);              // :e! -- back to what is on the device
        return this.open(this.path);
      case 'run': return this.run();
      case 'stop': return this.stop();
      default: return undefined;
    }
  }

  // A click moves vim's cursor to where it landed; a drag (or a touch
  // selection) becomes visual mode over what was selected.
  _vimSync() {
    if (!this.vimOn || this.vst.mode === 'insert') return;
    const ta = this.core.ta;
    const a = ta.selectionStart, b = ta.selectionEnd;
    const p = this._painted;
    if (p && p[0] === a && p[1] === b) return;           // our own selection
    const t = this.core.value;
    // A one-character selection is how normal mode draws its cursor, so only
    // a longer one is the user selecting.
    if (b - a > 1) {
      const back = ta.selectionDirection === 'backward';
      this.vst = { ...this.vst, mode: 'visual', anchor: back ? b - 1 : a };
      this.vcur = back ? a : b - 1;
    } else {
      this.vcur = clampNormal(t, a);
      if (this.vst.mode !== 'normal') this.vst = { ...this.vst, mode: 'normal' };
    }
    this._vimPaint();
    this._updateFind();
    this._renderTitle();
  }

  _vimPaint() {
    const ta = this.core.ta;
    const t = ta.value;
    const c = Math.max(0, Math.min(this.vcur, t.length));
    const st = this.vst;
    if (!this.vimOn || st.mode === 'insert') { ta.setSelectionRange(c, c); this._painted = [c, c]; return; }
    if (st.mode === 'visual' || st.mode === 'vline') {
      let a = Math.min(st.anchor, c), b = Math.max(st.anchor, c) + 1;
      if (st.mode === 'vline') {
        a = t.lastIndexOf('\n', a - 1) + 1;
        const e = t.indexOf('\n', b - 1);
        b = e < 0 ? t.length : e;
      }
      ta.setSelectionRange(a, Math.min(b, t.length), c < st.anchor ? 'backward' : 'forward');
      this._painted = [a, Math.min(b, t.length)];
      return;
    }
    // normal: a one-character selection is the block cursor, except on an
    // empty line or at the very end, where there is no character to cover
    const block = c < t.length && t[c] !== '\n' ? 1 : 0;
    ta.setSelectionRange(c, c + block);
    this._painted = [c, c + block];
  }

  // Active editor window -> the 'editor' context: the workspace bindings
  // plus the editor's own (Ctrl+S, F5, Alt+B).
  context() { return 'editor'; }

  action(id) {
    if (id === 'editor-save') return this.save();
    if (id === 'editor-run') return this.run();
    if (id === 'editor-stop') return this.stop();
    if (id === 'editor-sidebar') return this.toggleSidebar();
    if (id === 'editor-to-files') return this.focusFiles();
    if (id === 'editor-to-text') return this.focusText();
    return undefined;
  }

  // Ctrl+H: into the file list, ready for j/k (opening the sidebar if it is
  // shut). Ctrl+L: back into the text.
  focusFiles() {
    if (this._sidebar.hidden) { this._applySidebar(true); this.app.setPref('sidebar', true); }
    this._files.focus({ preventScroll: true });
    this._files.setAttribute('data-nav-active', '');
    this.tree.listController().enter();
    this.tree.invalidate();
  }

  focusText() {
    this.core.focus();
    this._vimSync();
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

  // { create: true } (from :e) opens a path that does not exist yet as a new,
  // unsaved buffer -- as vim does -- rather than reporting it missing.
  async open(path, { create = false } = {}) {
    let b = this.buffers.get(path);
    if (!b) {
      if (/\.(gz|png|ico|bin)$/i.test(path)) {
        b = { text: '', base: fnv1a(''), lang: 'text', binary: true };
      } else {
        const r = await fs.readText(path);
        if (!r.ok && create && r.status === 404) {
          this.buffers.set(path, { text: '', base: '', lang: languageOf(path), binary: false });
          this._openBuffer(path);
          this._say(`"${path}" [New] — :w writes it to the device`);
          return true;
        }
        if (!r.ok) { this._say(`Could not open ${path}: ${r.error}`, 'err'); return false; }
        b = { text: r.text, base: fnv1a(r.text), lang: languageOf(path), binary: r.text.includes('\0') };
      }
      this.buffers.set(path, b);
    }
    this._openBuffer(path);
    this._say(b.binary ? 'Binary file — download it to inspect it.' : '');
    return true;
  }

  _openBuffer(path) {
    const b = this.buffers.get(path);
    this.path = path;
    this.core.setValue(b.binary ? '' : b.text, b.lang);
    this.core.readOnly = b.binary;
    // A fresh vim state, but the last search survives a change of file, as in vim.
    const { search, word, sdir, hl } = this.vst;
    this.vst = { ...initialState(), search, word, sdir, hl };
    this.vcur = 0;
    this._painted = null;
    this.tree.reveal(path);
    this._updateFind();
    this._renderTitle();
    this.core.focus();
  }

  // :q -- close the file. Its buffer goes with it (unsaved changes too: the
  // caller has already asked, or was told to with !). Focus moves to the file
  // list, where the next file is picked.
  close() {
    if (!this.path) return;
    const was = this.path;
    this.buffers.delete(was);
    this.path = null;
    this.core.setValue('', 'text');
    this.core.readOnly = true;
    this.vst = { ...initialState(), search: this.vst.search, word: this.vst.word, sdir: this.vst.sdir, hl: this.vst.hl };
    this.core.setFind(null);
    this._say(`Closed ${was}`);
    this.tree.invalidate();
    this._renderTitle();
    this.focusFiles();
  }

  _edited(text) {
    const b = this.buffers.get(this.path);
    if (!b) return;
    b.text = text;
    this.tree.invalidate();
    this._updateFind();
    this._renderTitle();
  }

  // --- the verbs ----------------------------------------------------------------

  // True when the file was written.
  async save() {
    const b = this.buffers.get(this.path);
    if (!b || b.binary) return false;
    const text = this.core.value;
    const r = await fs.write(this.path, text);
    if (!r.ok) { this._say(`Save failed: ${r.error}`, 'err'); return false; }
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
    return true;
  }

  // :w <path> -- write the buffer to another file and stay on this one, as
  // vim does.
  async writeCopy(path) {
    if (!this.path && !this.core.value) return false;
    const r = await fs.write(path, this.core.value);
    if (!r.ok) { this._say(`Write to ${path} failed: ${r.error}`, 'err'); return false; }
    this._say(`"${path}" written`);
    await this.tree.refresh();
    return true;
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
    // A new file starts in insert mode: there is nothing to move over yet.
    this.vst = { ...initialState(), mode: 'insert' };
    this.vcur = 0;
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
    this._sMode.textContent = this.vimOn ? modeLabel(this.vst) : '';
    this._keysBtn.setAttribute('aria-pressed', String(this.vimOn));
    this._keysBtn.classList.toggle('on', this.vimOn);
  }

  render() {
    this.core.render();
    this.tree.render();
  }
}
