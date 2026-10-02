// What a file or folder's menu offers in the editor sidebar. Pure: the
// window turns each { id } into the action (editor.js _fileAct).
//
//   file     Open (if it is not the open one), Download, Rename / move…,
//            Delete…; a script (lua/x.lua or staged/x.lua) also Start, Stop,
//            Commit to lua/, Revert to the next tier
//   folder   New file here…, Upload here…, Refresh, Rename / move…
//
// The path itself is added by the menu as the last item, and copies.
export function fileMenuItems(node, { isOpen = false, script = null } = {}) {
  if (node.dir) {
    return [
      { id: 'new', label: 'New file here…' },
      { id: 'upload', label: 'Upload here…' },
      { id: 'refresh', label: 'Refresh' },
      { id: 'rename', label: 'Rename / move…' },
    ];
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
