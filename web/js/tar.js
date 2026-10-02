// Tar archives, for restoring a folder (or the whole filesystem) the device
// streamed out as GET /api/tar/<folder> -- or one made anywhere else.
//
// Plain tar only: no .tar.gz. A .gz file INSIDE an archive is just a file,
// carried and written back exactly as stored (js/app.js.gz stays
// js/app.js.gz). Read: ustar (name + prefix), GNU long names ('L', 'K') and
// pax headers ('x' per entry, 'g' global: path and size), which is what GNU
// tar, bsdtar and Python's tarfile write. Ignored on purpose: permissions,
// owner and group, and times -- LittleFS has none of them, so a file from a
// Linux PC with mode 0600 or 0755, owned by anyone, restores the same.
// Symlinks, hard links, devices and FIFOs have nothing to become on the
// device and are reported as skipped. Pure (no DOM), tested.

const BLOCK = 512;

// The device's limits (src/fs/fs.h): FS_PATH_MAX 128 is the whole path,
// terminator included; FS_FILE_MAX 128 KB.
export const PATH_MAX = 127;
export const FILE_MAX = 128 * 1024;

const dec = new TextDecoder();

function str(b, off, len) {
  const s = b.subarray(off, off + len);
  const z = s.indexOf(0);
  return dec.decode(z < 0 ? s : s.subarray(0, z));
}

// Octal, space/NUL padded; or GNU base-256 for big values (top bit set).
function num(b, off, len) {
  if (b[off] & 0x80) {
    let v = b[off] & 0x3f;
    for (let i = 1; i < len; i++) v = v * 256 + b[off + i];
    return v;
  }
  const t = str(b, off, len).trim();
  return t ? parseInt(t, 8) : 0;
}

function checksumOk(h) {
  let sum = 0;
  for (let i = 0; i < BLOCK; i++) sum += i >= 148 && i < 156 ? 32 : h[i];
  return sum === num(h, 148, 8);
}

// pax records: "<len> key=value\n" ...
function pax(b) {
  const out = {};
  const s = dec.decode(b);
  let i = 0;
  while (i < s.length) {
    const sp = s.indexOf(' ', i);
    if (sp < 0) break;
    const n = parseInt(s.slice(i, sp), 10);
    if (!(n > 0)) break;
    const rec = s.slice(sp + 1, i + n - 1);          // without the trailing \n
    const eq = rec.indexOf('=');
    if (eq > 0) out[rec.slice(0, eq)] = rec.slice(eq + 1);
    i += n;
  }
  return out;
}

const KIND = { '0': 'file', '\0': 'file', '': 'file', '7': 'file', '5': 'dir',
               '1': 'hard link', '2': 'symlink', '3': 'character device', '4': 'block device', '6': 'FIFO' };

// bytes (Uint8Array) -> { entries: [{ path, dir, data? }], skipped: [{ path, why }] }.
// Paths as named in the archive, normalised: no leading "./", folders without
// their trailing "/". Throws on something that is not a tar at all.
export function parseTar(bytes) {
  const b = bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes);
  const entries = [], skipped = [];
  let off = 0, longName = null, local = {}, global = {};
  let sawEnd = false;
  while (off + BLOCK <= b.length) {
    const h = b.subarray(off, off + BLOCK);
    if (h.every((x) => x === 0)) { sawEnd = true; break; }
    if (!checksumOk(h)) {
      if (!entries.length && !skipped.length) throw new Error('not a tar archive (no valid header at the start)');
      throw new Error(`damaged archive: bad header checksum at byte ${off}`);
    }
    const type = String.fromCharCode(h[156]);
    let size = num(h, 124, 12);
    const isUstar = str(h, 257, 6).startsWith('ustar');
    let name = str(h, 0, 100);
    if (isUstar && str(h, 345, 155)) name = `${str(h, 345, 155)}/${name}`;
    const dataOff = off + BLOCK;
    const next = dataOff + Math.ceil(size / BLOCK) * BLOCK;
    if (type === 'L' || type === 'K' || type === 'x' || type === 'g') {
      if (dataOff + size > b.length) throw new Error('archive ends in the middle of an entry');
      const data = b.subarray(dataOff, dataOff + size);
      if (type === 'L') longName = str(data, 0, data.length);
      else if (type === 'x') local = pax(data);
      else if (type === 'g') global = { ...global, ...pax(data) };
      off = next;                                    // 'K' (a long link target): not needed
      continue;
    }
    const meta = { ...global, ...local };
    if (meta.path) name = meta.path;
    if (longName != null) name = longName;
    if (meta.size != null && /^\d+$/.test(meta.size)) size = Number(meta.size);
    longName = null; local = {};
    const realNext = dataOff + Math.ceil(size / BLOCK) * BLOCK;

    const path = name.replace(/^(\.\/)+/, '').replace(/\/+$/, '');
    const kind = KIND[type] ?? `type '${type}'`;
    const dir = kind === 'dir' || (kind === 'file' && name.endsWith('/') && size === 0);
    if (!path || path === '.') {
      // the archive's own "./" -- nothing to make
    } else if (dir) {
      entries.push({ path, dir: true });
    } else if (kind === 'file') {
      if (dataOff + size > b.length) throw new Error(`archive ends in the middle of ${path}`);
      entries.push({ path, dir: false, data: b.slice(dataOff, dataOff + size) });
    } else {
      skipped.push({ path, why: `${kind} (not a file)` });
    }
    off = realNext;
  }
  if (!sawEnd && !entries.length && !skipped.length) throw new Error('not a tar archive');
  // Every tar writer ends with zero blocks; without them the file was cut
  // short (an interrupted download), and restoring part of it is worse than
  // restoring none.
  if (!sawEnd) throw new Error('archive is incomplete: it ends in the middle of an entry, or without its end marker');
  return { entries, skipped };
}

// Why a destination path cannot be written, or null when it can.
export function badPath(p) {
  if (!p) return 'empty name';
  if (p.startsWith('/')) return 'an absolute path';
  const parts = p.split('/');
  if (parts.some((s) => s === '..')) return 'leads outside the folder (..)';
  if (parts.some((s) => s === '' || s === '.')) return 'an empty path segment';
  if (new TextEncoder().encode(p).length > PATH_MAX) return `path longer than ${PATH_MAX} bytes`;
  if (/[\0\\]/.test(p)) return 'a character the device cannot name';
  return null;
}

// What restoring `parsed` into folder `target` ('' = the root) would do.
// existing: Set of file paths already on the device (from listings).
// Nothing is ever deleted: archive files are added or overwrite, everything
// else stays. cache/ is skipped (the board regenerates it from the sources).
export function planRestore(parsed, target, existing = new Set()) {
  const dirs = new Set(), adds = [], overwrites = [], skipped = [...parsed.skipped];
  const seen = new Map();
  const dest = (p) => (target ? `${target}/${p}` : p);
  for (const e of parsed.entries) {
    const to = dest(e.path);
    const why = badPath(e.path) || badPath(to);
    if (why) { skipped.push({ path: e.path, why }); continue; }
    if (to === 'cache' || to.startsWith('cache/')) { skipped.push({ path: e.path, why: 'cache/ (rebuilt on the device)' }); continue; }
    if (e.dir) { dirs.add(to); continue; }
    if (e.data.length > FILE_MAX) { skipped.push({ path: e.path, why: `over the device's ${FILE_MAX / 1024} KB file limit` }); continue; }
    seen.set(to, { path: to, data: e.data });       // a later copy of the same name wins, as tar -x does
  }
  for (const w of seen.values()) (existing.has(w.path) ? overwrites : adds).push(w);
  // A folder needs making only if no file will make it anyway (a PUT makes
  // its parents); empty folders are what the explicit mkdir is for.
  const filled = new Set();
  for (const w of seen.values()) {
    const parts = w.path.split('/');
    for (let i = 1; i < parts.length; i++) filled.add(parts.slice(0, i).join('/'));
  }
  const mkdirs = [...dirs].filter((d) => !filled.has(d) && ![...dirs].some((o) => o.startsWith(d + '/')) && !existing.has(d)).sort();
  const total = [...seen.values()].reduce((n, w) => n + w.data.length, 0);
  return { mkdirs, adds, overwrites, skipped, bytes: total, folders: dirs.size };
}

// The folders whose listings tell what already exists: every parent of a file.
export function foldersOf(parsed, target) {
  const out = new Set();
  for (const e of parsed.entries) {
    if (e.dir) continue;
    const to = target ? `${target}/${e.path}` : e.path;
    if (badPath(e.path) || badPath(to)) continue;
    const i = to.lastIndexOf('/');
    out.add(i < 0 ? '' : to.slice(0, i));
  }
  return [...out].sort();
}
