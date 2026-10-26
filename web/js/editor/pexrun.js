// Starting and stopping a PEX app from the editor. The Lua script buttons
// stay on their own path (script.draft / script.start / script.stop). A
// .pex file uses these instead, so the Lua sources on the device are never
// the launcher and are never written.

/** "pex/anker.m33.pex" -> "anker". Anything else is not the installed app file. */
export function pexNameOf(path) {
  const m = /^pex\/([A-Za-z0-9_-]+)\.[A-Za-z0-9]+\.pex$/.exec(path || '');
  return m ? m[1] : null;
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
 * path -> state for every app pex.list reports. A second instance is renamed
 * name#2 and has no file of its own, so it is left unmarked.
 */
export function pexRunningPaths(list) {
  if (!list) return {};
  const arch = list.arch || 'm33';
  const out = {};
  for (const m of list.modules || []) {
    if (!m?.name || !PEX_STATES.has(m.state) || String(m.name).includes('#')) continue;
    out[`pex/${m.name}.${arch}.pex`] = m.state;
  }
  return out;
}

/**
 * Load the app if it is not loaded, then start it. An app that is already
 * running is left alone: a second pex.start is an error.
 * Returns { started: true } or { started: false, state: 'running' }.
 */
export async function startPex(api, name) {
  let info = null;
  try {
    info = await api.call('pex.info', { name });
  } catch (e) {
    if (e.code !== 'not_loaded') throw e;
  }
  if (info?.state === 'running') return { started: false, state: 'running' };
  if (!info) await api.call('pex.load', { name });
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
