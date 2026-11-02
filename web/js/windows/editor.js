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
import { freshnessAction, changeAction, FRESH_MS, nextFreshDelay } from '../editor/freshness.js';
import { ScriptIO } from '../scriptio.js';
import { EditorCore } from '../editor/core.js';
import { FileTree } from '../editor/filetree.js';
import { languageOf } from '../editor/highlight.js';
import { arenaMarks, shownArena, scriptNameOf, shadowedBy } from '../editor/status.js';
import { initialState, handle as vimHandle, modeLabel, clampNormal, searchMatches } from '../editor/vim.js';
import { copyText } from '../clipboard.js';
import { openCellMenu } from '../controls/cellmenu.js';
import { fileMenuItems, fileInfo, dirInfo, folderSize, sizeInfo } from '../editor/filemenu.js';
import { pexNameOf, pexFileName, pexRunningPaths, isPexFile, startPex, stopPex } from '../editor/pexrun.js';
import { buildIdOf } from '../editor/pexmap.js';
import { PexPartition, PEX_PART } from '../editor/pexpart.js';
import { buildButtonState, projectBuildItems } from '../editor/pexfile.js';
import { LongPress } from '../editor/longpress.js';
import { RestoreDialog } from '../editor/restore.js';
import { ImagePreview } from '../editor/preview.js';
import { isGz, plainName, imageKind, imageType, isWebAsset, shadowInfo, gzipText } from '../gz.js';
import { bytes } from '../editor/filemenu.js';
import { TabState, dropZone, tabIndexAt } from '../editor/tabs.js';
import { ConfirmDelete } from '../editor/confirmdelete.js';
import { EditorHelp } from '../editor/help.js';
import { SearchPanel } from '../editor/searchpanel.js';
import { planDrop, dropDir, moveDest, treeOf, deleteTree, isVital, dropEntries, readDrop } from '../editor/dnd.js';
import { planTransfer, measure, runTransfer, parseLfsFree, sdFreeBytes, lfsFreeChunk } from '../editor/transfer.js';
import { badPath, encodeTar, appendTar } from '../tar.js';
import { gestureOf } from '../editor/select.js';
import { createJobs, byteLine, byteFrac, fileLine, jobError } from '../editor/jobs.js';
import { readChunks, writeChunks } from '../xfer/wsfile.js';
import { JobList } from '../editor/joblist.js';
import { archivePlan, pruneCovered } from '../editor/archive.js';

const ARENA_POLL_MS = 3000;
const nextFrame = () => new Promise((resolve) => requestAnimationFrame(resolve));

const NO_SD = 'The compiler lives on the SD card and this firmware has no SD.';
const LANG_NAMES = { lua: 'Lua', json: 'JSON', text: 'Plain text' };

// A peer editor is this same page in a frame. Same-origin storage would
// otherwise share one tab list between the local board and every other one.
const _peerTabs = new URLSearchParams(location.search).get('peer');
const TABS_KEY = _peerTabs ? `btgw.editor.tabs:${_peerTabs}` : 'btgw.editor.tabs';
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
    this.pexStates = {};            // pex/<name>.<arch>.pex -> pex.list state
    this.pexInstalled = new Set();  // names pex.installed reports
    this.pexPart = null;            // last pex.installed reply, for the partition tab
    this.pexArch = 'm33';
    this._pexFocus = null;          // module the partition tab describes when the tree has not selected one
    this._partSig = '';
    this.runtime = null;            // script.list's runtime block
    this.notice = '';
    this._pollTimer = 0;
    this._freshTimer = 0;
    this._freshFlight = null;
    this._freshIx = 0;
    this._freshWait = 0;
    this._freshNext = 0;
    this._freshSkip = '';
    this._freshAsking = false;
    this._eventsLive = false;
    this.ws.on('fs.changed', (data) => { this._onFsChanged(data); });
    this.jobs = createJobs();
    this.jobList = new JobList(this.app, this.jobs);
    this._boardSeq = 1;
    this.ws.on('fs.job', (data) => this.jobs.notify(data));
    this._proj = null;
    this._projGen = 0;
    this._buildPin = new Map();
    this._building = false;
    this.pexPool = null;
    this.pexPolicy = '';

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
        <button type="button" class="tool-btn ed-b-build" title="This file is not in a project" aria-label="Build" disabled><span class="ed-build-label">Build</span></button>
        <button type="button" class="tool-btn ed-b-brun" title="Build and run" aria-label="Build and run" disabled>and run</button>
        <button type="button" class="tool-btn ed-b-new" title="New file" aria-label="New file">${icon('new')}</button>
        <button type="button" class="tool-btn ed-b-upload" title="Upload files from this computer" aria-label="Upload">${icon('upload')}</button>
        <button type="button" class="tool-btn ed-b-download" title="Download this file" aria-label="Download">${icon('download')}</button>
        <button type="button" class="tool-btn ed-b-svg" title="Show the SVG as a picture or as text" hidden>Text</button>
        <button type="button" class="tool-btn ed-b-split" title="Split the editor: the file in a second half, side by side (Alt+\\) — again to unsplit" aria-label="Split editor" aria-pressed="false">${icon('split')}</button>
        <button type="button" class="tool-btn ed-b-help" title="The editor's keys (Alt+/)" aria-label="Editor keys">?</button>
        <button type="button" class="tool-btn ed-b-pex" title="PEX partition" aria-label="PEX partition">PEX</button>
        <button type="button" class="tool-btn ed-b-more" title="More" aria-label="More actions">${icon('more')}</button>
        <input type="file" class="ed-upload-input" multiple hidden>
        <input type="file" class="ed-tar-input" accept=".tar,application/x-tar" hidden>
      </div>
      <div class="ed-main">
        <aside class="ed-sidebar">
          <div class="ed-tabs" role="tablist">
            <button type="button" class="ed-tab" role="tab" data-tab="files" aria-selected="true">Files</button>
            <button type="button" class="ed-tab" role="tab" data-tab="search" aria-selected="false" title="Search in files (Ctrl+Shift+F)">Search</button>
          </div>
          <div class="ed-files" role="tree" aria-label="Files on the device"></div>
          <div class="ed-search" hidden></div>
        </aside>
        <div class="ed-groups"></div>
      </div>
      <div class="ed-log" hidden>
        <div class="ed-log-bar">
          <span class="ed-log-title">Build log</span>
          <span class="spacer"></span>
          <button type="button" class="tool-btn ed-log-hide" aria-label="Hide build log">Hide</button>
        </div>
        <pre class="ed-log-body"></pre>
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
        <button type="button" class="pane-btn" data-act="pexpart">PEX partition…</button>
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
    this._log = q('.ed-log');
    this._logBody = q('.ed-log-body');
    q('.ed-log-hide').addEventListener('click', () => { this._log.hidden = true; });
    this.help = new EditorHelp(this.app);
    this._searchEl = q('.ed-search');
    this.search = new SearchPanel(this._searchEl, {
      list: fs.list,
      readText: fs.readText,
      buffer: (p) => (this.buffers.has(p) && !this.buffers.get(p).binary
        ? (this.path === p ? this.core.value : this.buffers.get(p).text) : null),
      onOpen: (p, m) => this.openAt(p, m),
    });
    for (const t of el.querySelectorAll('.ed-tab')) t.addEventListener('click', () => this.sidebarTab(t.dataset.tab));
    this._splitBtn = q('.ed-b-split');
    this.restorer = new RestoreDialog(this.app, {
      onDone: async ({ target }) => { await this.tree.refresh(); if (target) this.tree.reveal(target); this._poll(); },
      track: (title) => this._job(title, { onAbort: () => { this.restorer.stop = true; } }),
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
      onOpen: (p) => this._activateFile(p),
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
      const next = this.tree.applyPick(gestureOf(e), row.dataset.path);
      if (next.open) {
        const n = this.tree.find(row.dataset.path);
        if (n?.dir) this.tree.toggle(n);
        else this._activateFile(row.dataset.path);
      }
      this.tree.invalidate();
      this._paintPartition();
      this._renderTitle();
    });

    q('.ed-b-sidebar').addEventListener('click', () => this.toggleSidebar());
    this._keysBtn = q('.ed-b-keys');
    this._keysBtn.addEventListener('click', () => this.app.setPref('vim', !this.vimOn));
    q('.ed-b-save').addEventListener('click', () => this.save());
    this._runBtn = q('.ed-b-run');
    this._stopBtn = q('.ed-b-stop');
    this._runBtn.addEventListener('click', () => this.run());
    this._stopBtn.addEventListener('click', () => this.stop());
    this._buildBtn = q('.ed-b-build');
    this._buildLabel = q('.ed-build-label');
    this._brunBtn = q('.ed-b-brun');
    this._buildBtn.addEventListener('click', () => this.build());
    this._brunBtn.addEventListener('click', () => this.build({ run: true }));
    q('.ed-b-new').addEventListener('click', () => this.newFile());
    q('.ed-b-upload').addEventListener('click', () => { this._uploadVia = ''; this._uploadDir = undefined; this._upload.click(); });
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
    q('.ed-b-pex').addEventListener('click', () => this.open(PEX_PART));
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
      // Shift+Space reaches this list. Ctrl+Space does not (nav.js ignores it),
      // so that gesture is a listener on the list itself, below.
      if (e.code === 'Space' && e.shiftKey) {
        if (!this.tree.sel) return false;
        this.tree.applyPick('range', this.tree.sel);
        return true;
      }
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
    this._files.addEventListener('keydown', (e) => {
      if (e.code !== 'Space' || e.altKey || e.repeat) return;
      if (!(e.ctrlKey || e.metaKey)) return;
      if (!this.tree.sel) return;
      e.preventDefault();
      e.stopPropagation();
      this.tree.applyPick(e.shiftKey ? 'range' : 'toggle', this.tree.sel);
    });
    keys.nav.registerList(this._files, files);
    keys.nav.registerList(this.search.results, this.search.listController());
    // The Esc ladder: insert -> normal (vim takes it), then normal -> leave
    // the text (vim declines, the key layer blurs), then Alt+hjkl.
    for (const v of this.views.values()) keys.ownEscape(v.core.ta, (e) => this._vimEsc(e));
  }

  // --- search in files ----------------------------------------------------------

  // The sidebar's tab: 'files' or 'search'.
  sidebarTab(which) {
    const search = which === 'search';
    if (this._sidebar.hidden) { this._applySidebar(true); this.app.setPref('sidebar', true); }
    this._files.hidden = search;
    this._searchEl.hidden = !search;
    for (const t of this.el.querySelectorAll('.ed-tab')) t.setAttribute('aria-selected', String(t.dataset.tab === which));
    if (search) this.search.focus();
  }

  // Ctrl+Shift+F: the Search tab, its box filled with the selected text
  // (one line of it) when there is some.
  openSearch() {
    const ta = this.core.ta;
    const sel = ta.value.slice(ta.selectionStart, ta.selectionEnd);
    this.sidebarTab('search');
    // vim's block cursor is a one-character selection: not a query
    const vimCursor = this.vimOn && this.vst.mode === 'normal';
    if (sel && !sel.includes('\n') && !vimCursor) this.search.focus(sel);
  }

  // A search result: the file, the match selected and scrolled to.
  async openAt(path, m) {
    if (!(await this.open(path))) return;
    const n = this.core.value.length;
    const at = Math.min(m.at, n);
    if (this.vimOn) {
      this.vst = { ...this.vst, mode: 'normal' };
      this.vcur = at;
      this._vimPaint();
    } else {
      this.core.ta.setSelectionRange(at, Math.min(at + m.len, n));
      this.core.revealCaret(at);
    }
    this._say(`${path}:${m.line}:${m.col + 1}`);
    this._renderTitle();
  }

  // vim's :grep / :vimgrep -- `:grep needle`, `:grep /re.?gex/ lua/profiles`,
  // `:grep "two words" js`: the Search tab runs it.
  grepEx(arg) {
    const a = arg.trim();
    let q, rest, regex = false;
    let m;
    if ((m = /^\/((?:\\.|[^/])+)\/\s*(.*)$/.exec(a))) { q = m[1]; rest = m[2]; regex = true; }
    else if ((m = /^"([^"]+)"\s*(.*)$/.exec(a))) { q = m[1]; rest = m[2]; }
    else { [q, rest = ''] = a.split(/\s+(.*)/s); }
    if (!q) { this.sidebarTab('search'); return undefined; }
    this.search.option('regex', regex);
    this.sidebarTab('search');
    this.focusText();
    return this.search.run({ q, dir: rest.trim() }).then((r) => {
      if (r?.matches) this.search.step(1);
      else this._say(r ? `No matches for ${q}` : 'Not searched', 'warn');
    });
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
    const surface = el.querySelector('.ed-surface');
    v.core = new EditorCore(surface);
    v.preview = new ImagePreview(surface);
    v.part = new PexPartition(surface, {
      onPack: () => this._pexCompact(),
      onSelect: (name) => this._selectPex(name),
      onRemove: (name) => this._pexUninstall(name),
      onStart: (name) => this._pexStart(name),
      onStop: (name) => this._pexStop(name),
    });
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
      v.part.hide();
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
      // A tap on a draggable tab sometimes starts a drag the browser then
      // drops without dragend. The file still switches; the row would not.
      const endStuckDrag = () => {
        if (!this._tabDrag) return;
        requestAnimationFrame(() => {
          if (!this._tabDrag) return;
          this._tabDrag = false;
          this._groups.classList.remove('is-dnd');
          for (const o of this.views.values()) {
            o.el.querySelector('.ed-dropzone').hidden = true;
            o.el.querySelector('.ed-tabmark').hidden = true;
            o.bar.querySelectorAll('.is-dragging').forEach((t) => t.classList.remove('is-dragging'));
          }
          this._renderTitle();
        });
      };
      window.addEventListener('pointerup', endStuckDrag, true);
      window.addEventListener('pointercancel', endStuckDrag, true);
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
    if (v.shown && v.shown !== p && v.shown !== PEX_PART && g.tabs.includes(v.shown) && this.buffers.has(v.shown)) {
      v.saved.set(v.shown, v.core.saveView());
    }
    v.shown = p;
    // A fresh vim state, but the last search survives a change of file, as in vim.
    const { search, word, sdir, hl } = v.vst;
    v.vst = { ...initialState(), search, word, sdir, hl };
    v.painted = null;
    v.ins = null;
    if (p === PEX_PART) {
      v.core.scroll.hidden = true;
      v.preview.hide();
      v.core.setFind(null);
      this._partSig = '';
      this._paintPartition();
      return;
    }
    v.part.hide();
    v.core.scroll.hidden = false;
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
      if (sig === v.sig) return;
      const tabs = [...v.bar.querySelectorAll(':scope > .ed-ftab')];
      const same = tabs.length === g.tabs.length && tabs.every((t, i) => t.dataset.path === g.tabs[i]);
      // Same files: move the highlight in place. Replacing the row is what
      // makes a tap lose its selection when the browser also started a drag
      // (the drag must keep its node, so a full redraw is skipped).
      if (same) {
        tabs.forEach((t, i) => {
          const on = i === g.active;
          t.classList.toggle('is-active', on);
          t.classList.toggle('is-dirty', !!dirty[i]);
          t.setAttribute('aria-selected', String(on));
        });
        v.sig = sig;
        v.bar.hidden = !g.tabs.length;
        tabs[g.active]?.scrollIntoView({ block: 'nearest', inline: 'nearest' });
        return;
      }
      if (this._tabDrag) return;
      v.sig = sig;
      v.bar.hidden = !g.tabs.length;
      const base = (p) => (p === PEX_PART ? 'PEX partition' : p.split('/').pop());
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
    if (this.path && !String(this.path).includes('://')) this.tree.reveal(this.path);
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
    if (this.path && !String(this.path).includes('://')) this.tree.reveal(this.path);
    if (this.path) this.core.focus(); else this.focusFiles();
    return true;
  }

  _afterTabs() {
    this._layout();
    this._showAll();
    this._updateFind();
    this._renderTitle();
    this._saveTabs();
    if (this.path && !String(this.path).includes('://')) this.tree.reveal(this.path);
    if (this.path) this.core.focus(); else this.focusFiles();
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
    this.search.invalidate();
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
      case 'grep': return this.grepEx(arg);
      case 'cnext': return this.search.step(1) || this._say('No search results — :grep pattern [folder]', 'warn');
      case 'cprev': return this.search.step(-1) || this._say('No search results — :grep pattern [folder]', 'warn');
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
    if (id === 'editor-search-files') return this.openSearch();
    if (id === 'editor-search-next') return this.search.step(1) || this._say('No search results — Ctrl+Shift+F searches', 'warn');
    if (id === 'editor-search-prev') return this.search.step(-1) || this._say('No search results — Ctrl+Shift+F searches', 'warn');
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
    this._bootP = this._restoreTabs().then(() => this._freshAll());
    this._poll();
    clearInterval(this._pollTimer);
    this._pollTimer = setInterval(() => this._poll(), ARENA_POLL_MS);
    this._armFresh();
  }

  onHide() {
    clearInterval(this._pollTimer);
    this._pollTimer = 0;
    clearInterval(this._freshTimer);
    this._freshTimer = 0;
  }

  onConnect() {
    const board = this._boardName();
    if (board) {
      for (const [dir, pin] of this._buildPin) {
        if (pin.board && pin.board !== board) this._buildPin.delete(dir);
      }
    }
    this._proj = null;
    if (this._pollTimer) { this.tree.refresh(); this._poll(); }
    this._adoptFresh();
    this._scheduleProject();
  }

  // fs.stat on reconnect. An older image has no such op, and the poll stays.
  async _adoptFresh() {
    const paths = this._freshPaths().slice(0, 32);
    if (!paths.length) return;
    let stat;
    try {
      stat = await this.ws.call('fs.stat', { paths });
    } catch {
      this._freshAll();
      return;
    }
    if (stat?.pending) {
      stat = await new Promise((resolve, reject) => {
        const timer = setTimeout(() => { off(); reject(new Error('fs.stat timed out')); }, 20000);
        const off = this.ws.on('fs.stated', (data) => { clearTimeout(timer); off(); resolve(data); });
      }).catch(() => null);
      if (!stat) { this._freshAll(); return; }
    }
    this._eventsLive = true;
    clearInterval(this._freshTimer);
    this._freshTimer = 0;
    for (const row of stat.files || []) await this._applyChange(row);
  }

  _onFsChanged(data) {
    if (!data?.path) return;
    if (!this._eventsLive) {
      this._eventsLive = true;
      clearInterval(this._freshTimer);
      this._freshTimer = 0;
    }
    this._applyChange(data);
  }

  async _applyChange(row) {
    const path = row?.path;
    const b = path && this.buffers.get(path);
    if (!b || path === this._freshSkip) return;
    const action = changeAction({
      binary: b.binary,
      partition: b.partition,
      gz: isGz(path),
      hash: row.hash,
      base: b.base,
      missing: row.op === 'delete' || !!row.missing,
    });
    if (action === 'read') await this._freshPath(path);
  }

  _armFresh() {
    clearInterval(this._freshTimer);
    this._freshTimer = 0;
    if (this._eventsLive) return;
    this._freshTimer = setInterval(() => this._freshOne(), FRESH_MS);
  }

  // Open text files, in a stable order, skipping one whose save is in flight.
  _freshPaths() {
    const out = [];
    for (const [path, b] of this.buffers) {
      if (!b || b.binary || b.partition || path === this._freshSkip) continue;
      out.push(path);
    }
    out.sort();
    return out;
  }

  _freshOne() {
    if (this._freshFlight || this._freshAsking) return;
    if (Date.now() < this._freshNext) return;
    const paths = this._freshPaths();
    if (!paths.length) return;
    const path = paths[this._freshIx % paths.length];
    this._freshIx++;
    this._freshFlight = this._freshPath(path).finally(() => { this._freshFlight = null; });
  }

  _freshAll() {
    if (this._freshFlight || this._freshAsking) return this._freshFlight;
    const paths = this._freshPaths();
    this._freshFlight = (async () => {
      for (const path of paths) {
        if (this._freshAsking) return;
        const busy = await this._freshPath(path);
        if (busy) return;
      }
    })().finally(() => { this._freshFlight = null; });
    return this._freshFlight;
  }

  async _freshPath(path) {
    const b = this.buffers.get(path);
    if (!b || b.binary || b.partition || path === this._freshSkip) return false;
    if (this.path === path && this.core) b.text = this.core.value;
    const rev = b.rev || 0;
    const r = await fs.readText(path);
    const now = this.buffers.get(path);
    if (!now || now !== b || (now.rev || 0) !== rev || path === this._freshSkip) return false;
    if (this.path === path && this.core) now.text = this.core.value;
    const remote = r.ok
      ? { status: 200, hash: fnv1a(r.text), text: r.text }
      : { status: r.status, offline: !!r.offline };
    const action = freshnessAction({
      binary: now.binary,
      partition: now.partition,
      isNew: !!now.isNew,
      pristine: !!now.isNew && now.text === '',
      dirty: this._isDirty(now),
      base: now.base,
      kept: now.keptRemote,
      notedGone: !!now.notedGone,
      askedGone: !!now.askedGone,
      remote,
    });
    if (action === 'busy') {
      this._freshWait = nextFreshDelay(this._freshWait);
      this._freshNext = Date.now() + this._freshWait;
      return true;
    }
    if (action === 'error') return false;
    this._freshWait = 0;
    if (action === 'reload') await this._takeRemote(path, now, remote.text);
    else if (action === 'ask') await this._askRemote(path, now, remote);
    else if (action === 'gone') await this._askGone(path, now);
    else if (action === 'gone-dirty') {
      now.notedGone = true;
      this._say(`${path} was deleted on another device. This copy is still here.`, 'warn');
    }
    return false;
  }

  async _takeRemote(path, b, text) {
    if (!this.buffers.get(path)) return;
    const pristine = !!b.isNew && b.text === '';
    if (this._isDirty(b) && !pristine) return;
    b.text = text;
    b.base = fnv1a(text);
    delete b.isNew;
    delete b.keptRemote;
    delete b.notedGone;
    delete b.askedGone;
    for (const v of this.views.values()) {
      if (v.shown === path) v.core.adopt(text);
    }
    this.search.invalidate(path);
    this.tree.invalidate();
    this._renderTitle();
    this._say(`Updated ${path} from another device.`);
  }

  async _askRemote(path, b, remote) {
    if (this._freshAsking || !this.buffers.get(path)) return;
    this._freshAsking = true;
    const reload = confirm(`${path} changed on another device. Reload it and discard your edits?`);
    this._freshAsking = false;
    if (!this.buffers.get(path)) return;
    if (!reload) {
      b.keptRemote = remote.hash;
      this._say(`Keeping your edits to ${path}.`, 'warn');
      return;
    }
    this.buffers.delete(path);
    await this.open(path);
    this._say(`Updated ${path} from another device.`);
  }

  async _askGone(path, b) {
    if (this._freshAsking || !this.buffers.get(path)) return;
    b.askedGone = true;
    this._freshAsking = true;
    const close = confirm(`${path} was deleted on another device. Close it?`);
    this._freshAsking = false;
    if (!close || !this.buffers.get(path)) return;
    for (let gi = this.tabs.groups.length - 1; gi >= 0; gi--) {
      if (this.tabs.groups[gi]?.tabs.includes(path)) this.closeTab(path, gi, { force: true });
    }
  }

  async _poll() {
    if (!this.ws.connected) return;
    try {
      // pex.list is absent on firmware built before PEX. A failure leaves the
      // Lua marks updating exactly as they did.
      const [loaded, list, pex, inst] = await Promise.all([
        this.ws.call('script.loaded'),
        this.ws.call('script.list'),
        this.ws.call('pex.list').catch(() => null),
        this.ws.call('pex.installed').catch(() => null),
      ]);
      this.marks = arenaMarks(loaded.units);
      this.list = list;
      this.runtime = list.runtime || null;
      this.pexStates = pexRunningPaths(pex);
      this.pexArch = pex?.arch || this.pexArch;
      this.pexPool = pex?.pool || this.pexPool;
      this.pexPolicy = pex?.policy || this.pexPolicy;
      this.pexInstalled = new Set((inst?.modules || []).map((m) => m.name));
      this.pexPart = inst;
      this.tree.invalidate();
      this._renderTitle();
      this._paintPartition();
    } catch { /* offline or an older firmware: markers simply stay as they were */ }
  }

  // --- buffers ---------------------------------------------------------------

  _isDirty(b) { return !b.binary && fnv1a(b.text) !== b.base; }

  _mark(path) {
    const b = this.buffers.get(path);
    return {
      dirty: !!b && this._isDirty(b),
      arena: shownArena(this.marks.get(path), b?.base),
      pex: this.pexStates[path],
    };
  }

  // A file into a buffer (once; every tab and half shares it). Returns the
  // buffer, or null after saying why not. { create: true } (from :e, :tabnew,
  // :sp) makes a missing file a new, unsaved buffer -- as vim does -- rather
  // than reporting it missing.
  async _load(path, { create = false } = {}) {
    if (path === PEX_PART) {
      let part = this.buffers.get(path);
      if (!part) {
        part = { text: '', base: fnv1a(''), lang: '', binary: true, partition: true, fresh: true };
        this.buffers.set(path, part);
      }
      return part;
    }
    let b = this.buffers.get(path);
    if (b) return b;
    const img = imageKind(path);
    if (!img && /\.(bin|luac|bak|pex)$/i.test(path)) {
      // A .pex is the app itself. Shown as binary so Save cannot rewrite it
      // with the empty text buffer. Run / Stop start and stop the app.
      b = { text: '', base: fnv1a(''), lang: 'text', binary: true };
    } else {
      // Stream the body so the operations list moves before the text is copied
      // into the textarea. The read yields so Abort can run. One frame shows
      // the last progress line, and the buffer is kept only if Abort is still
      // clear after that frame. Done is marked on the frame after the copy.
      const base = path.split('/').pop();
      const job = this._job(`Opening ${base}`);
      const watch = {
        signal: job.signal,
        onProgress: ({ got, total }) => job.set(byteLine('Opening', base, got, total), byteFrac(got, total)),
      };
      const later = (line) => requestAnimationFrame(() => job.done(line));
      const stopped = () => {
        if (!job.signal.aborted) return false;
        job.aborted();
        return true;
      };
      if (img) {
        const r = await fs.readFileStream(path, watch);
        if (r.aborted) { job.aborted(); return null; }
        if (stopped()) return null;
        if (!r.ok) { job.fail(jobError(r.error)); this._say(`Could not open ${path}: ${jobError(r.error)}`, 'err'); return null; }
        job.set(byteLine('Opening', base, r.size, r.total || r.size), 0.99);
        await nextFrame();
        if (stopped()) return null;
        const blob = new Blob([r.bytes], { type: imageType(path) });
        const text = img === 'svg' ? await blob.text() : '';
        if (stopped()) return null;
        b = { text, base: fnv1a(text), lang: 'text', binary: img !== 'svg', image: img, blob, showText: false };
        later(`Opened ${base}`);
      } else {
        const r = await fs.readTextStream(path, watch);
        if (r.aborted) { job.aborted(); return null; }
        if (stopped()) return null;
        if (!r.ok && r.offline && isGz(path)) {
          const msg = jobError(r.error);
          job.fail(msg);
          this._say(`Could not open ${path}: ${msg}`, 'err');
          return null;
        }
        if (!r.ok && create && r.status === 404) {
          b = { text: '', base: '', lang: languageOf(path), binary: false, isNew: true };
          later(`New file ${base}`);
        } else if (!r.ok) {
          job.fail(jobError(r.error));
          this._say(`Could not open ${path}: ${jobError(r.error)}`, 'err');
          return null;
        } else {
          job.set(byteLine('Opening', base, r.size, r.total || r.size), 0.99);
          await nextFrame();
          if (stopped()) return null;
          b = { text: r.text, base: fnv1a(r.text), lang: languageOf(path), binary: r.text.includes('\0') };
          if (stopped()) return null;
          later(`Opened ${base}`);
        }
      }
    }
    this.buffers.set(path, b);
    b.fresh = true;
    return b;
  }

  // Show a file in the focused half: its tab if it has one, else a new tab.
  async open(path, { create = false } = {}) {
    if (path === PEX_PART) return this._openPartition();
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
    if (!String(path).includes('://')) this.tree.reveal(path);
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
    this._freshSkip = path;
    // A .gz stays compressed: the browser packs it, the board cannot.
    const body = isGz(path) ? await gzipText(text) : text;
    // A file of the web UI keeps the version it replaces as <file>.bak --
    // renamed, so the .bak is the stored bytes exactly.
    let bak = null;
    if (isWebAsset(path) && b.base !== '') {
      bak = `${path}.bak`;
      await fs.remove(bak);
      const mv = await fs.rename(path, bak);
      if (!mv.ok && mv.status !== 404) {
        this._freshSkip = '';
        this._say(`Save stopped: could not keep ${bak} (${mv.error})`, 'err');
        return false;
      }
      if (!mv.ok) bak = null;                      // nothing there to keep
    }
    const r = await fs.write(path, body);
    if (!r.ok) {
      this._freshSkip = '';
      if (bak) await fs.rename(bak, path);         // put the old one back
      this._say(`Save failed: ${jobError(r.error)}`, 'err');
      return false;
    }
    b.text = text;
    b.base = fnv1a(text);
    this._freshSkip = '';
    b.rev = (b.rev || 0) + 1;
    delete b.keptRemote;
    this.search.invalidate(path);
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
    this._proj = null;
    this._scheduleProject();
    return true;
  }

  // :w <path> -- write the buffer to another file and stay on this one, as
  // vim does.
  async writeCopy(path) {
    if (!this.path && !this.core.value) return false;
    const r = await fs.write(path, this.core.value);
    if (!r.ok) { this._say(`Write to ${path} failed: ${jobError(r.error)}`, 'err'); return false; }
    this._say(`"${path}" written`);
    await this.tree.refresh();
    return true;
  }

  async run() {
    const pex = this._activePex();
    if (pex) return this._pexStart(pex);
    if (this.path === PEX_PART) { this._say('Select an installed module. Run starts it from the partition.', 'warn'); return; }
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
    const pex = this._activePex();
    if (pex) return this._pexStop(pex);
    if (this.path === PEX_PART) return;
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

  // The toolbar Run / Stop buttons, and the file menu's Start / Stop on a
  // .pex file. Lua scripts never come through here.
  async _pexStart(name) {
    if (!name) return;
    return this._pexRun(`Starting ${name}`, async (job) => {
      job.set(`Starting ${name}`, 0.45);
      const r = await startPex(this.ws, name);
      return r.started ? `Started ${name}` : `${name} is already running`;
    });
  }

  async _pexStop(name) {
    if (!name) return;
    return this._pexRun(`Stopping ${name}`, async (job) => {
      job.set(`Stopping ${name}`, 0.45);
      const r = await stopPex(this.ws, name);
      if (r.unloaded) return `Stopped and unloaded ${name}`;
      return { warn: true, text: `Stopped ${name}. It is still loaded: ${r.error || 'unload failed'}` };
    });
  }

  // One PEX operation in the operations list. Firmware does not report byte
  // progress or accept a cancel, so the line names the stage. A dropped
  // connection is not the word "unreachable". Install does not copy the
  // .pex onto LittleFS: the image goes to the PEX partition.
  async _pexRun(title, work) {
    const job = this._job(title);
    job.set(title, 0.08);
    try {
      const line = await work(job);
      const text = typeof line === 'string' ? line : (line?.text || title);
      job.done(text);
      if (text) this._say(text, line?.warn ? 'warn' : '');
    } catch (e) {
      if (e?.cancelled) {
        const text = e.message || 'Cancelled';
        job.done(text);
        this._say(text);
      } else {
        const raw = e?.message || String(e);
        let msg = jobError(raw);
        if (msg === 'The device did not answer.' && /^(Installing|Restoring)/.test(title)) {
          msg = 'The device did not answer. Nothing was copied to internal storage.';
        }
        if (String(this.pexPolicy).includes('signed-only') && /sign|unsigned|policy/i.test(raw) && !/pexlink\.py sign/.test(msg)) {
          msg += ' Sign on the host with python3 pex/tools/pexlink.py sign.';
        }
        job.fail(msg);
        this._say(msg, 'err');
      }
    }
    await this._poll();
    this.tree.refresh();
  }

  async _deployManifest(name) {
    try {
      const { deployDriverManifest } = await import('../editor/drivermanifests.js');
      const man = await deployDriverManifest((p, data) => fs.write(p, data), name);
      if (man.ok) return man.path ? ` Wrote ${man.path}.` : '';
      return { warn: true, text: ` ${jobError(man.error || 'could not write the driver manifest')}` };
    } catch (e) {
      return { warn: true, text: ` ${jobError(e?.message || String(e))}` };
    }
  }

  _installLine(name, from, ready, extra) {
    const where = from ? ` from ${from}` : '';
    const base = ready === false
      ? `Installed ${name}${where}. It loads into RAM until the next boot reserves its RW area.`
      : `Installed ${name}${where}.`;
    if (extra?.warn) return { warn: true, text: `${base}${extra.text}` };
    return `${base}${extra || ''}`;
  }

  async _pexMark(path) {
    const base = path.split('/').pop();
    return this._pexRun(`Marking ${base} verified`, async (job) => {
      const { pexIdentity, markVerified } = await import('../editor/pexproj.js');
      const id = pexIdentity(path);
      if (!id) throw new Error('That file is not a PEX module.');
      job.set(`Reading ${base}`, 0.25);
      const got = await fs.readBlob(path);
      if (!got.ok) throw new Error(got.error || `could not read ${path}`);
      const data = new Uint8Array(await got.blob.arrayBuffer());
      job.set('Writing the verified copy', 0.7);
      const res = await markVerified({
        name: id.name,
        arch: id.arch,
        bytes: data,
        buildId: buildIdOf(data),
        when: new Date().toISOString(),
        write: (p, body) => fs.write(p, body),
        rename: (a, b) => fs.rename(a, b),
        remove: (p) => fs.remove(p),
      });
      if (!res.ok) throw new Error(res.error || 'Mark verified failed.');
      return `Marked ${res.file} verified`;
    });
  }

  async _pexRestore(path) {
    const base = path.split('/').pop();
    return this._pexRun(`Restoring ${base}`, async (job) => {
      const { pexIdentity, restoreVerified } = await import('../editor/pexproj.js');
      const id = pexIdentity(path);
      if (!id) throw new Error('That file is not a PEX module.');
      job.set('Reading the verified copy', 0.25);
      const key = `pex/${id.name}.${id.arch}.pex`;
      const state = this.pexStates?.[key];
      const res = await restoreVerified({
        name: id.name,
        arch: id.arch,
        loaded: !!state,
        running: state === 'running',
        poolLargest: this.pexPool?.largest,
        confirm: (text) => confirm(text),
        unload: (name) => stopPex(this.ws, name),
        ws: this.ws,
        write: (p, data) => fs.write(p, data),
        read: async (p) => {
          const r = await fs.readBlob(p);
          if (!r.ok) return { ok: false, error: jobError(r.error) };
          return { ok: true, bytes: new Uint8Array(await r.blob.arrayBuffer()) };
        },
      });
      if (res.cancelled) {
        const err = new Error(res.error || 'Restore cancelled.');
        err.cancelled = true;
        throw err;
      }
      if (!res.ok) throw new Error(res.error || 'Restore failed.');
      job.set('Installed', 0.9);
      if (res.warn) return { warn: true, text: `Restored ${id.name} from ${res.file}. ${res.warn}` };
      return `Restored ${id.name} from ${res.file}`;
    });
  }

  async _pexInstall(name) {
    if (!name) return;
    return this._pexRun(`Installing ${name}`, async (job) => {
      job.set(`Installing ${name} into the PEX partition`, 0.4);
      const r = await this.ws.call('pex.install', { name });
      job.set('Writing the driver manifest', 0.85);
      const extra = await this._deployManifest(name);
      return this._installLine(name, '', r?.xip_ready, extra);
    });
  }

  // A .pex that is not pex/<name>.<arch>.pex, including one in a build
  // directory on the card. Firmware streams an sd: install through a 4 KB
  // window. It is not copied to LittleFS. A RAM load is the path that
  // still needs the whole file in the pool.
  async _pexInstallFrom(path) {
    const base = path.split('/').pop();
    return this._pexRun(`Installing ${base}`, async (job) => {
      const { pexIdentity, installFromCard } = await import('../editor/pexproj.js');
      const { sdLoadPath } = await import('../editor/pexfile.js');
      const id = pexIdentity(path);
      const sdPath = sdLoadPath(path);
      if (!id || !sdPath) throw new Error('Install from the card needs a file named name.arch.pex under sd/.');
      const key = `pex/${id.name}.${id.arch}.pex`;
      const state = this.pexStates?.[key];
      if (state) {
        const ask = `Unload ${id.name} and replace the installed module? Cancel leaves it loaded. The file stays on the card.`;
        if (!confirm(ask)) {
          const err = new Error(`Left ${id.name} loaded.`);
          err.cancelled = true;
          throw err;
        }
        job.set(`Unloading ${id.name}`, 0.3);
        const u = await stopPex(this.ws, id.name);
        if (!u?.unloaded) throw new Error(u?.error || `Unload of ${id.name} failed.`);
      }
      job.set(`Installing ${id.name} from the card`, 0.55);
      const r = await installFromCard({ ws: this.ws, sdPath, name: id.name });
      job.set('Writing the driver manifest', 0.85);
      const extra = await this._deployManifest(id.name);
      return this._installLine(id.name, 'the card', r?.xip_ready, extra);
    });
  }

  async _pexUninstall(name) {
    if (!name) return;
    if (!confirm(`Uninstall ${name} from the PEX partition? The file in pex/ stays. A running copy has to be stopped and unloaded first.`)) return;
    return this._pexRun(`Uninstalling ${name}`, async (job) => {
      job.set(`Uninstalling ${name}`, 0.45);
      await this.ws.call('pex.uninstall', { name });
      return `Uninstalled ${name}`;
    });
  }

  async _pexCompact() {
    return this._pexRun('Packing the PEX partition', async (job) => {
      job.set('Packing the PEX partition', 0.45);
      const r = await this.ws.call('pex.compact', {});
      const n = Number(r?.moved) || 0;
      return `Packed the PEX partition: ${n} file${n === 1 ? '' : 's'} moved, ${bytes(r?.free)} free, largest run ${bytes(r?.largest)}.`;
    });
  }

  // A .pex (and a gzipped copy) opens its menu. Editable files still open a tab.
  // The toolbar still follows the selected module: see _activePex.
  _activateFile(path) {
    if (isPexFile(path)) { this._rowMenu(path); return; }
    return this.open(path);
  }

  // The row the user pointed at wins, so Run / Stop follow a .pex file even
  // while a script stays open. The open buffer is the fallback. The partition
  // view keeps the block it last described when the tree is on something else.
  _activePex() {
    const selected = pexNameOf(this.tree?.sel);
    if (selected) return selected;
    const open = pexNameOf(this.path);
    if (open) return open;
    return this.path === PEX_PART ? (this._pexFocus || null) : null;
  }

  _selectedPexName() {
    return pexNameOf(this.tree?.sel) || this._pexFocus || null;
  }

  _selectPex(name) {
    if (!name) return false;
    this._pexFocus = name;
    const path = `pex/${name}.${this.pexArch}.pex`;
    if (this.tree.find(path)) {
      this.tree.sel = path;
      this.tree.reveal(path);
    }
    this._partSig = '';
    this._paintPartition();
    return false;
  }

  async _openPartition() {
    const b = await this._load(PEX_PART);
    if (!b) return false;
    b.fresh = false;
    this._openBuffer(PEX_PART, { force: true });
    if (!this.pexPart) this._poll();
    this._say('PEX partition. Select a module. Start and Stop run that installed copy. Uninstall removes it. Pack slides the rest together.');
    return true;
  }

  _paintPartition() {
    const selected = this._selectedPexName();
    const states = {};
    for (const [path, state] of Object.entries(this.pexStates || {})) {
      const name = pexNameOf(path);
      if (name) states[name] = state;
    }
    const sig = JSON.stringify([this.pexPart, selected, states]);
    if (sig === this._partSig) return;
    this._partSig = sig;
    for (const v of this.views.values()) {
      if (v.shown === PEX_PART && v.part) v.part.show(this.pexPart, { selected, states });
    }
  }

  _act_pexpart() { return this.open(PEX_PART); }

  _boardName() {
    const b = this.app.sysInfo?.board;
    return typeof b === 'string' && b ? b : null;
  }

  // The open text buffer, when the build should compile it in place of the
  // card copy. Only this path is substituted. Other dirty tabs stay on the card.
  _activeSource() {
    const path = this.path;
    const b = path && this.buffers.get(path);
    if (!b || b.binary || b.partition) return null;
    if (path === this.path && this.core) b.text = this.core.value;
    return { path, text: b.text, dirty: this._isDirty(b) };
  }

  async _readProjectText(path) {
    const b = this.buffers.get(path);
    if (b && !b.binary && !b.partition) {
      const text = path === this.path && this.core ? this.core.value : b.text;
      return { ok: true, text, status: 200 };
    }
    return fs.readText(path);
  }

  _scheduleProject() {
    const path = this.path;
    if (this._proj?.filePath === path && (this._proj.ready || this._proj.loading)) return;
    const gen = ++this._projGen;
    this._proj = { filePath: path, ready: false, loading: true };
    this._loadProject(path, gen);
  }

  async _loadProject(path, gen) {
    if (!path || path === PEX_PART) {
      if (gen !== this._projGen) return;
      this._proj = { filePath: path, ready: true, project: null, selection: null, errors: [] };
      this._renderTitle();
      return;
    }
    try {
      const { loadProject } = await import('../editor/pexproj.js');
      const pin = path && this._projPin(path);
      const spec = await loadProject({
        filePath: path,
        readText: (p) => this._readProjectText(p),
        arch: this.pexArch,
        board: this._boardName(),
        pin,
      });
      if (gen !== this._projGen) return;
      this._proj = { ...spec, ready: true, loading: false };
    } catch (e) {
      if (gen !== this._projGen) return;
      this._proj = {
        filePath: path, ready: true, loading: false, project: null, selection: null,
        errors: [e.message || String(e)],
      };
    }
    this._renderTitle();
  }

  // A pin applies to the project directory, which we do not know until the
  // Pexfile is found. Pass every pin whose directory is a parent of the file;
  // loadProject only honors a pin that is one of the bound targets.
  _projPin(filePath) {
    let best = null;
    let bestLen = -1;
    for (const [dir, pin] of this._buildPin) {
      if (filePath === dir || filePath.startsWith(`${dir}/`)) {
        if (dir.length > bestLen) { best = pin.target; bestLen = dir.length; }
      }
    }
    return best;
  }

  _rememberPin(dir, target) {
    if (!dir || !target) return;
    this._buildPin.set(dir, { target: target.name, board: target.board || null });
  }

  _logLine(level, text) {
    this._log.hidden = false;
    this._logBody.textContent += (this._logBody.textContent ? '\n' : '') + text;
    this._logBody.scrollTop = this._logBody.scrollHeight;
    const kind = level === 'error' ? 'err' : level === 'warn' ? 'warn' : '';
    this._say(text, kind);
  }

  // Build the project of `from` (the open file, unless the Pexfile menu
  // named one). `target` pins that target for the session. `all` builds
  // every target. `run` continues into the target's run ram or run flash.
  async build({ from = null, target = null, all = false, run = false, pin = false } = {}) {
    if (this._building) { this._say('A build is already running.', 'warn'); return; }
    const origin = from || this.path;
    if (!origin || origin === PEX_PART) {
      this._say('This file is not in a project.', 'warn');
      return;
    }
    if (origin.startsWith('sd/') && !fs.sdAvailable()) { this._say(NO_SD, 'err'); return; }
    const job = this._job(`Building ${origin.split('/').pop()}`);
    this._building = true;
    this._log.hidden = false;
    this._logBody.textContent = '';
    try {
      const { loadProject, executeTarget } = await import('../editor/pexproj.js');
      const { buildModule } = await import('../editor/pexbuild.js');
      const spec = await loadProject({
        filePath: origin,
        readText: (p) => this._readProjectText(p),
        arch: this.pexArch,
        board: this._boardName(),
        pin: target || this._projPin(origin),
      });
      if (!spec.project) {
        const why = spec.errors[0] || 'This file is not in a project.';
        job.fail(why);
        this._say(why, 'warn');
        return;
      }
      if (spec.errors.length) {
        for (const e of spec.errors) this._logLine('error', e);
        job.fail(spec.errors[0]);
        return;
      }
      const targets = all
        ? spec.targets
        : spec.targets.filter((t) => t.name === (target || spec.selection?.target?.name));
      if (!targets.length) {
        const why = spec.selection?.reason || 'No target.';
        job.fail(why);
        this._say(why, 'warn');
        return;
      }
      if (pin && targets.length === 1) {
        this._rememberPin(spec.dir, targets[0]);
        this._proj = null;
      }
      let lastError = '';
      for (const t of targets) {
        const key = `pex/${spec.project.name}.${t.arch}.pex`;
        const result = await executeTarget({
          project: spec.project,
          target: t,
          andRun: run && !all,
          active: this._activeSource(),
          readText: (p) => this._readProjectText(p),
          write: (p, data) => fs.write(p, data),
          list: (p) => fs.list(p),
          compile: async (opts) => {
            const report = (m) => {
              const p = /Fetching the compiler/.test(m) ? 0.15
                : /^Fetching/.test(m) ? 0.3
                : /^Compiling/.test(m) ? 0.55
                : /^Linking/.test(m) ? 0.85 : null;
              job.set(m, p ?? undefined);
              opts.onStatus?.(m);
            };
            if (job.signal.aborted) { const err = new Error('Aborted'); err.aborted = true; throw err; }
            const compiled = await buildModule({ ...opts, onStatus: report });
            // compileToElf cannot be stopped mid-call. Abort stays Aborting
            // until it returns, and the result is then thrown away.
            if (job.signal.aborted) { const err = new Error('Aborted'); err.aborted = true; throw err; }
            return compiled;
          },
          confirm: (text) => confirm(text),
          ws: this.ws,
          poolLargest: this.pexPool?.largest,
          policy: this.pexPolicy,
          running: this.pexStates?.[key] === 'running',
          loaded: !!this.pexStates?.[key],
          unload: (name) => stopPex(this.ws, name),
          onLine: (level, text) => {
            this._logLine(level, text);
            if (level !== 'debug' && level !== 'error') job.set(text);
          },
        });
        if (!result.ok && result.error) lastError = result.error;
      }
      await this.tree.refresh();
      if (run) await this._poll();
      if (job.signal.aborted) job.aborted();
      else if (lastError) { const shown = jobError(lastError); job.fail(shown); this._say(shown, 'err'); }
      else job.done('Build finished');
    } catch (e) {
      const msg = e && e.message ? e.message : String(e);
      if (job.signal.aborted || e?.aborted) job.aborted();
      else {
        const shown = jobError(msg);
        job.fail(shown);
        this._logLine('error', shown);
      }
    } finally {
      this._building = false;
      this._scheduleProject();
    }
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
    if (this.path === PEX_PART) { this._poll(); return; }
    if (!this.path) return;
    const b = this.buffers.get(this.path);
    if (b && this._isDirty(b) && !confirm(`Discard unsaved changes to ${this.path}?`)) return;
    this.buffers.delete(this.path);
    this.open(this.path);            // a fresh read: both halves showing it take the new text
  }

  _act_rename() { return this.renameFile(this.path); }
  _act_delete() { return this.deleteFile(this.path); }

  // One line in the operations list. Abort cancels the fetch; the line stays
  // Aborting until done/fail/aborted, and a later call is ignored.
  _job(title, { boardId = 0, onAbort = null, onBoard = null } = {}) {
    const ctl = new AbortController();
    let settled = false;
    const id = this.jobs.start({
      title,
      boardId,
      onAbort: () => { ctl.abort(); onAbort?.(); },
      onBoard,
    });
    const settle = (fn) => { if (settled) return; settled = true; fn(); };
    return {
      id,
      boardId,
      signal: ctl.signal,
      set: (line, progress) => { if (!settled) this.jobs.update(id, { line, progress }); },
      done: (line) => settle(() => {
        if (ctl.signal.aborted) this.jobs.finish(id, { aborted: true });
        else {
          if (line) this.jobs.update(id, { line, progress: 1 });
          this.jobs.finish(id);
        }
      }),
      fail: (error) => settle(() => {
        if (ctl.signal.aborted) this.jobs.finish(id, { aborted: true });
        else this.jobs.finish(id, { error: error || 'failed' });
      }),
      aborted: () => settle(() => this.jobs.finish(id, { aborted: true })),
      request: () => this.jobs.requestAbort(id),
    };
  }

  // A delete the board can report file by file (fs.job) when it accepts the
  // batch. The id in that event is boardId, not the list's own id.
  _removeJob(title) {
    const boardId = ++this._boardSeq;
    const job = this._job(title, {
      boardId,
      onBoard: (d) => {
        if (!d || (d.kind && d.kind !== 'delete')) return;
        const total = d.total || 1;
        const index = d.index || 0;
        const line = d.path ? `Deleting ${d.path} · ${index} of ${total}` : `Deleting · ${index} of ${total}`;
        job.set(line, total ? index / total : 0);
      },
    });
    return job;
  }

  _saveBlob(blob, name) {
    const a = document.createElement('a');
    a.href = URL.createObjectURL(blob);
    a.download = name;
    a.click();
    setTimeout(() => URL.revokeObjectURL(a.href), 1000);
  }

  // Tabs, buffers and the picked set lose paths that actually went.
  _forgetPaths(gone) {
    const set = gone instanceof Set ? gone : new Set(gone || []);
    const dead = (p) => {
      for (const g of set) if (p === g || p.startsWith(`${g}/`)) return true;
      return false;
    };
    for (const p of this.tabs.openPaths()) {
      if (!set.has(p)) continue;
      this.tabs.closeEverywhere(p);
      this.buffers.delete(p);
      this.search.invalidate(p);
      for (const v of this.views.values()) v.saved.delete(p);
    }
    for (const p of [...this.tree.picked]) if (dead(p)) this.tree.picked.delete(p);
    if (this.tree.anchor && dead(this.tree.anchor)) this.tree.anchor = null;
    if (this.tree.sel && dead(this.tree.sel)) this.tree.sel = null;
    this._layout();
    this._showAll();
    this._saveTabs();
  }

  // One ordered delete. Flash paths of a short list go in one POST; a 404 or
  // 413 (this image, or a list that does not fit) falls back to one DELETE
  // per path, still as this job. A path under sd/ stays sequential.
  async _removePaths(paths, job) {
    const boardId = job.boardId || 0;
    const body = JSON.stringify({ paths, id: boardId });
    const tooBig = paths.length > 32 || body.length > 4096;
    const flashOnly = paths.every((p) => p !== 'sd' && !p.startsWith('sd/'));
    const gone = new Set();
    if (!tooBig && flashOnly && paths.length) {
      job.set(`Deleting ${paths.length} paths`, 0);
      const r = await fs.fetchBody(fs.removePostUrl(), { method: 'POST', body, signal: job.signal });
      if (r.aborted) {
        job.aborted();
        return { ok: false, aborted: true, error: 'aborted', gone };
      }
      if (r.ok) {
        let data = {};
        try { data = JSON.parse(new TextDecoder().decode(r.bytes)); } catch { /* not JSON */ }
        if (data.ok === false) {
          const n = Number(data.removed) || 0;
          for (const p of paths.slice(0, n)) gone.add(p);
          job.fail(jobError(data.error || 'delete failed'));
          return { ok: false, error: data.error || 'delete failed', gone };
        }
        for (const p of paths) gone.add(p);
        job.done(`Deleted ${paths.length}`);
        return { ok: true, gone };
      }
      if (r.status !== 404 && r.status !== 413) {
        job.fail(jobError(r.error));
        return { ok: false, error: r.error, gone };
      }
    }
    for (let i = 0; i < paths.length; i++) {
      if (job.signal.aborted) {
        job.aborted();
        return { ok: false, aborted: true, error: 'aborted', gone };
      }
      job.set(`Deleting ${paths[i]} · ${i + 1} of ${paths.length}`, paths.length ? i / paths.length : 0);
      const r = await fs.remove(paths[i]);
      if (job.signal.aborted) {
        if (r.ok || r.status === 404) gone.add(paths[i]);
        job.aborted();
        return { ok: false, aborted: true, error: 'aborted', gone };
      }
      if (!r.ok && r.status !== 404) {
        const error = `${paths[i]}: ${jobError(r.error)}`;
        job.fail(error);
        return { ok: false, error, gone };
      }
      gone.add(paths[i]);
    }
    job.done(`Deleted ${paths.length}`);
    return { ok: true, gone };
  }

  async _expandDelete(roots) {
    const isDir = (p) => !!this.tree.find(p)?.dir;
    const pruned = pruneCovered(roots, isDir);
    const files = [];
    const dirs = [];
    const seenF = new Set();
    const seenD = new Set();
    const vitals = [];
    const addVital = (p) => { if (isVital(p) && !vitals.includes(p)) vitals.push(p); };
    for (const p of pruned) {
      if (isDir(p)) {
        const t = await treeOf(fs.list, p);
        for (const f of t.files) if (!seenF.has(f.path)) { seenF.add(f.path); files.push(f); }
        for (const d of t.dirs) {
          if (seenD.has(d)) continue;
          seenD.add(d);
          dirs.push(d);
          addVital(d);
        }
      } else if (!seenF.has(p)) {
        seenF.add(p);
        files.push({ path: p, size: this.tree.find(p)?.size || 0 });
      }
    }
    dirs.sort((a, b) => b.split('/').length - a.split('/').length);
    return { roots: pruned, files, dirs, vitals };
  }

  async deletePicked() {
    const roots = [...this.tree.picked];
    if (roots.length < 2) return false;
    let exp;
    try { exp = await this._expandDelete(roots); }
    catch (e) { this._say(`Could not list the selection: ${e.message}`, 'err'); return false; }
    if (!exp.files.length && !exp.dirs.length) return false;
    if (!(await this.confirmDelete.askList(exp))) { this._say('Not deleted'); return false; }
    const ordered = [...exp.files.map((f) => f.path), ...exp.dirs];
    const job = this._removeJob(`Deleting ${exp.roots.length} items`);
    const removed = await this._removePaths(ordered, job);
    this._forgetPaths(removed.gone);
    if (removed.aborted) this._say('Delete aborted', 'warn');
    else if (!removed.ok) this._say(`Delete stopped at ${removed.error}`, 'err');
    else this._say(`Deleted ${exp.roots.length} items`);
    await this.tree.refresh();
    for (const p of [...this.tree.picked]) if (!this.tree.find(p)) this.tree.picked.delete(p);
    this.tree.invalidate();
    this._renderTitle();
    return !!removed.ok;
  }

  // Any file, open or not (the file menu); the open one by default.
  async renameFile(path) {
    if (!path || path === PEX_PART) return;
    const to = prompt('Rename or move to:', path)?.trim();
    if (!to || to === path) return;
    return this.place(path, to, { move: true });
  }

  async copyTo(path) {
    if (!path || path === PEX_PART) return;
    const to = prompt('Copy to:', path)?.trim();
    if (!to || to === path) return;
    return this.place(path, to, { move: false });
  }

  // Free bytes on the destination, or an error that names which volume
  // could not be read. LittleFS has no HTTP usage field; file.media does.
  async _volumeFree(vol) {
    if (vol === 'sd') {
      if (!fs.sdAvailable()) return { free: null, error: 'Could not read SD free space, so the copy was not started.' };
      const s = await fs.sdStatus();
      const free = sdFreeBytes(s);
      if (free == null) return { free: null, error: 'Could not read SD free space, so the copy was not started.' };
      return { free };
    }
    let r;
    try {
      r = await this.ws.call('script.eval', { chunk: lfsFreeChunk() });
    } catch {
      return { free: null, error: 'Could not read LittleFS free space, so the copy was not started.' };
    }
    const free = r && r.ok !== false ? parseLfsFree(r.value) : null;
    if (free == null) return { free: null, error: 'Could not read LittleFS free space, so the copy was not started.' };
    return { free };
  }

  async _destHit(dest) {
    const name = dest.split('/').pop();
    const parent = dest.split('/').slice(0, -1).join('/');
    const r = await fs.list(parent);
    if (!r.ok) {
      if (r.status === 404) return { hit: null };
      return { error: r.error || `could not list ${parent || '/'}` };
    }
    const f = (r.files || []).find((x) => x.name === name);
    if (!f) return { hit: null };
    return { hit: { dir: !!f.dir, size: f.size || 0, alloc: f.alloc || 0 } };
  }

  // Open tabs follow a move, including files inside a moved folder.
  _followMove(src, dest) {
    for (const p of [...this.tabs.openPaths()]) {
      if (p === src) this._renamed(p, dest);
      else if (p.startsWith(`${src}/`)) this._renamed(p, dest + p.slice(src.length));
    }
  }

  // Copy, or move. A move on the same volume is a rename. Anything that
  // crosses between the card and internal storage is read here and written
  // there, and the source is removed only after that copy finishes. Space
  // is checked first. A failed write leaves whatever it already created.
  async place(src, dest, { move = false } = {}) {
    if (!src || src === PEX_PART) return;
    const node = this.tree.find(src);
    if (!node) { this._say(`Not in the list: ${src}`, 'warn'); return; }
    const verb = move ? 'Move' : 'Copy';
    let measured;
    try {
      measured = await measure((p) => fs.list(p), src, { dir: !!node.dir, size: node.size || 0, alloc: node.alloc || 0 });
    } catch (e) {
      this._say(`${verb} failed: ${e.message}`, 'err');
      return;
    }
    const looked = await this._destHit(dest);
    if (looked.error) { this._say(`${verb} failed: ${looked.error}`, 'err'); return; }
    const args = { src, dest, move, dir: !!node.dir, measured, destHit: looked.hit };
    let plan = planTransfer({ ...args, free: null });
    if (!plan.ok && plan.code === 'free') {
      const got = await this._volumeFree(plan.destVol);
      if (got.free == null) { this._say(got.error, 'err'); return; }
      plan = planTransfer({ ...args, free: got.free });
    }
    if (!plan.ok && plan.code === 'space') {
      const where = plan.destVol === 'sd' ? 'the SD card' : 'internal storage';
      this._say(`Not enough space on ${where}: ${bytes(plan.need)} needed, ${bytes(plan.free)} free.`, 'err');
      return;
    }
    if (!plan.ok) { this._say(plan.error, 'err'); return; }
    if (plan.replace && !confirm(`Replace ${dest}? A failed write removes the file that is there now.`)) {
      this._say(move ? 'Not moved.' : 'Not copied.');
      return;
    }
    const job = this._job(`${move ? 'Moving' : 'Copying'} ${src.split('/').pop()}`);
    job.set(`${move ? 'Moving' : 'Copying'} ${src} to ${dest}`);
    let result;
    try {
      result = await runTransfer(plan, {
        signal: job.signal,
        read: (path, opts) => fs.readFileStream(path, opts),
        write: (path, data) => fs.write(path, new Blob([data])),
        mkdir: (path) => fs.mkdir(path),
        remove: (path) => fs.remove(path),
        rename: (from, to) => fs.rename(from, to),
        onProgress: ({ done, total, path, got }) => {
          const at = (done || 0) + (got || 0);
          const whole = total || at;
          job.set(`Copying ${String(path || dest).split('/').pop()} · ${bytes(at)} of ${bytes(whole)}`, whole ? at / whole : 0);
        },
      });
    } catch (e) {
      job.fail(jobError(e.message || String(e)));
      this._say(`${verb} failed: ${jobError(e.message || e)}`, 'err');
      return;
    }
    if (result.aborted) {
      job.aborted();
      const where = result.copied
        ? `Move aborted while removing ${src}. ${dest} has the copy.`
        : result.partial
          ? `${verb} aborted. ${dest} has the files copied so far. ${src} was left in place.`
          : `${verb} aborted.`;
      this._say(where, 'warn');
    } else if (!result.ok) {
      job.fail(jobError(result.error));
      if (result.copied) this._say(`Copied to ${dest}, but removing ${src} stopped: ${result.error}`, 'err');
      else if (result.partial) this._say(`${verb} stopped at ${result.error}. ${dest} has the files copied so far. ${src} was left in place.`, 'err');
      else this._say(`${verb} failed: ${result.error}`, 'err');
    } else {
      job.done(move ? `Moved to ${dest}` : `Copied to ${dest}`);
      if (move) this._followMove(src, dest);
      this._say(move ? `Moved ${src} to ${dest}` : `Copied ${src} to ${dest}`);
    }
    this.search.invalidate();
    await this.tree.refresh();
    if (result?.ok) this.tree.reveal(dest);
    this._renderTitle();
    this._poll();
  }

  async deleteFile(path) {
    if (!path || path === PEX_PART) return;
    if (!confirm(`Delete ${path} from the device? This cannot be undone.`)) return;
    const job = this._job(`Deleting ${path.split('/').pop()}`);
    job.set(`Deleting ${path}`);
    const r = await fs.remove(path);
    if (!r.ok) { job.fail(jobError(r.error)); this._say(`Delete failed: ${jobError(r.error)}`, 'err'); return; }
    job.done(`Deleted ${path}`);
    this.tree.picked.delete(path);
    if (this.tree.anchor === path) this.tree.anchor = null;
    this.buffers.delete(path);
    this.search.invalidate(path);
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
    const ordered = [...t.files.map((f) => f.path), ...t.dirs];
    const job = this._removeJob(`Deleting ${path}/`);
    const removed = await this._removePaths(ordered, job);
    this._forgetPaths(removed.gone);
    if (removed.aborted) this._say('Delete aborted', 'warn');
    else if (!removed.ok) this._say(`Delete stopped at ${removed.error}`, 'err');
    else this._say(empty ? `Deleted ${path}/` : `Deleted ${path}/ and everything in it`);
    await this.tree.refresh();
    this._renderTitle();
    return !!removed.ok;
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
    this.search.invalidate();
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
    let done = 0, wrote = 0, failed = null;
    const job = (uploads.length || plan.dirs.length) ? this._job('Uploading') : null;
    const steps = plan.dirs.length + uploads.length || 1;
    for (const d of plan.dirs) {
      if (job?.signal.aborted) { job.aborted(); failed = 'aborted'; break; }
      job?.set(`Uploading ${d}/ · ${done} of ${steps}`, done / steps);
      const r = await fs.mkdir(d);
      if (!r.ok) { failed = `${d}/: ${jobError(r.error)}`; break; }
      done++;
    }
    for (const u of failed ? [] : uploads) {
      if (job?.signal.aborted) { job.aborted(); failed = 'aborted'; break; }
      this._say(`Uploading ${wrote + 1} of ${uploads.length}: ${u.dest}`);
      job?.set(`Uploading ${u.dest.split('/').pop()} · ${done + 1} of ${steps}`, done / steps);
      const r = await fs.write(u.dest, u.file);
      if (!r.ok) { failed = `${u.dest}: ${jobError(r.error)}`; break; }
      wrote++;
      done++;
    }
    if (job && !job.signal.aborted && failed !== 'aborted') {
      if (failed) job.fail(jobError(failed));
      else job.done(`Uploaded ${uploads.length} file${uploads.length === 1 ? '' : 's'}`);
    }
    await this.tree.refresh();
    if (dir) this.tree.reveal(dir);
    // a file open in a tab, just replaced: show what is on the device now
    for (const u of uploads.slice(0, wrote)) {
      const b = this.buffers.get(u.dest);
      if (b && !this._isDirty(b)) { this.buffers.delete(u.dest); if (this.tabs.showing(u.dest)) await this._reloadShown(u.dest); }
    }
    const where = dir ? `${dir}/` : 'the root';
    const parts = [];
    if (uploads.length || plan.dirs.length) parts.push(`Uploaded ${wrote} of ${uploads.length} file${uploads.length === 1 ? '' : 's'} to ${where}`);
    if (plan.skipped.length) parts.push(`skipped ${plan.skipped.length} (${plan.skipped.slice(0, 3).map((x) => `${x.path}: ${x.why}`).join('; ')})`);
    if (failed === 'aborted') this._say(parts.join(', ') || 'Upload stopped.', 'warn');
    else if (failed) this._say(`${parts.join(', ')} — stopped at ${failed}`, 'err');
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
  // The same path as Rename / move, including a drop onto the other volume.
  async moveTo(src, folder) {
    const dest = moveDest(src, folder);
    if (!dest) return;
    return this.place(src, dest, { move: true });
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
    this._renderTitle();
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
    let builds = null;
    if (!node.dir && node.name === 'Pexfile') {
      const { loadProject } = await import('../editor/pexproj.js');
      const spec = await loadProject({
        filePath: path,
        readText: (p) => this._readProjectText(p),
        arch: this.pexArch,
        board: this._boardName(),
        pin: this._projPin(path),
      });
      if (spec.project && !spec.errors.length) {
        builds = projectBuildItems(spec.project, { arch: this.pexArch, board: this._boardName() });
      }
    }
    const items = fileMenuItems(node, {
      card: node.dir && path === fs.SD_ROOT && fs.sdAvailable(),
      isOpen: this.path === path,
      script: scriptNameOf(path),
      pex: pexNameOf(path),
      installed: this.pexInstalled.has(pexNameOf(path)),
      empty: node.dir && node.children?.length === 0,
      hasBak: siblings.includes(`${node.name}.bak`),
      builds,
      picked: this.tree.picked.size,
      inPicked: this.tree.picked.has(path),
      selected: this.tree.picked.has(path),
    }).map((it) => ({ label: it.label, keep: it.keep, cls: it.danger ? 'sm-danger' : '', fn: () => this._fileAct(it.id, node, it) }));
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
      menu?.setInfo(sizeInfo(t, node.path));
    } catch (e) {
      menu?.setInfo(`Could not count: ${e.message}`);
    }
  }

  _fileAct(id, node, item) {
    const p = node.path;
    switch (id) {
      case 'pex-build': return this.build({ from: p, target: item?.target, pin: true });
      case 'pex-brun': return this.build({ from: p, target: item?.target, run: true, pin: true });
      case 'pex-build-all': return this.build({ from: p, all: true });
      case 'open': return this.open(p);
      case 'start': return this.scriptOp('script.start', p, (n) => `Started ${n}`);
      case 'stop': return this.scriptOp('script.stop', p, (n) => `Stopped ${n}`);
      case 'pex-start': return this._pexStart(pexNameOf(p));
      case 'pex-stop': return this._pexStop(pexNameOf(p));
      case 'pex-install': return pexNameOf(p) ? this._pexInstall(pexNameOf(p)) : this._pexInstallFrom(p);
      case 'pex-uninstall': return this._pexUninstall(pexNameOf(p));
      case 'pex-mark': return this._pexMark(p);
      case 'pex-restore': return this._pexRestore(p);
      case 'pex-partition':
        this._selectPex(pexNameOf(p) || pexFileName(p));
        return this.open(PEX_PART);
      case 'commit': return this.scriptOp('script.commit', p, (n) => `Committed ${n} to lua/${n}.lua`);
      case 'revert': return this.scriptOp('script.revert', p, (n) => `Reverted ${n}: the next tier down runs now`);
      case 'download': return this.download(p);
      case 'download-ws': return this.downloadWs(p);
      case 'copy': return this.copyTo(p);
      case 'rename': return this.renameFile(p);
      case 'select': this.tree.applyPick('toggle', p); this.tree.render(); return undefined;
      case 'tar-set': return this.downloadArchive([...this.tree.picked]);
      case 'delete':
        if (this.tree.picked.size > 1 && this.tree.picked.has(p)) return this.deletePicked();
        return node.dir ? this.deleteFolder(p) : this.deleteFile(p);
      case 'size': return this.sizeFolder(node);
      case 'new': return this.newFile(p);
      case 'mkdir': return this.newFolder(p);
      case 'sd-settings': return this.app.settingsModal?.open('sect-sd');
      case 'upload': this._uploadVia = ''; this._uploadDir = p; return this._upload.click();
      case 'upload-ws': this._uploadVia = 'WebSocket'; this._uploadDir = p; return this._upload.click();
      case 'refresh': return this.tree.refresh(node).then(() => this.tree.invalidate());
      case 'tar': return this.downloadTar(p);
      case 'untar': this._tarTarget = p; return this._tarInput.click();
      case 'compress': return this.convert(p, true);
      case 'decompress': return this.convert(p, false);
      case 'restore-bak': return this.restoreBak(p);
      default: return undefined;
    }
  }

  // A folder ('' = everything) as a .tar. The body is fetched so the ring
  // moves and Abort stops it. id= is for a newer image's fs.job notes; this
  // image ignores the query and the line is byte progress.
  async downloadTar(folder = '') {
    const name = `${folder ? folder.split('/').pop() : 'btgw'}.tar`;
    const label = folder ? `${folder}/` : 'the whole filesystem';
    const url = fs.tarUrl(folder, 0);
    if (!url) { this._say('The device streams a tar from flash only', 'warn'); return; }
    const watch = this._tarWatch(`Downloading ${name}`, name);
    const urlId = fs.tarUrl(folder, watch.boardId);
    const r = await fs.fetchBody(urlId || url, {
      signal: watch.job.signal,
      onProgress: ({ got, total }) => {
        watch.st.got = got;
        watch.st.total = total;
        watch.st.overallGot = got;
        if (!watch.st.overall) watch.st.overall = total;
        this._paintTar(watch.job, watch.st, name);
      },
    });
    if (r.aborted) { watch.job.aborted(); this._say('Download aborted', 'warn'); return; }
    if (!r.ok) { watch.job.fail(jobError(r.error)); this._say(`Download failed: ${jobError(r.error)}`, 'err'); return; }
    this._saveBlob(new Blob([r.bytes], { type: 'application/x-tar' }), name);
    watch.job.done(`Downloaded ${name}`);
    this._say(`Downloaded ${label} as a .tar`);
  }

  _tarWatch(title, label = title) {
    const boardId = ++this._boardSeq;
    const st = {
      got: 0, total: 0, path: '', index: 0, files: 0, size: 0, fileAt: 0,
      overall: 0, overallGot: 0, prefix: '',
    };
    const job = this._job(title, {
      boardId,
      onBoard: (d) => {
        if (!d || (d.kind && d.kind !== 'tar')) return;
        if (d.path && d.path !== st.path) {
          st.fileAt = st.got;
          st.path = d.path;
          st.size = Number(d.size) || 0;
        }
        if (d.index) st.index = d.index;
        if (d.total) st.files = d.total;
        this._paintTar(job, st, label);
      },
    });
    return { job, boardId, st };
  }

  _paintTar(job, st, label) {
    const frac = st.overall > 0 ? st.overallGot / st.overall : byteFrac(st.got, st.total);
    let line;
    if (st.path) {
      const into = Math.max(0, st.got - st.fileAt);
      line = fileLine(st.path, { index: st.index, total: st.files, got: into, size: st.size });
      if (st.prefix) line = `${st.prefix} · ${line}`;
    } else {
      line = byteLine(st.prefix || 'Downloading', label, st.got, st.total);
    }
    job.set(line, frac);
  }

  async _listPacked(paths, job) {
    const dirs = [];
    const files = [];
    const seenD = new Set();
    const seenF = new Set();
    for (const p of paths) {
      if (job.signal.aborted) { job.aborted(); return null; }
      const node = this.tree.find(p);
      if (node?.dir) {
        let t;
        try { t = await treeOf(fs.list, p); }
        catch (e) { const msg = jobError(e.message); job.fail(msg); this._say(`Could not list ${p}: ${msg}`, 'err'); return null; }
        for (const d of t.dirs) if (!seenD.has(d)) { seenD.add(d); dirs.push(d); }
        for (const f of t.files) if (!seenF.has(f.path)) { seenF.add(f.path); files.push(f); }
      } else if (!seenF.has(p)) {
        seenF.add(p);
        files.push({ path: p, size: node?.size || 0 });
      }
    }
    dirs.sort((a, b) => a.split('/').length - b.split('/').length);
    return { dirs, files };
  }

  async _readPacked(spec, job, { flashBytes = 0, cardBytes = 0, prefix = 'Card' } = {}) {
    const entries = spec.dirs.map((d) => ({ name: d, dir: true }));
    let doneBytes = 0;
    const overall = flashBytes + cardBytes;
    for (let i = 0; i < spec.files.length; i++) {
      if (job.signal.aborted) { job.aborted(); return null; }
      const f = spec.files[i];
      const size = f.size || 0;
      const paint = (got) => {
        const line = `${prefix} · ${fileLine(f.path, { index: i + 1, total: spec.files.length, got, size: size || got })}`;
        const soFar = flashBytes + doneBytes + got;
        job.set(line, overall > 0 ? soFar / overall : (i + (size ? got / size : 0)) / (spec.files.length || 1));
      };
      paint(0);
      const r = await fs.readFileStream(f.path, {
        signal: job.signal,
        onProgress: ({ got }) => paint(got),
      });
      if (r.aborted) { job.aborted(); return null; }
      if (!r.ok) { job.fail(jobError(r.error)); this._say(`Could not read ${f.path}: ${jobError(r.error)}`, 'err'); return null; }
      entries.push({ name: f.path, data: r.bytes });
      doneBytes += r.size;
      paint(r.size);
    }
    return entries;
  }

  async downloadArchive(paths) {
    const isDir = (p) => !!this.tree.find(p)?.dir;
    const plan = archivePlan(paths, isDir);
    if (plan.kind === 'empty') return;
    if (plan.kind === 'get') return this.downloadTar(plan.folder);
    const name = 'files.tar';
    const watch = this._tarWatch(`Downloading ${name}`, name);
    const { job, boardId, st } = watch;
    const mtime = Math.floor(Date.now() / 1000);
    let card = null;
    if (plan.card) {
      job.set('Listing the card', 0);
      card = await this._listPacked(plan.card, job);
      if (!card) return;
    }
    const cardBytes = card ? card.files.reduce((n, f) => n + (f.size || 0), 0) : 0;
    const take = async (url, opts) => fs.fetchBody(url, {
      ...opts,
      signal: job.signal,
      onProgress: ({ got, total }) => {
        st.got = got;
        st.total = total;
        st.overall = (total || 0) + cardBytes;
        st.overallGot = got;
        this._paintTar(job, st, name);
      },
    });
    let bytes = null;
    if (plan.kind === 'card') {
      const entries = await this._readPacked(card, job, { cardBytes, prefix: 'Card' });
      if (!entries) return;
      try { bytes = encodeTar(entries, mtime); }
      catch (e) { const msg = jobError(e.message); job.fail(msg); this._say(msg, 'err'); return; }
    } else if (plan.kind === 'mix-get') {
      st.prefix = 'Archive';
      const url = fs.tarUrl(plan.folder, boardId);
      if (!url) { job.fail('The device streams a tar from flash only'); return; }
      const r = await take(url);
      if (r.aborted) { job.aborted(); this._say('Download aborted', 'warn'); return; }
      if (!r.ok) { job.fail(jobError(r.error)); this._say(`Download failed: ${jobError(r.error)}`, 'err'); return; }
      st.prefix = '';
      const entries = await this._readPacked(card, job, { flashBytes: r.size, cardBytes, prefix: 'Card' });
      if (!entries) return;
      try { bytes = appendTar(r.bytes, entries, mtime); }
      catch (e) { const msg = jobError(e.message); job.fail(msg); this._say(msg, 'err'); return; }
    } else {
      st.prefix = 'Archive';
      job.set('Packing the archive', 0);
      const body = JSON.stringify({ paths: plan.flash, mtime, id: boardId });
      const r = await take(fs.tarPostUrl(), { method: 'POST', body });
      if (r.aborted) { job.aborted(); this._say('Download aborted', 'warn'); return; }
      if (!r.ok && r.status === 404) {
        job.fail('The board cannot pack a chosen list yet.');
        this._say('The board cannot pack a chosen list yet.', 'warn');
        return;
      }
      if (!r.ok) { job.fail(jobError(r.error)); this._say(`Download failed: ${jobError(r.error)}`, 'err'); return; }
      bytes = r.bytes;
      if (plan.kind === 'mix-post') {
        st.prefix = '';
        const entries = await this._readPacked(card, job, { flashBytes: r.size, cardBytes, prefix: 'Card' });
        if (!entries) return;
        try { bytes = appendTar(bytes, entries, mtime); }
        catch (e) { const msg = jobError(e.message); job.fail(msg); this._say(msg, 'err'); return; }
      }
    }
    this._saveBlob(new Blob([bytes], { type: 'application/x-tar' }), name);
    job.done(`Downloaded ${name}`);
    this._say('Downloaded files.tar');
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
    if (!r.ok) { this._say(`Could not read ${path}: ${jobError(r.error)}`, 'err'); return false; }
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
      if (!r.ok) { this._say(`Restore stopped at ${from} → ${to}: ${jobError(r.error)}`, 'err'); await this.tree.refresh(); return false; }
    }
    this.buffers.delete(path);
    await this.tree.refresh();
    if (this.path === path) await this.open(path);
    this._say(`Restored ${path} from its .bak (reload the page to use a restored web file)`);
    return true;
  }

  // A folder, made at once (an empty one too: PUT with a trailing slash, on
  // flash and on the SD card alike).
  async newFolder(dir = this.tree.currentDir) {
    const name = prompt('New folder (path on the device):', dir ? `${dir}/` : '')?.trim().replace(/\/+$/, '');
    if (!name || name === dir) return;
    const why = badPath(name);
    if (why) { this._say(`Not a folder name: ${why}`, 'warn'); return; }
    const r = await fs.mkdir(name);
    if (!r.ok) { this._say(`Could not make ${name}: ${jobError(r.error)}`, 'err'); return; }
    this._say(`Made ${name}/`);
    await this.tree.refresh();
    this.tree.invalidate();
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
    const list = [...files];
    const via = this._uploadVia || 'HTTP';
    this._uploadVia = '';
    this._upload.value = '';
    if (!list.length) return;
    if (via === 'WebSocket') return this.uploadWs(list, dir);
    const job = this._job('Uploading');
    let wrote = 0;
    for (let i = 0; i < list.length; i++) {
      if (job.signal.aborted) { job.aborted(); break; }
      const f = list[i];
      const dest = dir ? `${dir}/${f.name}` : f.name;
      job.set(`Uploading ${f.name} · HTTP · ${i + 1} of ${list.length}`, list.length ? i / list.length : 0);
      const r = await fs.write(dest, f);
      if (job.signal.aborted) { job.aborted(); break; }
      if (!r.ok) {
        job.fail(jobError(r.error));
        this._say(`Upload of ${f.name} failed: ${jobError(r.error)}`, 'err');
        wrote = -1;
        break;
      }
      wrote++;
      this._say(`Uploaded ${dest}`);
    }
    if (wrote === list.length) job.done(`Uploaded ${list.length}`);
    this.tree.refresh();
  }

  async download(path = this.path) {
    if (!path || path === PEX_PART) return;
    const b = this.buffers.get(path);
    const name = (isGz(path) ? plainName(path) : path).split('/').pop();
    // The buffer if it is text (what you see is what you get, saved or not).
    if (b && !b.binary) {
      const job = this._job(`Downloading ${name}`);
      const text = path === this.path ? this.core.value : b.text;
      job.set(byteLine('Downloading', name, text.length, text.length, 'HTTP'));
      this._saveBlob(new Blob([text], { type: 'text/plain' }), name);
      job.done(`Downloaded ${name}`);
      return;
    }
    const job = this._job(`Downloading ${name}`);
    const r = await fs.readFileStream(path, {
      signal: job.signal,
      onProgress: ({ got, total }) => job.set(byteLine('Downloading', name, got, total, 'HTTP'), byteFrac(got, total)),
    });
    if (r.aborted) { job.aborted(); return; }
    if (!r.ok) { job.fail(jobError(r.error)); this._say(`Download failed: ${jobError(r.error)}`, 'err'); return; }
    this._saveBlob(new Blob([r.bytes]), name);
    job.done(`Downloaded ${name}`);
  }

  // The socket this editor already holds. A page opened on one board does
  // not put `node` on the frame: that socket is the board. A caller that
  // shares one socket across boards passes the other board's mDNS name.
  async downloadWs(path = this.path, node) {
    if (!path || path === PEX_PART) return;
    const name = (isGz(path) ? plainName(path) : path).split('/').pop();
    const job = this._job(`Downloading ${name}`);
    const r = await readChunks((op, params, n) => this.ws.call(op, params, n), path, {
      signal: job.signal,
      node,
      onProgress: ({ got, total }) => job.set(byteLine('Downloading', name, got, total, 'WebSocket'), byteFrac(got, total)),
    });
    if (r.aborted) { job.aborted(); return; }
    if (!r.ok) { job.fail(jobError(r.error)); this._say(`Download failed: ${jobError(r.error)}`, 'err'); return; }
    this._saveBlob(new Blob([r.bytes]), name);
    job.done(`Downloaded ${name}`);
  }

  async uploadWs(files, dir = this.tree.currentDir, node) {
    const list = [...files];
    const job = this._job('Uploading');
    let wrote = 0;
    for (let i = 0; i < list.length; i++) {
      if (job.signal.aborted) { job.aborted(); break; }
      const f = list[i];
      const dest = dir ? `${dir}/${f.name}` : f.name;
      const buf = new Uint8Array(await f.arrayBuffer());
      const r = await writeChunks((op, params, n) => this.ws.call(op, params, n), dest, buf, {
        signal: job.signal,
        node,
        onProgress: ({ got, total }) => job.set(byteLine('Uploading', f.name, got, total, 'WebSocket'), byteFrac(got, total)),
      });
      if (r.aborted) { job.aborted(); break; }
      if (!r.ok) {
        job.fail(jobError(r.error));
        this._say(`Upload of ${f.name} failed: ${jobError(r.error)}`, 'err');
        wrote = -1;
        break;
      }
      wrote++;
      this._say(`Uploaded ${dest}`);
    }
    if (wrote === list.length) job.done(`Uploaded ${list.length}`);
    this.tree.refresh();
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
    const part = this.path === PEX_PART;
    this._name.textContent = part ? 'PEX partition' : (this.path || 'No file open');
    const m = this.path && !part ? this._mark(this.path) : {};
    const pex = this._activePex();
    // A selected .pex is not the open buffer (a click opens its menu). The
    // status line still names that app, beside whatever the open file says.
    const beside = pex && pex !== pexNameOf(this.path)
      ? this.pexStates[`pex/${pex}.${this.pexArch}.pex`] : null;
    const bits = [];
    if (m.dirty) bits.push('● modified');
    if (m.arena === 'loaded') bits.push('loaded');
    if (m.arena === 'stale') bits.push('loaded — changed since');
    const app = m.pex || beside;
    if (app === 'running') bits.push('running');
    else if (app === 'faulted') bits.push('faulted');
    else if (app) bits.push(app);
    this._state.textContent = bits.length ? `  ${bits.join(' · ')}` : '';
    const pexClass = app === 'running' ? ' ed-loaded' : app === 'faulted' ? ' ed-stale' : app ? ' ed-idle' : '';
    const runState = m.arena ? ' ed-' + m.arena : pexClass;
    this._state.className = `ed-state${runState}${m.dirty ? ' is-dirty' : ''}`;
    if (this._runBtn) {
      this._runBtn.title = pex
        ? `Start the ${pex} PEX app (F5)`
        : 'Run as a draft — nothing is written to flash (F5)';
      this._stopBtn.title = pex
        ? `Stop the ${pex} PEX app (Shift+F5)`
        : 'Stop the script (Shift+F5)';
    }
    this._scheduleProject();
    this._applyBuildButton();
    this._sPath.textContent = part ? 'PEX partition' : (this.path || '');
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

  _applyBuildButton() {
    if (!this._buildBtn) return;
    const spec = this._proj;
    const here = spec && spec.filePath === this.path && spec.ready;
    let st;
    if (!this.path || this.path === PEX_PART) st = buildButtonState(null, null);
    else if (!here) st = { disabled: true, label: 'Build', title: 'Looking for a Pexfile…' };
    else if (!spec.project) {
      st = spec.errors?.length
        ? { disabled: true, label: 'Build', title: spec.errors[0] }
        : buildButtonState(null, null);
    } else st = buildButtonState(spec.project, spec.selection, { boardKnown: !!this._boardName() });
    this._buildBtn.disabled = st.disabled;
    this._brunBtn.disabled = st.disabled;
    this._buildLabel.textContent = st.label;
    this._buildBtn.title = st.title;
    this._buildBtn.setAttribute('aria-label', st.label);
    const runTitle = st.disabled ? st.title : st.title.replace(/^Build /, 'Build and run ');
    this._brunBtn.title = runTitle;
    this._brunBtn.setAttribute('aria-label', runTitle);
  }

  render() {
    for (const v of this.views.values()) v.core.render();
    this.tree.render();
    this._paintPartition();
  }
}
