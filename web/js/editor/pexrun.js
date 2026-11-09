// Starting and stopping a PEX app from the editor. The Lua script buttons
// stay on their own path (script.draft / script.start / script.stop). A
// .pex file uses these instead, so the Lua sources on the device are never
// the launcher and are never written.

// A module's own file sits in one of two homes: pex/<name>.<arch>.pex on
// LittleFS (older images), or sd/pex/<name>.<arch>.pex on the card. Only
// the top of each folder counts: sd/pex/good/ holds verified copies, and a
// build directory holds build outputs.
const HOME = /^(sd\/)?pex\/([A-Za-z0-9_-]+)\.[A-Za-z0-9]+\.pex$/;

/** "pex/anker.m33.pex" or "sd/pex/anker.m33.pex" -> "anker". Anything else is not an app's own file. */
export function pexNameOf(path) {
  const m = HOME.exec(path || '');
  return m ? m[2] : null;
}

/** True for sd/pex/<name>.<arch>.pex: the home file is on the card. */
export function pexHomeOnSd(path) {
  const m = HOME.exec(path || '');
  return !!(m && m[1]);
}

/** Both homes a module's file can have, LittleFS first. */
export function pexHomePaths(name, arch = 'm33') {
  if (!name) return [];
  return [`pex/${name}.${arch}.pex`, `sd/pex/${name}.${arch}.pex`];
}

/** Basename of name.arch.pex, including a copy under sd/. A .gz name is not one. */
export function pexFileName(path) {
  const base = String(path || '').split('/').pop();
  const m = /^([A-Za-z_][A-Za-z0-9_-]*)\.[A-Za-z0-9]+\.pex$/i.exec(base || '');
  return m ? m[1] : null;
}

/** A .pex the file list should not open as text, including one on the card and a gzipped copy. */
export function isPexFile(path) {
  return /\.pex(\.gz)?$/i.test(path || '');
}

const PEX_STATES = new Set(['loaded', 'running', 'stopped', 'exited', 'faulted']);

/**
 * path -> state for every app pex.list reports, under both homes (pex/ and
 * sd/pex/) so whichever file the tree shows is marked. A second instance is
 * renamed name#2 and has no file of its own, so it is left unmarked.
 */
export function pexRunningPaths(list) {
  if (!list) return {};
  const arch = list.arch || 'm33';
  const out = {};
  for (const m of list.modules || []) {
    if (!m?.name || !PEX_STATES.has(m.state) || String(m.name).includes('#')) continue;
    for (const p of pexHomePaths(m.name, arch)) out[p] = m.state;
  }
  return out;
}

/**
 * Load the app if it is not loaded, then start it. An app that is already
 * running is left alone: a second pex.start is an error.
 * pex.load {name} uses the installed partition copy, then the LittleFS file.
 * When neither exists it answers not_found; `loadSd` (when given) then loads
 * the module from its file on the card, and the start follows.
 * Returns { started: true } or { started: false, state: 'running' }.
 */
export async function startPex(api, name, { loadSd = null } = {}) {
  let info = null;
  try {
    info = await api.call('pex.info', { name });
  } catch (e) {
    if (e.code !== 'not_loaded') throw e;
  }
  if (info?.state === 'running') return { started: false, state: 'running' };
  if (!info) {
    try {
      await api.call('pex.load', { name });
    } catch (e) {
      if (e?.code !== 'not_found' || !loadSd) throw e;
      await loadSd();
    }
  }
  await api.call('pex.start', { name });
  return { started: true };
}

/**
 * Stop, then unload. Stop leaves the module loaded (its ops stay registered).
 * Unload frees it. The firmware tells Lua first, so a module Lua loaded can
 * still go. If stop fails, the module is left as it was. If unload fails,
 * the stop has already happened.
 * Returns { stopped: true, unloaded: true } or
 * { stopped: true, unloaded: false, error }.
 */
export async function stopPex(api, name) {
  await api.call('pex.stop', { name });
  try {
    await api.call('pex.unload', { name });
  } catch (e) {
    const error = (e && e.message) || String(e);
    return { stopped: true, unloaded: false, error };
  }
  return { stopped: true, unloaded: true };
}
