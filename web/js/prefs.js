// What the browser remembers between visits: the window arrangement, and a
// handful of UI preferences (vim in the editor, the sidebar). localStorage
// only -- nothing here is device state, and nothing here may break the page:
// a hand-edited, stale or foreign record degrades to the defaults.
//
// Taken from the km003c web app's prefs.js with its history removed. That
// file carries a cookie format, two migrations and a meter's chart settings;
// this app starts at v1 and has none of them. The one migration kept is the
// gateway's own: the old Explorer stored an unversioned {tree, floating}
// under the same key, and that tree format is exactly what Layout.fromJSON
// reads, so an old arrangement carries over (panes that no longer exist are
// dropped by fromJSON, as for any record).

import { Layout } from './tiles.js';

const STORE_KEY = 'btgw.layout';
const STORE_VERSION = 1;
const PREFS_KEY = 'btgw.prefs';

// Private windows and storage-blocked browsers throw rather than returning
// null. Every access goes through these, so the page works without storage.
function storageGet(key) {
  try {
    return localStorage.getItem(key);
  } catch {
    return null;
  }
}

function storageSet(key, value) {
  try {
    localStorage.setItem(key, value);
  } catch {
    // Out of quota or blocked: the arrangement is simply not remembered.
  }
}

function readJSON(key) {
  const raw = storageGet(key);
  if (!raw) return null;
  try {
    const rec = JSON.parse(raw);
    return rec && typeof rec === 'object' ? rec : null;
  } catch {
    return null;
  }
}

/* Returns { layout: Layout, mode, active }. `defaults` supplies the layout
   and mode when nothing usable is stored. */
export function readLayoutStore(defaults, knownPanes) {
  const rec = readJSON(STORE_KEY);
  // v1, or the old Explorer's unversioned record (no `v`, same tree shape).
  if (rec && (rec.v === STORE_VERSION || rec.v === undefined) && rec.tree !== undefined) {
    const layout = Layout.fromJSON({ tree: rec.tree, floating: rec.floating }, knownPanes);
    if (layout.leaves().length) {
      return {
        mode: typeof rec.mode === 'string' ? rec.mode : defaults.mode,
        layout,
        active: Number.isInteger(rec.active) ? rec.active : 0,
      };
    }
  }
  return { mode: defaults.mode, layout: defaults.layout(), active: 0 };
}

/* `state` is LayoutManager.state: { mode, tree, floating, active }. */
export function writeLayoutStore(state) {
  storageSet(STORE_KEY, JSON.stringify({
    v: STORE_VERSION,
    mode: state.mode,
    tree: state.tree,
    floating: state.floating,
    active: state.active,
  }));
}

// UI preferences, one small object. Unknown keys are kept, so a newer page
// and an older one sharing a browser do not erase each other's settings.
const PREF_DEFAULTS = {
  vim: true,              // vim keys in the editor
  sidebar: true,          // editor sidebar open
  sidebarWidth: 220,      // px
};

function touchFirst() {
  try {
    return typeof matchMedia === 'function' && matchMedia('(pointer: coarse)').matches;
  } catch {
    return false;
  }
}

export function readPrefs() {
  const rec = readJSON(PREFS_KEY) || {};
  const out = { ...PREF_DEFAULTS, ...rec };
  // Never chosen: off on a touch-first device, where vim's normal mode would
  // leave an on-screen keyboard with nothing to type into; on everywhere else.
  if (typeof rec.vim !== 'boolean') out.vim = !touchFirst();
  else if (typeof out.vim !== 'boolean') out.vim = PREF_DEFAULTS.vim;
  if (typeof out.sidebar !== 'boolean') out.sidebar = PREF_DEFAULTS.sidebar;
  if (!Number.isFinite(out.sidebarWidth) || out.sidebarWidth < 120 || out.sidebarWidth > 600) {
    out.sidebarWidth = PREF_DEFAULTS.sidebarWidth;
  }
  return out;
}

export function writePrefs(patch) {
  storageSet(PREFS_KEY, JSON.stringify({ ...(readJSON(PREFS_KEY) || {}), ...patch }));
}
