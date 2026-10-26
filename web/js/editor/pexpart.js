// The PEX partition, drawn in one editor tab (path pex://partition). It is
// not a file: every .pex shares it. Start and Stop act on the selected
// installed module by name, so the file does not have to be in the tree.
// Pack is pex.compact. Uninstall is pex.uninstall for that module: the
// file in pex/ stays, and a running copy has to be stopped and unloaded
// first. A running module stays where it is during Pack. There is no
// reserved growth gap: a replacement is written into free space before
// the old copy is dropped, so a gap kept "for growth" is the space that
// replacement needs.

import { bytes } from './filemenu.js';

export const PEX_PART = 'pex://partition';
export const SECTOR = 4096;
export const INDEX_SECTORS = 2;

/** The picture pex.installed draws: one cell per sector, modules by offset. */
export function partitionModel(info) {
  const size = Number(info?.size) || 0;
  const sectors = Math.floor(size / SECTOR);
  const indexSectors = Math.min(INDEX_SECTORS, sectors);
  const indexBytes = indexSectors * SECTOR;
  const free = Number(info?.free) || 0;
  const largest = Number(info?.largest) || 0;
  const data = Math.max(0, size - indexBytes);
  const modules = (info?.modules || []).map((m) => {
    const start = Math.floor(Number(m.offset) / SECTOR);
    const count = Math.max(0, Math.ceil(Number(m.size) / SECTOR));
    return { ...m, start, count };
  }).sort((a, b) => a.start - b.start || String(a.name).localeCompare(String(b.name)));
  const cells = [];
  for (let s = 0; s < sectors; s++) {
    if (s < indexSectors) { cells.push({ kind: 'index', sector: s }); continue; }
    const mod = modules.find((m) => s >= m.start && s < m.start + m.count);
    cells.push(mod ? { kind: 'module', sector: s, name: mod.name } : { kind: 'free', sector: s });
  }
  return {
    mounted: info?.mounted !== false && !!info,
    size, sectors, indexBytes, used: Math.max(0, data - free), free, largest,
    fragmented: free > largest,
    rwArea: Number(info?.rw_area) || 0,
    seq: info?.seq,
    moves: Number(info?.moves) || 0,
    modules, cells,
  };
}

/** Which header buttons apply to the selected installed module. `state` is
 *  the live pex.list state. Start is refused only while it is running.
 *  Stop stays available for anything still resident, including a stopped copy. */
export function partitionActions(mod, state) {
  if (!mod) return { start: false, stop: false, remove: false, state: '' };
  const live = state || (mod.loaded ? 'loaded' : '');
  const resident = live === 'running' || live === 'loaded' || live === 'stopped'
    || live === 'exited' || live === 'faulted';
  return { start: live !== 'running', stop: resident, remove: true, state: live };
}

function el(tag, cls, text) {
  const n = document.createElement(tag);
  if (cls) n.className = cls;
  if (text != null) n.textContent = text;
  return n;
}

export class PexPartition {
  constructor(surface, { onPack, onSelect, onRemove, onStart, onStop } = {}) {
    this.el = document.createElement('div');
    this.el.className = 'ed-pexpart';
    this.el.hidden = true;
    surface.appendChild(this.el);
    this.onPack = onPack || (() => {});
    this.onSelect = onSelect || (() => {});
    this.onRemove = onRemove || (() => {});
    this.onStart = onStart || (() => {});
    this.onStop = onStop || (() => {});
    this.el.addEventListener('click', (e) => {
      const act = (cls, fn) => {
        const btn = e.target.closest(cls);
        if (!btn) return false;
        if (!btn.disabled && btn.dataset.pex) fn(btn.dataset.pex);
        return true;
      };
      if (act('.ed-px-start', (name) => this.onStart(name))) return;
      if (act('.ed-px-stop', (name) => this.onStop(name))) return;
      if (e.target.closest('.ed-px-pack')) { this.onPack(); return; }
      if (act('.ed-px-remove', (name) => this.onRemove(name))) return;
      const row = e.target.closest('[data-pex]');
      if (row?.dataset.pex) this.onSelect(row.dataset.pex);
    });
  }

  get shown() { return !this.el.hidden; }

  hide() { this.el.hidden = true; }

  show(info, { selected = null, states = null } = {}) {
    this.el.hidden = false;
    this.el.replaceChildren(...this._nodes(info, selected, states || {}));
  }

  _nodes(info, selected, states) {
    if (!info) {
      return [el('div', 'ed-px-note dim', 'Asking the device for the PEX partition…')];
    }
    const m = partitionModel(info);
    const head = el('div', 'ed-px-head');
    const mod = m.modules.find((x) => x.name === selected);
    const act = partitionActions(mod, mod ? states[mod.name] : '');
    head.append(
      el('div', 'ed-px-sum',
        `${bytes(m.size)} partition · index ${bytes(m.indexBytes)} · used ${bytes(m.used)} · free ${bytes(m.free)} · largest run ${bytes(m.largest)} · RW area ${bytes(m.rwArea)}`),
      el('button', 'pane-btn ed-px-start', 'Start'),
      el('button', 'pane-btn ed-px-stop', 'Stop'),
      el('button', 'pane-btn ed-px-remove', 'Uninstall'),
      el('button', 'pane-btn ed-px-pack', 'Pack'),
    );
    const bind = (cls, on, title) => {
      const btn = head.querySelector(cls);
      btn.type = 'button';
      btn.disabled = !on;
      if (mod && on) btn.dataset.pex = mod.name;
      btn.title = title;
    };
    bind('.ed-px-start', act.start, mod
      ? (act.state === 'running' ? `${mod.name} is already running.` : `Load and start ${mod.name} from the partition.`)
      : 'Select an installed module in the table.');
    bind('.ed-px-stop', act.stop, mod
      ? `Stop and unload ${mod.name}.`
      : 'Select an installed module in the table.');
    bind('.ed-px-remove', act.remove, mod
      ? `Remove ${mod.name} from the partition. The file in pex/ stays. A running copy has to be stopped and unloaded first.`
      : 'Select an installed module in the table.');
    head.querySelector('.ed-px-pack').type = 'button';
    head.querySelector('.ed-px-pack').title = 'Slide installed files together. A running module stays where it is.';
    const noteText = m.fragmented
      ? `Free space is in more than one piece: ${bytes(m.free)} free, largest run ${bytes(m.largest)}. Pack slides the files together. A running module stays where it is.`
      : m.free === 0
      ? 'No free space. Pack can only gather a gap a running module left behind. A replacement is written into free space before the old copy is dropped, so installing a larger file needs a free run while the old copy is still there.'
      : 'Pack slides installed files together and leaves the free space as one run. A running module stays where it is. A replacement is written into that free space before the old copy is dropped, so a gap kept for growth would be the space the replacement needs.';
    const note = el('div', 'ed-px-note dim', noteText);
    const map = el('div', 'ed-px-map');
    map.setAttribute('role', 'img');
    map.setAttribute('aria-label', 'PEX partition, one cell per 4 KB sector');
    for (const c of m.cells) {
      const cell = el('span', `ed-px-cell is-${c.kind === 'module' ? 'mod' : c.kind}`);
      if (c.kind === 'module') {
        cell.dataset.pex = c.name;
        if (c.name === selected) cell.classList.add('is-sel');
      }
      cell.title = c.kind === 'index' ? `sector ${c.sector}: index`
        : c.kind === 'free' ? `sector ${c.sector}: free`
        : `sector ${c.sector}: ${c.name}`;
      map.appendChild(cell);
    }
    const legend = el('div', 'ed-px-legend dim');
    for (const [cls, label] of [['is-index', 'index'], ['is-mod', 'installed'], ['is-free', 'free'], ['is-sel', 'selected']]) {
      const item = el('span');
      item.append(el('span', `ed-px-swatch ed-px-cell ${cls}`), document.createTextNode(label));
      legend.appendChild(item);
    }
    const table = el('table', 'ed-px-table');
    const thead = document.createElement('thead');
    const hr = document.createElement('tr');
    for (const h of ['Name', 'State', 'Version', 'Offset', 'Size', 'Code', 'RW', 'Signed', 'XIP']) {
      hr.appendChild(el('th', '', h));
    }
    thead.appendChild(hr);
    const tbody = document.createElement('tbody');
    for (const rowMod of m.modules) {
      const tr = document.createElement('tr');
      tr.dataset.pex = rowMod.name;
      if (rowMod.name === selected) tr.classList.add('is-sel');
      const rowAct = partitionActions(rowMod, states[rowMod.name]);
      for (const v of [rowMod.name, rowAct.state || '—', rowMod.version, bytes(rowMod.offset), bytes(rowMod.size), bytes(rowMod.code), bytes(rowMod.rw),
        rowMod.signed ? 'yes' : 'no', rowMod.xip_ready ? 'yes' : 'no']) {
        tr.appendChild(el('td', '', String(v ?? '')));
      }
      tbody.appendChild(tr);
    }
    table.append(thead, tbody);
    const detail = el('div', 'ed-px-detail');
    if (!selected) detail.textContent = 'Select an installed module. Start and Stop run it from the partition, with no file in the list. Uninstall removes that copy. The file in pex/, if there is one, stays.';
    else if (!mod) detail.textContent = `${selected} is not in the partition. Install it from the file's menu. The copy in pex/ stays either way.`;
    else {
      detail.textContent = `${mod.name} ${mod.version} · ${mod.model} · offset ${bytes(mod.offset)} · ${bytes(mod.size)} · code ${bytes(mod.code)} · RW ${bytes(mod.rw)} · ${mod.signed ? 'signed' : 'unsigned'} · ${act.state || 'not loaded'} · ${mod.xip_ready ? 'runs from flash' : 'loads into RAM'}. Start and Stop use this installed copy.`;
    }
    const meta = el('div', 'ed-px-note dim', `index #${m.seq ?? '—'} · ${m.moves} move${m.moves === 1 ? '' : 's'}`);
    return [head, note, map, legend, table, detail, meta];
  }
}
