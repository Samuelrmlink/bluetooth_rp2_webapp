// Driver identity for the scanner. A manifest is a JSON file next to a
// module (pex/<name>.driver.json, or the same under sd/) or inside a Lua
// profile (lua/profiles/<name>/driver.json). It is not a host call: the
// scanner reads the files. Loaded and installed come from pex.list,
// pex.installed and script.list, which the window already has.
//
// match is OR of sets. A set passes when every rule in `all` passes.
// Rules: mfg_company (first two bytes of manufacturer data), uuid16,
// uuid128, name_contains. A rule with none of those keys fails closed.

import { scriptNameOf } from './editor/status.js';

const RULE_KEYS = ['mfg_company', 'uuid16', 'uuid128', 'name_contains'];

const hex = (s) => String(s ?? '').replace(/[^0-9a-f]/gi, '').toLowerCase();

/** What a parsed advertisement (adparse.js) offers a rule. */
export function factsFromAd(ad, dev = {}) {
  return {
    name: ad?.nameComplete || ad?.nameShort || dev?.name || '',
    company: ad?.mfg ? ad.mfg.company.toString(16).padStart(4, '0') : '',
    uuid16: [...(ad?.uuid16?.complete || []), ...(ad?.uuid16?.incomplete || [])],
    uuid128: [...(ad?.uuid128?.complete || []), ...(ad?.uuid128?.incomplete || [])],
  };
}

function rulePasses(rule, facts) {
  const keys = RULE_KEYS.filter((k) => rule?.[k] != null && rule[k] !== '');
  if (!keys.length) return false;
  for (const k of keys) {
    if (k === 'mfg_company' && hex(rule.mfg_company) !== hex(facts.company)) return false;
    if (k === 'uuid16' && !(facts.uuid16 || []).some((u) => hex(u) === hex(rule.uuid16))) return false;
    if (k === 'uuid128' && !(facts.uuid128 || []).some((u) => hex(u) === hex(rule.uuid128))) return false;
    if (k === 'name_contains' && !String(facts.name).toLowerCase().includes(String(rule.name_contains).toLowerCase())) return false;
  }
  return true;
}

/** One { all: [rules] } set. An empty set matches nothing. */
export function matchSet(set, facts) {
  const all = set?.all;
  if (!Array.isArray(all) || !all.length) return false;
  return all.every((r) => rulePasses(r, facts));
}

export function driverMatches(manifest, facts) {
  return (manifest?.match || []).some((set) => matchSet(set, facts));
}

export function matchingDrivers(manifests, facts) {
  return (manifests || []).filter((m) => driverMatches(m, facts));
}

/**
 * Whether Start should be offered. `state` holds the sets the scanner
 * filled from pex.list / pex.installed / script.list and the directory
 * listings: runningPex, runningLua, installed, loadedPex, pexFiles, luaFiles.
 */
export function driverStatus(manifest, state = {}) {
  const has = (set, v) => !!(v && state[set] && state[set].has(v));
  if (manifest?.kind === 'pex') {
    const name = manifest.id;
    const present = has('installed', name) || has('loadedPex', name) || has('pexFiles', manifest.file);
    return { running: has('runningPex', name), present, startName: name || null };
  }
  if (manifest?.kind === 'lua') {
    const startName = scriptNameOf(manifest.file || '');
    const present = !!(startName && (has('luaFiles', manifest.file)
      || has('luaFiles', `lua/${startName}.lua`)
      || has('luaFiles', `lua/${startName}.lua.gz`)));
    return { running: has('runningLua', startName), present, startName };
  }
  return { running: false, present: false, startName: null };
}

/** Cell text. A known driver is named even when its file is not on the device. */
export function driverLabel(matches, state) {
  return (matches || []).map((m) => {
    const st = driverStatus(m, state);
    const title = m.title || m.id;
    if (st.running) return `${title} · running`;
    if (!st.present) return `${title} · not on device`;
    return title;
  }).join(', ');
}

function take(text, seen, out) {
  let j;
  try { j = JSON.parse(text); } catch { return; }
  if (!j || typeof j.id !== 'string' || seen.has(j.id) || !Array.isArray(j.match)) return;
  seen.add(j.id);
  out.push(j);
}

async function readListing(fs, dir, { pexFiles, luaFiles, seen, manifests, manifest }) {
  const r = await fs.list(dir);
  if (!r?.ok) return;
  for (const f of r.files || []) {
    if (f.dir) continue;
    const path = `${dir}/${f.name}`;
    if (pexFiles && /\.pex$/i.test(f.name)) pexFiles.add(path);
    if (luaFiles && /\.lua(\.gz)?$/i.test(f.name)) luaFiles.add(path);
    if (manifest && /\.driver\.json$/i.test(f.name)) {
      const body = await fs.readText(path);
      if (body?.ok) take(body.text, seen, manifests);
    }
  }
}

/** A save or delete here should reload the manifests. Exact directory names
 *  count. A longer path has to keep the slash, so `pexfoo` does not match. */
const DRIVER_WATCH = ['pex', 'sd/pex', 'lua/profiles', 'sd/lua/profiles'];

export function driverWatchPath(path) {
  if (typeof path !== 'string' || !path) return false;
  const p = path.replace(/^\/+/, '');
  return DRIVER_WATCH.some((root) => p === root || p.startsWith(`${root}/`));
}

/**
 * Manifests on flash and, when `sd` is set, the card. One id wins, flash
 * first. Directory listings also record which .pex and top-level .lua files
 * exist, which is how Start knows the file is there.
 */
export async function loadDriverManifests(fs, { sd = false } = {}) {
  const manifests = [];
  const seen = new Set();
  const pexFiles = new Set();
  const luaFiles = new Set();
  const bags = { pexFiles, luaFiles, seen, manifests };
  await readListing(fs, 'pex', { ...bags, manifest: true });
  const prof = await fs.list('lua/profiles');
  if (prof?.ok) {
    for (const f of prof.files || []) {
      if (!f.dir) continue;
      const body = await fs.readText(`lua/profiles/${f.name}/driver.json`);
      if (body?.ok) take(body.text, seen, manifests);
    }
  }
  await readListing(fs, 'lua', { ...bags, pexFiles: null, manifest: false });
  if (sd) {
    await readListing(fs, 'sd/pex', { ...bags, manifest: true });
    const sdProf = await fs.list('sd/lua/profiles');
    if (sdProf?.ok) {
      for (const f of sdProf.files || []) {
        if (!f.dir) continue;
        const body = await fs.readText(`sd/lua/profiles/${f.name}/driver.json`);
        if (body?.ok) take(body.text, seen, manifests);
      }
    }
    await readListing(fs, 'sd/lua', { ...bags, pexFiles: null, manifest: false });
  }
  return { manifests, pexFiles, luaFiles };
}
