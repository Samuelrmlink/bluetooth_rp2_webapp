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
import { openCellMenu } from '../controls/cellmenu.js';
import { fileMenuItems, fileInfo, dirInfo, folderSize, sizeInfo } from '../editor/filemenu.js';
import { LongPress } from '../editor/longpress.js';
import { RestoreDialog } from '../editor/restore.js';
import { ImagePreview } from '../editor/preview.js';
import { isGz, plainName, imageKind, imageType, isWebAsset, shadowInfo, gzipText } from '../gz.js';
import { bytes } from '../editor/filemenu.js';
import { TabState, dropZone, tabIndexAt } from '../editor/tabs.js';
import { ConfirmDelete } from '../editor/confirmdelete.js';
import { EditorHelp } from '../editor/help.js';
import { planDrop, dropDir, moveDest, treeOf, deleteTree, isVital, dropEntries, readDrop } from '../editor/dnd.js';

const ARENA_POLL_MS = 3000;
const LANG_NAMES = { lua: 'Lua', json: 'JSON', text: 'Plain text' };

const TABS_KEY = 'btgw.editor.tabs';
// What a drag carries: a row of the file list, or an editor tab ({ path, gid }).
const ROW_TYPE = 'application/x-btgw-path';
const TAB_TYPE = 'application/x-btgw-tab';
const escHtml = (s) => String(s).replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));

const icon = (id) => `<svg aria-hidden="true"><use href="#i-${id}" xlink:href="#i-${id}"/></svg>`;

export class EditorWindow {
  constructor(el, ctx) {
    this.app = ctx.app;
    this.ws = ctx.ws;
    this.io = new ScriptIO(ctx.ws);
    this.buffers = new Map();       // path -> { text, base, lang, binary }, shared by every tab and half
    // Tabs and the split (editor/tabs.js); each half is a view with its own
    // text surface, cursor, undo and vim state (_makeView). `this.path`,
    // `this.core` and the vim state are always the FOCUSED half's.
    this.tabs = new TabState();
    this.views = new Map();         // group id -> view
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
        <button type="button" class="tool-btn ed-b-svg" title="Show the SVG as a picture or as text" hidden>Text</button>
        <button type="button" class="tool-btn ed-b-split" title="Split the editor: the file in a second half, side by side (Alt+\\) — again to unsplit" aria-label="Split editor" aria-pressed="false">${icon('split')}</button>
        <button type="button" class="tool-btn ed-b-help" title="The editor's keys (Alt+/)" aria-label="Editor keys">?</button>
        <button type="button" class="tool-btn ed-b-more" title="More" aria-label="More actions">${icon('more')}</button>
        <input type="file" class="ed-upload-input" multiple hidden>
        <input type="file" class="ed-tar-input" accept=".tar,application/x-tar" hidden>
      </div>
      <div class="ed-main">
        <aside class="ed-sidebar">
          <div class="ed-tabs" role="tablist"><button type="button" class="ed-tab" role="tab" aria-selected="true">Files</button></div>
          <div class="ed-files" role="tree" aria-label="Files on the device"></div>
        </aside>
        <div class="ed-groups"></div>
      </div>
      <div class="ed-status">
        <span class="ed-s-mode"></span>
        <span class="ed-s-path dim"></span>
        <span class="ed-s-size dim"></span>
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
        <hr>
        <button type="button" class="pane-btn" data-act="split">Split / unsplit the editor (Alt+\\)</button>
        <button type="button" class="pane-btn" data-act="flip">Side by side ↔ one above the other (Alt+Shift+\\)</button>
        <button type="button" class="pane-btn" data-act="other">Move this tab to the other half (Alt+Shift+O)</button>
        <hr>
        <button type="button" class="pane-btn" data-act="tarall">Download everything (.tar)</button>
        <button type="button" class="pane-btn" data-act="restore">Restore archive… (.tar)</button>
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
    this._sSize = q('.ed-s-size');
    this._sPos = q('.ed-s-pos');
    this._sLang = q('.ed-s-lang');
    this._sArena = q('.ed-s-arena');
    this._more = q('.ed-more-menu');
    this._moreBtn = q('.ed-b-more');
    this._upload = q('.ed-upload-input');
    this._tarInput = q('.ed-tar-input');
    this._svgBtn = q('.ed-b-svg');

    this._groups = q('.ed-groups');
    // Between the halves: drag it (or double-click: even halves); from the
    // keyboard, Alt+> / Alt+< / Alt+. (editor-split-grow / -shrink / -even).
    this._splitter = document.createElement('div');
    this._splitter.className = 'ed-splitter';
    this._splitter.setAttribute('role', 'separator');
    this._splitter.title = 'Drag to resize · double-click: even halves · Alt+> / Alt+< / Alt+.';
    this._setupSplitter();
    this.confirmDelete = new ConfirmDelete(this.app);
    this.help = new EditorHelp(this.app);
    this._splitBtn = q('.ed-b-split');
    this.restorer = new RestoreDialog(this.app, {
      onDone: async ({ target }) => { await this.tree.refresh(); if (target) this.tree.reveal(target); this._poll(); },
    });
    // Vim: a pure reducer (editor/vim.js) driven from here, one state per
    // half. vcur is vim's logical cursor -- in normal mode the textarea shows
    // it as a one-character selection, which is what makes it look like a block.
    this.vimOn = !!this.app.prefs.vim;
    // A selection made with the mouse or a finger, in normal mode, becomes
    // visual mode -- so y / d / c / p act on it. 'select' and selectionchange
    // also fire for the selection _vimPaint sets itself; _painted tells them apart.
    // Not WHILE a pointer is down: repainting the block cursor mid-drag
    // resets the browser's drag anchor, so a drag could never select more
    // than one character. The release decides.
    this._pointerDown = false;
    window.addEventListener('pointerup', () => {
      if (!this._pointerDown) return;
      this._pointerDown = false;
      if (document.activeElement === this.core.ta) this._vimSync();
    }, true);
    window.addEventListener('pointercancel', () => { this._pointerDown = false; }, true);
    document.addEventListener('selectionchange', () => {
      if (!this._pointerDown && document.activeElement === this.core.ta) this._vimSync();
    });
    this._layout();

    this.tree = new FileTree(this._files, {
      onOpen: (p) => this.open(p),
      mark: (p) => this._mark(p),
      onError: (m) => this.app.log('!', m),
    });
    // Right-click a file or folder: its menu (openFileMenu). The keyboard
    // equivalent is m / Shift+F10 / the Menu key in the list (attachKeys);
    // on a touch screen, a long press on the row, or its ⋯ button.
    this._fileMenu = document.createElement('div');
    this._fileMenu.className = 's-menu ed-file-menu';
    this._press = new LongPress({ onFire: (p) => this._rowMenu(p) });
    // On the window, not the list: opening the menu re-renders the rows, and
    // a row taken out of the page no longer passes its pointer's events up;
    // and every pointerdown, anywhere, must reach the press (longpress.js).
    window.addEventListener('pointerdown', (e) => {
      const row = this._files.contains(e.target) && !e.target.closest('.ft-more') && e.target.closest('.ft-row');
      this._press.down({ id: e.pointerId, x: e.clientX, y: e.clientY, type: e.pointerType, key: row ? row.dataset.path : null, primary: e.isPrimary });
    }, true);
    window.addEventListener('pointermove', (e) => this._press.move({ id: e.pointerId, x: e.clientX, y: e.clientY }));
    window.addEventListener('pointerup', (e) => this._press.up({ id: e.pointerId }));
    window.addEventListener('pointercancel', (e) => this._press.up({ id: e.pointerId }));
    // The click that follows a long press, wherever it lands: the menu has
    // just opened under the finger, and the lift must not press Delete…
    // (detail 0 is a keyboard's click, never the finger's).
    window.addEventListener('click', (e) => {
      if (e.detail > 0 && this._press.takeClick()) { e.preventDefault(); e.stopPropagation(); }
    }, true);
    this._files.addEventListener('contextmenu', (e) => {
      const row = e.target.closest('.ft-row');
      if (!row) return;
      e.preventDefault();
      // Mobile browsers send their own contextmenu on a long press: that is
      // the press firing (early, perhaps), never a second menu.
      if (this._press.active || e.pointerType === 'touch' || e.pointerType === 'pen') { this._press.fire(); return; }
      this._rowMenu(row.dataset.path);
    });
    this._setupDnd(el);
    this._files.addEventListener('click', (e) => {
      const row = e.target.closest('.ft-row');
      if (!row) return;
      if (e.target.closest('.ft-more')) { this._rowMenu(row.dataset.path); return; }   // the row's menu, not the row
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
    q('.ed-b-upload').addEventListener('click', () => { this._uploadDir = undefined; this._upload.click(); });
    q('.ed-b-download').addEventListener('click', () => this.download());
    this._upload.addEventListener('change', () => this.upload([...this._upload.files], this._uploadDir));
    this._tarInput.addEventListener('change', () => {
      const f = this._tarInput.files[0];
      this._tarInput.value = '';
      if (f) this.restorer.open(f, this._tarTarget || '');
    });
    this._svgBtn.addEventListener('click', () => this._toggleSvg());
    this._splitBtn.addEventListener('click', () => this.toggleSplit());
    q('.ed-b-help').addEventListener('click', () => this.help.toggle());
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
    const files = this.tree.listController();
    // First refusal on a key in the file list: the selected file's menu.
    files.key = (e) => {
      if (e.ctrlKey || e.altKey || e.metaKey) return false;
      // s / i: the selected file in a split, side by side / stacked (NERDTree's keys)
      if ((e.key === 's' || e.key === 'i') && !e.shiftKey) {
        const n = this.tree.sel && this.tree.find(this.tree.sel);
        if (!n || n.dir) return false;
        e.preventDefault();
        this.openSplit(n.path, e.key === 's' ? 'right' : 'bottom');
        return true;
      }
      if (e.key === 'ContextMenu' || (e.key === 'F10' && e.shiftKey) || (e.key === 'm' && !e.shiftKey)) {
        if (!this.tree.sel) return false;
        e.preventDefault();
        this.openFileMenu(this.tree.sel);
        return true;
      }
      return false;
    };
    keys.nav.registerList(this._files, files);
    // The Esc ladder: insert -> normal (vim takes it), then normal -> leave
    // the text (vim declines, the key layer blurs), then Alt+hjkl.
    for (const v of this.views.values()) keys.ownEscape(v.core.ta, (e) => this._vimEsc(e));
  }

  // --- tabs and the split ----------------------------------------------------------

  // The focused half, and its state under the names the rest of this file
  // has always used.
  get v() { return this.views.get(this.tabs.group.id); }
  get core() { return this.v.core; }
  get preview() { return this.v.preview; }
  get path() { return this.tabs.activePath(); }
  get vst() { return this.v.vst; }
  set vst(st) { this.v.vst = st; }
  get vcur() { return this.v.vcur; }
  set vcur(c) { this.v.vcur = c; }
  get _painted() { return this.v.painted; }
  set _painted(p) { this.v.painted = p; }
  get _ins() { return this.v.ins; }
  set _ins(i) { this.v.ins = i; }

  // One half: a tab bar over a text surface (and the picture preview).
  _makeView(id) {
    const el = document.createElement('div');
    el.className = 'ed-group';
    el.innerHTML = '<div class="ed-tabbar" role="tablist" aria-label="Open files" hidden></div><div class="ed-surface"></div>'
      + '<div class="ed-dropzone" hidden></div><div class="ed-tabmark" hidden></div>';
    const v = { id, el, bar: el.querySelector('.ed-tabbar'), shown: null, saved: new Map(),
                vst: initialState(), vcur: 0, painted: null, ins: null, sig: '' };
    v.core = new EditorCore(el.querySelector('.ed-surface'));
    v.preview = new ImagePreview(el.querySelector('.ed-surface'));
    v.core.readOnly = true;
    const ta = v.core.ta;
    // Whichever half is touched or focused is the one keys and toolbar act on.
    el.addEventListener('pointerdown', () => this._focusGroup(id), true);
    el.addEventListener('focusin', () => this._focusGroup(id));
    ta.addEventListener('keydown', (e) => this._vimKey(e));
    ta.addEventListener('pointerdown', () => { this._pointerDown = true; });
    for (const ev of ['select', 'focus']) {
      ta.addEventListener(ev, () => {
        if (ev === 'focus') this._focusGroup(id);
        if (!this._pointerDown && this.v === v) this._vimSync();
      });
    }
    // A paste in normal or visual mode (Ctrl+V, the phone's Paste) puts the
    // clipboard in as p would -- the only way the system clipboard reaches
    // the editor over http, where pages cannot read it on demand.
    ta.addEventListener('paste', (e) => this._vimPaste(e));
    v.core.onChange = (text) => this._edited(text, v);
    v.core.onCursor = (l, c) => { if (this.v === v) this._sPos.textContent = `Ln ${l}, Col ${c}`; };
    v.bar.addEventListener('click', (e) => {
      const t = e.target.closest('.ed-ftab');
      if (!t) return;
      const gi = this.tabs.groups.findIndex((g) => g.id === id);
      if (e.target.closest('.ed-ftab-x')) { this.closeTab(t.dataset.path, gi); return; }
      this.tabs.focus = gi;
      this.open(t.dataset.path);
    });
    // middle-click closes, as in every browser and editor
    v.bar.addEventListener('auxclick', (e) => {
      const t = e.button === 1 && e.target.closest('.ed-ftab');
      if (!t) return;
      e.preventDefault();
      this.closeTab(t.dataset.path, this.tabs.groups.findIndex((g) => g.id === id));
    });
    this.keys?.ownEscape(ta, (e) => this._vimEsc(e));
    this._setupViewDnd(v);
    return v;
  }

  // Views follow the groups: made when a half appears, dropped when it goes,
  // in the groups' order (moved only when out of order: moving a node takes
  // the focus out of it).
  _layout() {
    const ids = this.tabs.groups.map((g) => g.id);
    for (const [id, v] of this.views) {
      if (ids.includes(id)) continue;
      v.preview.hide();
      v.el.remove();
      this.views.delete(id);
    }
    for (const id of ids) if (!this.views.has(id)) this.views.set(id, this._makeView(id));
    // [half, splitter, half] or [half]; nodes moved only when out of place
    // (moving a node takes the focus out of it)
    const want = ids.map((id) => this.views.get(id).el);
    if (want.length > 1) want.splice(1, 0, this._splitter);
    else this._splitter.remove();
    want.forEach((el, i) => { if (this._groups.children[i] !== el) this._groups.insertBefore(el, this._groups.children[i] || null); });
    ids.forEach((id, i) => this.views.get(id).el.classList.toggle('is-focus', i === this.tabs.focus));
    this._applyRatio();
    this._groups.classList.toggle('is-split', this.tabs.split);
    this._groups.classList.toggle('ed-stacked', this.tabs.orient === 'v');
    this._splitBtn.setAttribute('aria-pressed', String(this.tabs.split));
    this._splitBtn.classList.toggle('on', this.tabs.split);
  }

  // The halves' shares of the room.
  _applyRatio() {
    const els = this.tabs.groups.map((g) => this.views.get(g.id)?.el).filter(Boolean);
    const r = this.tabs.ratio;
    els.forEach((el, i) => { el.style.flex = els.length > 1 ? `${i ? 1 - r : r} 1 0` : ''; });
    this._splitter.setAttribute('aria-orientation', this.tabs.orient === 'h' ? 'vertical' : 'horizontal');
    this._splitter.setAttribute('aria-valuenow', String(Math.round(r * 100)));
  }

  _setupSplitter() {
    const sp = this._splitter;
    let drag = null;
    sp.addEventListener('pointerdown', (e) => {
      if (e.button !== 0) return;
      e.preventDefault();
      sp.setPointerCapture(e.pointerId);
      drag = { id: e.pointerId };
      sp.classList.add('is-dragging');
      this._groups.classList.add('is-resizing');
    });
    sp.addEventListener('pointermove', (e) => {
      if (!drag || e.pointerId !== drag.id) return;
      const r = this._groups.getBoundingClientRect();
      this.tabs.setRatio(this.tabs.orient === 'h' ? (e.clientX - r.left) / r.width : (e.clientY - r.top) / r.height);
      this._applyRatio();
    });
    const end = (e) => {
      if (!drag || e.pointerId !== drag.id) return;
      drag = null;
      sp.classList.remove('is-dragging');
      this._groups.classList.remove('is-resizing');
      this._saveTabs();
    };
    sp.addEventListener('pointerup', end);
    sp.addEventListener('pointercancel', end);
    sp.addEventListener('dblclick', () => this.resizeSplit(null));
  }

  // From the keyboard: the focused half grows (+) or shrinks (-) by a step;
  // null evens them out.
  resizeSplit(delta) {
    if (!this.tabs.split) { this._say('Not split — Alt+\\ splits the editor', 'warn'); return; }
    if (delta == null) this.tabs.setRatio(0.5); else this.tabs.resize(delta);
    this._applyRatio();
    this._saveTabs();
    const share = this.tabs.focus === 0 ? this.tabs.ratio : 1 - this.tabs.ratio;
    this._say(`This half: ${Math.round(share * 100)}% of the editor`);
  }

  // vim's :resize / :vertical resize -- N lines (stacked) or columns (side
  // by side) for the focused half; +N / -N relative; none: as big as allowed.
  resizeEx(arg, axis) {
    if (!this.tabs.split) { this._say('Not split — :sp or :vs first', 'warn'); return; }
    if (this.tabs.orient !== axis) {
      this._say(axis === 'v' ? 'The halves are side by side: :vertical resize sets the width' : 'The halves are stacked: :resize sets the height', 'warn');
      return;
    }
    const ta = this.core.ta;
    const cs = getComputedStyle(ta);
    let unit;
    if (axis === 'v') unit = parseFloat(cs.lineHeight) || parseFloat(cs.fontSize) * 1.4;
    else {
      const c = (this._measure ||= document.createElement('canvas')).getContext('2d');
      c.font = `${cs.fontSize} ${cs.fontFamily}`;
      unit = c.measureText('0').width || 8;
    }
    const box = this._groups.getBoundingClientRect();
    const total = axis === 'v' ? box.height : box.width;
    const share = this.tabs.focus === 0 ? this.tabs.ratio : 1 - this.tabs.ratio;
    const m = /^([+-]?)(\d+)$/.exec(arg.trim());
    let want;
    if (!arg.trim()) want = 1;
    else if (!m) { this._say(`:resize takes a number, +N or -N`, 'warn'); return; }
    else {
      const n = Number(m[2]) * unit / total;
      want = m[1] === '+' ? share + n : m[1] === '-' ? share - n : n;
    }
    this.tabs.setRatio(this.tabs.focus === 0 ? want : 1 - want);
    this._applyRatio();
    this._saveTabs();
  }

  // Open a file in a split on a side ('right' / 'bottom' from the file list's
  // s / i), loading it first.
  async openSplit(path, side) {
    if (!(await this._load(path))) return false;
    this.tabs.splitTo(side, path);
    this._afterTabs();
    return true;
  }

  // Drags onto a half: its tab bar (a place in the row) and its text (an
  // edge splits there, the middle opens it in this half). What can be
  // dragged: a tab (from this half or the other) or a file from the list.
  _setupViewDnd(v) {
    const kind = (e) => {
      const t = [...(e.dataTransfer?.types || [])];
      return t.includes(TAB_TYPE) ? 'tab' : t.includes(ROW_TYPE) ? 'row' : null;
    };
    const gi = () => this.tabs.groups.findIndex((g) => g.id === v.id);
    const zoneEl = v.el.querySelector('.ed-dropzone');
    const mark = v.el.querySelector('.ed-tabmark');
    const clear = () => { zoneEl.hidden = true; mark.hidden = true; };
    const barIndex = (e) => {
      const tabs = [...v.bar.querySelectorAll('.ed-ftab')];
      const i = tabIndexAt(tabs.map((t) => t.getBoundingClientRect()), e.clientX);
      const br = v.bar.getBoundingClientRect();
      const x = i < tabs.length ? tabs[i].getBoundingClientRect().left : (tabs.at(-1)?.getBoundingClientRect().right ?? br.left);
      mark.style.left = `${x - br.left + v.bar.scrollLeft}px`;
      return i;
    };
    v.el.addEventListener('dragover', (e) => {
      const k = kind(e);
      if (!k) return;
      e.preventDefault();
      e.stopPropagation();                         // not the editor's file-drop handling
      e.dataTransfer.dropEffect = k === 'tab' ? 'move' : 'copy';
      if (v.bar.contains(e.target) || v.el.querySelector('.ed-tabbar') === e.target) {
        zoneEl.hidden = true;
        mark.hidden = false;
        barIndex(e);
        return;
      }
      mark.hidden = true;
      const surf = v.el.querySelector('.ed-surface').getBoundingClientRect();
      zoneEl.dataset.zone = dropZone(surf, e.clientX, e.clientY, this.tabs.split);
      zoneEl.style.setProperty('--bar', `${v.bar.hidden ? 0 : v.bar.offsetHeight}px`);
      zoneEl.hidden = false;
    });
    v.el.addEventListener('dragleave', (e) => { if (!e.relatedTarget || !v.el.contains(e.relatedTarget)) clear(); });
    v.el.addEventListener('drop', async (e) => {
      const k = kind(e);
      if (!k) return;
      e.preventDefault();
      e.stopPropagation();
      const onBar = !mark.hidden;
      const index = onBar ? barIndex(e) : -1;
      const zone = zoneEl.dataset.zone || 'center';
      clear();
      this._tabDrag = false;                       // the dragged tab's own dragend may never come: it is re-drawn
      this._groups.classList.remove('is-dnd');
      let path, from = -1;
      if (k === 'tab') {
        try { ({ path, gid: from } = JSON.parse(e.dataTransfer.getData(TAB_TYPE))); } catch { return; }
        from = this.tabs.groups.findIndex((g) => g.id === from);
        if (from < 0 || !this.tabs.groups[from].tabs.includes(path)) return;
      } else {
        path = e.dataTransfer.getData(ROW_TYPE);
        if (this.tree.find(path)?.dir) { this._say('Drop a file here, not a folder', 'warn'); return; }
        if (!(await this._load(path))) return;
      }
      const to = gi();
      if (to < 0) return;
      if (onBar) {
        if (from >= 0) this.tabs.moveTab(path, from, to, index);
        else this.tabs.insert(path, to, index);
      } else if (zone === 'center') {
        if (from >= 0) this.tabs.moveTab(path, from, to);
        else this.tabs.open(path, to);
      } else {
        this.tabs.splitTo(zone, path, from);
      }
      this._afterTabs();
    });
    // a tab starts a drag (any pointer: tabs have no long-press menu)
    v.bar.addEventListener('dragstart', (e) => {
      const t = e.target.closest?.('.ed-ftab');
      if (!t) return;
      e.dataTransfer.effectAllowed = 'move';
      e.dataTransfer.setData(TAB_TYPE, JSON.stringify({ path: t.dataset.path, gid: v.id }));
      t.classList.add('is-dragging');
      this._tabDrag = true;
      this._groups.classList.add('is-dnd');
    });
    // (dragend is sent to the dragged element; if a re-draw replaced it, to
    // nothing -- so the window's, as the backstop)
    if (!this._dragendHooked) {
      this._dragendHooked = true;
      window.addEventListener('dragend', () => {
        this._groups.classList.remove('is-dnd');
        if (this._tabDrag) { this._tabDrag = false; this._renderTitle(); }
      }, true);
    }
    v.bar.addEventListener('dragend', (e) => {
      e.target.closest?.('.ed-ftab')?.classList.remove('is-dragging');
      this._tabDrag = false;
      for (const o of this.views.values()) { o.el.querySelector('.ed-dropzone').hidden = true; o.el.querySelector('.ed-tabmark').hidden = true; }
      this._renderTitle();
    });
  }

  _focusGroup(id) {
    const gi = this.tabs.groups.findIndex((g) => g.id === id);
    if (gi < 0 || gi === this.tabs.focus) return;
    this.tabs.focus = gi;
    this._layout();
    this._updateFind();
    this._renderTitle();
    this._saveTabs();
  }

  // Put the half's showing tab into its text surface. force: the buffer was
  // (re)loaded from the device, so the view kept for it no longer applies.
  _show(gi, force = false) {
    const g = this.tabs.groups[gi];
    const v = g && this.views.get(g.id);
    if (!v) return;
    const p = this.tabs.activePath(gi);
    if (v.shown === p && !force) return;
    if (v.shown && v.shown !== p && g.tabs.includes(v.shown) && this.buffers.has(v.shown)) {
      v.saved.set(v.shown, v.core.saveView());
    }
    v.shown = p;
    // A fresh vim state, but the last search survives a change of file, as in vim.
    const { search, word, sdir, hl } = v.vst;
    v.vst = { ...initialState(), search, word, sdir, hl };
    v.painted = null;
    v.ins = null;
    const b = p && this.buffers.get(p);
    if (!b) {
      v.core.setValue('', 'text');
      v.core.readOnly = true;
      v.core.setFind(null);
      v.preview.hide();
      v.vcur = 0;
      return;
    }
    v.core.setValue(b.binary ? '' : b.text, b.lang);
    v.core.readOnly = b.binary;
    if (force) v.saved.delete(p);
    else v.core.restoreView(v.saved.get(p));
    v.vcur = clampNormal(v.core.value, v.core.ta.selectionStart);
    this._applyPreview(v);
  }

  _showAll(force = false) {
    this.tabs.groups.forEach((_, gi) => this._show(gi, force));
  }

  // The tab rows: the file name (with its folder when two share a name), a
  // dot when modified, and a close button that is always there -- a touch
  // screen has no hover to reveal it.
  _renderTabs() {
    this.tabs.groups.forEach((g) => {
      const v = this.views.get(g.id);
      if (!v) return;
      const dirty = g.tabs.map((p) => this._mark(p).dirty);
      const sig = JSON.stringify([g.tabs, g.active, dirty, this.tabs.split]);
      if (sig === v.sig || this._tabDrag) return;
      v.sig = sig;
      v.bar.hidden = !g.tabs.length;
      const base = (p) => p.split('/').pop();
      v.bar.innerHTML = g.tabs.map((p, i) => {
        const twin = g.tabs.some((o) => o !== p && base(o) === base(p));
        const dir = p.split('/').slice(0, -1).join('/');
        const on = i === g.active;
        return `<div class="ed-ftab${on ? ' is-active' : ''}${dirty[i] ? ' is-dirty' : ''}" role="tab" draggable="true" aria-selected="${on}" data-path="${escHtml(p)}" title="${escHtml(p)} — drag to move it, or onto the text's edge to split">`
          + `<span class="ed-ftab-dot" aria-hidden="true">●</span><span class="ed-ftab-name">${escHtml(base(p))}</span>`
          + (twin && dir ? `<span class="ed-ftab-dir">${escHtml(dir)}</span>` : '')
          + `<button type="button" class="ed-ftab-x" tabindex="-1" aria-label="Close ${escHtml(p)}" title="Close (Alt+Q)">×</button></div>`;
      }).join('');
      v.bar.querySelector('.is-active')?.scrollIntoView({ block: 'nearest', inline: 'nearest' });
    });
  }

  _saveTabs() {
    try { localStorage.setItem(TABS_KEY, JSON.stringify(this.tabs)); } catch { /* private mode: tabs just do not survive a reload */ }
  }

  // After a reload: the tabs and split as they were. Only the files showing
  // are read now; the others when their tab is picked.
  async _restoreTabs() {
    if (this._restored) return;
    this._restored = true;
    let saved = null;
    try { saved = JSON.parse(localStorage.getItem(TABS_KEY)); } catch { /* none */ }
    const t = TabState.from(saved);
    if (!t.openPaths().size || this.tabs.openPaths().size) return;
    for (let gi = t.groups.length - 1; gi >= 0; gi--) {
      for (let p = t.activePath(gi); p; p = t.activePath(gi)) {
        if (this.buffers.has(p) || await this._load(p)) break;
        t.close(p, gi);                               // gone from the device since
        if (gi >= t.groups.length) break;
      }
    }
    if (this.tabs.openPaths().size) return;           // something was opened meanwhile
    this.tabs = t;
    this._layout();
    this._showAll();
    this._renderTitle();
    if (this.path) this.tree.reveal(this.path);
  }

  // Close one tab; force: the caller has already asked about unsaved work.
  // A file still open in the other half keeps its buffer.
  closeTab(path, gi = this.tabs.focus, { force = false } = {}) {
    const g = this.tabs.groups[gi];
    if (!g || !g.tabs.includes(path)) return false;
    const b = this.buffers.get(path);
    const elsewhere = this.tabs.groups.some((o, i) => i !== gi && o.tabs.includes(path));
    if (!force && b && this._isDirty(b) && !elsewhere &&
        !confirm(`${path} has unsaved changes. Close it and lose them?`)) return false;
    this.views.get(g.id)?.saved.delete(path);
    this.tabs.close(path, gi);
    if (!this.tabs.isOpen(path)) this.buffers.delete(path);
    this._layout();
    this._showAll();
    this._say(`Closed ${path}`);
    this.tree.invalidate();
    this._renderTitle();
    this._saveTabs();
    if (this.path) { this.tree.reveal(this.path); this.core.focus(); } else this.focusFiles();
    return true;
  }

  _afterTabs() {
    this._layout();
    this._showAll();
    this._updateFind();
    this._renderTitle();
    this._saveTabs();
    if (this.path) { this.tree.reveal(this.path); this.core.focus(); } else this.focusFiles();
  }

  async stepTab(delta) { const p = this.tabs.step(delta); if (p) await this.open(p); }
  async gotoTab(n) { const p = this.tabs.goto(n); if (p) await this.open(p); }

  moveTab(delta) {
    if (!this.tabs.move(delta)) return;
    this._renderTitle();
    this._saveTabs();
  }

  toggleSplit() {
    if (!this.tabs.split && !this.path) { this._say('Open a file first: the split shows it twice.', 'warn'); return; }
    // Too narrow for two columns of code (a phone): one above the other.
    if (!this.tabs.split && this._groups.clientWidth && this._groups.clientWidth < 640) this.tabs.orient = 'v';
    this.tabs.toggleSplit();
    this._afterTabs();
    this._say(this.tabs.split ? 'Split: Alt+O moves between the halves, Alt+\\ unsplits' : 'One half again — its tabs joined these');
  }

  flipSplit() {
    this.tabs.flip();
    this._layout();
    this._saveTabs();
    this.core.focus();
  }

  focusOther() {
    if (!this.tabs.split) { this._say('Not split — Alt+\\ splits the editor', 'warn'); return; }
    this.tabs.focusOther();
    this._layout();
    this._updateFind();
    this._renderTitle();
    this._saveTabs();
    if (this.path) this.core.focus();
  }

  moveToOther() {
    if (!this.tabs.moveToOther()) { this._say('Only one file is open: split instead (Alt+\\)', 'warn'); return; }
    this._afterTabs();
  }

  // :sp / :vs [path]: a file (this one by default) in the other half.
  async splitOpen(path, orient) {
    if (!path) {
      if (!this.path) return;
      this.tabs.splitOpen(this.path, orient);
      this._afterTabs();
      return;
    }
    const before = this.tabs.split;
    this.tabs.splitOpen(null, orient);
    this._layout();
    if (!(await this.open(path, { create: true })) && !before) {
      this.tabs.unsplit();
      this._afterTabs();
    }
  }

  only() {
    if (!this.tabs.split) return;
    this.tabs.unsplit();
    this._afterTabs();
  }

  // A file renamed (or stored compressed / uncompressed): its buffer, tabs
  // and kept views follow it.
  _renamed(from, to) {
    const b = this.buffers.get(from);
    this.buffers.delete(from);
    if (b) { b.lang = languageOf(to); this.buffers.set(to, b); }
    this.tabs.rename(from, to);
    for (const v of this.views.values()) {
      if (v.saved.has(from)) { v.saved.set(to, v.saved.get(from)); v.saved.delete(from); }
      if (v.shown === from) { v.shown = to; v.core.lang = languageOf(to); v.core._dirty = true; }
    }
    this._layout();
    this._showAll();
    this._saveTabs();
  }

  // --- vim ---------------------------------------------------------------------

  onPrefs(prefs) {
    if (!!prefs.vim === this.vimOn) return;
    this.vimOn = !!prefs.vim;
    for (const v of this.views.values()) {
      v.vst = initialState();
      const p = v.core.ta.selectionStart;
      v.core.ta.setSelectionRange(p, p);
      v.painted = null;
    }
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
      case 'tabnext': return /^\d+$/.test(arg) ? this.gotoTab(Number(arg)) : this.stepTab(1);
      case 'tabprev': return this.stepTab(-(Number(arg) || 1));
      case 'tabnew':
        if (arg) return this.open(arg, { create: true });
        return this.newFile();
      case 'tabclose':
        if (!this.path) return undefined;
        if (dirty && !force && !this.tabs.groups.some((g, i) => i !== this.tabs.focus && g.tabs.includes(this.path))) {
          this._say('E37: No write since last change (add ! to override)', 'warn');
          return undefined;
        }
        return this.closeTab(this.path, this.tabs.focus, { force: true });
      case 'split': return this.splitOpen(arg, 'v');
      case 'vsplit': return this.splitOpen(arg, 'h');
      case 'only': return this.only();
      case 'resize': return this.resizeEx(arg, 'v');
      case 'vertical': {
        const m = /^res(?:ize)?\b\s*(.*)$/.exec(arg);
        if (!m) { this._say(`Not an editor command: vertical ${arg}`, 'warn'); return undefined; }
        return this.resizeEx(m[1], 'h');
      }
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

  // Paint vim's cursor / selection, then bring the cursor into view: a
  // cursor set by code is not scrolled to as a typed one is, so without this
  // G, 200j, n or :120 moved the cursor off-screen and the view stayed put.
  _vimPaint() {
    this._vimPaintSel();
    this.core.revealCaret(Math.max(0, Math.min(this.vcur, this.core.ta.value.length)));
  }

  _vimPaintSel() {
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
  // In the file list, its own keys (the hint bar shows them); everything the
  // editor binds still applies there (keys.js: 'editor-files' inherits 'editor').
  context() {
    return this._files.hasAttribute('data-nav-active') && this._files.contains(document.activeElement)
      ? 'editor-files' : 'editor';
  }

  action(id) {
    if (id === 'editor-save') return this.save();
    if (id === 'editor-run') return this.run();
    if (id === 'editor-stop') return this.stop();
    if (id === 'editor-sidebar') return this.toggleSidebar();
    if (id === 'editor-to-files') return this.focusFiles();
    if (id === 'editor-to-text') return this.focusText();
    if (id === 'editor-help') return this.help.toggle();
    if (id === 'editor-split-grow') return this.resizeSplit(0.05);
    if (id === 'editor-split-shrink') return this.resizeSplit(-0.05);
    if (id === 'editor-split-even') return this.resizeSplit(null);
    if (id === 'editor-tab-next') return this.stepTab(1);
    if (id === 'editor-tab-prev') return this.stepTab(-1);
    if (id === 'editor-tab-right') return this.moveTab(1);
    if (id === 'editor-tab-left') return this.moveTab(-1);
    if (id === 'editor-tab-close') return this.path ? this.closeTab(this.path) : undefined;
    if (id === 'editor-split') return this.toggleSplit();
    if (id === 'editor-split-flip') return this.flipSplit();
    if (id === 'editor-other') return this.focusOther();
    if (id === 'editor-to-other') return this.moveToOther();
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
    this._restoreTabs();
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

  // A file into a buffer (once; every tab and half shares it). Returns the
  // buffer, or null after saying why not. { create: true } (from :e, :tabnew,
  // :sp) makes a missing file a new, unsaved buffer -- as vim does -- rather
  // than reporting it missing.
  async _load(path, { create = false } = {}) {
    let b = this.buffers.get(path);
    if (b) return b;
    const img = imageKind(path);
    if (img) {
      // a picture: shown, not edited -- except an SVG, which is text too
      const r = await fs.readBlob(path);
      if (!r.ok) { this._say(`Could not open ${path}: ${r.error}`, 'err'); return null; }
      const blob = new Blob([r.blob], { type: imageType(path) });
      const text = img === 'svg' ? await blob.text() : '';
      b = { text, base: fnv1a(text), lang: 'text', binary: img !== 'svg', image: img, blob, showText: false };
    } else if (/\.(bin|luac|bak)$/i.test(path)) {
      b = { text: '', base: fnv1a(''), lang: 'text', binary: true };
    } else {
      // a .gz arrives as its text: the device sends it Content-Encoding: gzip
      const r = await fs.readText(path);
      if (!r.ok && r.offline && isGz(path)) {
        this._say(`Could not open ${path}: the browser could not unpack it — is it really gzip?`, 'err');
        return null;
      }
      if (!r.ok && create && r.status === 404) {
        b = { text: '', base: '', lang: languageOf(path), binary: false, isNew: true };
      } else if (!r.ok) {
        this._say(`Could not open ${path}: ${r.error}`, 'err');
        return null;
      } else {
        b = { text: r.text, base: fnv1a(r.text), lang: languageOf(path), binary: r.text.includes('\0') };
      }
    }
    this.buffers.set(path, b);
    b.fresh = true;
    return b;
  }

  // Show a file in the focused half: its tab if it has one, else a new tab.
  async open(path, { create = false } = {}) {
    const b = await this._load(path, { create });
    if (!b) return false;
    const fresh = b.fresh;
    b.fresh = false;
    this._openBuffer(path, { force: fresh });
    if (b.isNew) {
      delete b.isNew;
      this._say(`"${path}" [New] — :w writes it to the device`);
      return true;
    }
    this._say(b.image === 'bitmap' ? '' : b.binary ? 'Binary file — download it to inspect it.' : '');
    // plain wins: x.lua runs, not x.lua.gz, when both are there
    const sh = shadowInfo(path, this._siblings(path));
    if (sh?.shadowed) this._say(`${sh.by} is in this folder too, and wins: it is what the device uses, not this file.`, 'warn');
    return true;
  }

  // The names in a file's folder, as the tree last listed them.
  _siblings(path) {
    const dir = path.split('/').slice(0, -1).join('/');
    const parent = dir ? this.tree.find(dir) : this.tree.root;
    return (parent?.children || []).map((c) => c.name);
  }

  // An SVG: picture or text (for every half showing it).
  _toggleSvg() {
    const b = this.path && this.buffers.get(this.path);
    if (!b || b.image !== 'svg') return;
    b.text = this.core.value;
    b.showText = !b.showText;
    for (const v of this.views.values()) if (v.shown === this.path) this._applyPreview(v);
    this._renderTitle();
  }

  _applyPreview(v) {
    const b = v.shown && this.buffers.get(v.shown);
    const picture = !!b && (b.image === 'bitmap' || (b.image === 'svg' && !b.showText));
    if (!picture) { v.preview.hide(); return; }
    const n = this.tree.find(v.shown);
    const blob = b.image === 'svg' ? new Blob([b.text], { type: 'image/svg+xml' }) : b.blob;
    v.preview.show(blob, { note: n && !n.dir ? fileInfo(n) : '' });
  }

  _openBuffer(path, { force = false } = {}) {
    this.tabs.open(path);
    this._layout();
    this._show(this.tabs.focus, force);
    // reloaded from the device: the other half showing it gets the new text too
    if (force) this.tabs.groups.forEach((_, gi) => { if (gi !== this.tabs.focus && this.tabs.activePath(gi) === path) this._show(gi, true); });
    this.tree.reveal(path);
    this._updateFind();
    this._renderTitle();
    this._saveTabs();
    this.core.focus();
  }

  // :q -- close the file's tab (unsaved changes too: the caller has already
  // asked, or was told to with !). With no tab left, focus moves to the file
  // list, where the next file is picked.
  close() {
    if (!this.path) return;
    this.closeTab(this.path, this.tabs.focus, { force: true });
  }

  // An edit in one half; the other half, if it shows the same file, follows.
  _edited(text, v = this.v) {
    const b = v.shown && this.buffers.get(v.shown);
    if (!b) return;
    b.text = text;
    for (const o of this.views.values()) if (o !== v && o.shown === v.shown) o.core.adopt(text);
    this.tree.invalidate();
    this._updateFind();
    this._renderTitle();
  }

  // --- the verbs ----------------------------------------------------------------

  // True when the file was written.
  async save() {
    const b = this.buffers.get(this.path);
    if (!b || b.binary) return false;
    const path = this.path;
    const text = this.core.value;
    // A .gz stays compressed: the browser packs it, the board cannot.
    const body = isGz(path) ? await gzipText(text) : text;
    // A file of the web UI keeps the version it replaces as <file>.bak --
    // renamed, so the .bak is the stored bytes exactly.
    let bak = null;
    if (isWebAsset(path) && b.base !== '') {
      bak = `${path}.bak`;
      await fs.remove(bak);
      const mv = await fs.rename(path, bak);
      if (!mv.ok && mv.status !== 404) { this._say(`Save stopped: could not keep ${bak} (${mv.error})`, 'err'); return false; }
      if (!mv.ok) bak = null;                      // nothing there to keep
    }
    const r = await fs.write(path, body);
    if (!r.ok) {
      if (bak) await fs.rename(bak, path);         // put the old one back
      this._say(`Save failed: ${r.error}`, 'err');
      return false;
    }
    b.text = text;
    b.base = fnv1a(text);
    const packed = typeof body === 'string' ? '' : ` (stored compressed: ${bytes(body.size)} for ${bytes(new TextEncoder().encode(text).length)} of text)`;
    this._say(`Saved ${path}${packed}${bak ? ` — the previous version is ${bak.split('/').pop()}` : ''}`);
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
    this.open(this.path);            // a fresh read: both halves showing it take the new text
  }

  _act_rename() { return this.renameFile(this.path); }
  _act_delete() { return this.deleteFile(this.path); }

  // Any file, open or not (the file menu); the open one by default.
  async renameFile(path) {
    if (!path) return;
    const to = prompt('Rename or move to:', path)?.trim();
    if (!to || to === path) return;
    const r = await fs.rename(path, to);
    if (!r.ok) { this._say(`Rename failed: ${r.error}`, 'err'); return; }
    this._renamed(path, to);
    this._say(`Renamed ${path} to ${to}`);
    await this.tree.refresh();
    this.tree.reveal(to);
    this._renderTitle();
  }

  async deleteFile(path) {
    if (!path || !confirm(`Delete ${path} from the device? This cannot be undone.`)) return;
    const r = await fs.remove(path);
    if (!r.ok) { this._say(`Delete failed: ${r.error}`, 'err'); return; }
    this.buffers.delete(path);
    this._say(`Deleted ${path}`);
    for (const v of this.views.values()) v.saved.delete(path);
    this.tabs.closeEverywhere(path);
    this._layout();
    this._showAll();
    this._saveTabs();
    this.tree.refresh();
    this._renderTitle();
  }

  // A folder: an empty one in one step; any other with everything in it,
  // after saying how much that is. The gateway's own folders (cfg/, lua/,
  // js/ ...) ask for their name to be typed rather than a click.
  async deleteFolder(path) {
    if (!path) return false;
    let t;
    try { t = await treeOf(fs.list, path); } catch (e) { this._say(`Could not list ${path}/: ${e.message}`, 'err'); return false; }
    const empty = !t.files.length && t.dirs.length === 1;
    // Always asked, in the app's own dialog: what goes, file by file, and
    // Cancel the default (editor/confirmdelete.js).
    if (!(await this.confirmDelete.ask({ path, files: t.files, dirs: t.dirs, vital: !empty && isVital(path) }))) {
      this._say(`Not deleted: ${path}/`);
      return false;
    }
    let failed = null;
    const gone = new Set();
    try {
      await deleteTree(fs.list, fs.remove, path, (d, n, p) => { gone.add(p); this._say(`Deleting ${path}/ … ${d} of ${n}`); });
    } catch (e) { failed = e.message; }
    // the tabs of what went (all of it, or as far as it got)
    for (const p of this.tabs.openPaths()) {
      if (!gone.has(p)) continue;
      this.tabs.closeEverywhere(p);
      this.buffers.delete(p);
      for (const v of this.views.values()) v.saved.delete(p);
    }
    this._layout();
    this._showAll();
    this._saveTabs();
    if (failed) this._say(`Delete stopped at ${failed}`, 'err');
    else this._say(empty ? `Deleted ${path}/` : `Deleted ${path}/ and everything in it`);
    await this.tree.refresh();
    this._renderTitle();
    return !failed;
  }

  // --- drag and drop ------------------------------------------------------------
  //
  // In: files and folders from the computer, dropped on a folder row (into
  // it), a file row (beside it), the list's empty space (the root) or
  // anywhere else in the editor (the folder the list has selected). A .tar
  // dropped by itself is extracted there, through Restore's preview.
  // Out: a row dragged out of the browser saves the file -- a folder as a
  // .tar (Chrome and Edge; elsewhere use the menu's Download).
  // Within: a row dropped on a folder row moves it there.

  _setupDnd(el) {
    const ROW = ROW_TYPE;
    this.tree.canDrag = matchMedia('(pointer: fine)').matches;
    let over = null;                                  // the highlighted element
    const mark = (target) => {
      if (over === target) return;
      over?.classList.remove('is-drop');
      over = target;
      over?.classList.add('is-drop');
    };
    const end = () => { mark(null); this.tree.holdRender = false; this.tree.invalidate(); };
    // Where a drop at this event lands, and the element that shows it.
    const where = (e) => {
      const row = this._files.contains(e.target) && e.target.closest('.ft-row');
      if (row) {
        const node = this.tree.find(row.dataset.path);
        const dir = dropDir(node);
        const dirRow = dir ? this._files.querySelector(`.ft-row[data-path="${CSS.escape(dir)}"]`) : null;
        return { dir, show: dirRow || this._files };
      }
      if (this._files.contains(e.target)) return { dir: '', show: this._files };
      return { dir: this.tree.currentDir, show: this._groups };
    };
    this._files.addEventListener('dragstart', (e) => {
      const row = e.target.closest?.('.ft-row');
      if (!row) return;
      const path = row.dataset.path;
      const node = this.tree.find(path);
      const dt = e.dataTransfer;
      dt.effectAllowed = 'copyMove';
      dt.setData(ROW, path);
      dt.setData('text/plain', path);
      // out of the browser: the file (a .gz arrives unpacked, so it is named
      // for what it holds), a folder as the device's .tar of it
      const url = new URL(node?.dir ? fs.tarUrl(path) : fs.fileUrl(path), location.href).href;
      const name = node?.dir ? `${node.name}.tar` : plainName(node?.name || path.split('/').pop());
      dt.setData('DownloadURL', `${node?.dir ? 'application/x-tar' : 'application/octet-stream'}:${name}:${url}`);
      dt.setData('text/uri-list', url);
      this.tree.holdRender = true;
      this._groups.classList.add('is-dnd');
    });
    this._files.addEventListener('dragend', end);
    el.addEventListener('dragover', (e) => {
      const types = [...(e.dataTransfer?.types || [])];
      const internal = types.includes(ROW);
      if (!internal && !types.includes('Files')) return;
      if (internal && !this._files.contains(e.target)) { mark(null); return; }
      e.preventDefault();
      e.dataTransfer.dropEffect = internal ? 'move' : 'copy';
      this.tree.holdRender = true;
      mark(where(e).show);
    });
    el.addEventListener('dragleave', (e) => {
      if (!e.relatedTarget || !el.contains(e.relatedTarget)) end();
    });
    el.addEventListener('drop', (e) => {
      const types = [...(e.dataTransfer?.types || [])];
      if (!types.includes(ROW) && !types.includes('Files')) return;
      e.preventDefault();
      const { dir } = where(e);
      const src = e.dataTransfer.getData(ROW);
      const entries = src ? null : dropEntries(e.dataTransfer);   // now, while the event lasts
      end();
      if (src) this.moveTo(src, dir);
      else this.dropFiles(entries, dir);
    });
  }

  // Files and folders from the computer into `dir`.
  async dropFiles(entries, dir) {
    let items;
    try { items = await readDrop(entries); } catch (e) { this._say(`Could not read the drop: ${e.message}`, 'err'); return; }
    if (!items.length) return;
    // what is there already, for the "replace?" question
    const join = (p) => (dir ? `${dir}/${p}` : p);
    const parents = new Set(items.filter((i) => !i.dir).map((i) => join(i.path).split('/').slice(0, -1).join('/')));
    const existing = new Set();
    for (const d of parents) {
      const r = await fs.list(d);
      if (r.offline) { this._say('The device is unreachable.', 'err'); return; }
      for (const f of r.ok ? r.files : []) if (!f.dir) existing.add(d ? `${d}/${f.name}` : f.name);
    }
    const plan = planDrop(items, dir, existing);
    let uploads = plan.uploads;
    if (plan.overwrites.length) {
      const list = plan.overwrites.slice(0, 12).join('\n') + (plan.overwrites.length > 12 ? `\n… and ${plan.overwrites.length - 12} more` : '');
      if (!confirm(`${plan.overwrites.length} of these are on the device already:\n\n${list}\n\nOK replaces them; Cancel keeps them (the rest are still uploaded).`)) {
        uploads = uploads.filter((u) => !existing.has(u.dest));
      }
    }
    let done = 0, failed = null;
    for (const d of plan.dirs) {
      const r = await fs.mkdir(d);
      if (!r.ok) { failed = `${d}/: ${r.error}`; break; }
    }
    for (const u of failed ? [] : uploads) {
      this._say(`Uploading ${done + 1} of ${uploads.length}: ${u.dest}`);
      const r = await fs.write(u.dest, u.file);
      if (!r.ok) { failed = `${u.dest}: ${r.error}`; break; }
      done++;
    }
    await this.tree.refresh();
    if (dir) this.tree.reveal(dir);
    // a file open in a tab, just replaced: show what is on the device now
    for (const u of uploads.slice(0, done)) {
      const b = this.buffers.get(u.dest);
      if (b && !this._isDirty(b)) { this.buffers.delete(u.dest); if (this.tabs.showing(u.dest)) await this._reloadShown(u.dest); }
    }
    const where = dir ? `${dir}/` : 'the root';
    const parts = [];
    if (uploads.length || plan.dirs.length) parts.push(`Uploaded ${done} of ${uploads.length} file${uploads.length === 1 ? '' : 's'} to ${where}`);
    if (plan.skipped.length) parts.push(`skipped ${plan.skipped.length} (${plan.skipped.slice(0, 3).map((x) => `${x.path}: ${x.why}`).join('; ')})`);
    if (failed) this._say(`${parts.join(', ')} — stopped at ${failed}`, 'err');
    else if (parts.length) this._say(parts.join(', '), plan.skipped.length ? 'warn' : '');
    // a .tar dropped by itself: extracted here, after the preview
    if (plan.tars.length) {
      if (plan.tars.length > 1) this._say(`Extracting ${plan.tars[0].name} — drop the other archives one at a time`, 'warn');
      this.restorer.open(plan.tars[0], dir);
    }
  }

  // The tab showing `path` takes the device's copy again (after an upload
  // replaced it).
  async _reloadShown(path) {
    const b = await this._load(path);
    if (!b) return;
    b.fresh = false;
    this.tabs.groups.forEach((_, gi) => { if (this.tabs.activePath(gi) === path) this._show(gi, true); });
    this._renderTitle();
  }

  // A row dropped on a folder: moved there (a folder with everything in it).
  async moveTo(src, folder) {
    const dest = moveDest(src, folder);
    if (!dest) return;
    const r = await fs.rename(src, dest);
    if (!r.ok) {
      this._say(r.status === 409 || /exist/i.test(r.error || '') ? `Not moved: ${dest} already exists` : `Move failed: ${r.error}`, 'err');
      return;
    }
    // open tabs follow, the folder's contents too
    for (const p of [...this.tabs.openPaths()]) {
      if (p === src) this._renamed(p, dest);
      else if (p.startsWith(src + '/')) this._renamed(p, dest + p.slice(src.length));
    }
    this._say(`Moved ${src} to ${dest}`);
    await this.tree.refresh();
    this.tree.reveal(dest);
    this._renderTitle();
    this._poll();
  }

  // A script's tier operations by name, for a file that need not be open.
  async scriptOp(op, path, done) {
    const name = scriptNameOf(path);
    if (!name) return;
    try {
      await this.ws.call(op, { script: name });
      this._say(done(name));
    } catch (e) {
      this._say(`${op}: ${e.message}`, 'err');
    }
    await this._poll();
    this.tree.refresh();
  }

  // A pointer asked for a row's menu: select the row too, since the
  // keyboard's menu is always the selected row's.
  _rowMenu(path) {
    this.tree.sel = path;
    this.tree.invalidate();
    this.tree.render();
    this.openFileMenu(path);
  }

  // The menu for one file or folder: right-click, a long press or the ⋯
  // button on a touch screen, or m / Shift+F10 / the Menu key in the list.
  // Items come from editor/filemenu.js (tested); the path itself is the last
  // item and copies.
  async openFileMenu(path) {
    const node = this.tree.find(path);
    if (!node) return false;
    // A folder's menu says what is in it, and offers Delete only when empty:
    // list it first if it has never been opened (one request).
    if (node.dir && !node.children) await this.tree.load(node);
    const row = this._files.querySelector(`.ft-row[data-path="${CSS.escape(path)}"]`);
    if (!row) return false;
    const siblings = node.dir ? [] : this._siblings(path);
    const items = fileMenuItems(node, {
      isOpen: this.path === path,
      script: scriptNameOf(path),
      empty: node.dir && node.children?.length === 0,
      hasBak: siblings.includes(`${node.name}.bak`),
    }).map((it) => ({ label: it.label, keep: it.keep, cls: it.danger ? 'sm-danger' : '', fn: () => this._fileAct(it.id, node) }));
    this.tree.menuPath = path;
    this.tree.invalidate();
    this.tree.render();
    const anchor = this._files.querySelector(`.ft-row[data-path="${CSS.escape(path)}"]`) || row;
    this._menu = openCellMenu(this.app, {
      anchor, menuEl: this._fileMenu, title: node.dir ? `${path}/` : path, items, value: path,
      info: node.dir ? dirInfo(node.children) : this._fileLine(node, siblings),
      wrap: this._files, paneId: 'editor-file-menu',
      onClose: () => { this.tree.menuPath = null; this.tree.invalidate(); this._menu = null; },
    });
    return this._menu;
  }

  _fileLine(node, siblings) {
    const sh = shadowInfo(node.path, siblings);
    const note = !sh ? '' : sh.shadowed ? ` · not used: ${sh.by} wins` : ` · wins over ${sh.by}`;
    return fileInfo(node) + note;
  }

  // Calculate size: every file under the folder, listed one folder at a time,
  // only when asked. Answers on the menu's info line.
  async sizeFolder(node) {
    const menu = this._menu;
    menu?.setInfo('counting…');
    try {
      const t = await folderSize(fs.list, node.path, (p) => menu?.setInfo(`counting… ${p.files} files so far`));
      menu?.setInfo(sizeInfo(t));
    } catch (e) {
      menu?.setInfo(`Could not count: ${e.message}`);
    }
  }

  _fileAct(id, node) {
    const p = node.path;
    switch (id) {
      case 'open': return this.open(p);
      case 'start': return this.scriptOp('script.start', p, (n) => `Started ${n}`);
      case 'stop': return this.scriptOp('script.stop', p, (n) => `Stopped ${n}`);
      case 'commit': return this.scriptOp('script.commit', p, (n) => `Committed ${n} to lua/${n}.lua`);
      case 'revert': return this.scriptOp('script.revert', p, (n) => `Reverted ${n}: the next tier down runs now`);
      case 'download': return this.download(p);
      case 'rename': return this.renameFile(p);
      case 'delete': return node.dir ? this.deleteFolder(p) : this.deleteFile(p);
      case 'size': return this.sizeFolder(node);
      case 'new': return this.newFile(p);
      case 'upload': this._uploadDir = p; return this._upload.click();
      case 'refresh': return this.tree.refresh(node).then(() => this.tree.invalidate());
      case 'tar': return this.downloadTar(p);
      case 'untar': this._tarTarget = p; return this._tarInput.click();
      case 'compress': return this.convert(p, true);
      case 'decompress': return this.convert(p, false);
      case 'restore-bak': return this.restoreBak(p);
      default: return undefined;
    }
  }

  // A folder ('' = everything) as a .tar, streamed by the device straight
  // into the browser's download.
  downloadTar(folder = '') {
    const a = document.createElement('a');
    a.href = fs.tarUrl(folder);
    a.download = `${folder ? folder.split('/').pop() : 'btgw'}.tar`;
    a.click();
    this._say(`Downloading ${folder ? `${folder}/` : 'the whole filesystem'} as a .tar`);
  }

  _act_split() { return this.toggleSplit(); }
  _act_flip() { return this.flipSplit(); }
  _act_other() { return this.moveToOther(); }

  _act_tarall() { return this.downloadTar(''); }
  _act_restore() { this._tarTarget = ''; this._tarInput.click(); }

  // Store compressed (x -> x.gz) or uncompressed (x.gz -> x). The browser
  // does the packing; the old name goes once the new one is written.
  async convert(path, compress) {
    const to = compress ? `${path}.gz` : plainName(path);
    const b = this.buffers.get(path);
    if (b && this._isDirty(b)) { this._say(`Save ${path} first.`, 'warn'); return false; }
    if (this._siblings(path).includes(to.split('/').pop()) && !confirm(`${to} exists. Replace it?`)) return false;
    const r = await fs.readBlob(path);            // a .gz arrives unpacked
    if (!r.ok) { this._say(`Could not read ${path}: ${r.error}`, 'err'); return false; }
    const body = compress ? await gzipText(r.blob) : r.blob;
    const w = await fs.write(to, body);
    if (!w.ok) { this._say(`Could not write ${to}: ${w.error}`, 'err'); return false; }
    const d = await fs.remove(path);
    if (!d.ok) this._say(`Wrote ${to}, but could not remove ${path}: ${d.error}`, 'warn');
    else if (compress) this._say(`Stored compressed: ${to}, ${bytes(body.size)} (was ${bytes(r.blob.size)})`);
    else this._say(`Stored uncompressed: ${to}, ${bytes(body.size)}`);
    this._renamed(path, to);
    await this.tree.refresh();
    this.tree.reveal(to);
    this._renderTitle();
    this._poll();
    return true;
  }

  // Swap a file with its .bak: the version before the last save comes back,
  // and the one it replaces becomes the .bak -- so doing it twice undoes it.
  async restoreBak(path) {
    const bak = `${path}.bak`, tmp = `${path}.swap`;
    if (!confirm(`Bring back ${bak.split('/').pop()}? The current ${path.split('/').pop()} becomes the .bak.`)) return false;
    const steps = [[path, tmp], [bak, path], [tmp, bak]];
    for (const [from, to] of steps) {
      const r = await fs.rename(from, to);
      if (!r.ok) { this._say(`Restore stopped at ${from} → ${to}: ${r.error}`, 'err'); await this.tree.refresh(); return false; }
    }
    this.buffers.delete(path);
    await this.tree.refresh();
    if (this.path === path) await this.open(path);
    this._say(`Restored ${path} from its .bak (reload the page to use a restored web file)`);
    return true;
  }

  async newFile(dir = this.tree.currentDir) {
    const name = prompt('New file (path on the device):', `${dir}/`)?.trim();
    if (!name || name.endsWith('/')) return;
    // Not written until saved -- a new file is an unsaved buffer, like any
    // editor's. Its base is a hash no text has, so it shows as modified.
    if (!this.buffers.has(name)) this.buffers.set(name, { text: '', base: '', lang: languageOf(name), binary: false });
    this._openBuffer(name, { force: true });
    // A new file starts in insert mode: there is nothing to move over yet.
    this.vst = { ...this.vst, mode: 'insert' };
    this.vcur = 0;
    this._say(`New file — Ctrl+S writes ${name} to the device`);
    this.tree.invalidate();
    this._renderTitle();
    this.core.focus();
  }

  async upload(files, dir = this.tree.currentDir) {
    for (const f of files) {
      const dest = dir ? `${dir}/${f.name}` : f.name;
      const r = await fs.write(dest, f);
      this._say(r.ok ? `Uploaded ${dest}` : `Upload of ${f.name} failed: ${r.error}`, r.ok ? '' : 'err');
    }
    this._upload.value = '';
    this.tree.refresh();
  }

  async download(path = this.path) {
    if (!path) return;
    const b = this.buffers.get(path);
    // The buffer if it is text (what you see is what you get, saved or not);
    // the device's bytes if it is binary.
    let blob;
    if (b && !b.binary) {
      // the open file's buffer is the text box; another's is its kept text
      blob = new Blob([path === this.path ? this.core.value : b.text], { type: 'text/plain' });
    } else {
      const r = await fs.readBlob(path);
      if (!r.ok) { this._say(`Download failed: ${r.error}`, 'err'); return; }
      blob = r.blob;
    }
    const a = document.createElement('a');
    a.href = URL.createObjectURL(blob);
    // a .gz arrives unpacked (Content-Encoding), so it is saved as what it holds
    a.download = (isGz(path) ? plainName(path) : path).split('/').pop();
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
    // the open file's size and what it takes on flash, as listed (saved state)
    const n = this.path && this.tree.find(this.path);
    this._sSize.textContent = n && !n.dir ? fileInfo(n) : '';
    this._sLang.textContent = b ? LANG_NAMES[b.lang] || '' : '';
    this._sArena.textContent = this.runtime
      ? `arena ${Math.round(this.runtime.used / 1024)}/${Math.round(this.runtime.arena_bytes / 1024)} KB` : '';
    this._sMode.textContent = this.vimOn ? modeLabel(this.vst) : '';
    this._svgBtn.hidden = b?.image !== 'svg';
    this._svgBtn.textContent = b?.showText ? 'Preview' : 'Text';
    this._renderTabs();
    this._keysBtn.setAttribute('aria-pressed', String(this.vimOn));
    this._keysBtn.classList.toggle('on', this.vimOn);
  }

  render() {
    for (const v of this.views.values()) v.core.render();
    this.tree.render();
  }
}
