// The workspace renderer: a tiling window manager over the panes.
//
// Where the panes go is decided by js/tiles.js (a split tree plus a list
// of floating windows). This module turns that model into DOM and keeps
// the two in step: it builds a `.split` / `.tile` tree, wires a splitter
// per split node, positions floating windows over the workspace, and moves
// the pane elements themselves into whichever tile shows them.
//
// Three rules from the old two-slot layout survive because they were hard
// won:
//   * panes are never destroyed, only moved (appendChild) — uPlot keeps
//     its canvases across every rearrangement; anything not on screen sits
//     in #pane-park, which is display:none;
//   * every level of the flex chain has min-width AND min-height 0, or a
//     chart's intrinsic size stops a splitter dead;
//   * a lone visible child gets flex-grow 1, never its fractional share —
//     a grow below 1 claims only that fraction of the free space.
//
// Two render levels. Structural changes — split, close, swap, float, a mode
// change, the phone tabs threshold — rebuild the container DOM; that costs
// a full re-mount of the panes, so it happens only when the tree's shape
// changed. Everything else — a splitter drag frame, a channel toggle, a
// window resize — is a geometry pass over the existing elements. apply()
// decides which by comparing a structural signature, so callers just call
// apply(). Every path ends in onChange(), which is where app.js re-measures
// uPlot (its cached rects go stale the moment a pane moves).
//
// Modes. `auto` shows the tree as the user built it, except below the
// "too small for two" thresholds where it shows one tile at a time with a
// tab bar (tabs). `side` / `stack` pin the ROOT split's direction — a
// render-time override that leaves the stored tree alone, so unpinning
// restores exactly what was there — and never fall to tabs. `tabs` forces
// tabs. The resolved mode is published as #workspace[data-mode].
//
// What this does not own: anything inside a pane, the keyboard layer
// (keys.js drives the same public methods the buttons do), and the
// Settings panel, which is a floating window of its own kind.

import { Layout, neighbour, RESIZE_STEP, FLOAT_MIN } from './tiles.js';
import { makeSplitter, clamp } from './splitter.js';

export const MODES = ['auto', 'side', 'stack', 'tabs'];

// Below these a second pane is worse than useless. 720px wide is about
// where two charts stay legible side by side; 560px tall is two 120px-
// minimum charts plus the events header and chrome.
const MIN_W_SIDE = 720;
const MIN_H_STACK = 560;

// Keyboard nudges and resizes for floating windows, as fractions of the
// workspace.
const FLOAT_NUDGE = 0.04;
const FLOAT_RESIZE = 0.05;

// Dropping a pane on another one can mean two things, and where you let go
// says which: the middle of the tile swaps the two panes, anywhere else
// splits the tile and gives the pane that edge of it. This is the size of
// the middle — a quarter of the width and a quarter of the height, centred
// — so that splitting is what you get by default and swapping is a
// deliberate aim at the centre.
const SWAP_ZONE = 0.25;

// How far either side of a divider still counts as being on it. The line
// itself is a few pixels wide and sits between two windows' own edge zones,
// so without a little reach it would be a target only a steady hand could
// hit — and with too much it would swallow the edge zones it sits between.
const DIVIDER_GRAB = 4;

// Groups the picker pins to the bottom of its list, in this order, whatever
// order their panes were registered in (constructor option `pinLast`). Every
// other group keeps registration order.


// Travelling one way means arriving at the opposite edge of what you reach:
// move down onto a tile and you come in through its top. Both halves of the
// feature turn a direction into a landing edge this way, which is what keeps
// a keyboard move and a drop on the same edge doing the same thing.
const OPPOSITE = { left: 'right', right: 'left', up: 'down', down: 'up' };

// The eight grab points around a floating window, and what the pointer turns
// into over each. Compass letters because they compose: 'ne' is the north and
// east edges together, which is exactly what that corner drags.
const FLOAT_EDGES = ['n', 's', 'e', 'w', 'ne', 'nw', 'se', 'sw'];
const FLOAT_CURSOR = {
  n: 'ns-resize',
  s: 'ns-resize',
  e: 'ew-resize',
  w: 'ew-resize',
  ne: 'nesw-resize',
  sw: 'nesw-resize',
  nw: 'nwse-resize',
  se: 'nwse-resize',
};

// The rect a floating window takes when `edge` is dragged by (dx, dy), both
// as fractions of the workspace.
//
// Dragging a left or top edge moves the origin as well as the size, and it is
// the size that has to stop: without clamping the delta first, a window
// already at FLOAT_MIN would carry on sliding while its width stayed put.
// The north edge additionally stops at the top of the workspace, because the
// model clamps y there anyway (_clampFloat) — letting the height grow past
// that point would leave the two disagreeing, and the bottom edge would creep
// down while the top stood still.
function floatResizeRect(r0, edge, dx, dy) {
  const out = { x: r0.x, y: r0.y, w: r0.w, h: r0.h };
  if (edge.includes('w')) {
    const d = Math.min(dx, r0.w - FLOAT_MIN);
    out.x = r0.x + d;
    out.w = r0.w - d;
  } else if (edge.includes('e')) {
    out.w = Math.max(FLOAT_MIN, r0.w + dx);
  }
  if (edge.includes('n')) {
    const d = clamp(dy, -r0.y, r0.h - FLOAT_MIN);
    out.y = r0.y + d;
    out.h = r0.h - d;
  } else if (edge.includes('s')) {
    out.h = Math.max(FLOAT_MIN, r0.h + dy);
  }
  return out;
}

const SVG_NS = 'http://www.w3.org/2000/svg';
const XLINK_NS = 'http://www.w3.org/1999/xlink';

// The height seen while nothing was being typed into.
let restingH = 0;
let restingW = 0;

const typingNow = () => {
  const ae = document.activeElement;
  if (!ae) return false;
  if (ae.tagName === 'TEXTAREA' || ae.isContentEditable) return true;
  return ae.tagName === 'INPUT' && !['checkbox', 'radio', 'range', 'button', 'submit', 'reset', 'color', 'file'].includes(ae.type);
};

function viewportH() {
  // Not innerHeight: a mobile URL bar collapsing changes the visible height
  // without changing innerHeight, and mode would latch to the wrong branch.
  const h = window.visualViewport?.height ?? window.innerHeight;
  // But an on-screen keyboard is not a smaller screen: while a text box has
  // focus, a height lost at the same width is the keyboard's, and the
  // arrangement must not flip to tabs or turn its split around under the
  // user's thumbs (seen on a Galaxy Z Fold6 typing in the Console).
  if (typingNow() && restingH && h < restingH && window.innerWidth === restingW) return restingH;
  restingH = h;
  restingW = window.innerWidth;
  return h;
}

function isLandscape() {
  return window.innerWidth > viewportH();
}

// An icon from the sprite in index.html. createElementNS rather than
// innerHTML, like everything else here; href with the xlink fallback for
// older WebKit.
function icon(name) {
  const svg = document.createElementNS(SVG_NS, 'svg');
  svg.setAttribute('aria-hidden', 'true');
  const use = document.createElementNS(SVG_NS, 'use');
  use.setAttribute('href', `#i-${name}`);
  use.setAttributeNS(XLINK_NS, 'xlink:href', `#i-${name}`);
  svg.appendChild(use);
  return svg;
}

function button(cls, title, name, onClick) {
  const b = document.createElement('button');
  b.type = 'button';
  b.className = `tile-btn ${cls}`;
  b.title = title;
  b.setAttribute('aria-label', title);
  b.appendChild(icon(name));
  b.addEventListener('click', onClick);
  return b;
}

export class LayoutManager {
  /*
    panes:  [{ id, title, el, isEmpty? }] — isEmpty() lets a pane say it has
            nothing to show (every channel deselected, PD monitoring off);
            its tile then collapses and its sibling takes the room. Panes
            come and go after construction too (addPane/removePane): a
            meter's panes arrive with it and are parked while it is away.
    paneTitle(id): the title for a pane id that is not registered — the
            placeholder a tile shows for a meter that is not attached.
    layout: a Layout (tiles.js) — the tree and floating windows.
    mode:   'auto' | 'side' | 'stack' | 'tabs'
    active: index into layout.windows() to start focused on.
    onChange(): after every geometry pass. app.js re-measures uPlot here.
  */
  constructor({ panes, layout, mode, active, onChange, paneTitle = null, paneParts = null, pinLast = [] }) {
    this._pinLast = pinLast;
    this._panes = new Map(panes.map((p) => [p.id, p]));
    this.layout = layout;
    this._onChange = onChange;
    this._onCommit = null;
    // Names a pane that is not registered right now (a meter's, while the
    // meter is away) for the tile that is keeping its place.
    this._paneTitle = paneTitle;
    // The same, split into the picker's two halves — see _buildTile.
    this._paneParts = paneParts;

    this._workspace = document.getElementById('workspace');
    this._tiles = document.getElementById('tiles');
    this._floatLayer = document.getElementById('floating-layer');
    this._tabbar = document.getElementById('tabbar');
    this._park = document.getElementById('pane-park');

    this.override = MODES.includes(mode) ? mode : 'auto';
    // What toggleTabs() goes back to when tabs are switched off again. Not
    // 'auto': someone who pinned "side by side", tried tabs and turned them
    // off again meant to get their pinned arrangement back, not the
    // automatic one.
    this._preTabsMode = this.override === 'tabs' ? 'auto' : this.override;
    this._mode = null;
    this._version = 0; // bumps on every structural change
    this._sig = null; // signature the DOM was last built for
    this._els = new Map(); // node id -> element (tiles, splits, splitters, floats)
    this._splitters = []; // makeSplitter handles to destroy on rebuild
    this._pickers = new Map(); // leaf/float id -> <select>

    const start = Number.isInteger(active) ? this.layout.at(active) : null;
    this.active = (start || this.layout.leaves()[0]).id;
    // Most recently active first; breaks ties in directional focus.
    this._history = [this.active];

    // The pane drag (see _wireSwapDrag): null when idle.
    this._drag = null;
    // The keyboard split-move gesture (see beginSplitMove): null when idle.
    this._splitMove = null;
    // The live preview of a move (see beginPreview): null when idle.
    this._preview = null;

    this._buildTabs();
    this._wireActivation();
    this._wireSwapDrag();

    window.addEventListener('resize', () => this.apply());
    window.visualViewport?.addEventListener('resize', () => this.apply());
  }

  // --- what app.js and the tests read -------------------------------------

  get mode() {
    return this._mode;
  }

  get panes() {
    return [...this._panes.values()];
  }

  get activeId() {
    return this.active;
  }

  // True while a pane is being dragged to another window.
  get dragging() {
    return !!(this._drag && this._drag.moved);
  }

  // What the prefs layer persists.
  get state() {
    return {
      mode: this.override,
      ...this.layout.toJSON(),
      active: Math.max(0, this.layout.indexOf(this.active)),
    };
  }

  // Bumped on every structural change (a split, a close, a float, a pane
  // added or removed), not on the geometry passes that follow a channel
  // toggle or a window resize. A caller that must react to the tiles
  // actually moving watches this rather than onChange.
  get version() {
    return this._version;
  }

  paneTitle(id) {
    return this._panes.get(id)?.title ?? this._paneTitle?.(id) ?? id;
  }

  hasPane(id) {
    return this._panes.has(id);
  }

  paneEl(id) {
    return this._panes.get(id)?.el || null;
  }

  // Retitles a registered pane (a meter's model arrived after its panes
  // were named); pickers and tabs are rebuilt.
  setPaneTitle(id, title) {
    const pane = this._panes.get(id);
    if (!pane || pane.title === title) return;
    pane.title = title;
    this._rebuildTabs();
    this._version++;
    this.apply();
  }

  // Registers a pane after construction — a meter's, when it arrives. It
  // becomes placeable (pickers, tabs, the tree's known set); a tile that
  // was keeping its place shows it again. Nothing is placed automatically:
  // where a new pane goes is the user's decision.
  addPane(pane) {
    if (this._panes.has(pane.id)) return;
    this._panes.set(pane.id, pane);
    this.layout.addKnown(pane.id);
    this._rebuildTabs();
    this._version++;
    this.apply();
  }

  // Unregisters a pane (its meter went away). Its element is parked and it
  // leaves the pickers, but any tile holding it keeps holding it — shown as
  // a placeholder naming the pane — so the arrangement survives a replug.
  removePane(id) {
    const pane = this._panes.get(id);
    if (!pane) return;
    this._park.appendChild(pane.el);
    this._panes.delete(id);
    this._rebuildTabs();
    this._version++;
    this.apply();
  }

  // Renames a pane in the tree and the registry — the one-time step when
  // the serial-less panes the page starts with turn out to be a
  // particular meter's.
  renamePane(oldId, newId, title, short) {
    const pane = this._panes.get(oldId);
    if (!pane) return false;
    pane.id = newId;
    if (title) pane.title = title;
    if (short) pane.short = short;
    // Same position in the pickers and the tab bar: rebuild the map in
    // order rather than delete+set, which would send it to the end.
    this._panes = new Map([...this._panes].map(([k, v]) => [k === oldId ? newId : k, v]));
    this.layout.renamePane(oldId, newId);
    this._rebuildTabs();
    this._version++;
    this.apply();
    return true;
  }

  // Fired after every discrete mutation and every drag release — where
  // persistence belongs. Never per pointermove.
  onCommit(fn) {
    this._onCommit = fn;
  }

  _commit() {
    this._onCommit?.();
  }

  // --- mode -----------------------------------------------------------------

  setMode(mode) {
    if (!MODES.includes(mode)) return;
    if (mode !== 'tabs') this._preTabsMode = mode;
    this.override = mode;
    this.apply();
  }

  // The toolbar's tabs button. Off goes back to whatever was in force before
  // tabs were turned on, so the button and the Arrangement select stay two
  // views of one setting. Refused when the screen is too small for two panes
  // — tabs are not a choice there, and app.js greys the button out to say so.
  toggleTabs() {
    if (this.override !== 'tabs' && !this.roomyForTiles) return false;
    this.setMode(this.override === 'tabs' ? this._preTabsMode : 'tabs');
    this._commit();
    return true;
  }

  // The direction the root split is drawn with: the pin, else the tree's own.
  _rootDir() {
    if (this.override === 'side') return 'row';
    if (this.override === 'stack') return 'col';
    const r = this.layout.root;
    return r.kind === 'split' ? r.dir : isLandscape() ? 'row' : 'col';
  }

  // Is there room on this screen to show two panes at once? Below this the
  // workspace falls back to tabs on its own, and the toolbar's tabs toggle
  // has nothing to toggle — both read this one definition rather than each
  // keeping its own idea of "too small".
  get roomyForTiles() {
    const w = window.innerWidth;
    const h = viewportH();
    return (w >= MIN_W_SIDE && w > h) || h >= MIN_H_STACK;
  }

  _resolveMode() {
    if (this.override === 'tabs') return 'tabs';
    if (this.override === 'side' || this.override === 'stack') return this.override;
    if (!this.roomyForTiles) return 'tabs';
    return this._rootDir() === 'row' ? 'side' : 'stack';
  }

  // --- the two render levels --------------------------------------------------

  apply() {
    const mode = this._resolveMode();
    this._mode = mode;
    const tabs = mode === 'tabs';
    this._workspace.dataset.mode = mode;
    const sig = `${tabs}|${this._rootDir()}|${this._version}`;
    if (sig !== this._sig) {
      this._rebuild(tabs);
      this._sig = sig;
    }
    this._geometry(tabs);
    this._syncPickers();
    this._syncTabs();
    this._syncActive();
    this._syncPreview();
    this._onChange?.();
  }

  // Runs a model mutation, then rebuilds and persists. `fn` returns the id
  // to focus afterwards, or null/undefined for "keep the current one".
  _mutate(fn) {
    const next = fn();
    this._version++;
    if (next != null && this.layout.find(next)) this.active = next;
    this.apply();
    this._commit();
    return next;
  }

  _rebuild(tabs) {
    // Keyboard focus on a header control survives the rebuild: remember
    // which tile and which control, and put it back on the new element.
    // (A pane's own controls move with the pane and keep focus by
    // themselves.)
    const ae = document.activeElement;
    const focusedTile = ae?.closest?.('.tile');
    const keep = focusedTile && ae.closest('.tile-head')
      ? { id: Number(focusedTile.dataset.tile), cls: [...ae.classList].find((c) => c !== 'tile-btn') }
      : null;

    // Panes first, before anything is thrown away: an element inside a
    // subtree that gets replaced is gone, canvases and all.
    for (const p of this._panes.values()) this._park.appendChild(p.el);
    for (const s of this._splitters) s.destroy();
    this._splitters = [];
    this._els.clear();
    this._pickers.clear();

    const rootDir = this._rootDir();
    const tree = this._buildNode(this.layout.root, rootDir, true);
    this._tiles.replaceChildren(tree);

    const floats = this.layout.floating.map((f) => this._buildTile(f, true));
    this._floatLayer.replaceChildren(...floats);
    this._floatLayer.hidden = tabs || floats.length === 0;

    // Mount. Every window with a pane gets its element; the rest stay parked.
    for (const w of this.layout.windows()) {
      if (!w.pane) continue;
      const pane = this._panes.get(w.pane);
      const body = this._els.get(w.id)?.querySelector(':scope > .tile-body');
      if (pane && body) body.appendChild(pane.el);
    }

    if (keep) {
      const el = this._els.get(keep.id)?.querySelector(`.tile-head .${keep.cls}`);
      if (el && !el.disabled) el.focus({ preventScroll: true });
    }
  }

  // The active window's element (a tile or a floating window).
  activeEl() {
    return this._els.get(this.active) || null;
  }

  // Puts the keyboard on the active tile's pane picker (Alt+P). Not in
  // tabs mode, where the header is hidden and the tab bar does that job.
  focusPicker(id = this.active) {
    if (this._mode === 'tabs') return false;
    const pick = this._pickers.get(id);
    if (!pick || pick.getClientRects().length === 0) return false;
    pick.focus({ preventScroll: true });
    return true;
  }

  _buildNode(node, rootDir, isRoot) {
    if (node.kind === 'leaf') return this._buildTile(node, false);
    const el = document.createElement('div');
    el.className = 'split';
    el.dataset.split = String(node.id);
    el.dataset.dir = isRoot ? rootDir : node.dir;
    const a = this._buildNode(node.a, rootDir, false);
    const sep = document.createElement('div');
    sep.className = 'splitter';
    sep.dataset.axis = el.dataset.dir === 'row' ? 'x' : 'y';
    sep.title = 'Drag to resize';
    const b = this._buildNode(node.b, rootDir, false);
    el.append(a, sep, b);
    this._els.set(node.id, el);
    this._els.set(`sep:${node.id}`, sep);
    this._wireSplitter(sep, node, el);
    return el;
  }

  _wireSplitter(sep, node, el) {
    let startRatio = 0;
    let span = 1;
    const axis = sep.dataset.axis;
    this._splitters.push(
      makeSplitter(sep, {
        axis,
        onStart: () => {
          startRatio = node.ratio;
          const r = el.getBoundingClientRect();
          span = Math.max(1, axis === 'x' ? r.width : r.height);
        },
        onDrag: (delta) => {
          this.layout.setRatio(node.id, startRatio + delta / span);
          this._geometry(this._mode === 'tabs');
          this._onChange?.();
        },
        onEnd: () => this._commit(),
      }),
    );
  }

  // One tile: header (picker, split buttons, grab area, float, close) and
  // body. The same for a floating window, which just adds a class and
  // positions itself; its split buttons are hidden by CSS since a floating
  // window has no split to make.
  _buildTile(win, floating) {
    const el = document.createElement('section');
    el.className = floating ? 'tile floating' : 'tile';
    el.dataset.tile = String(win.id);
    el.dataset.pane = win.pane || '';
    el.tabIndex = -1;

    const head = document.createElement('div');
    head.className = 'tile-head';

    // Grouped by kind: the heading says what the panes are, each entry says
    // which meter. The list then reads down the meters of one kind instead of
    // repeating the kind against every meter — and, because a <select> is as
    // wide as its widest entry, the control shrinks to the width of a meter
    // name rather than a whole sentence. That width is the point: what it
    // gives back is .tile-grab, the part of the header you drag the window by.
    const pick = document.createElement('select');
    pick.className = 'pane-pick';
    pick.setAttribute('aria-label', 'Pane to show');
    const none = document.createElement('option');
    none.value = '';
    // Short on purpose: an empty tile has nothing else in its picker, so this
    // one line would set the control's width on its own.
    none.textContent = 'choose\u2026';
    pick.appendChild(none);

    const order = [];
    const byGroup = new Map();
    const add = (group, value, text) => {
      if (!byGroup.has(group)) {
        byGroup.set(group, []);
        order.push(group);
      }
      byGroup.get(group).push({ value, text });
    };
    for (const p of this._panes.values()) add(p.group ?? '', p.id, p.short ?? p.title);
    // A pane this tile is holding for a meter that is away: its own entry,
    // under its own kind, so the picker still says what the tile is for.
    if (win.pane && !this._panes.has(win.pane)) {
      const parts = this._paneParts?.(win.pane);
      add(parts?.group ?? '', win.pane, `${parts?.short ?? this.paneTitle(win.pane)} (away)`);
    }
    // Stable, so everything else keeps the order it was registered in.
    const rank = (g) => { const k = this._pinLast.indexOf(g); return k < 0 ? -1 : k; };
    order.sort((a, b) => rank(a) - rank(b));
    for (const g of order) {
      let parent = pick;
      if (g) {
        parent = document.createElement('optgroup');
        parent.label = g;
        pick.appendChild(parent);
      }
      for (const e of byGroup.get(g)) {
        const o = document.createElement('option');
        o.value = e.value;
        o.textContent = e.text;
        parent.appendChild(o);
      }
    }
    pick.addEventListener('change', () => this.setPane(win.id, pick.value || null));
    this._pickers.set(win.id, pick);

    const grab = document.createElement('span');
    grab.className = 'tile-grab';
    grab.title = floating ? 'Drag to move' : 'Drag to move this pane';

    const close = button('tile-close', 'Close (Alt+W)', 'close', () => this.close(win.id));

    head.append(
      pick,
      button('split-h', 'Split horizontally (Alt+Enter)', 'split-h', () => this.split('row', win.id)),
      button('split-v', 'Split vertically (Alt+Shift+Enter)', 'split-v', () => this.split('col', win.id)),
      grab,
      button('tile-float', floating ? 'Tile this window (Alt+T)' : 'Float this pane (Alt+T)', floating ? 'tile' : 'float',
        () => this.toggleFloat(win.id)),
      close,
    );

    const body = document.createElement('div');
    body.className = 'tile-body';
    if (!win.pane) {
      const ph = document.createElement('div');
      ph.className = 'tile-placeholder dim';
      ph.textContent = 'Choose a pane above';
      body.appendChild(ph);
    } else if (!this._panes.has(win.pane)) {
      // The meter this pane belongs to is not attached. The tile keeps its
      // place — and its share of the screen — so plugging the meter back
      // in puts it exactly where it was.
      const ph = document.createElement('div');
      ph.className = 'tile-placeholder pane-missing dim';
      ph.textContent = `${this.paneTitle(win.pane)} \u2014 not attached`;
      body.appendChild(ph);
    }

    el.append(head, body);
    this._els.set(win.id, el);
    if (floating) {
      this._wireFloatMove(el, grab, win);
      this._wireFloatResize(el, win);
    }
    return el;
  }

  // A floating window moves by its grab area. makeSplitter already does the
  // pointer-capture and window-listener dance; the deltas it reports are
  // turned into workspace fractions so the window keeps its place when the
  // workspace changes size.
  _wireFloatMove(el, grab, win) {
    let x0 = 0;
    let y0 = 0;
    this._splitters.push(
      makeSplitter(grab, {
        axis: 'x',
        onStart: () => {
          x0 = win.x;
          y0 = win.y;
          this.focus(win.id);
        },
        onDrag: (_d, { dx, dy }) => {
          const r = this._workspace.getBoundingClientRect();
          this.layout.setFloatRect(win.id, { x: x0 + dx / Math.max(1, r.width), y: y0 + dy / Math.max(1, r.height) });
          this._geometry(false);
          this._onChange?.();
        },
        onEnd: () => this._commit(),
      }),
    );
    grab.style.cursor = 'move';
  }

  // A floating window is the one thing on screen with a free size, and until
  // now the only way to change it was the keyboard. Each edge and each corner
  // gets a grab strip, with the cursor that says what it will do.
  //
  // The strips live inside the tile rather than overhanging it, because .tile
  // clips its children (overflow: hidden) and a handle hung outside the
  // border would simply not be there. They are narrower than the header's 8px
  // side padding, so they never take a header button's click; the top strip
  // covers the first 2px of those buttons, which is what a window manager's
  // top edge does everywhere.
  _wireFloatResize(el, win) {
    for (const edge of FLOAT_EDGES) {
      const h = document.createElement('div');
      h.className = 'float-resize';
      h.dataset.edge = edge;
      el.appendChild(h);
      let r0 = null;
      this._splitters.push(
        makeSplitter(h, {
          onStart: () => {
            r0 = { x: win.x, y: win.y, w: win.w, h: win.h };
            this.focus(win.id);
          },
          onDrag: (_d, { dx, dy }) => {
            const ws = this._workspace.getBoundingClientRect();
            const rect = floatResizeRect(r0, edge, dx / Math.max(1, ws.width), dy / Math.max(1, ws.height));
            this.layout.setFloatRect(win.id, rect);
            this._geometry(false);
            this._onChange?.();
          },
          onEnd: () => this._commit(),
        }),
      );
      // makeSplitter sets a one-axis cursor of its own; this handle has one
      // of eight, and two of them are diagonal.
      h.style.cursor = FLOAT_CURSOR[edge];
    }
  }

  // Hidden flags, grow factors and float rects, from the model and the
  // panes' isEmpty(). The tree is never mutated here: hiding a tile whose
  // pane has nothing to show is a rendering decision, and reversing it
  // restores the exact shape.
  _geometry(tabs) {
    const leaves = this.layout.leaves();
    const empty = (leaf) => !!(leaf.pane && this._panes.get(leaf.pane)?.isEmpty?.());
    let shown = null;
    if (tabs) {
      const a = this.layout.find(this.active);
      shown = a && a.kind === 'leaf' ? a : leaves[0];
    }
    // If every leaf is empty, show them all: a workspace with no picker on
    // screen has no way back.
    const allEmpty = !tabs && leaves.every(empty);
    const visible = (n) => {
      if (n.kind === 'leaf') return tabs ? n === shown : allEmpty || !empty(n);
      return visible(n.a) || visible(n.b);
    };
    const walk = (n) => {
      const el = this._els.get(n.id);
      if (!el) return;
      el.hidden = !visible(n);
      if (n.kind !== 'split') return;
      const va = visible(n.a);
      const vb = visible(n.b);
      const sep = this._els.get(`sep:${n.id}`);
      if (sep) sep.hidden = tabs || !(va && vb);
      const ea = this._els.get(n.a.id);
      const eb = this._els.get(n.b.id);
      // The lone-survivor rule: a single visible child grows to fill.
      if (ea) ea.style.flexGrow = va && vb ? String(n.ratio) : '1';
      if (eb) eb.style.flexGrow = va && vb ? String(1 - n.ratio) : '1';
      walk(n.a);
      walk(n.b);
    };
    walk(this.layout.root);

    if (!tabs && this.layout.floating.length) {
      const r = this._workspace.getBoundingClientRect();
      for (const f of this.layout.floating) {
        const el = this._els.get(f.id);
        if (!el) continue;
        el.style.left = `${(f.x * r.width).toFixed(1)}px`;
        el.style.top = `${(f.y * r.height).toFixed(1)}px`;
        el.style.width = `${(f.w * r.width).toFixed(1)}px`;
        el.style.height = `${(f.h * r.height).toFixed(1)}px`;
        el.style.zIndex = String(10 + this.layout.floating.indexOf(f));
      }
    }
    this._tabbar.hidden = !tabs;
  }

  _syncPickers() {
    for (const w of this.layout.windows()) {
      const pick = this._pickers.get(w.id);
      const el = this._els.get(w.id);
      if (pick && pick.value !== (w.pane || '')) pick.value = w.pane || '';
      if (el) el.dataset.pane = w.pane || '';
      // The last tile cannot be closed once it is already empty — there
      // would be nothing left to close it from.
      const closeBtn = el?.querySelector(':scope > .tile-head > .tile-close');
      if (closeBtn) closeBtn.disabled = w.kind === 'leaf' && !w.pane && this.layout.leaves().length === 1;
      const floatBtn = el?.querySelector(':scope > .tile-head > .tile-float');
      if (floatBtn) floatBtn.disabled = w.kind === 'leaf' && !w.pane;
    }
  }

  _syncActive() {
    if (!this.layout.find(this.active)) this.active = this.layout.leaves()[0].id;
    for (const [id, el] of this._els) {
      if (typeof id === 'number' && el.classList.contains('tile')) {
        if (id === this.active) el.setAttribute('data-active', '');
        else el.removeAttribute('data-active');
      }
    }
  }

  // --- tabs (small screens) -------------------------------------------------

  _rebuildTabs() {
    this._tabbar.replaceChildren();
    this._buildTabs();
  }

  _buildTabs() {
    for (const pane of this._panes.values()) {
      const btn = document.createElement('button');
      btn.type = 'button';
      btn.className = 'tab';
      btn.dataset.pane = pane.id;
      btn.textContent = pane.title;
      // Showing a pane in tabs mode is putting it in the active tile; the
      // swap rule pulls it out of wherever it was, floating included.
      btn.addEventListener('click', () => this.showPane(pane.id));
      this._tabbar.appendChild(btn);
    }
  }

  // Keep the active tab on screen: the bar scrolls now (style.css #tabbar),
  // and with a pane per meter the active one is often past the edge.
  _scrollTabIntoView(btn) {
    btn?.scrollIntoView({ inline: 'nearest', block: 'nearest' });
  }

  _syncTabs() {
    const a = this.layout.find(this.active);
    const leaf = a && a.kind === 'leaf' ? a : this.layout.leaves()[0];
    for (const btn of this._tabbar.querySelectorAll('.tab')) {
      const active = btn.dataset.pane === leaf.pane;
      const was = btn.classList.contains('active');
      btn.classList.toggle('active', active);
      if (active && !was) this._scrollTabIntoView(btn);
    }
  }

  // --- active window --------------------------------------------------------

  // Activation follows the pointer and focus. Capture phase, because
  // chart.js preventDefaults pointerdown inside a plot, which suppresses the
  // focus that would otherwise have told us.
  _wireActivation() {
    const onDown = (e) => {
      const tile = e.target.closest?.('.tile');
      if (tile) this.focus(Number(tile.dataset.tile));
    };
    this._workspace.addEventListener('pointerdown', onDown, true);
    this._workspace.addEventListener('focusin', onDown);
  }

  // Makes a window the active one. `dom` also moves keyboard focus onto it —
  // only for keyboard-initiated moves, so a rebuild never steals focus from
  // a text field.
  focus(id, { dom = false } = {}) {
    const w = this.layout.find(id);
    if (!w || w.kind === 'split') return false;
    const changed = id !== this.active;
    this.active = id;
    if (changed) this._history = [id, ...this._history.filter((h) => h !== id)].slice(0, 32);
    if (w.kind === 'float' && this.layout.floating[this.layout.floating.length - 1] !== w) {
      this.layout.raise(id);
      this._geometry(this._mode === 'tabs');
    }
    if (changed && this._mode === 'tabs') {
      this._geometry(true);
      this._syncTabs();
    }
    this._syncActive();
    if (dom) this._els.get(id)?.focus({ preventScroll: true });
    if (changed) {
      this._onChange?.();
      // Which window is active is part of the arrangement that comes back
      // after a reload, so a focus change is a commit like any other.
      this._commit();
    }
    return true;
  }

  _activeLeafId() {
    const a = this.layout.find(this.active);
    return a && a.kind === 'leaf' ? a.id : this.layout.leaves()[0].id;
  }

  // Visible windows with their on-screen rects, for geometric navigation.
  windowRects() {
    const out = [];
    for (const w of this.layout.windows()) {
      const el = this._els.get(w.id);
      if (!el || el.hidden || el.offsetParent === null) continue;
      const r = el.getBoundingClientRect();
      if (r.width === 0 || r.height === 0) continue;
      out.push({ id: w.id, rect: { left: r.left, top: r.top, right: r.right, bottom: r.bottom } });
    }
    return out;
  }

  focusDir(dir) {
    // In tabs mode there is only ever one tile on screen, so there is no
    // neighbour to find — but there is a row of tabs, and left/right is what
    // anyone would press to walk it. Up/down stay dead: nothing is above or
    // below.
    if (this._mode === 'tabs') {
      if (dir !== 'left' && dir !== 'right') return false;
      return this.stepTab(dir === 'left' ? -1 : 1);
    }
    const rects = this.windowRects();
    const from = rects.find((r) => r.id === this.active);
    if (!from) return false;
    const to = neighbour(from.rect, rects.filter((r) => r.id !== this.active), dir, this._history);
    return to != null && this.focus(to, { dom: true });
  }

  // The tab bar in its own order: one tab per registered pane, which is what
  // _buildTabs renders and what clicking one shows.
  //
  // Emphatically NOT ring(). ring() is the list of *tiles*, and the bar has a
  // tab for every pane whether or not a tile is holding it — two tiles, five
  // tabs. Stepping the tiles therefore skipped tabs, which is what "Alt+L
  // moves two tabs over" was.
  _tabPanes() {
    return [...this._panes.keys()];
  }

  // The pane the one visible tile is showing, i.e. which tab is lit.
  _shownPane() {
    const a = this.layout.find(this.active);
    const leaf = a && a.kind === 'leaf' ? a : this.layout.leaves()[0];
    return leaf?.pane ?? null;
  }

  // One tab along the bar, wrapping. Showing a tab is putting its pane in the
  // visible tile — the same thing clicking it does, swap rule and all.
  stepTab(step) {
    const panes = this._tabPanes();
    if (!panes.length) return false;
    const i = panes.indexOf(this._shownPane());
    const n = panes.length;
    // Nothing shown (an empty placeholder): start at the first tab.
    this.showPane(panes[i < 0 ? 0 : (((i + step) % n) + n) % n]);
    return true;
  }

  // Alt+1..9: the nth tab where there is a tab bar, else the nth tile in tree
  // order — the same list the eye reads in each mode.
  focusTab(n) {
    if (this._mode === 'tabs') {
      const id = this._tabPanes()[n - 1];
      if (id == null) return false;
      this.showPane(id);
      return true;
    }
    const id = this.ring()[n - 1];
    return id != null && this.focus(id, { dom: true });
  }

  // The windows Mod+N cycles through, in order: tiles in tree order, then
  // floating windows. In tabs mode only tiles are on screen; floats are
  // reached through the tab bar. (app.js appends the Settings panel.)
  ring() {
    const ws = this.layout.windows();
    return (this._mode === 'tabs' ? ws.filter((w) => w.kind === 'leaf') : ws).map((w) => w.id);
  }

  // Tabbed mode's own order: the tab bar lists every registered pane, in the
  // order they were registered (_buildTabs) -- NOT ring(), which is the tiles
  // of the tree kept behind the tabs and would skip every pane that is not in
  // one. Alt+N walks this.
  tabOrder() {
    return [...this._panes.keys()];
  }

  activeTabIndex() {
    const a = this.layout.find(this.active);
    const leaf = a && a.kind === 'leaf' ? a : this.layout.leaves()[0];
    return this.tabOrder().indexOf(leaf?.pane);
  }

  focusNext(step) {
    const ring = this.ring();
    if (!ring.length) return false;
    const i = Math.max(0, ring.indexOf(this.active));
    const n = ring.length;
    return this.focus(ring[(((i + step) % n) + n) % n], { dom: true });
  }

  // --- mutations exposed to buttons and keys ----------------------------------

  setPane(id, paneId) {
    const target = id === 'active' ? this._activeLeafId() : id;
    if (!this.layout.find(target)) return;
    this._mutate(() => this.layout.setPane(target, paneId));
  }

  // "Show this pane": in the active tile, swapping it out of wherever it was.
  showPane(paneId) {
    this.setPane('active', paneId);
  }

  // A new tile opens empty and its picker takes the keyboard, so the next
  // thing typed (or J/K) chooses the pane — whether the split came from a
  // key or a click.
  split(dir, id = this._activeLeafId()) {
    if (this._mode === 'tabs') return null;
    const fresh = this._mutate(() => this.layout.split(id, dir));
    if (fresh != null) this.focusPicker(fresh);
    return fresh;
  }

  close(id = this.active) {
    if (this._mode === 'tabs' && this.layout.find(id)?.kind === 'leaf') return null;
    return this._mutate(() => this.layout.close(id));
  }

  toggleFloat(id = this.active) {
    if (this._mode === 'tabs') return null;
    const w = this.layout.find(id);
    if (!w) return null;
    if (w.kind === 'float') {
      const beside = this._activeLeafId();
      return this._mutate(() => this.layout.tile(id, beside === id ? null : beside));
    }
    if (w.kind === 'leaf') return this._mutate(() => this.layout.float(id));
    return null;
  }

  // Swap the active window's pane with its neighbour's in `dir`; a floating
  // window is nudged instead.
  swapDir(dir) {
    const w = this.layout.find(this.active);
    if (!w) return false;
    if (w.kind === 'float') return this.nudge(dir);
    const rects = this.windowRects();
    const from = rects.find((r) => r.id === this.active);
    if (!from) return false;
    const to = neighbour(from.rect, rects.filter((r) => r.id !== this.active), dir);
    if (to == null) return false;
    // Focus follows the pane: the window you were in is now over there.
    const src = this.active;
    this._mutate(() => {
      this.layout.swapPanes(src, to);
      return to;
    });
    this._els.get(to)?.focus({ preventScroll: true });
    return true;
  }

  // --- live preview of a move ----------------------------------------------
  //
  // A move is shown before it is made. The pane being moved gives up its
  // place the moment the gesture starts, so the rest of the layout reflows
  // into it and you can see the room it is leaving behind; wherever it would
  // land it is drawn there at reduced opacity, so what you are looking at is
  // the result rather than a hint of it. Full opacity is the signal that it
  // has actually happened.
  //
  // Every step restores the snapshot and re-applies from scratch rather than
  // undoing the last one: a move is a removal plus an insertion, and
  // unpicking that in the general case is far more error-prone than simply
  // doing it again from a known state. Nothing is persisted until the move is
  // confirmed, and cancelling puts the tree back exactly, ids included.
  beginPreview(srcId) {
    const w = this.layout.find(srcId);
    if (!w || w.kind === 'split' || w.pane == null) return false;
    this._preview = { src: srcId, snap: this.layout.snapshot(), active: this.active, key: null, leaf: null };
    return true;
  }

  get previewing() {
    return !!this._preview;
  }

  // The leaf the moved pane is currently drawn in, if it is anywhere.
  get previewLeaf() {
    return this._preview?.leaf ?? null;
  }

  // Nowhere yet: the pane leaves the tree altogether and its room goes back
  // to the others. It is parked, so the ghost following the pointer (or the
  // banner, from the keyboard) is the only thing saying where it is.
  previewNone() {
    return this._applyPreview('none', () => {
      this.layout.close(this._preview.src);
      return null;
    });
  }

  previewInto(targetId, edge) {
    return this._applyPreview(`into:${targetId}:${edge}`, () =>
      this.layout.moveInto(this._preview.src, targetId, edge));
  }

  previewOutward(edge) {
    return this._applyPreview(`out:${edge}`, () => this.layout.moveOutward(this._preview.src, edge));
  }

  previewAcross(splitId, edge) {
    return this._applyPreview(`across:${splitId}:${edge}`, () =>
      this.layout.moveAcross(this._preview.src, splitId, edge));
  }

  previewSwap(targetId) {
    return this._applyPreview(`swap:${targetId}`, () => {
      // A swap frees no room — the other pane takes this one's place — so
      // the preview is both panes where they would end up, with the moved
      // one marked.
      this.layout.swapPanes(this._preview.src, targetId);
      return targetId;
    });
  }

  _applyPreview(key, fn) {
    const p = this._preview;
    if (!p) return false;
    if (p.key === key) return true;
    this.layout.restore(p.snap);
    const leaf = fn();
    // A move the model refuses (it would rebuild the same shape) previews as
    // the arrangement you already have, with nothing marked.
    p.leaf = leaf ?? null;
    if (leaf == null && key !== 'none') this.layout.restore(p.snap);
    p.key = key;
    this._version++;
    // The keyboard stays with the pane it is moving, so that after a commit
    // the focus is already where the pane went.
    if (p.leaf != null) this.active = p.leaf;
    this.apply();
    return true;
  }

  // Confirmed: keep the tree as it stands, drop the transparency, persist.
  commitPreview() {
    const p = this._preview;
    if (!p) return false;
    this._preview = null;
    this._syncPreview();
    this._commit();
    if (p.leaf != null) this._els.get(p.leaf)?.focus({ preventScroll: true });
    return true;
  }

  cancelPreview() {
    const p = this._preview;
    if (!p) return false;
    this._preview = null;
    this.layout.restore(p.snap);
    this.active = p.active;
    this._version++;
    this.apply();
    return true;
  }

  _syncPreview() {
    const leaf = this._preview?.leaf ?? null;
    for (const el of this._workspace.querySelectorAll('.tile[data-preview]')) {
      if (Number(el.dataset.tile) !== leaf) el.removeAttribute('data-preview');
    }
    if (leaf != null) this._els.get(leaf)?.setAttribute('data-preview', '');
  }

  // --- keyboard split-move, in two phases ----------------------------------
  //
  // One keystroke cannot say both which window to share and which half of it
  // to take, and reading the half from the direction travelled (what this did
  // at first) leaves the far half unreachable. So the gesture is held: while
  // Alt+Shift+S is down the direction keys walk a highlighted target from
  // window to window; on release, the direction keys move the pane's preview
  // between that window's halves, and Enter or Space settles it.
  //
  // The pane keeps its place through phase one and lifts out of the tree at
  // the first keystroke of phase two — not at the start. Freeing the room
  // straight away would be truer to the mouse, but it moves every window
  // before you have picked one, and picking is directional: with the gap
  // already closed, "the window to the right" is measured against a rectangle
  // that nothing occupies any more.
  //
  // The mouse says both things at once — which window the pointer is over and
  // which edge — so it needs none of the phases and is wired separately.
  beginSplitMove() {
    const w = this.layout.find(this.active);
    if (this._mode === 'tabs') return false;
    // A placeholder has nothing to move, and a floating window has no place
    // in the tree to be split into (Alt+T tiles it first).
    if (!w || w.kind !== 'leaf' || w.pane == null) return false;
    const src = this.active;
    const from = this.windowRects().find((r) => r.id === src);
    if (!from) return false;
    // Measured once and kept, for the same reason the drag keeps a drop map:
    // phase two rearranges the tiles the selection is being made among.
    this._splitMove = { src, from: from.rect, target: null, drops: this.windowRects() };
    return true;
  }

  // Phase one: walk the target. The first step goes from where the pane was,
  // each one after that from wherever the highlight has got to, so a
  // direction can be pressed repeatedly to cross the workspace.
  stepSplitTarget(dir) {
    const sm = this._splitMove;
    if (!sm) return false;
    const fromRect = sm.target == null
      ? sm.from
      : sm.drops.find((r) => r.id === sm.target)?.rect;
    if (!fromRect) return false;
    const cands = sm.drops.filter((r) => r.id !== sm.src && r.id !== sm.target);
    const to = neighbour(fromRect, cands, dir);
    if (to == null) return false;
    this._paintSplitTarget(to);
    sm.target = to;
    return true;
  }

  // Phase two: show the pane in one half of the chosen window. With no window
  // chosen — released without having picked one — the direction is read as an
  // edge of the workspace to move out to instead, which is the one move that
  // has no target by nature. Nothing is settled until confirmSplitMove.
  previewSplitSide(edge) {
    const sm = this._splitMove;
    if (!sm) return false;
    // The pane lifts out here, on the first side pressed.
    if (!this.previewing && !this.beginPreview(sm.src)) return false;
    this._paintSplitTarget(null);
    return sm.target == null ? this.previewOutward(edge) : this.previewInto(sm.target, edge);
  }

  confirmSplitMove() {
    if (!this._splitMove) return false;
    this._splitMove = null;
    // Nothing chosen yet: Enter on an untouched gesture should not commit the
    // pane into limbo.
    if (this.previewLeaf == null) return this.cancelPreview();
    return this.commitPreview();
  }

  cancelSplitMove() {
    if (!this._splitMove && !this.previewing) return false;
    this._paintSplitTarget(null);
    this._splitMove = null;
    return this.cancelPreview();
  }

  get splitMoveTarget() {
    return this._splitMove?.target ?? null;
  }

  // The highlight is the drag's own drop indicator, which already means
  // "this window" when it covers a whole tile.
  _paintSplitTarget(id) {
    const prev = this._splitMove?.target;
    if (prev != null && prev !== id) this._els.get(prev)?.removeAttribute('data-drop');
    if (id != null) this._els.get(id)?.setAttribute('data-drop', 'target');
  }

  // The toolbar's swap-splits button. Flips every split at once; when the
  // root direction is pinned in Settings the pin flips with it, or the one
  // split the eye goes to first would be the one that did not move.
  transpose() {
    if (this._mode === 'tabs') return false;
    if (this.layout.root.kind !== 'split') return false;
    if (this.override === 'side' || this.override === 'stack') {
      this.override = this.override === 'side' ? 'stack' : 'side';
      this._preTabsMode = this.override;
    }
    this._mutate(() => {
      this.layout.transpose();
      return this.active;
    });
    return true;
  }

  nudge(dir, step = FLOAT_NUDGE) {
    const w = this.layout.find(this.active);
    if (!w || w.kind !== 'float') return false;
    const dx = dir === 'left' ? -step : dir === 'right' ? step : 0;
    const dy = dir === 'up' ? -step : dir === 'down' ? step : 0;
    this.layout.moveFloat(w.id, dx, dy);
    this._geometry(false);
    this._onChange?.();
    this._commit();
    return true;
  }

  // --- drag a pane to another window --------------------------------------
  //
  // Two ways to start: a plain drag on a tile header's empty stretch
  // (.tile-grab), or Alt+drag anywhere on a tile or floating window. Alt is
  // claimed in the capture phase so the chart underneath never sees a
  // pointerdown and starts panning. After 4px of travel a ghost with the
  // pane's title follows the pointer and the window under it is marked as
  // the drop target. A floating window grabbed with Alt moves instead — it
  // has a position, and moving it is what dragging a window means.
  //
  // What releasing does depends on where in the target you release (see
  // _dropZone): the middle swaps the two panes, and anywhere else splits the
  // target and gives the pane the half you dropped on. Neither is a guess,
  // because the arrangement on screen is already the one you would get — the
  // pane is drawn in its new place, transparent, from the moment the drag
  // starts (see beginPreview).
  _wireSwapDrag() {
    this._workspace.addEventListener(
      'pointerdown',
      (e) => {
        if (!e.isPrimary || e.button !== 0 || this._drag) return;
        const tileEl = e.target.closest?.('.tile');
        if (!tileEl || !this._workspace.contains(tileEl)) return;
        const onGrab = !!e.target.closest('.tile-grab');
        const onControl = !!e.target.closest('button, select, input, textarea, .splitter, .float-resize');
        if (!onGrab && !(e.altKey && !onControl)) return;
        const id = Number(tileEl.dataset.tile);
        const win = this.layout.find(id);
        if (!win) return;
        // A floating window's grab already moves it (see _wireFloatMove);
        // only Alt on its body needs handling here.
        if (win.kind === 'float' && onGrab) return;
        if (win.kind === 'leaf' && !win.pane) return; // nothing to drag
        const ac = new AbortController();
        this._drag = {
          id,
          mode: win.kind === 'float' ? 'move' : 'swap',
          pointerId: e.pointerId,
          x0: e.clientX,
          y0: e.clientY,
          fx: win.x,
          fy: win.y,
          moved: false,
          ghost: null,
          drops: null,
          ac,
        };
        this.focus(id);
        if (e.altKey) {
          e.preventDefault();
          e.stopPropagation();
        }
        const opts = { signal: ac.signal };
        window.addEventListener('pointermove', (ev) => this._dragMove(ev), opts);
        window.addEventListener('pointerup', (ev) => this._dragEnd(ev, true), opts);
        window.addEventListener('pointercancel', (ev) => this._dragEnd(ev, false), opts);
      },
      true,
    );
  }

  _dragMove(e) {
    const d = this._drag;
    if (!d || e.pointerId !== d.pointerId) return;
    const dx = e.clientX - d.x0;
    const dy = e.clientY - d.y0;
    if (!d.moved) {
      if (Math.hypot(dx, dy) < 4) return;
      d.moved = true;
      document.body.style.userSelect = 'none';
      if (d.mode === 'swap') {
        const g = document.createElement('div');
        g.className = 'tile-ghost';
        const pane = this._panes.get(this.layout.find(d.id)?.pane);
        g.textContent = pane ? pane.title : '';
        document.body.appendChild(g);
        d.ghost = g;
        // From here the pane is in transit: it gives up its place, the rest
        // of the layout reflows into it, and it is drawn wherever it would
        // land at reduced opacity until this is released.
        this.beginPreview(d.id);
        this.previewNone();
        // The windows to drop into, measured once, with the pane already out
        // of the way. A fixed map rather than a hit test on the live DOM: the
        // preview moves the tiles about, so asking the document what is under
        // the pointer finds the layout the last preview produced and the
        // answer chases itself — a drop on one half becomes a swap becomes a
        // drop on the other, flickering, with the pointer standing still.
        d.drops = this.windowRects();
        d.dividers = this.dividerRects();
      }
      this._onChange?.(); // the banner switches to the drag context
    }
    e.preventDefault();
    if (d.mode === 'move') {
      const r = this._workspace.getBoundingClientRect();
      this.layout.setFloatRect(d.id, { x: d.fx + dx / Math.max(1, r.width), y: d.fy + dy / Math.max(1, r.height) });
      this._geometry(false);
      this._onChange?.();
      return;
    }
    d.ghost.style.left = `${e.clientX + 12}px`;
    d.ghost.style.top = `${e.clientY + 12}px`;
    // The line between two windows takes precedence over either of them: it
    // is a thin target sitting inside their edge zones, so the only way to
    // reach it is to test it first.
    const onLine = d.dividers.find((v) => {
      const r = v.rect;
      return e.clientX >= r.left - DIVIDER_GRAB && e.clientX < r.right + DIVIDER_GRAB
        && e.clientY >= r.top - DIVIDER_GRAB && e.clientY < r.bottom + DIVIDER_GRAB;
    });
    if (onLine) {
      this.previewAcross(onLine.id, this._dividerEdge(onLine, e.clientX, e.clientY));
      return;
    }
    // Last match wins: floating windows come after the tiles in the list and
    // are drawn over them.
    let over = null;
    for (const c of d.drops) {
      const r = c.rect;
      if (e.clientX >= r.left && e.clientX < r.right && e.clientY >= r.top && e.clientY < r.bottom) over = c;
    }
    if (!over) {
      this.previewNone();
      return;
    }
    const zone = this._dropZone(over, e.clientX, e.clientY);
    if (zone === 'swap') this.previewSwap(over.id);
    else this.previewInto(over.id, zone);
  }

  // The dividers between tiled windows, as a fixed map like windowRects().
  // Read from the DOM because a split's drawn direction is not always its
  // own — the root's can be pinned in Settings (see _rootDir) — and a band
  // dropped across it has to be perpendicular to the line that is actually
  // on screen.
  dividerRects() {
    const out = [];
    for (const sep of this._tiles.querySelectorAll('.split > .splitter')) {
      if (sep.offsetParent === null) continue;
      const r = sep.getBoundingClientRect();
      if (r.width === 0 || r.height === 0) continue;
      const split = sep.parentElement;
      out.push({
        id: Number(split.dataset.split),
        dir: split.dataset.dir,
        rect: { left: r.left, top: r.top, right: r.right, bottom: r.bottom },
      });
    }
    return out;
  }

  // Which side of the pair the pane takes: along a vertical line (two
  // windows side by side) the band goes above or below, chosen by how far
  // down the line the pointer is.
  _dividerEdge(v, x, y) {
    if (v.dir === 'row') {
      return (y - v.rect.top) / Math.max(1, v.rect.bottom - v.rect.top) < 0.5 ? 'up' : 'down';
    }
    return (x - v.rect.left) / Math.max(1, v.rect.right - v.rect.left) < 0.5 ? 'left' : 'right';
  }

  // 'swap', or the edge of the window the pane would land on. The centre
  // SWAP_ZONE of each axis swaps; outside it the nearest edge wins, which
  // makes splitting the easy thing to hit and swapping the deliberate one.
  //
  // A floating window always swaps: it is one window at one position, with
  // no tree inside it to split.
  _dropZone({ id, rect }, x, y) {
    if (this.layout.find(id)?.kind === 'float') return 'swap';
    const w = rect.right - rect.left;
    const h = rect.bottom - rect.top;
    if (w <= 0 || h <= 0) return 'swap';
    const u = (x - rect.left) / w;
    const v = (y - rect.top) / h;
    if (Math.abs(u - 0.5) < SWAP_ZONE / 2 && Math.abs(v - 0.5) < SWAP_ZONE / 2) return 'swap';
    const edges = [
      ['left', u],
      ['right', 1 - u],
      ['up', v],
      ['down', 1 - v],
    ];
    return edges.reduce((best, cur) => (cur[1] < best[1] ? cur : best))[0];
  }

  _dragEnd(e, drop) {
    const d = this._drag;
    if (!d || e.pointerId !== d.pointerId) return;
    const took = drop && d.moved && this.previewing && this.previewLeaf != null;
    this._finishDrag();
    if (d.mode === 'move') {
      if (d.moved) this._commit();
      return;
    }
    // The tree already shows the result; releasing over somewhere just keeps
    // it and takes the transparency off. Released over nothing — or on a move
    // the model refused — and it goes back the way it was.
    if (took) this.commitPreview();
    else this.cancelPreview();
  }

  _finishDrag() {
    const d = this._drag;
    if (!d) return;
    d.ac.abort();
    d.ghost?.remove();
    document.body.style.userSelect = '';
    this._drag = null;
    this._onChange?.();
  }

  // Escape while dragging: nothing moves.
  cancelDrag() {
    if (!this._drag) return false;
    const d = this._drag;
    const wasMove = d.mode === 'move' && d.moved;
    this._finishDrag();
    this.cancelPreview();
    if (wasMove) {
      this.layout.setFloatRect(d.id, { x: d.fx, y: d.fy });
      this._geometry(false);
      this._onChange?.();
    }
    return true;
  }

  // Alt+-/= and the Shift forms. For a tile: move the nearest split of
  // that orientation one step left/up (sign < 0) or right/down (sign > 0),
  // whichever side of it the tile is on. For a floating window, which has
  // no split: its own width/height.
  // The special resize (tiles.js edgeResize). One press pushes an edge out
  // past a border the pane does not own, restructuring so that the pane
  // alone gets the room; the opposite press hands it back. Ordinary
  // resizeActive below is untouched — it still moves a border and takes the
  // whole side with it, which is the right thing when the window across that
  // border is the same size as this one.
  edgeResizeActive(edge, out) {
    if (this._mode === 'tabs') return false;
    const w = this.layout.find(this.active);
    // A floating window has no borders in the tree to push against.
    if (!w || w.kind !== 'leaf' || w.pane == null) return false;
    // Every refusal inside edgeResize happens before it touches the tree, so
    // a refused press really is a no-op: no re-render, nothing written.
    const fresh = this.layout.edgeResize(this.active, edge, out);
    if (fresh == null) return false;
    this._version++;
    this.active = fresh;
    this.apply();
    this._commit();
    this._els.get(fresh)?.focus({ preventScroll: true });
    return true;
  }

  resizeActive(axis, sign) {
    const w = this.layout.find(this.active);
    if (!w) return false;
    if (w.kind === 'float') {
      this.layout.resizeFloat(w.id, axis === 'x' ? sign * FLOAT_RESIZE : 0, axis === 'y' ? sign * FLOAT_RESIZE : 0);
    } else if (this.layout.resize(w.id, axis, sign * RESIZE_STEP) == null) {
      return false;
    }
    this._geometry(this._mode === 'tabs');
    this._onChange?.();
    this._commit();
    return true;
  }
}

export { clamp };
