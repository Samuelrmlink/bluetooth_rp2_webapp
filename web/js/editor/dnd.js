// Drag and drop for the editor's file list, and deleting a folder with
// everything in it. The decisions are here, pure and tested; the window
// (windows/editor.js) does the events and the requests.
//
//   in      files and folders dragged from the computer: uploaded where they
//           are dropped (a folder row: into it; a file row: beside it). A
//           .tar dropped by itself is EXTRACTED there, through the same
//           preview as Restore archive -- a .tar inside a dropped folder is
//           just a file and is uploaded as one.
//   out     a row dragged out of the browser (Chrome, Edge): the file, or a
//           folder as a .tar the device streams
//   within  a row dropped on a folder row: moved there

import { badPath, FILE_MAX } from '../tar.js';

// FAT32's file limit, for drops onto the SD card (sd/...). Here rather than
// from fs.js, which needs a page: this module is pure and unit-tested.
const SD_FILE_MAX = 4 * 1024 * 1024 * 1024 - 1;

export const isTar = (name) => /\.tar$/i.test(name);

// What a drop would do. items: [{ path, file?, dir? }] -- the dropped files
// and folders by their path within the drop ("photos/a.jpg"), as
// readDrop() collects them. target: the folder ('' = the root).
// existing: Set of file paths already on the device there.
export function planDrop(items, target, existing = new Set()) {
  const join = (p) => (target ? `${target}/${p}` : p);
  const out = { tars: [], uploads: [], overwrites: [], dirs: [], skipped: [] };
  for (const it of items) {
    if (!it.dir && !it.path.includes('/') && isTar(it.path)) { out.tars.push(it.file); continue; }
    const dest = join(it.path);
    const why = badPath(it.path) || badPath(dest);
    if (why) { out.skipped.push({ path: it.path, why }); continue; }
    if (dest === 'cache' || dest.startsWith('cache/')) { out.skipped.push({ path: it.path, why: 'cache/ is rebuilt by the device' }); continue; }
    if (it.dir) { out.dirs.push(dest); continue; }
    const max = /^sd(\/|$)/.test(dest) ? SD_FILE_MAX : FILE_MAX;
    if (it.file.size > max) { out.skipped.push({ path: it.path, why: max === FILE_MAX ? `over the device's ${FILE_MAX / 1024} KB file limit` : 'over FAT32\'s 4 GB file limit' }); continue; }
    out.uploads.push({ dest, file: it.file });
    if (existing.has(dest)) out.overwrites.push(dest);
  }
  // a folder that a file will make anyway needs no mkdir of its own
  const made = new Set(out.uploads.flatMap((u) => u.dest.split('/').slice(0, -1).map((_, i, a) => a.slice(0, i + 1).join('/'))));
  out.dirs = out.dirs.filter((d) => !made.has(d) && !out.dirs.some((o) => o.startsWith(d + '/')));
  return out;
}

// The folder a drop on this row means: the folder itself, or the file's folder.
export function dropDir(node) {
  if (!node) return '';
  return node.dir ? node.path : node.path.split('/').slice(0, -1).join('/');
}

// Where moving `src` into `folder` puts it, or null when it cannot go there
// (into itself or its own subfolder, or nowhere new).
export function moveDest(src, folder) {
  const name = src.split('/').pop();
  const dest = folder ? `${folder}/${name}` : name;
  if (dest === src || folder === src || folder.startsWith(src + '/')) return null;
  if (badPath(dest)) return null;
  return dest;
}

// Every file and folder under `path`, deepest first, so deleting in this
// order empties each folder before it is removed. list(dir) -> { ok, files }.
export async function treeOf(list, path) {
  const files = [], dirs = [];
  const walk = async (p) => {
    const r = await list(p);
    if (!r.ok) throw new Error(r.error || `could not list ${p}`);
    for (const f of r.files) {
      const child = `${p}/${f.name}`;
      if (f.dir) await walk(child);
      else files.push({ path: child, size: f.size || 0 });
    }
    dirs.push(p);                                   // after its contents
  };
  await walk(path);
  return { files, dirs };
}

// Delete a folder and everything in it: files first, then the folders from
// the deepest up. Stops at the first refusal and says how far it got --
// never a silent half-done delete.
export async function deleteTree(list, remove, path, onProgress = () => {}) {
  const { files, dirs } = await treeOf(list, path);
  const total = files.length + dirs.length;
  let done = 0;
  for (const p of [...files.map((f) => f.path), ...dirs]) {
    const r = await remove(p);
    if (!r.ok && r.status !== 404) {
      const e = new Error(`${p}: ${r.error || 'refused'} (${done} of ${total} removed)`);
      e.done = done;
      throw e;
    }
    onProgress(++done, total, p);
  }
  return { files: files.length, dirs: dirs.length };
}

// Folders whose loss takes something essential with it: the gateway's
// settings, its scripts, the web UI itself. Deleting one asks for its name
// to be typed, not just a click.
export function isVital(path) {
  return /^(cfg|lua|js|css|ota|var|sd)$/.test(path);
}

// --- the browser side of a drop -------------------------------------------------

// The dropped things, taken DURING the drop event (a DataTransfer is empty
// once the event returns): folder-aware entries where the browser has them.
export function dropEntries(dt) {
  const out = [];
  for (const it of dt?.items || []) {
    if (it.kind !== 'file') continue;
    out.push(it.webkitGetAsEntry?.() || it.getAsFile());
  }
  if (!out.length) for (const f of dt?.files || []) out.push(f);
  return out.filter(Boolean);
}

// Entries -> [{ path, file } | { path, dir: true }], folders walked.
export async function readDrop(entries) {
  const items = [];
  const fileOf = (e) => new Promise((ok, bad) => e.file(ok, bad));
  const readAll = async (dir) => {
    const reader = dir.createReader();
    const all = [];
    for (;;) {
      const batch = await new Promise((ok, bad) => reader.readEntries(ok, bad));
      if (!batch.length) return all;          // readEntries hands them over ~100 at a time
      all.push(...batch);
    }
  };
  const walk = async (e, prefix) => {
    const path = prefix ? `${prefix}/${e.name}` : e.name;
    if (e.isFile) { items.push({ path, file: await fileOf(e) }); return; }
    if (e.isDirectory) {
      items.push({ path, dir: true });
      for (const c of await readAll(e)) await walk(c, path);
    }
  };
  for (const e of entries) {
    if (e instanceof File) items.push({ path: e.name, file: e });
    else await walk(e, '');
  }
  return items;
}
