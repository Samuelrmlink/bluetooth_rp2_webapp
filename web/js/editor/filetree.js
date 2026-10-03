// The editor sidebar's file tree: the device's whole LittleFS, not just its
// Lua. Config, staged copies and web assets are all files you may want to
// read or fix, and cfg/ is served on purpose.
//
// Lazy: the device lists one directory level per request, so a directory is
// fetched when it is first expanded, never the whole tree up front.
//
// A keyboard list (nav.js): j/k move, l or Enter opens a file or expands a
// directory, h collapses -- or, on a file or a closed directory, jumps to
// its parent, the way netrw and NERDTree do.

import * as fs from '../fs.js';

export class FileTree {
  // mark(path) -> { dirty: bool, arena: 'loaded'|'stale'|undefined }
  constructor(host, { onOpen, mark, onError }) {
    this.host = host;
    this.onOpen = onOpen;
    this.mark = mark;
    this.onError = onError || (() => {});
    this.root = { path: '', name: '', dir: true, open: true, children: null };
    this.sel = null;              // selected path
    this._dirty = true;
  }

  async load(node = this.root) {
    const r = await fs.list(node.path);
    if (!r.ok) {
      this.onError(`list ${node.path || '/'}: ${r.error}`);
      node.children = node.children || [];
      return;
    }
    const old = new Map((node.children || []).map((c) => [c.name, c]));
    node.children = r.files.map((f) => {
      const path = node.path ? `${node.path}/${f.name}` : f.name;
      const prev = old.get(f.name);
      // Keep an expanded directory's state and contents across a refresh.
      return prev && prev.dir === !!f.dir ? { ...prev, size: f.size, alloc: f.alloc }
        : { path, name: f.name, dir: !!f.dir, size: f.size, alloc: f.alloc, open: false, children: null };
    });
    this._dirty = true;
  }

  // Re-list every expanded directory -- after a save, upload, rename or delete.
  async refresh(node = this.root) {
    if (!node.dir || !node.open) return;
    await this.load(node);
    await Promise.all((node.children || []).filter((c) => c.dir && c.open).map((c) => this.refresh(c)));
  }

  find(path, node = this.root) {
    if (node.path === path) return node;
    for (const c of node.children || []) {
      if (path === c.path || path.startsWith(c.path + '/')) return this.find(path, c);
    }
    return null;
  }

  async toggle(node, open = !node.open) {
    if (!node.dir) return;
    node.open = open;
    if (open && !node.children) await this.load(node);
    this._dirty = true;
  }

  // Expand the directories above a path so it is visible, and select it.
  async reveal(path) {
    const parts = path.split('/');
    let node = this.root;
    for (let i = 0; i < parts.length - 1; i++) {
      if (!node.children) await this.load(node);
      node = (node.children || []).find((c) => c.name === parts[i] && c.dir);
      if (!node) return;
      if (!node.open) await this.toggle(node, true);
    }
    this.sel = path;
    this._dirty = true;
    this._scroll = true;
  }

  // The directory new files and uploads go to: the selected directory, or
  // the one holding the selected file.
  get currentDir() {
    const n = this.sel && this.find(this.sel);
    if (!n) return 'lua';
    return n.dir ? n.path : n.path.split('/').slice(0, -1).join('/');
  }

  visible() {
    const out = [];
    const walk = (node, depth) => {
      for (const c of node.children || []) {
        out.push({ node: c, depth });
        if (c.dir && c.open) walk(c, depth + 1);
      }
    };
    walk(this.root, 0);
    return out;
  }

  listController() {
    const idx = () => this.visible().findIndex((v) => v.node.path === this.sel);
    const at = (i) => {
      const v = this.visible();
      if (!v.length) return false;
      this.sel = v[Math.max(0, Math.min(v.length - 1, i))].node.path;
      this._dirty = true;
      this._scroll = true;
      return true;
    };
    const cur = () => this.sel && this.find(this.sel);
    return {
      enter: () => (this.sel ? true : at(0)),
      move: (d) => at(Math.max(0, idx()) + d),
      first: () => at(0),
      last: () => at(this.visible().length - 1),
      count: () => this.visible().length,
      activate: () => { const n = cur(); if (n) (n.dir ? this.toggle(n) : this.onOpen(n.path)); },
      right: () => { const n = cur(); if (n) (n.dir ? this.toggle(n, true) : this.onOpen(n.path)); },
      left: () => {
        const n = cur();
        if (!n) return;
        if (n.dir && n.open) { this.toggle(n, false); return; }
        const parent = n.path.split('/').slice(0, -1).join('/');
        if (parent) { this.sel = parent; this._dirty = true; this._scroll = true; }
      },
    };
  }

  invalidate() { this._dirty = true; }

  render() {
    // Not while a drag is over the list: replacing the rows would drop the
    // drag's source and the drop highlight with them.
    if (!this._dirty || this.holdRender) return;
    this._dirty = false;
    const frag = document.createDocumentFragment();
    for (const { node, depth } of this.visible()) {
      const row = document.createElement('div');
      row.className = `ft-row${node.dir ? ' ft-dir' : ''}`;
      row.dataset.path = node.path;
      // Draggable with a mouse only: on a touch screen a long press is the
      // row's menu, and a draggable row would turn it into a drag.
      if (this.canDrag) row.draggable = true;
      row.style.paddingLeft = `${6 + depth * 12}px`;
      if (node.path === this.sel) row.setAttribute('aria-selected', 'true');
      if (node.path === this.menuPath) row.classList.add('is-menu-row');   // its menu is open
      const tw = document.createElement('span');
      tw.className = 'ft-twisty';
      tw.textContent = node.dir ? (node.open ? '▾' : '▸') : '';
      const name = document.createElement('span');
      name.className = 'ft-name';
      name.textContent = node.name;
      row.append(tw, name);
      if (!node.dir) {
        const m = this.mark(node.path);
        if (m.dirty) row.classList.add('is-dirty');
        const badges = document.createElement('span');
        badges.className = 'ft-badges';
        if (m.arena) {
          const b = document.createElement('span');
          b.className = `ft-arena ft-${m.arena}`;
          b.textContent = 'L';
          b.title = m.arena === 'loaded' ? 'Loaded in the Lua arena, and matches the device'
            : 'Loaded in the Lua arena, but the file has changed since (or it runs from a draft)';
          badges.appendChild(b);
        }
        if (m.dirty) {
          const d = document.createElement('span');
          d.className = 'ft-dot';
          d.textContent = '●';
          d.title = 'Modified, not saved to the device';
          badges.appendChild(d);
        }
        row.appendChild(badges);
      }
      // The menu, for a finger: CSS shows it on coarse pointers only. Out of
      // the tab order -- the keyboard has m / Shift+F10 for the same thing.
      const more = document.createElement('button');
      more.type = 'button';
      more.className = 'ft-more';
      more.tabIndex = -1;
      more.textContent = '⋯';
      more.setAttribute('aria-label', `Menu for ${node.name}`);
      row.appendChild(more);
      frag.appendChild(row);
    }
    this.host.replaceChildren(frag);
    if (this._scroll) {
      this._scroll = false;
      this.host.querySelector('[aria-selected]')?.scrollIntoView({ block: 'nearest' });
    }
  }
}
