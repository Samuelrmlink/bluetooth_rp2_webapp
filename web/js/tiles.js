// The shape of the workspace, and nothing else.
//
// A binary tree of splits whose leaves hold panes, plus a list of floating
// windows. This module owns every question of the form "where is pane X"
// and every change to the answer — split, close, swap, float, resize — and
// it does so without touching the DOM, which is what lets it be tested
// with node:test alone and lets layout.js be a renderer rather than a
// second copy of the rules.
//
// Two invariants hold after every operation:
//   * there is always at least one leaf (closing the last one empties it
//     into a placeholder instead of removing it);
//   * a pane id appears at most once across the tree and the floating
//     list. A pane is one DOM element — the uPlot canvases, the events
//     table — so it can be tiled, floating or parked, never two of those.
//
// Shapes (ids are session-local integers, never persisted):
//   leaf   { kind:'leaf',  id, pane: string|null }        null = placeholder
//   split  { kind:'split', id, dir: 'row'|'col', ratio, a, b }
//          'row' = a beside b (a left) — "Split horizontal"
//          'col' = a above  b          — "Split vertical"
//   float  { kind:'float', id, pane, x, y, w, h }         fractions of the
//          workspace, so a rotated tablet keeps the window where it was
//
// Persisted form (compact, for localStorage):
//   tree:     "graphs" | null | ["row", 0.55, a, b]
//   floating: [{ p: "values", x, y, w, h }]

export const MIN_RATIO = 0.15;
export const MAX_RATIO = 0.85;
export const RESIZE_STEP = 0.05;
// A floating window can never be smaller than this share of the workspace,
// nor be pushed so far that less than a tenth of it stays on screen — its
// header has to remain reachable, or it could not be dragged back.
export const FLOAT_MIN = 0.2;
export const FLOAT_KEEP = 0.1;
// Guards for a corrupt or hostile stored layout: deeper or busier than
// anyone builds by hand, and well under what a screen can show.
const MAX_DEPTH = 8;
const MAX_LEAVES = 16;

function clamp(v, lo, hi) {
  return Math.max(lo, Math.min(v, hi));
}

function cloneNode(n) {
  return n.kind === 'leaf' ? { ...n } : { ...n, a: cloneNode(n.a), b: cloneNode(n.b) };
}

function num(v, fallback) {
  return Number.isFinite(v) ? v : fallback;
}

// A pane that belongs to one device: 'anker-pps.controls@E8EECCB6897E',
// 'pinecil.controls@C0A1B2…'. A stored layout may name a device's windows
// before the page has heard of that device — its script is not running yet,
// or it is still handshaking — and those leaves
// are kept as they are (see fromJSON) so the arrangement survives.
// <plugin>.<window>@<device key>; the dot joins plugin and window name.
const DEVICE_PANE = /^[a-z][a-z0-9.-]*@[A-Za-z0-9._:-]{1,40}$/;

export function isDevicePane(id) {
  return typeof id === 'string' && DEVICE_PANE.test(id);
}

export class Layout {
  constructor(knownPanes) {
    this._known = new Set(knownPanes || []);
    this._next = 1;
    this.root = null;
    this.floating = [];
  }

  isKnown(id) {
    return this._known.has(id);
  }

  // A pane that may be placed from now on (a device's windows, as it appears).
  addKnown(id) {
    this._known.add(id);
  }

  // Renames a pane wherever it sits — a leaf or a float. What the page
  // does once when the serial-less panes it started with turn out to be
  // a particular device's. Returns false when no window holds `oldId`.
  renamePane(oldId, newId) {
    // The new name is placeable either way; a pane that is not in any
    // window right now (a Live Values pane nobody has opened) is still
    // renamed for the pickers' sake.
    this._known.add(newId);
    this._known.delete(oldId);
    const w = this.ownerOf(oldId);
    if (!w) return false;
    w.pane = newId;
    return true;
  }

  _id() {
    return this._next++;
  }

  _leaf(pane) {
    return { kind: 'leaf', id: this._id(), pane: pane ?? null };
  }

  _split(dir, ratio, a, b) {
    return { kind: 'split', id: this._id(), dir, ratio: clamp(num(ratio, 0.5), MIN_RATIO, MAX_RATIO), a, b };
  }

  // --- construction ---------------------------------------------------------

  static single(paneId, knownPanes) {
    const l = new Layout(knownPanes);
    l.root = l._leaf(l._known.has(paneId) ? paneId : null);
    return l;
  }

  // The stored form back into a layout. Never throws: anything it cannot
  // make sense of degrades to a placeholder, and a pane that appears twice
  // keeps its first occurrence — the alternative to being lenient here is a
  // stored layout that can brick the page.
  static fromJSON(json, knownPanes) {
    const l = new Layout(knownPanes);
    const seen = new Set();
    const leavesLeft = { n: MAX_LEAVES };
    const build = (node, depth) => {
      if (Array.isArray(node)) {
        const [dir, ratio, a, b] = node;
        if ((dir === 'row' || dir === 'col') && depth < MAX_DEPTH && node.length === 4) {
          const na = build(a, depth + 1);
          const nb = build(b, depth + 1);
          return l._split(dir, ratio, na, nb);
        }
        return l._leaf(null);
      }
      if (leavesLeft.n-- <= 0) return l._leaf(null);
      if (typeof node === 'string' && (l._known.has(node) || isDevicePane(node)) && !seen.has(node)) {
        seen.add(node);
        l._known.add(node);
        return l._leaf(node);
      }
      return l._leaf(null);
    };
    const tree = json && typeof json === 'object' ? json.tree : undefined;
    l.root = tree === undefined ? l._leaf(null) : build(tree, 0);
    const fl = json && Array.isArray(json.floating) ? json.floating : [];
    for (const f of fl) {
      if (!f || typeof f !== 'object' || typeof f.p !== 'string') continue;
      if ((!l._known.has(f.p) && !isDevicePane(f.p)) || seen.has(f.p)) continue;
      seen.add(f.p);
      l._known.add(f.p);
      l.floating.push(l._float(f.p, f.x, f.y, f.w, f.h));
    }
    return l;
  }

  // What the two-slot cookie used to say, as a two-leaf tree. Same
  // fallbacks the old constructor applied: an unknown pane id falls back to
  // the first registered pane, and a secondary equal to the primary yields.
  static fromLegacy({ mode, primary, secondary, ratio } = {}, knownPanes, landscape = true) {
    const known = [...(knownPanes || [])];
    const l = new Layout(known);
    const p = known.includes(primary) ? primary : known[0];
    let s = known.includes(secondary) ? secondary : known[1];
    if (s === p) s = known.find((k) => k !== p);
    if (p === undefined) {
      l.root = l._leaf(null);
      return l;
    }
    if (s === undefined) {
      l.root = l._leaf(p);
      return l;
    }
    const dir = mode === 'side' ? 'row' : mode === 'stack' ? 'col' : landscape ? 'row' : 'col';
    l.root = l._split(dir, num(Number(ratio), 0.55), l._leaf(p), l._leaf(s));
    return l;
  }

  _float(pane, x, y, w, h) {
    const f = { kind: 'float', id: this._id(), pane, x: num(x, 0.25), y: num(y, 0.2), w: num(w, 0.5), h: num(h, 0.5) };
    this._clampFloat(f);
    return f;
  }

  toJSON() {
    const enc = (n) => (n.kind === 'leaf' ? n.pane : [n.dir, Number(n.ratio.toFixed(4)), enc(n.a), enc(n.b)]);
    return {
      tree: enc(this.root),
      floating: this.floating.map((f) => ({
        p: f.pane,
        x: Number(f.x.toFixed(4)),
        y: Number(f.y.toFixed(4)),
        w: Number(f.w.toFixed(4)),
        h: Number(f.h.toFixed(4)),
      })),
    };
  }

  // --- queries --------------------------------------------------------------

  // Preorder, a before b: left before right, top before bottom. This is the
  // order the Mod+n ring walks, so it should read the way the screen does.
  leaves() {
    const out = [];
    const walk = (n) => {
      if (n.kind === 'leaf') out.push(n);
      else {
        walk(n.a);
        walk(n.b);
      }
    };
    walk(this.root);
    return out;
  }

  // Every window: tiles in tree order, then floating windows in z-order
  // (last = topmost).
  windows() {
    return this.leaves().concat(this.floating);
  }

  find(id) {
    let hit = null;
    const walk = (n) => {
      if (hit) return;
      if (n.id === id) hit = n;
      else if (n.kind === 'split') {
        walk(n.a);
        walk(n.b);
      }
    };
    walk(this.root);
    return hit || this.floating.find((f) => f.id === id) || null;
  }

  parentOf(id) {
    let hit = null;
    const walk = (n) => {
      if (hit || n.kind !== 'split') return;
      if (n.a.id === id || n.b.id === id) hit = n;
      else {
        walk(n.a);
        walk(n.b);
      }
    };
    walk(this.root);
    return hit;
  }

  // The leaf or floating window holding a pane, or null when it is parked.
  ownerOf(paneId) {
    return this.windows().find((w) => w.pane === paneId) || null;
  }

  firstLeaf(node) {
    let n = node;
    while (n.kind === 'split') n = n.a;
    return n;
  }

  indexOf(id) {
    return this.windows().findIndex((w) => w.id === id);
  }

  at(i) {
    const ws = this.windows();
    return ws.length ? ws[((i % ws.length) + ws.length) % ws.length] : null;
  }

  // Replaces `node` with `repl` wherever it hangs.
  _replace(node, repl) {
    const parent = this.parentOf(node.id);
    if (!parent) this.root = repl;
    else if (parent.a === node) parent.a = repl;
    else parent.b = repl;
  }

  // --- mutations ------------------------------------------------------------

  // The leaf becomes {dir, a: itself, b: a placeholder}. The placeholder is
  // deliberate: a pane cannot be in two tiles, and guessing which other
  // pane the user wanted is worse than asking.
  split(leafId, dir) {
    const leaf = this.find(leafId);
    if (!leaf || leaf.kind !== 'leaf') return null;
    if (dir !== 'row' && dir !== 'col') return null;
    if (this.leaves().length >= MAX_LEAVES) return null;
    const fresh = this._leaf(null);
    const s = this._split(dir, 0.5, leaf, fresh);
    this._replace(leaf, s);
    return fresh.id;
  }

  // Returns the id worth focusing next: the sibling's first leaf, or the
  // emptied root. Closing the last tile leaves a placeholder rather than
  // nothing — a workspace with no tile has no picker to recover with.
  close(id) {
    const n = this.find(id);
    if (!n) return null;
    if (n.kind === 'float') {
      this.floating = this.floating.filter((f) => f !== n);
      return this.leaves()[0].id;
    }
    if (n.kind !== 'leaf') return null;
    const parent = this.parentOf(id);
    if (!parent) {
      n.pane = null;
      return n.id;
    }
    const sibling = parent.a === n ? parent.b : parent.a;
    this._replace(parent, sibling);
    return this.firstLeaf(sibling).id;
  }

  // Assigning a pane already shown elsewhere swaps the two: the other window
  // receives this one's old pane (possibly none). That is the only reading
  // of "put X here" that keeps every pane in one place.
  setPane(id, paneId) {
    const win = this.find(id);
    if (!win || win.kind === 'split') return null;
    if (paneId == null) {
      win.pane = null;
      return id;
    }
    if (!this._known.has(paneId)) return id;
    const other = this.ownerOf(paneId);
    if (other && other !== win) other.pane = win.pane;
    win.pane = paneId;
    return id;
  }

  swapPanes(idA, idB) {
    const a = this.find(idA);
    const b = this.find(idB);
    if (!a || !b || a === b || a.kind === 'split' || b.kind === 'split') return false;
    [a.pane, b.pane] = [b.pane, a.pane];
    return true;
  }

  // --- moving a pane onto another one ----------------------------------------
  //
  // Swapping is one reading of "move this pane there". The other is to
  // share that tile's room with it: the target splits and the pane takes
  // one half. Which half is not a free choice — it is the half the pane
  // arrived at. Dropping on a tile's top edge puts the pane on top, and
  // moving *down* onto a tile means entering through its top edge, so the
  // mouse and the keyboard can never disagree about a move the user thinks
  // of as one thing.
  //
  // `edge` names the side of the target the pane comes to rest against:
  // 'left' | 'right' | 'up' | 'down'.
  _edgeSplit(edge) {
    return {
      dir: edge === 'left' || edge === 'right' ? 'row' : 'col',
      first: edge === 'left' || edge === 'up',
    };
  }

  // Would putting the pane on that side of `anchor` rebuild the shape it is
  // already in? Then it is not a move, and doing it anyway would reset that
  // split's ratio for nothing.
  _sameShape(src, anchor, dir, first) {
    const parent = this.parentOf(src.id);
    return !!parent && parent.dir === dir
      && (first ? parent.a === src : parent.b === src)
      && (first ? parent.b : parent.a) === anchor;
  }

  // Takes the pane out of `srcId` — a tile or a floating window — and plants
  // it beside `targetLeafId` in a new split, on the named edge of it.
  //
  // The removal goes first, and through close(), so the source's room falls
  // back to its sibling exactly as closing it would: a move must not leave a
  // hole where the pane was. That is safe even when the target sits inside
  // the subtree close() rewrites, because nodes are objects and _replace()
  // only ever re-points a parent's `a`/`b` — the target node is still the
  // same node, still in the tree, wherever the collapse hung it. Which is
  // why the target is captured as a node here and never looked up by id
  // again afterwards.
  moveInto(srcId, targetLeafId, edge) {
    const src = this.find(srcId);
    const target = this.find(targetLeafId);
    if (!src || !target || src === target) return null;
    if (target.kind !== 'leaf' || src.kind === 'split' || src.pane == null) return null;
    if (edge !== 'left' && edge !== 'right' && edge !== 'up' && edge !== 'down') return null;
    // Tile to tile is leaf-neutral — one closed, one opened — so the cap can
    // only bite when the pane is coming in from a floating window. Checked
    // before anything moves, because there is no unwinding a half-done move.
    if (src.kind === 'float' && this.leaves().length >= MAX_LEAVES) return null;
    const { dir, first } = this._edgeSplit(edge);
    if (this._sameShape(src, target, dir, first)) return null;
    const pane = src.pane;
    this.close(srcId);
    const fresh = this._leaf(pane);
    this._replace(target, this._split(dir, 0.5, first ? fresh : target, first ? target : fresh));
    return fresh.id;
  }

  // Onto the line between two windows rather than onto either of them: the
  // pane takes a band across the whole of that split, so it ends up beside
  // both of them instead of halving one. Structurally it wraps the split
  // node, which is the one thing moveInto cannot do — that always targets a
  // leaf.
  //
  // `edge` is the side of the pair the pane takes, perpendicular to the split
  // it was dropped on: a band above or below two windows that sit side by
  // side, or beside two that are stacked.
  moveAcross(srcId, splitId, edge) {
    const src = this.find(srcId);
    const target = this.find(splitId);
    if (!src || !target || target.kind !== 'split') return null;
    if (src.kind === 'split' || src.pane == null) return null;
    if (edge !== 'left' && edge !== 'right' && edge !== 'up' && edge !== 'down') return null;
    if (src.kind === 'float' && this.leaves().length >= MAX_LEAVES) return null;
    const { dir, first } = this._edgeSplit(edge);
    // Taking the pane out can dissolve the very split it was dropped on —
    // that happens whenever the pane is one of its two children, since
    // removing a child replaces the split with its sibling. The band then has
    // only that sibling to span, which is the honest answer rather than an
    // error: it degenerates into an ordinary move beside it.
    const parent = this.parentOf(srcId);
    const anchor = parent === target ? (target.a === src ? target.b : target.a) : target;
    if (this._sameShape(src, anchor, dir, first)) return null;
    const pane = src.pane;
    this.close(srcId);
    const fresh = this._leaf(pane);
    this._replace(anchor, this._split(dir, 0.5, first ? fresh : anchor, first ? anchor : fresh));
    return fresh.id;
  }

  // --- the special resize ---------------------------------------------------
  //
  // Push one edge of a pane out past a border it does not own, or pull it back.
  //
  // The ordinary resize (resize(), below) nudges the nearest ancestor split of
  // that axis, which moves the whole side: in a binary tree siblings share
  // their parent's extent, so widening a pane widens everything stacked with
  // it. That is the right answer when the window across the border is the
  // same size as this one, and no answer at all when it is not — when a pane
  // is one of a group and wants room from a window the group as a whole is
  // up against.
  //
  // Giving it that room alone means changing the shape, not a ratio: the pane
  // leaves its parent and is re-inserted one level further out, spanning that
  // level, and the window it took from is re-nested with whatever the pane
  // was grouped with. One level per call — going two levels out is calling
  // it twice — and `out: false` is the exact reverse.
  //
  //   row( col( row(LVa,LVb), BAND ), LG022 )    BAND's right edge, out
  //   -> col( row( row(LVa,LVb), LG022 ), BAND )
  //
  // Refused where the ordinary resize already does the job, which is exactly
  // when the pane is a direct child of the border split — its sibling then
  // covers precisely the pane's own extent.
  edgeResize(leafId, edge, out = true) {
    const src = this.find(leafId);
    if (!src || src.kind !== 'leaf' || src.pane == null) return null;
    if (edge !== 'left' && edge !== 'right' && edge !== 'up' && edge !== 'down') return null;
    const axis = edge === 'left' || edge === 'right' ? 'row' : 'col';
    // Which child of a split of that axis this pane must be on for the split
    // to be the border on this side: the `a` child has the line to its right
    // and below it.
    const near = edge === 'right' || edge === 'down';
    const parent = this.parentOf(leafId);
    if (!parent) return null;
    // The side the pane sits on within its own parent, as an edge — the side
    // it keeps when it is lifted out.
    const side = parent.dir === 'row'
      ? (parent.a === src ? 'left' : 'right')
      : (parent.a === src ? 'up' : 'down');

    const target = out ? this._outwardAnchor(src, axis, near, parent) : this._inwardAnchor(src, axis, near, parent);
    if (!target) return null;
    // Measured before anything moves, so the pane keeps the dimension it is
    // NOT growing instead of landing on a bare half.
    //
    // The two directions want different things. Going out, the pane lands in
    // a bigger box, so keeping its size means its share of that box: its own
    // extent over the anchor's. Going back in, the box is smaller than the
    // pane — its share of it would be more than all of it — so what is kept
    // instead is the share it had of the split it is leaving, which is
    // exactly what makes the pair an undo of each other.
    const wrapAxis = side === 'left' || side === 'right' ? 'row' : 'col';
    const anchorFrac = this._axisFraction(target, wrapAxis);
    const frac = out
      ? (anchorFrac > 0 ? this._axisFraction(src, wrapAxis) / anchorFrac : 0.5)
      : (parent.dir === wrapAxis ? (parent.a === src ? parent.ratio : 1 - parent.ratio) : 0.5);
    const fresh = target.kind === 'leaf'
      ? this.moveInto(leafId, target.id, side)
      : this.moveAcross(leafId, target.id, side);
    if (fresh == null) return null;
    const made = this.parentOf(fresh);
    if (made) {
      const first = side === 'left' || side === 'up';
      this.setRatio(made.id, first ? frac : 1 - frac);
    }
    return fresh;
  }

  // Outward: the subtree to wrap is the border split itself, so the pane
  // comes to span everything that border encloses.
  //
  // Not the pane's grandparent, which is the tempting answer and the wrong
  // one: a pane's width is set only by the splits of that axis above it, so
  // lifting it past a split of the *other* axis moves it in the tree while
  // leaving it exactly as wide as it was. Going up to the border means one
  // press crosses one line the eye can see, which is what "twice for two
  // windows out" has to mean.
  _outwardAnchor(src, axis, near, parent) {
    let node = src;
    let up = parent;
    while (up) {
      if (up.dir === axis && (near ? up.a === node : up.b === node)) {
        // A direct child of the border has a neighbour exactly its own size;
        // that is the ordinary resize's job, not this one.
        return up === parent ? null : up;
      }
      node = up;
      up = this.parentOf(up.id);
    }
    // No border on that side at all: already against the workspace edge.
    return null;
  }

  // Inward: back down into the neighbour the pane took from — that
  // neighbour's own near-side child, which is where it came from.
  _inwardAnchor(src, axis, near, parent) {
    const sib = parent.a === src ? parent.b : parent.a;
    // A bare neighbour has nothing to descend into, and one split the other
    // way does not bound this edge at all: neither is a move.
    if (sib.kind !== 'split' || sib.dir !== axis) return null;
    return near ? sib.a : sib.b;
  }

  // What share of the whole workspace this node covers along one axis,
  // from the ratios alone — the same number the renderer would measure,
  // without the model having to know about pixels.
  _axisFraction(node, axis) {
    let f = 1;
    let cur = node;
    let up = this.parentOf(cur.id);
    while (up) {
      if (up.dir === axis) f *= up.a === cur ? up.ratio : 1 - up.ratio;
      cur = up;
      up = this.parentOf(up.id);
    }
    return f;
  }

  // Nothing in that direction: the pane leaves whatever it was nested in and
  // takes that whole edge of the workspace, the way i3's move does at the
  // edge of a workspace. It is the only way to restructure the top level
  // without the mouse.
  //
  // The side is the mirror of moveInto's, and deliberately so: moving left
  // *into* a neighbour makes the pane that neighbour's right half, because
  // it entered from the right — but moving left *out of everything* puts it
  // at the far left. One says where you came in, the other where you ended
  // up.
  moveOutward(srcId, dir) {
    const src = this.find(srcId);
    if (!src || src.kind === 'split' || src.pane == null) return null;
    if (dir !== 'left' && dir !== 'right' && dir !== 'up' && dir !== 'down') return null;
    // A lone tile has nothing to move out of, and a float would be the
    // second leaf that does not fit.
    if (src.kind === 'leaf' && this.leaves().length < 2) return null;
    if (src.kind === 'float' && this.leaves().length >= MAX_LEAVES) return null;
    const wantDir = dir === 'left' || dir === 'right' ? 'row' : 'col';
    const first = dir === 'left' || dir === 'up';
    // Already the whole of that edge: re-wrapping would rebuild the same
    // shape and throw away the split's ratio on the way.
    const parent = this.parentOf(srcId);
    if (parent && parent === this.root && parent.dir === wantDir
        && (first ? parent.a === src : parent.b === src)) {
      return null;
    }
    const pane = src.pane;
    this.close(srcId);
    const fresh = this._leaf(pane);
    const rest = this.root;
    this.root = this._split(wantDir, 0.5, first ? fresh : rest, first ? rest : fresh);
    return fresh.id;
  }

  // --- tentative changes ------------------------------------------------
  //
  // A structural copy of the tree and the float list, ids and all, so that a
  // change can be shown and then taken back exactly. That is what the live
  // preview of a move needs (layout.js): every frame of it is "the original
  // arrangement, plus this one move", so each step restores and re-applies
  // rather than trying to undo the last one.
  //
  // Not toJSON(): that throws the ids away, and the renderer keys its
  // elements, its keyboard focus and its active window on them.
  snapshot() {
    return { root: cloneNode(this.root), floating: this.floating.map((f) => ({ ...f })) };
  }

  // Restorable more than once — the copy is made on the way out as well as
  // on the way in, so the snapshot is never handed to the live tree.
  restore(snap) {
    if (!snap) return false;
    this.root = cloneNode(snap.root);
    this.floating = snap.floating.map((f) => ({ ...f }));
    return true;
  }

  // Flips every split in the tree at once: side by side becomes stacked and
  // back. Ratios are kept, because a 70/30 row turned on its side is a 70/30
  // column — that is what "the same arrangement, rotated" means, and
  // recomputing them would throw away proportions the user set by hand.
  // Floating windows have no orientation to flip (they are placed by
  // fraction of the workspace, not by any split), so they do not move.
  transpose() {
    let flipped = false;
    const walk = (n) => {
      if (n.kind !== 'split') return;
      n.dir = n.dir === 'row' ? 'col' : 'row';
      flipped = true;
      walk(n.a);
      walk(n.b);
    };
    walk(this.root);
    return flipped;
  }

  setRatio(splitId, ratio) {
    const s = this.find(splitId);
    if (!s || s.kind !== 'split') return false;
    s.ratio = clamp(num(ratio, s.ratio), MIN_RATIO, MAX_RATIO);
    return true;
  }

  // Moves the nearest ancestor split of that orientation by `delta`: on
  // the x axis a positive delta moves the split to the right, on the y
  // axis down. The direction is absolute, not "grow this tile" — Mod+-
  // always moves the split left whichever side of it the active tile is
  // on, the way Omarchy's resize binds behave — so the same key does the
  // same visible thing from either neighbour.
  resize(leafId, axis, delta) {
    const dir = axis === 'x' ? 'row' : 'col';
    let node = this.find(leafId);
    if (!node || node.kind !== 'leaf') return null;
    let parent = this.parentOf(node.id);
    while (parent) {
      if (parent.dir === dir) {
        this.setRatio(parent.id, parent.ratio + delta);
        return parent.id;
      }
      node = parent;
      parent = this.parentOf(parent.id);
    }
    return null;
  }

  // --- floating -------------------------------------------------------------

  _clampFloat(f) {
    f.w = clamp(f.w, FLOAT_MIN, 1);
    f.h = clamp(f.h, FLOAT_MIN, 1);
    f.x = clamp(f.x, -(f.w - FLOAT_KEEP), 1 - FLOAT_KEEP);
    f.y = clamp(f.y, 0, 1 - FLOAT_KEEP);
  }

  // Pops a tile out into a floating window. The tile leaves the tree the
  // way close() removes it; the pane keeps living, now in the float list.
  // A placeholder has nothing to float.
  float(leafId) {
    const leaf = this.find(leafId);
    if (!leaf || leaf.kind !== 'leaf' || leaf.pane === null) return null;
    const pane = leaf.pane;
    this.close(leafId);
    // Cascade successive windows so a second one does not land exactly on
    // the first and hide it.
    const k = this.floating.length;
    const f = this._float(pane, 0.2 + 0.04 * k, 0.12 + 0.04 * k, 0.5, 0.55);
    this.floating.push(f);
    return f.id;
  }

  // Puts a floating window back in the tree, beside `besideLeafId` (or the
  // first leaf) as a horizontal split — or as the only tile when the tree
  // is nothing but an empty placeholder.
  tile(floatId, besideLeafId) {
    const f = this.find(floatId);
    if (!f || f.kind !== 'float') return null;
    this.floating = this.floating.filter((x) => x !== f);
    if (this.root.kind === 'leaf' && this.root.pane === null) {
      this.root.pane = f.pane;
      return this.root.id;
    }
    let target = besideLeafId != null ? this.find(besideLeafId) : null;
    if (!target || target.kind !== 'leaf') target = this.leaves()[0];
    const fresh = this._leaf(f.pane);
    this._replace(target, this._split('row', 0.5, target, fresh));
    return fresh.id;
  }

  // Raises a floating window to the top of the z-order.
  raise(floatId) {
    const f = this.find(floatId);
    if (!f || f.kind !== 'float') return false;
    this.floating = this.floating.filter((x) => x !== f).concat([f]);
    return true;
  }

  moveFloat(id, dx, dy) {
    const f = this.find(id);
    if (!f || f.kind !== 'float') return false;
    f.x += dx;
    f.y += dy;
    this._clampFloat(f);
    return true;
  }

  resizeFloat(id, dw, dh) {
    const f = this.find(id);
    if (!f || f.kind !== 'float') return false;
    f.w += dw;
    f.h += dh;
    this._clampFloat(f);
    return true;
  }

  setFloatRect(id, { x, y, w, h }) {
    const f = this.find(id);
    if (!f || f.kind !== 'float') return false;
    f.x = num(x, f.x);
    f.y = num(y, f.y);
    f.w = num(w, f.w);
    f.h = num(h, f.h);
    this._clampFloat(f);
    return true;
  }
}

// Which candidate is "to the `dir` of" a rectangle. Geometric rather than
// structural on purpose: the tree says who is nested under whom, but the
// user is looking at boxes, and the tile to the right of this one may be
// three splits away in the tree. Candidates are {id, rect} with rects in
// any one coordinate space (left, top, right, bottom).
//
// Rule: the candidate's leading edge must lie at or beyond this rect's
// trailing edge in `dir` (a pixel of slack for rounding), and the two must
// overlap on the other axis. Nearest edge wins; a tie goes to the one whose
// centre is closest on the other axis. With nothing overlapping, fall back
// to the nearest by edge gap plus centre offset, so a lone tile in a corner
// is still reachable.
export function neighbour(from, cands, dir, prefer = []) {
  const horizontal = dir === 'left' || dir === 'right';
  const gapOf = (c) => {
    switch (dir) {
      case 'right': return c.left - from.right;
      case 'left': return from.left - c.right;
      case 'down': return c.top - from.bottom;
      default: return from.top - c.bottom;
    }
  };
  const overlapOf = (c) =>
    horizontal
      ? Math.min(from.bottom, c.bottom) - Math.max(from.top, c.top)
      : Math.min(from.right, c.right) - Math.max(from.left, c.left);
  const centreOff = (c) =>
    horizontal
      ? Math.abs((c.top + c.bottom) / 2 - (from.top + from.bottom) / 2)
      : Math.abs((c.left + c.right) / 2 - (from.left + from.right) / 2);

  // Rank in the focus history, for ties: moving left out of a window into
  // a column of two equally near windows goes back to the one you were in
  // most recently, the way i3 does it, rather than always to the top one.
  const rank = (c) => {
    const i = prefer.indexOf(c.id);
    return i < 0 ? Infinity : i;
  };

  let best = null;
  let bestKey = Infinity;
  let bestOff = Infinity;
  const ahead = cands.filter((c) => gapOf(c.rect) >= -1);
  for (const c of ahead) {
    if (overlapOf(c.rect) <= 0) continue;
    const gap = gapOf(c.rect);
    const off = centreOff(c.rect);
    const tie = best && Math.abs(gap - bestKey) <= 0.5 && Math.abs(off - bestOff) <= 2;
    if (gap < bestKey - 0.5 || (!tie && Math.abs(gap - bestKey) <= 0.5 && off < bestOff) || (tie && rank(c) < rank(best))) {
      best = c;
      bestKey = gap;
      bestOff = off;
    }
  }
  if (best) return best.id;
  for (const c of ahead) {
    const key = gapOf(c.rect) + centreOff(c.rect);
    if (key < bestKey) {
      best = c;
      bestKey = key;
    }
  }
  return best ? best.id : null;
}
