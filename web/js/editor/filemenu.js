// What a file or folder's menu offers in the editor sidebar. Pure: the
// window turns each { id } into the action (editor.js _fileAct).
//
//   file     Open (if it is not the open one), Download, Store compressed /
//            uncompressed, Restore .bak (when there is one), Copy to…,
//            Rename / move…, Delete…; a script (lua/x.lua or staged/x.lua)
//            also Start, Stop, Commit to lua/, Revert to the next tier; a
//            PEX app (pex/<name>.<arch>.pex) also Start, Stop, Install,
//            Uninstall (only when that name is in the PEX partition), and
//            PEX partition…. A .pex elsewhere, including a build directory
//            on the SD card, offers Install (the card path calls pex.install
//            on that sd: path) and PEX partition…. Start calls pex.start.
//            Stop calls pex.stop and then pex.unload, not Lua.
//   folder   New file here…, Upload here…, Upload archive here…, Download
//            as .tar, Calculate size, Refresh, Copy to…, Rename / move…,
//            and Delete… once it is known to be empty. The card's own
//            top folder has none of copy, rename, or delete.
//
// The path itself is added by the menu as the last item, and copies.
import { isGz, canCompress, imageKind } from '../gz.js';

const onSd = (p) => /^sd(\/|$)/.test(p || '');

export function fileMenuItems(node, { isOpen = false, script = null, pex = null, empty = false, hasBak = false, card = false, installed = false, builds = null, picked = 0, inPicked = false, selected = false } = {}) {
  const pick = { id: 'select', label: selected ? 'Deselect' : 'Select' };
  // Several rows, and the cursor is one of them: the set is what the menu
  // acts on. Select stays so a finger can take a row back out.
  if (picked > 1 && inPicked) {
    return [
      pick,
      { id: 'tar-set', label: 'Download as archive' },
      { id: 'delete', label: 'Delete…', danger: true },
    ];
  }
  if (node.dir) {
    // card: the SD card's own top-level folder (the editor knows it from fs.js)
    const out = [
      { id: 'new', label: 'New file here…' },
      { id: 'mkdir', label: 'New folder here…' },
      { id: 'upload', label: 'Upload here…' },
      { id: 'untar', label: 'Upload archive here… (.tar)' },
      // the device streams tar from flash only
      ...(onSd(node.path) ? [] : [{ id: 'tar', label: 'Download as .tar' }]),
      // keep: it answers in the menu (the info line), so the menu stays open
      { id: 'size', label: 'Calculate size', keep: true },
      { id: 'refresh', label: 'Refresh' },
      ...(card ? [{ id: 'sd-settings', label: 'SD card settings…' }] : [
        { id: 'copy', label: 'Copy to…' },
        { id: 'rename', label: 'Rename / move…' },
      ]),
    ];
    if (card) { out.push(pick); return out; }   // the card itself: no rename, no delete (format it in Settings)
    // an empty folder goes in one step; any other with everything in it,
    // after saying how much that is (and, for the gateway's own folders,
    // asking for the name to be typed)
    out.push(pick);
    out.push(empty ? { id: 'delete', label: 'Delete folder…', danger: true }
      : { id: 'delete', label: 'Delete folder and everything in it…', danger: true });
    return out;
  }
  const out = [];
  // A Pexfile lists Build, Build and run, and Build all before Open.
  for (const it of builds || []) out.push(it);
  if (!isOpen) out.push({ id: 'open', label: 'Open' });
  if (script) {
    out.push({ id: 'start', label: `Start ${script}` });
    out.push({ id: 'stop', label: `Stop ${script}` });
    out.push({ id: 'commit', label: 'Commit to lua/' });
    out.push({ id: 'revert', label: 'Revert to the next tier' });
  } else if (pex) {
    out.push({ id: 'pex-start', label: `Start ${pex}` });
    out.push({ id: 'pex-stop', label: `Stop ${pex}` });
    out.push({ id: 'pex-install', label: `Install ${pex}` });
    // Uninstall only this name, and only when the partition already has it.
    // A running copy answers busy; the item stays so that answer is visible.
    if (installed) out.push({ id: 'pex-uninstall', label: `Uninstall ${pex}` });
    out.push({ id: 'pex-mark', label: `Mark ${pex} verified` });
    out.push({ id: 'pex-restore', label: `Restore verified ${pex}` });
    out.push({ id: 'pex-partition', label: 'PEX partition…' });
  } else if (/\.pex$/i.test(node.path || node.name || '') && !isGz(node.path || '')) {
    const base = (node.name || node.path || '').split('/').pop().replace(/\.pex$/i, '');
    // pex/<name>.<arch>.pex is the branch above. A module in an SD build
    // directory is installed from that path; firmware reads it into the pool.
    if (onSd(node.path)) out.push({ id: 'pex-install', label: `Install ${base}` });
    out.push({ id: 'pex-mark', label: `Mark ${base} verified` });
    out.push({ id: 'pex-restore', label: `Restore verified ${base}` });
    out.push({ id: 'pex-partition', label: 'PEX partition…' });
  }
  out.push({ id: 'download', label: 'Download' });
  if (canCompress(node.path)) out.push({ id: 'compress', label: 'Store compressed (.gz)' });
  else if (isGz(node.path) && imageKind(node.path) !== 'bitmap') out.push({ id: 'decompress', label: 'Store uncompressed' });
  if (hasBak) out.push({ id: 'restore-bak', label: 'Restore .bak (the version before the last save)' });
  out.push({ id: 'copy', label: 'Copy to…' });
  out.push({ id: 'rename', label: 'Rename / move…' });
  out.push(pick);
  out.push({ id: 'delete', label: 'Delete…', danger: true });
  return out;
}

// Sizes as people read them: 812 B, 4.0 KB, 1.21 MB.
export function bytes(n) {
  if (n == null) return '—';
  if (n < 1024) return `${n} B`;
  if (n < 1024 * 1024) return `${(n / 1024).toFixed(1)} KB`;
  return `${(n / (1024 * 1024)).toFixed(2)} MB`;
}

// A file's line in its menu: its size, and what it takes on flash. LittleFS
// stores a small file inline in its folder's metadata (no blocks of its own:
// alloc 0); a larger one takes whole 4 KB blocks.
export function fileInfo(node) {
  if (node.size == null) return '';
  if (isGz(node.path || node.name || '')) return `compressed (gzip) · ${fileInfo({ ...node, path: '', name: '' })}`;
  if (node.alloc == null) return bytes(node.size);
  if (node.alloc === 0) return `${bytes(node.size)} · stored inline in its folder (no blocks of its own)`;
  return `${bytes(node.size)} · ${bytes(node.alloc)} on flash`;
}

// A folder's line: what is directly in it.
export function dirInfo(children) {
  if (!children) return 'counting…';
  const f = children.filter((c) => !c.dir).length;
  const d = children.length - f;
  if (!children.length) return 'empty';
  const part = (n, one) => `${n} ${one}${n === 1 ? '' : 's'}`;
  return [f ? part(f, 'file') : '', d ? part(d, 'folder') : ''].filter(Boolean).join(', ');
}

// Every file under a folder, on request only (one listing per folder).
// list(path) -> { ok, files: [{name, dir, size, alloc}], block }.
// alloc counts each folder's own metadata too: a LittleFS directory is a
// pair of blocks, where inline files also live.
export async function folderSize(list, path, onProgress = () => {}) {
  const t = { files: 0, dirs: 0, bytes: 0, alloc: 0, block: 0 };
  const walk = async (p) => {
    const r = await list(p);
    if (!r.ok) throw new Error(r.error || `could not list ${p}`);
    t.block = r.block || t.block || 4096;
    t.dirs++;
    t.alloc += 2 * t.block;
    for (const f of r.files) {
      const child = p ? `${p}/${f.name}` : f.name;
      if (f.dir) { await walk(child); continue; }
      t.files++;
      t.bytes += f.size || 0;
      t.alloc += f.alloc ?? Math.ceil((f.size || 0) / t.block) * t.block;
    }
    onProgress({ ...t });
  };
  await walk(path);
  t.dirs--;                         // the folder itself is not one of its subfolders
  return t;
}

export function sizeInfo(t, path = '') {
  if (onSd(path)) {
    const sub = t.dirs ? ` and ${t.dirs} subfolder${t.dirs === 1 ? '' : 's'}` : '';
    return `${bytes(t.bytes)} in ${t.files} file${t.files === 1 ? '' : 's'}${sub} on the SD card`;
  }
  const sub = t.dirs ? ` and ${t.dirs} subfolder${t.dirs === 1 ? '' : 's'}` : '';
  return `${bytes(t.bytes)} in ${t.files} file${t.files === 1 ? '' : 's'}${sub} · ${bytes(t.alloc)} on flash, `
    + `counting each folder's ${bytes(2 * (t.block || 4096))} of metadata`;
}
