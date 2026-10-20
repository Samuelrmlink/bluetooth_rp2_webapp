// Search in files: the editor sidebar's Search tab (editor/searchpanel.js).
// The query, the matching and the walk over the device are here, pure and
// tested; reading goes through the caller's functions (fs.list / fs.readText,
// and the editor's open buffers, which win over the device's copy).

// Folders that are the gateway's own state or regenerable, never searched
// unless asked for by name: compiled bytecode, firmware images, counters.
const SKIP_DIRS = /^(cache|ota|var|sd)(\/|$)/;   // sd: the card (search it by starting there)
// Files that are not text: bytecode, binaries, backups, archives, bitmaps.
const SKIP_FILES = /\.(luac|bin|bak|tar|png|jpe?g|gif|webp|ico)(\.gz)?$/i;

export const MAX_MATCHES = 2000;
export const MAX_PER_FILE = 200;

// { q, matchCase, word, regex } -> { re } or { error }. A plain query is
// literal: "a.b" finds a.b, not axb.
export function compileQuery({ q, matchCase = false, word = false, regex = false }) {
  if (!q) return { error: '' };
  let src = regex ? q : q.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
  if (word) src = `\\b(?:${src})\\b`;
  try {
    const re = new RegExp(src, matchCase ? 'g' : 'gi');
    if (re.test('')) return { error: 'the pattern matches nothing at all (an empty match)' };
    re.lastIndex = 0;
    return { re };
  } catch (e) {
    return { error: e.message.replace(/^Invalid regular expression: /, '') };
  }
}

// Every match in a text: [{ line (1-based), col (0-based), at (offset), len, text }]
// where text is the whole line it is on.
export function searchText(text, re, max = MAX_PER_FILE) {
  const out = [];
  const starts = [0];
  for (let i = text.indexOf('\n'); i >= 0; i = text.indexOf('\n', i + 1)) starts.push(i + 1);
  const lineOf = (pos) => {
    let lo = 0, hi = starts.length - 1;
    while (lo < hi) {
      const mid = (lo + hi + 1) >> 1;
      if (starts[mid] <= pos) lo = mid; else hi = mid - 1;
    }
    return lo;
  };
  re.lastIndex = 0;
  for (let m = re.exec(text); m && out.length < max; m = re.exec(text)) {
    if (m[0].length === 0) { re.lastIndex++; continue; }
    const li = lineOf(m.index);
    const end = li + 1 < starts.length ? starts[li + 1] - 1 : text.length;
    out.push({ line: li + 1, col: m.index - starts[li], at: m.index, len: m[0].length, text: text.slice(starts[li], end) });
  }
  return out;
}

// Is this file searched? (A folder given by the user is searched whatever it is.)
export function searchable(path, root = '') {
  if (SKIP_FILES.test(path)) return false;
  if (!root && SKIP_DIRS.test(path)) return false;
  return true;
}

// Walk `root` ('' = everything but the skipped folders), reading each
// searchable file and reporting its matches as it goes.
//   list(dir) -> { ok, files: [{ name, dir, size }] }
//   read(path, size) -> { ok, text } (the caller caches and prefers buffers)
//   onFile(path, matches)   for every file with a match
//   isCancelled()           a newer search started: stop
// Returns { files, searched, matches, truncated, errors }.
export async function searchFiles({ list, read, root = '', re, onFile, isCancelled = () => false, onProgress = () => {}, concurrency = 1 }) {
  const paths = [];
  const walk = async (dir) => {
    if (isCancelled()) return;
    const r = await list(dir);
    if (!r.ok) throw new Error(r.error || `could not list ${dir || 'the root'}`);
    for (const f of r.files) {
      const p = dir ? `${dir}/${f.name}` : f.name;
      if (f.dir) { if (root || !SKIP_DIRS.test(p + '/')) await walk(p); }
      else if (searchable(p, root)) paths.push({ path: p, size: f.size });
    }
  };
  await walk(root);
  const res = { files: 0, searched: 0, matches: 0, truncated: false, errors: [] };
  // Reads in flight at once, results taken in order. 1: the board serves
  // file reads one at a time (measured: 3 in flight was no faster, 12-13 s
  // for 113 files), so more only queue there. The panel's cache makes a
  // second search take under a second.
  const ahead = new Map();
  const fetchAt = (i) => {
    if (i < paths.length && !ahead.has(i)) ahead.set(i, read(paths[i].path, paths[i].size));
  };
  for (let i = 0; i < paths.length; i++) {
    const { path } = paths[i];
    if (isCancelled()) break;
    for (let k = i; k < i + concurrency; k++) fetchAt(k);
    onProgress(res.searched, paths.length, path);
    const r = await ahead.get(i);
    ahead.delete(i);
    res.searched++;
    if (!r.ok) { res.errors.push(`${path}: ${r.error}`); continue; }
    if (r.text.includes('\0')) continue;                 // binary after all
    const m = searchText(r.text, re, Math.min(MAX_PER_FILE, MAX_MATCHES - res.matches));
    if (!m.length) continue;
    res.files++;
    res.matches += m.length;
    onFile(path, m);
    if (res.matches >= MAX_MATCHES) { res.truncated = true; break; }
  }
  onProgress(res.searched, paths.length, '');
  return res;
}
