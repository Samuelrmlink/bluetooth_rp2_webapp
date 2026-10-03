// The Bluetooth gateway's web app: one page, every UI a window.
//
// The window manager (tiles/layout/keys/nav/dropdown/popover and the Settings
// and Help shells) is the km003c web app's, copied in and cut at its seams;
// this file is the glue, following the four patterns that app established:
//
//   1. panes are registered as {id, title, group, short, el} and never
//      destroyed -- their elements move between tiles and #pane-park;
//   2. getContext()/getNavRoots() tell the key layer which bindings apply and
//      where plain hjkl navigation roams;
//   3. Settings exists before its sections, which render into it;
//   4. one rAF loop draws only what is visible: windows in a tile, and
//      Settings sections that are open.
//
// A window is a small class: constructor(el, ctx), and optionally render(),
// onShow(), onHide(), dispose(). ctx carries the WS client, a bus scope the
// window's subscriptions go through, and the app for cross-window requests
// ("open this file in the editor"). Built-in windows are listed in
// windows/index.js; device windows come and go with devices (devices/host.js).

import { WsApi } from './wsapi.js';
import { Bus } from './bus.js';
import { Layout } from './tiles.js';
import { LayoutManager } from './layout.js';
import { Keys } from './keys.js';
import { Popover } from './popover.js';
import { SettingsModal } from './modals/settings.js';
import { HelpModal } from './modals/help.js';
import { readLayoutStore, writeLayoutStore, readPrefs, writePrefs } from './prefs.js';
import { BUILTIN_WINDOWS, DEFAULT_TREE } from './windows/index.js';
import { SETTINGS_SECTIONS } from './settings/index.js';
import { GattLog } from './gattlog.js';
import { DeviceStore } from './gattdb.js';
import { DeviceHost } from './devices/host.js';
import { PairingPrompt } from './pairing.js';

const NUDGE_PX = 24;
const RESIZE_PX = 32;

// Same ?host=/?port= overrides as fs.js, for a page served from a dev
// machine while the device is elsewhere.
function wsUrl() {
  const q = new URLSearchParams(location.search);
  if (q.get('ws')) return q.get('ws');
  const host = q.get('host') || location.hostname;
  const port = q.get('port') || location.port;
  return `ws://${host}${port ? ':' + port : ''}/ws`;
}

class App {
  constructor() {
    this.prefs = readPrefs();
    this.ws = new WsApi(wsUrl());
    this.bus = new Bus(this.ws);
    // One mirror of the devices the gateway knows, shared by every window
    // that shows them, so the Scanner and GATT can never disagree.
    this.devices = new DeviceStore().bind(this.ws);
    this._fitViewport();
    // Every GATT value and operation result, for every window that shows them
    // (GATT, GATT Events, and the events split inside GATT in tabbed view).
    this.gattLog = new GattLog(500);
    const gl = this.bus.owner();
    gl.on('char.value', (d) => this.gattLog.add({ addr: d.addr, id: d.id, uuid: d.uuid, src: d.source || 'notify', hex: d.value || '' }));
    gl.on('gatt.op_failed', (d) => this.gattLog.add({ addr: d.addr, src: 'error', note: `${d.op} failed, ATT error 0x${Number(d.att_error).toString(16).padStart(2, '0')} (characteristic #${d.chr})` }));
    this.windows = new Map();       // pane id -> window instance
    this._visible = new Set();      // pane ids currently in a tile or float

    this._buildWindows();
    this.settingsModal = new SettingsModal(this);
    // Passkey / comparison prompts whenever pairing needs a person (pairing.js).
    this.pairing = new PairingPrompt(this);
    // Every Settings-shaped dialog (js/modals/dialog.js). Windows are built
    // first and may already have registered theirs.
    this.dialogs = [this.settingsModal, ...(this.dialogs || [])];
    this.sections = SETTINGS_SECTIONS.map((S) => new S(this, this.settingsModal));
    this.helpModal = new HelpModal();
    this.popover = new Popover();

    this._initLayout();
    this._initKeys();
    // After the layout exists: devices register windows into it as they
    // appear, and a saved arrangement may already have a place for them.
    this.deviceHost = new DeviceHost(this);
    this._wireToolbar();
    this._wireConnection();

    this.ws.connect();
    requestAnimationFrame((t) => this._frame(t));
  }

  // --- windows ---------------------------------------------------------------

  _buildWindows() {
    const park = document.getElementById('pane-park');
    for (const def of BUILTIN_WINDOWS) {
      const el = document.createElement('section');
      el.className = `pane pane-${def.id}`;
      el.id = `pane-${def.id}`;
      park.appendChild(el);
      const win = new def.create(el, this._ctx(def.id));
      this.windows.set(def.id, { def, el, win });
    }
  }

  _ctx(owner) {
    return { app: this, ws: this.ws, bus: this.bus.owner(), owner };
  }

  _paneList() {
    return [...this.windows.entries()].map(([id, w]) => ({
      id, title: w.def.title, group: w.def.group, short: w.def.short || w.def.title, el: w.el,
    }));
  }

  // Bring a window into view and focus it -- used by other windows ("open in
  // the editor", "show in GATT") so a request never lands in a parked pane.
  show(paneId) {
    if (!this.windows.has(paneId)) return null;
    // showPane puts it in the active tile; focus() takes a TILE id, so it is
    // the active tile that gets focus, not the pane id (which focus ignored).
    this._layout.showPane(paneId);
    this._layout.focus(this._layout.activeId, { dom: true });
    return this.windows.get(paneId).win;
  }

  window(paneId) {
    return this.windows.get(paneId)?.win ?? null;
  }

  // A window that arrives at runtime -- a device's. `create(el)` builds it.
  // If a saved arrangement already has a place for this id (a device seen
  // in an earlier session), addPane puts it straight there.
  addWindow(id, meta, create) {
    const have = this.windows.get(id);
    if (have) return have.win;
    const el = document.createElement('section');
    el.className = 'pane pane-device';
    document.getElementById('pane-park').appendChild(el);
    const win = create(el);
    this.windows.set(id, { def: { id, ...meta }, el, win });
    this._layout.addPane({ id, title: meta.title, group: meta.group, short: meta.short, el });
    return win;
  }

  removeWindow(id) {
    const w = this.windows.get(id);
    if (!w) return;
    if (this._visible.has(id)) w.win.onHide?.();
    this._visible.delete(id);
    this.windows.delete(id);
    this._layout.removePane(id);
    w.el.remove();
  }

  // A line in the Console, from any window. The Console keeps it whether or
  // not it is on screen.
  log(src, text, level = src === '!' ? 'error' : undefined) {
    this.window('console')?.add('info', src, text, level);
  }

  showDevice(addr) {
    this.show('gatt')?.showDevice(addr);
  }

  // --- layout ----------------------------------------------------------------

  _initLayout() {
    const known = [...this.windows.keys()];
    const stored = readLayoutStore(
      { mode: 'auto', layout: () => Layout.fromJSON({ tree: DEFAULT_TREE, floating: [] }, known) },
      known,
    );
    this._layout = new LayoutManager({
      panes: this._paneList(),
      layout: stored.layout,
      mode: stored.mode,
      active: stored.active,
      onChange: () => this._afterLayout(),
      // A device window a saved arrangement remembers but whose device has
      // not reported yet: the tile keeps its place and says what it is for.
      paneTitle: (id) => {
        const [win, key] = id.split('@');
        const kind = win.split('.').pop();
        return `${kind.charAt(0).toUpperCase()}${kind.slice(1)} · device ${key} (not reporting)`;
      },
    });
    this._layout.onCommit(() => this._persistLayout());
    // The manager builds nothing until asked: apply() is the first render, and
    // it ends in onChange, which runs _afterLayout.
    this._layout.apply();
  }

  _persistLayout() {
    writeLayoutStore(this._layout.state);
  }

  // After every geometry pass: tell windows whether they are on screen, so a
  // parked one can stop work it only does to be seen. The popover closes on
  // any structural change -- the button it hangs from may have moved.
  _afterLayout() {
    const now = new Set();
    for (const [id, w] of this.windows) {
      if (!w.el.closest('#pane-park')) now.add(id);
    }
    for (const id of now) {
      if (!this._visible.has(id)) this.windows.get(id).win.onShow?.();
    }
    for (const id of this._visible) {
      if (!now.has(id)) this.windows.get(id)?.win.onHide?.();
    }
    this._visible = now;
    if (this._layoutVersion !== this._layout.version) {
      this._layoutVersion = this._layout.version;
      if (this.popover.isOpen) this.popover.close();
    }
    for (const id of now) this.windows.get(id).win.onResize?.();
  }

  // --- keys ------------------------------------------------------------------

  _initKeys() {
    const layout = this._layout;
    const settings = this.settingsModal;
    const dirs = { left: [-1, 0], right: [1, 0], up: [0, -1], down: [0, 1] };
    const move = (dir) => {
      if (this.getContext() === 'settings') {
        const [dx, dy] = dirs[dir];
        (this.activeDialog() || settings).nudge(dx * NUDGE_PX, dy * NUDGE_PX);
      } else {
        layout.swapDir(dir);
      }
    };
    const resize = (axis, sign) => {
      if (this.getContext() === 'settings') {
        (this.activeDialog() || settings).resizeBy(axis === 'x' ? sign * RESIZE_PX : 0, axis === 'y' ? sign * RESIZE_PX : 0);
      } else {
        layout.resizeActive(axis, sign);
      }
    };
    const activeWin = () => this.windows.get(layout.activeEl()?.dataset.pane || '')?.win;

    const actions = {
      'focus-left': () => layout.focusDir('left'),
      'focus-down': () => layout.focusDir('down'),
      'focus-up': () => layout.focusDir('up'),
      'focus-right': () => layout.focusDir('right'),
      'cycle-next': () => this._cycleWindow(1),
      'cycle-prev': () => this._cycleWindow(-1),
      'move-left': () => move('left'),
      'move-down': () => move('down'),
      'move-up': () => move('up'),
      'move-right': () => move('right'),
      'shrink-x': () => resize('x', -1),
      'grow-x': () => resize('x', 1),
      'shrink-y': () => resize('y', -1),
      'grow-y': () => resize('y', 1),
      'edge-out-x': () => layout.edgeResizeActive('right', true),
      'edge-in-x': () => layout.edgeResizeActive('right', false),
      'edge-out-y': () => layout.edgeResizeActive('down', true),
      'edge-in-y': () => layout.edgeResizeActive('down', false),
      'split-move-arm': () => {
        if (!layout.beginSplitMove()) return;
        this.keys.arm('split-move', {
          sticky: true,
          altUp: true,
          onDisarm: (released) => {
            if (!released) return layout.cancelSplitMove();
            this.keys.arm('split-side', { sticky: true, onDisarm: () => layout.cancelSplitMove() });
            return undefined;
          },
        });
      },
      'split-pick-left': () => layout.stepSplitTarget('left'),
      'split-pick-down': () => layout.stepSplitTarget('down'),
      'split-pick-up': () => layout.stepSplitTarget('up'),
      'split-pick-right': () => layout.stepSplitTarget('right'),
      'split-side-left': () => layout.previewSplitSide('left'),
      'split-side-down': () => layout.previewSplitSide('down'),
      'split-side-up': () => layout.previewSplitSide('up'),
      'split-side-right': () => layout.previewSplitSide('right'),
      'split-confirm': () => {
        this.keys.disarm(true);
        layout.confirmSplitMove();
      },
      'split-h': () => layout.split('row'),
      'split-v': () => layout.split('col'),
      'swap-splits': () => this._swapSplits(),
      'toggle-tabs': () => this._toggleTabs(),
      ...Object.fromEntries(
        Array.from({ length: 9 }, (_, i) => [`tab-${i + 1}`, () => layout.focusTab(i + 1)]),
      ),
      float: () => layout.toggleFloat(),
      close: () => {
        if (this.getContext() === 'settings') this._closeSettingsByKey();
        else layout.close();
      },
      'pick-pane': () => layout.focusPicker(),
      'pane-settings': () => activeWin()?.openSettings?.(),
      settings: () => {
        if (settings.isOpen) {
          settings.close();
          this._layout.focus(this._layout.activeId, { dom: true });
        } else {
          settings.open();
        }
      },
      help: () => this.helpModal.toggle(),
      escape: () => {
        const ctx = this.getContext();
        if (ctx === 'help') this.helpModal.escape();
        else if (ctx === 'drag') layout.cancelDrag();
        else if (ctx === 'popover') this.popover.close();
        else if (ctx === 'settings') this._closeSettingsByKey();
      },
    };
    // Window-specific actions (the editor's save/run/sidebar) are looked up on
    // whichever window is active, so a binding only does something where it
    // means something.
    for (const id of ['editor-save', 'editor-run', 'editor-stop', 'editor-sidebar', 'editor-to-files', 'editor-to-text',
      'editor-help', 'editor-search-files', 'editor-search-next', 'editor-search-prev', 'editor-split-grow', 'editor-split-shrink', 'editor-split-even', 'editor-tab-next', 'editor-tab-prev', 'editor-tab-right', 'editor-tab-left', 'editor-tab-close',
      'editor-split', 'editor-split-flip', 'editor-other', 'editor-to-other',
      'scanner-filter', 'scanner-filter-next', 'scanner-filter-prev', 'scanner-columns', 'scanner-clear',
      'events-search', 'events-next', 'events-prev', 'events-columns']) {
      actions[id] = () => activeWin()?.action?.(id);
    }

    this.keys = new Keys({
      actions,
      getContext: () => this.getContext(),
      getNavRoots: () => this.getNavRoots(),
      isTabs: () => layout.mode === 'tabs',
      hintEl: document.getElementById('keyhint'),
    });
    for (const [, w] of this.windows) w.win.attachKeys?.(this.keys);
  }

  getNavRoots() {
    const ctx = this.getContext();
    if (ctx === 'help') return [this.helpModal.panel];
    if (ctx === 'popover') return [this.popover.el];
    const dlg = this.activeDialog();
    if (dlg && (ctx === 'settings' || ctx === 'scanner-filter')) return [dlg.panel];
    if (ctx === 'settings') return [this.settingsModal.panel];
    const roots = [this._layout.activeEl()];
    if (this._layout.mode === 'tabs') roots.push(document.getElementById('tabbar'));
    return roots;
  }

  getContext() {
    if (this.helpModal?.isOpen) return 'help';
    if (this._layout?.dragging) return 'drag';
    if (this.popover?.isOpen) return 'popover';
    // An open dialog with focus (or, with focus nowhere, the newest one) is
    // the 'settings' context, unless it names one of its own.
    const dlg = this.activeDialog();
    if (dlg) return dlg.context?.() || 'settings';
    // A window may claim a context of its own while it has focus (the editor
    // does, so its bindings and banner apply only there).
    const w = this.windows.get(this._layout?.activeEl()?.dataset.pane || '')?.win;
    return w?.context?.() || 'workspace';
  }

  registerDialog(d) { (this.dialogs ||= []).push(d); return d; }

  activeDialog() {
    const open = (this.dialogs || []).filter((d) => d.isOpen);
    if (!open.length) return null;
    const focused = open.find((d) => d.hasFocus);
    if (focused) return focused;
    const ae = document.activeElement;
    if (!ae || ae === document.body) return open.sort((a, b) => b.openedAt - a.openedAt)[0];
    return null;
  }

  _closeSettingsByKey() {
    const d = this.activeDialog() || this.settingsModal;
    d.close();
    // Settings hands the keyboard back to the window; a window's own dialog
    // has already put it back where it was opened from.
    if (d === this.settingsModal || document.activeElement === document.body) {
      this._layout.focus(this._layout.activeId, { dom: true });
    }
  }

  _cycleWindow(step) {
    if (this._layout.mode === 'tabs') { this._cycleTab(step); return; }
    const ring = this._layout.ring().map((id) => ({ kind: 'win', id }));
    if (this.settingsModal.isOpen) ring.push({ kind: 'settings' });
    if (!ring.length) return;
    let i = ring.findIndex((r) =>
      (this.getContext() === 'settings' ? r.kind === 'settings' : r.kind === 'win' && r.id === this._layout.activeId));
    if (i < 0) i = 0;
    const n = ring.length;
    const next = ring[(((i + step) % n) + n) % n];
    if (next.kind === 'settings') this.settingsModal.focusPanel();
    else this._layout.focus(next.id, { dom: true });
  }

  // Tabbed mode: one tab at a time, in the tab bar's order, wrapping; an open
  // Settings panel is one more stop past either end. Showing a tab is what
  // clicking it does (showPane), and the keyboard stays on the window.
  _cycleTab(step) {
    const ids = this._layout.tabOrder();
    const n = ids.length;
    if (!n) return;
    const show = (k) => {
      this._layout.showPane(ids[((k % n) + n) % n]);
      this._layout.focus(this._layout.activeId, { dom: true });
    };
    if (this.getContext() === 'settings') { show(step > 0 ? 0 : n - 1); return; }
    const j = Math.max(0, this._layout.activeTabIndex()) + step;
    if (this.settingsModal.isOpen && (j >= n || j < 0)) { this.settingsModal.focusPanel(); return; }
    show(j);
  }

  // --- toolbar -----------------------------------------------------------------

  _wireToolbar() {
    this._swapSplitsBtn = document.getElementById('swap-splits-btn');
    this._tabsBtn = document.getElementById('tabs-btn');
    this._swapSplitsBtn.addEventListener('click', () => this._swapSplits());
    this._tabsBtn.addEventListener('click', () => this._toggleTabs());
    this.syncLayoutButtons();
  }

  // Also called by the Settings -> UI section, which holds the same choice.
  setLayoutMode(mode) {
    this._layout.setMode(mode);
    this.syncLayoutButtons();
    this._persistLayout();
  }

  get layoutMode() { return this._layout.override; }

  // The app is exactly as tall as what is visible -- above an on-screen
  // keyboard too -- and never scrolls as a page: a phone keyboard otherwise
  // pushed the top of the app (the Console's log) off screen to reveal the
  // text box. The viewport tag's interactive-widget=resizes-content does
  // this natively on Android; this covers the browsers that ignore it.
  _fitViewport() {
    const vv = window.visualViewport;
    if (!vv) return;
    const fit = () => {
      document.documentElement.style.setProperty('--app-h', `${Math.round(vv.height)}px`);
      if (window.scrollY || vv.offsetTop) window.scrollTo(0, 0);
    };
    vv.addEventListener('resize', fit);
    vv.addEventListener('scroll', fit);
    fit();
  }

  // Whether the workspace is showing tabs right now (forced on a small screen
  // or chosen) -- the GATT window offers its events split only then.
  get tabsMode() { return this._layout?.mode === 'tabs'; }

  syncLayoutButtons() {
    const tabs = this._layout.mode === 'tabs';
    const forced = tabs && !this._layout.roomyForTiles;
    this._tabsBtn.setAttribute('aria-pressed', String(tabs));
    this._tabsBtn.disabled = forced;
    this._tabsBtn.title = forced
      ? 'Tabbed view — no room for two windows on this screen'
      : 'Tabbed view — one window at a time (Alt+Shift+T)';
    this._swapSplitsBtn.disabled = tabs;
    for (const s of this.sections) s.syncLayout?.();
  }

  _swapSplits() {
    if (!this._layout.transpose()) return;
    this.syncLayoutButtons();
    this._persistLayout();
  }

  _toggleTabs() {
    if (!this._layout.toggleTabs()) return;
    this.syncLayoutButtons();
    this._persistLayout();
  }

  // --- connection ----------------------------------------------------------------

  _wireConnection() {
    const status = document.getElementById('conn-status');
    const info = document.getElementById('conn-info');
    this.ws.on('_open', async () => {
      status.textContent = 'Connected';
      status.className = 'connected';
      try {
        const s = await this.ws.call('sys.info');
        this.sysInfo = s;
        info.textContent = s.firmware ? `firmware ${s.firmware}` : '';
      } catch {
        info.textContent = '';
      }
      for (const [, w] of this.windows) w.win.onConnect?.();
      for (const s of this.sections) s.onConnect?.();
    });
    this.ws.on('_close', () => {
      status.textContent = 'Disconnected';
      status.className = 'disconnected';
      for (const [, w] of this.windows) w.win.onDisconnect?.();
    });
  }

  setPref(key, value) {
    this.prefs[key] = value;
    writePrefs({ [key]: value });
    for (const [, w] of this.windows) w.win.onPrefs?.(this.prefs);
  }

  // --- the frame -------------------------------------------------------------------

  _frame(t) {
    if (!document.hidden) {
      for (const id of this._visible) this.windows.get(id)?.win.render?.(t);
      if (this.settingsModal.isOpen) for (const s of this.sections) s.render?.(t);
    }
    requestAnimationFrame((tt) => this._frame(tt));
  }
}

window.__btgwApp = new App();
