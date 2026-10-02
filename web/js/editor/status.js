// What the editor shows about each file: saved or modified, and whether the
// Lua arena is running it -- the rules, kept DOM-free so they are tested.
//
// Four states, VS Code-style:
//   clean      on LittleFS, the buffer (if open) matches what was loaded from it
//   modified   the open buffer differs from what is on the device -- unsaved
//   loaded     the arena is running this file, and it matches flash
//   stale      the arena is running this file, but not what flash holds now:
//              it was edited and saved since, or it runs from a RAM draft
//
// "modified" is the buffer's business and is decided in the browser.
// "loaded"/"stale" come from the firmware's script.loaded, which reports for
// every running script and resident module the hash of what it compiled and
// the hash of its file now (see src/lua/lrt.c).

// The file a loaded unit came from. Firmware 1.95+ says (`path`: x.lua or
// x.lua.gz, whichever it compiled); otherwise the firmware's unit_path(),
// mirrored: scripts are <dir>/<name>.lua, a module's dots are directories,
// and <dir> is staged/ or lua/ by tier. A draft has no file; it is attributed
// to where the script's committed file is, because that is the file you edit.
export function unitPath(u) {
  if (u.path) return u.path;
  const rel = u.kind === 'module' ? u.name.replace(/\./g, '/') : u.name;
  return `${u.tier === 'staged' ? 'staged' : 'lua'}/${rel}.lua`;
}

// path -> { state: 'loaded'|'stale', tier, hash }, from a script.loaded reply.
export function arenaMarks(units) {
  const out = new Map();
  for (const u of units || []) {
    const state = u.tier === 'draft' || u.now == null || u.hash !== u.now ? 'stale' : 'loaded';
    const p = unitPath(u);
    // Two units on one path (a script and a module of the same name cannot
    // be, but be safe): stale wins, since it is the one that needs acting on.
    if (out.get(p)?.state !== 'stale') out.set(p, { state, tier: u.tier, hash: u.hash });
  }
  return out;
}

// The arena state to SHOW for a file, given what the editor knows about it.
//
// The firmware cannot compare a draft with anything -- a draft has no file,
// so it reports `now: null` -- and on its own that marks every draft as
// changed. But the editor knows the hash of the file as it last read or
// saved it (`savedHash`). If the draft running is byte-for-byte that file,
// what runs IS what is on flash: the everyday F5-then-Ctrl+S sequence, which
// would otherwise stay amber for good.
export function shownArena(mark, savedHash) {
  if (!mark) return undefined;
  if (mark.state === 'stale' && mark.tier === 'draft' && savedHash && mark.hash === savedHash) return 'loaded';
  return mark.state;
}

// The top-level script a path runs as, or null: lua/x.lua and staged/x.lua
// are the script "x" (lua/x.lua.gz too: stored compressed, the same
// script); anything deeper is a module and cannot be started.
export function scriptNameOf(path) {
  const m = /^(?:lua|staged)\/([A-Za-z0-9_-]+)\.lua(?:\.gz)?$/.exec(path);
  return m ? m[1] : null;
}

// After saving lua/<name>.lua, is something else what actually runs? The
// firmware resolves draft > staged > committed, so a staged copy or a RAM
// draft silently wins over the file just written. Returns the shadowing
// tier or null.
export function shadowedBy(path, list) {
  const name = scriptNameOf(path);
  if (!name || !path.startsWith('lua/')) return null;
  const slot = (list?.scripts || []).find((s) => s.name === name);
  if (slot?.draft) return 'draft';
  if ((list?.files || []).some((f) => f.name === name && f.tier === 'staged')) return 'staged';
  return null;
}
