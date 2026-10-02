// What a file or folder's menu offers in the editor sidebar. Pure: the
// window turns each { id } into the action (editor.js _fileAct).
//
//   file     Open (if it is not the open one), Download, Rename / move…,
//            Delete…; a script (lua/x.lua or staged/x.lua) also Start, Stop,
//            Commit to lua/, Revert to the next tier
//   folder   New file here…, Upload here…, Calculate size, Refresh,
//            Rename / move…, and Delete… once it is known to be empty
//
// The path itself is added by the menu as the last item, and copies.
export function fileMenuItems(node, { isOpen = false, script = null, empty = false } = {}) {
  if (node.dir) {
    const out = [
      { id: 'new', label: 'New file here…' },
      { id: 'upload', label: 'Upload here…' },
      // keep: it answers in the menu (the info line), so the menu stays open
      { id: 'size', label: 'Calculate size', keep: true },
      { id: 'refresh', label: 'Refresh' },
      { id: 'rename', label: 'Rename / move…' },
    ];
    // only an empty folder: the device refuses any other (nothing is ever
    // deleted recursively)
    if (empty) out.push({ id: 'delete', label: 'Delete folder…', danger: true });
    return out;
  }
  const out = [];
  if (!isOpen) out.push({ id: 'open', label: 'Open' });
  if (script) {
    out.push({ id: 'start', label: `Start ${script}` });
    out.push({ id: 'stop', label: `Stop ${script}` });
    out.push({ id: 'commit', label: 'Commit to lua/' });
    out.push({ id: 'revert', label: 'Revert to the next tier' });
  }
  out.push({ id: 'download', label: 'Download' });
  out.push({ id: 'rename', label: 'Rename / move…' });
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

export function sizeInfo(t) {
  const sub = t.dirs ? ` and ${t.dirs} subfolder${t.dirs === 1 ? '' : 's'}` : '';
  return `${bytes(t.bytes)} in ${t.files} file${t.files === 1 ? '' : 's'}${sub} · ${bytes(t.alloc)} on flash, `
    + `counting each folder's ${bytes(2 * (t.block || 4096))} of metadata`;
}
