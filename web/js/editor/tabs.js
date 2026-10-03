// The editor's tabs and its split, as state only -- the window draws it
// (windows/editor.js). Pure, tested.
//
//   groups   one, or two when split: each a row of tabs (file paths) and
//            the one showing
//   focus    the group the keys and the toolbar act on
//   orient   'h' side by side, 'v' one above the other
//
// A file may be open in both halves at once (the same buffer, two views);
// within one half a path appears once. Closing a half's last tab closes the
// half: a split is two views of something, never an empty pane.

let nextId = 1;
const group = () => ({ id: nextId++, tabs: [], active: -1 });

export class TabState {
  constructor() {
    this.groups = [group()];     // each { id, tabs, active }; id follows a half wherever it moves
    this.focus = 0;
    this.orient = 'h';
  }

  get split() { return this.groups.length > 1; }
  get group() { return this.groups[this.focus]; }

  activePath(gi = this.focus) {
    const g = this.groups[gi];
    return g && g.active >= 0 ? g.tabs[g.active] : null;
  }

  // Every path open anywhere, and whether one is showing in some half.
  openPaths() { return new Set(this.groups.flatMap((g) => g.tabs)); }
  isOpen(path) { return this.groups.some((g) => g.tabs.includes(path)); }
  showing(path) { return this.groups.some((g, i) => this.activePath(i) === path); }

  // Show `path` in a half: its tab if it has one, else a new tab just after
  // the one showing (as an editor opens a file "next to" the current one).
  open(path, gi = this.focus) {
    const g = this.groups[gi];
    let i = g.tabs.indexOf(path);
    if (i < 0) {
      i = g.active + 1;
      g.tabs.splice(i, 0, path);
    }
    g.active = i;
    this.focus = gi;
    return i;
  }

  // Close a tab; the one to its right shows next (else to its left). A half
  // left with no tabs goes, unsplitting. Returns false when it was not open.
  close(path, gi = this.focus) {
    const g = this.groups[gi];
    const i = g ? g.tabs.indexOf(path) : -1;
    if (i < 0) return false;
    g.tabs.splice(i, 1);
    if (g.active > i || g.active >= g.tabs.length) g.active--;
    if (!g.tabs.length) {
      g.active = -1;
      if (this.split) {
        this.groups.splice(gi, 1);
        this.focus = 0;
      }
    }
    return true;
  }

  // A deleted file: gone from every half.
  closeEverywhere(path) {
    for (let gi = this.groups.length - 1; gi >= 0; gi--) this.close(path, gi);
  }

  // A renamed (or compressed / uncompressed) file keeps its place. If the
  // new name was already open in a half, that half keeps just the one tab.
  rename(from, to) {
    for (const g of this.groups) {
      const i = g.tabs.indexOf(from);
      if (i < 0) continue;
      const dup = g.tabs.indexOf(to);
      if (dup >= 0) {
        const showing = g.tabs[g.active];
        g.tabs.splice(dup, 1);
        g.tabs[g.tabs.indexOf(from)] = to;
        g.active = Math.max(0, g.tabs.indexOf(showing === from || showing === to ? to : showing));
      } else {
        g.tabs[i] = to;
      }
    }
  }

  // Next / previous tab, wrapping.
  step(delta, gi = this.focus) {
    const g = this.groups[gi];
    if (g.tabs.length < 2) return this.activePath(gi);
    g.active = (g.active + delta + g.tabs.length) % g.tabs.length;
    return this.activePath(gi);
  }

  // The n-th tab (1-based), as vim's {n}gt. Out of range: no change.
  goto(n, gi = this.focus) {
    const g = this.groups[gi];
    if (n >= 1 && n <= g.tabs.length) g.active = n - 1;
    return this.activePath(gi);
  }

  // Move the showing tab one place left / right in its row.
  move(delta, gi = this.focus) {
    const g = this.groups[gi];
    const j = g.active + delta;
    if (g.active < 0 || j < 0 || j >= g.tabs.length) return false;
    [g.tabs[g.active], g.tabs[j]] = [g.tabs[j], g.tabs[g.active]];
    g.active = j;
    return true;
  }

  // Split: a second half showing the same file (or `path`), focused.
  // Already split: just open there.
  splitOpen(path = this.activePath(), orient = this.orient) {
    if (!this.split) {
      this.groups.push(group());
      this.orient = orient;
    }
    const other = this.focus === 0 ? 1 : 0;
    if (path) this.open(path, other);
    this.focus = other;
    return other;
  }

  // Back to one half: the other half's tabs join this one's (missing ones
  // appended), and what this half was showing stays showing.
  unsplit() {
    if (!this.split) return;
    const keep = this.groups[this.focus], gone = this.groups[this.focus ^ 1];
    for (const p of gone.tabs) if (!keep.tabs.includes(p)) keep.tabs.push(p);
    if (keep.active < 0 && keep.tabs.length) keep.active = 0;
    this.groups = [keep];
    this.focus = 0;
  }

  toggleSplit() {
    if (this.split) this.unsplit(); else this.splitOpen();
  }

  flip() { this.orient = this.orient === 'h' ? 'v' : 'h'; }

  // What a page reload can bring back (paths only; the files are re-read).
  toJSON() {
    return { groups: this.groups.map((g) => ({ tabs: g.tabs, active: g.active })), focus: this.focus, orient: this.orient };
  }

  static from(o) {
    const t = new TabState();
    const gs = (o?.groups || []).filter((g) => Array.isArray(g?.tabs) && g.tabs.length).slice(0, 2);
    if (!gs.length) return t;
    t.groups = gs.map((g) => {
      const tabs = [...new Set(g.tabs.filter((p) => typeof p === 'string' && p))];
      return { ...group(), tabs, active: Math.min(Math.max(0, g.active | 0), tabs.length - 1) };
    });
    t.focus = Math.min(o.focus | 0, t.groups.length - 1);
    t.orient = o.orient === 'v' ? 'v' : 'h';
    return t;
  }

  focusOther() {
    if (this.split) this.focus ^= 1;
    return this.focus;
  }

  // Move the showing tab to the other half (splitting first if need be).
  // A half emptied by it closes.
  moveToOther() {
    const path = this.activePath();
    if (!path) return false;
    if (!this.split) {
      if (this.group.tabs.length < 2) return false;   // nothing would stay behind
      this.splitOpen(path);
      this.close(path, 0);
      this.focus = this.groups.length - 1;
      return true;
    }
    const from = this.focus;
    this.open(path, from ^ 1);
    this.close(path, from);
    this.focus = this.groups.length === 1 ? 0 : from ^ 1;
    return true;
  }
}
