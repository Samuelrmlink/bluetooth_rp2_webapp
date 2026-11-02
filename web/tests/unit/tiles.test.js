import { test } from 'node:test';
import assert from 'node:assert/strict';
import { Layout, neighbour, MIN_RATIO, MAX_RATIO } from '../../js/tiles.js';

const PANES = ['graphs', 'events', 'values', 'offline-graphs', 'offline-events'];

// The stored form is the easiest thing to assert against, and it is what
// the app persists, so the shape tests read it back rather than poking at
// node objects.
const shape = (l) => l.toJSON().tree;

test('split turns a leaf into a split with a placeholder second child', () => {
  const l = Layout.single('graphs', PANES);
  const fresh = l.split(l.root.id, 'row');
  assert.deepEqual(shape(l), ['row', 0.5, 'graphs', null]);
  // The new leaf is the one to focus: it is empty and wants a pane picked.
  assert.equal(l.find(fresh).pane, null);
  // Splitting the placeholder again nests, in the direction asked for.
  l.split(fresh, 'col');
  assert.deepEqual(shape(l), ['row', 0.5, 'graphs', ['col', 0.5, null, null]]);
});

test('split rejects nonsense and never exceeds the leaf cap', () => {
  const l = Layout.single('graphs', PANES);
  assert.equal(l.split(l.root.id, 'diagonal'), null);
  assert.equal(l.split(999, 'row'), null);
  let id = l.root.id;
  for (let i = 0; i < 40; i++) {
    const n = l.split(id, i % 2 ? 'row' : 'col');
    if (n === null) break;
    id = n;
  }
  assert.ok(l.leaves().length <= 16, `leaves ${l.leaves().length}`);
});

test('close collapses the parent onto the sibling and says what to focus', () => {
  const l = Layout.single('graphs', PANES);
  const b = l.split(l.root.id, 'row');
  l.setPane(b, 'events');
  const c = l.split(b, 'col');
  l.setPane(c, 'values');
  // graphs | (events / values)
  assert.deepEqual(shape(l), ['row', 0.5, 'graphs', ['col', 0.5, 'events', 'values']]);
  const next = l.close(b);
  // The grandparent now links straight to the surviving sibling.
  assert.deepEqual(shape(l), ['row', 0.5, 'graphs', 'values']);
  assert.equal(l.find(next).pane, 'values');
});

test('closing the last tile leaves a placeholder, never nothing', () => {
  const l = Layout.single('graphs', PANES);
  const id = l.close(l.root.id);
  assert.equal(id, l.root.id);
  assert.equal(l.root.pane, null);
  assert.equal(l.leaves().length, 1);
  // And closing an already-empty root is a harmless no-op.
  assert.equal(l.close(l.root.id), l.root.id);
});

test('setPane swaps with wherever the pane already lives', () => {
  const l = Layout.single('graphs', PANES);
  const b = l.split(l.root.id, 'row');
  l.setPane(b, 'events');
  // Asking the placeholder-turned-events tile for graphs swaps the two.
  l.setPane(b, 'graphs');
  assert.deepEqual(shape(l), ['row', 0.5, 'events', 'graphs']);
  // A pane nobody shows just moves in; the old occupant is parked.
  l.setPane(b, 'values');
  assert.deepEqual(shape(l), ['row', 0.5, 'events', 'values']);
  assert.equal(l.ownerOf('graphs'), null);
  // null empties; an unknown id is ignored rather than trusted.
  l.setPane(b, null);
  assert.deepEqual(shape(l), ['row', 0.5, 'events', null]);
  l.setPane(b, 'not-a-pane');
  assert.deepEqual(shape(l), ['row', 0.5, 'events', null]);
});

test('setPane swap rule holds across a tile and a floating window', () => {
  const l = Layout.single('graphs', PANES);
  const b = l.split(l.root.id, 'row');
  l.setPane(b, 'events');
  const f = l.float(b);
  assert.equal(l.find(f).pane, 'events');
  // The tile asks for events: the float gets graphs in exchange.
  l.setPane(l.root.id, 'events');
  assert.equal(l.root.pane, 'events');
  assert.equal(l.find(f).pane, 'graphs');
});

test('swapPanes exchanges two windows and refuses splits', () => {
  const l = Layout.single('graphs', PANES);
  const b = l.split(l.root.id, 'row');
  l.setPane(b, 'events');
  const [a] = l.leaves();
  assert.equal(l.swapPanes(a.id, b), true);
  assert.deepEqual(shape(l), ['row', 0.5, 'events', 'graphs']);
  assert.equal(l.swapPanes(a.id, l.root.id), false);
  assert.equal(l.swapPanes(a.id, a.id), false);
});

test('resize moves the nearest ancestor split of the right axis in an absolute direction', () => {
  const l = Layout.single('graphs', PANES);
  const b = l.split(l.root.id, 'row'); // graphs | b
  const c = l.split(b, 'col'); // graphs | (b / c)
  const [a] = l.leaves();
  // A positive x delta moves the row split right, raising the ratio...
  assert.equal(l.resize(a.id, 'x', 0.1), l.root.id);
  assert.equal(l.root.ratio, 0.6);
  // ...from either side of it: the key means "split right", not "grow me".
  l.resize(b, 'x', 0.1);
  assert.equal(l.root.ratio, 0.7);
  l.resize(b, 'x', -0.1);
  assert.equal(l.root.ratio, 0.6);
  // A vertical resize from `c` (the lower tile) finds the column split, not
  // the row, and a positive y delta moves it down.
  const col = l.parentOf(c);
  assert.equal(l.resize(c, 'y', 0.1), col.id);
  assert.equal(col.ratio, 0.6);
  // No ancestor of that axis: nothing to do.
  assert.equal(l.resize(a.id, 'y', 0.1), null);
});

test('ratios are clamped whichever way they are set', () => {
  const l = Layout.single('graphs', PANES);
  l.split(l.root.id, 'row');
  l.setRatio(l.root.id, 0.01);
  assert.equal(l.root.ratio, MIN_RATIO);
  l.setRatio(l.root.id, 99);
  assert.equal(l.root.ratio, MAX_RATIO);
  l.setRatio(l.root.id, NaN);
  assert.equal(l.root.ratio, MAX_RATIO); // unchanged by garbage
});

test('float removes the tile from the tree; tile puts it back beside the active one', () => {
  const l = Layout.single('graphs', PANES);
  const b = l.split(l.root.id, 'row');
  l.setPane(b, 'events');
  const f = l.float(b);
  assert.notEqual(f, null);
  assert.deepEqual(shape(l), 'graphs');
  assert.deepEqual(l.toJSON().floating.map((x) => x.p), ['events']);
  // Back in, as a horizontal split beside the given tile.
  const leaf = l.tile(f, l.root.id);
  assert.deepEqual(shape(l), ['row', 0.5, 'graphs', 'events']);
  assert.equal(l.find(leaf).pane, 'events');
  assert.equal(l.floating.length, 0);
});

test('floating the only tile leaves a placeholder; tiling back fills it', () => {
  const l = Layout.single('graphs', PANES);
  const f = l.float(l.root.id);
  assert.equal(l.root.pane, null);
  assert.equal(l.leaves().length, 1);
  const id = l.tile(f, null);
  assert.equal(id, l.root.id);
  assert.equal(l.root.pane, 'graphs');
});

test('a placeholder cannot be floated', () => {
  const l = Layout.single('graphs', PANES);
  const b = l.split(l.root.id, 'row');
  assert.equal(l.float(b), null);
  assert.deepEqual(shape(l), ['row', 0.5, 'graphs', null]);
});

test('floating windows are clamped so their header stays reachable', () => {
  const l = Layout.single('graphs', PANES);
  const f = l.float(l.root.id);
  l.moveFloat(f, -5, -5);
  const w = l.find(f);
  assert.ok(w.x >= -(w.w - 0.1) - 1e-9);
  assert.equal(w.y, 0);
  l.moveFloat(f, 5, 5);
  assert.ok(w.x <= 0.9 + 1e-9);
  assert.ok(w.y <= 0.9 + 1e-9);
  l.resizeFloat(f, -5, -5);
  assert.equal(w.w, 0.2);
  assert.equal(w.h, 0.2);
  l.resizeFloat(f, 5, 5);
  assert.equal(w.w, 1);
  assert.equal(w.h, 1);
});

test('raise moves a floating window to the top of the ring', () => {
  const l = Layout.single('graphs', PANES);
  const b = l.split(l.root.id, 'row');
  l.setPane(b, 'events');
  const c = l.split(b, 'row');
  l.setPane(c, 'values');
  const f1 = l.float(b);
  const f2 = l.float(c);
  assert.deepEqual(l.windows().map((w) => w.pane), ['graphs', 'events', 'values']);
  l.raise(f1);
  assert.deepEqual(l.windows().map((w) => w.pane), ['graphs', 'values', 'events']);
  assert.equal(l.indexOf(f2), 1);
  assert.equal(l.at(-1).pane, 'events'); // wraps
});

test('the ring is tiles in reading order, then floats', () => {
  const l = Layout.single('graphs', PANES);
  const b = l.split(l.root.id, 'col'); // graphs / b
  l.setPane(b, 'events');
  const c = l.split(l.root.a.id, 'row'); // (graphs | c) / events
  l.setPane(c, 'values');
  assert.deepEqual(l.leaves().map((x) => x.pane), ['graphs', 'values', 'events']);
});

test('toJSON / fromJSON round-trip, floats included', () => {
  const l = Layout.single('graphs', PANES);
  const b = l.split(l.root.id, 'row');
  l.setPane(b, 'events');
  l.setRatio(l.root.id, 0.3);
  const c = l.split(b, 'col');
  l.setPane(c, 'values');
  l.float(c);
  const json = l.toJSON();
  const back = Layout.fromJSON(json, PANES);
  assert.deepEqual(back.toJSON(), json);
  assert.equal(back.leaves().length, 2);
  assert.equal(back.floating[0].pane, 'values');
});

test('fromJSON degrades gracefully instead of throwing', () => {
  // Unknown pane -> placeholder; duplicate -> second occurrence dropped.
  let l = Layout.fromJSON({ tree: ['row', 0.5, 'graphs', ['col', 0.5, 'graphs', 'bogus']] }, PANES);
  assert.deepEqual(shape(l), ['row', 0.5, 'graphs', ['col', 0.5, null, null]]);
  // Bad ratio -> 0.5; bad dir -> placeholder; wrong arity -> placeholder.
  l = Layout.fromJSON({ tree: ['row', 'x', 'graphs', 'events'] }, PANES);
  assert.equal(l.root.ratio, 0.5);
  l = Layout.fromJSON({ tree: ['sideways', 0.5, 'graphs', 'events'] }, PANES);
  assert.equal(shape(l), null);
  l = Layout.fromJSON({ tree: ['row', 0.5, 'graphs'] }, PANES);
  assert.equal(shape(l), null);
  // Garbage, absent, or not an object: a single placeholder.
  for (const junk of [undefined, null, 42, 'graphs', { tree: { nope: 1 } }, { floating: 'x' }]) {
    l = Layout.fromJSON(junk, PANES);
    assert.equal(l.leaves().length, 1, JSON.stringify(junk));
  }
  // A float with an unknown or already-tiled pane is dropped; rects clamp.
  l = Layout.fromJSON(
    { tree: 'graphs', floating: [{ p: 'graphs', x: 0, y: 0, w: 1, h: 1 }, { p: 'values', x: 9, y: 9, w: 0.01, h: 5 }, { p: 'nope' }, 'junk'] },
    PANES,
  );
  assert.equal(l.floating.length, 1);
  assert.equal(l.floating[0].pane, 'values');
  assert.equal(l.floating[0].w, 0.2);
  assert.equal(l.floating[0].h, 1);
  assert.ok(l.floating[0].x <= 0.9);
});

test('fromJSON caps depth', () => {
  let tree = 'graphs';
  for (let i = 0; i < 20; i++) tree = ['row', 0.5, tree, null];
  const l = Layout.fromJSON({ tree }, PANES);
  let depth = 0;
  let n = l.root;
  while (n.kind === 'split') {
    depth++;
    n = n.a;
  }
  assert.ok(depth <= 8, `depth ${depth}`);
});

test('fromLegacy turns the old cookie fields into a two-leaf tree', () => {
  let l = Layout.fromLegacy({ mode: 'stack', primary: 'values', secondary: 'events', ratio: '0.3' }, PANES, true);
  assert.deepEqual(shape(l), ['col', 0.3, 'values', 'events']);
  l = Layout.fromLegacy({ mode: 'side', primary: 'graphs', secondary: 'values', ratio: 0.55 }, PANES, false);
  assert.deepEqual(shape(l), ['row', 0.55, 'graphs', 'values']);
  // auto and tabs follow the orientation the page had at the time.
  l = Layout.fromLegacy({ mode: 'auto', primary: 'graphs', secondary: 'events', ratio: 0.55 }, PANES, true);
  assert.equal(l.root.dir, 'row');
  l = Layout.fromLegacy({ mode: 'tabs', primary: 'graphs', secondary: 'events', ratio: 0.55 }, PANES, false);
  assert.equal(l.root.dir, 'col');
  // Unknown ids fall back the way the old constructor did; a duplicate yields.
  l = Layout.fromLegacy({ mode: 'side', primary: 'nope', secondary: 'nope', ratio: 0.5 }, PANES, true);
  assert.deepEqual(shape(l), ['row', 0.5, 'graphs', 'events']);
  l = Layout.fromLegacy({ mode: 'side', primary: 'values', secondary: 'values', ratio: 0.5 }, PANES, true);
  assert.deepEqual(shape(l), ['row', 0.5, 'values', 'graphs']);
  // Nothing at all: a placeholder, or the only pane there is.
  assert.equal(Layout.fromLegacy({}, [], true).root.pane, null);
  assert.equal(Layout.fromLegacy({}, ['solo'], true).root.pane, 'solo');
});

// A 2x2 grid:  A B
//              C D
const R = (l, t, r, b) => ({ left: l, top: t, right: r, bottom: b });
const grid = [
  { id: 'A', rect: R(0, 0, 100, 100) },
  { id: 'B', rect: R(100, 0, 200, 100) },
  { id: 'C', rect: R(0, 100, 100, 200) },
  { id: 'D', rect: R(100, 100, 200, 200) },
];
const others = (id) => grid.filter((g) => g.id !== id);
const rect = (id) => grid.find((g) => g.id === id).rect;

test('neighbour walks a grid in all four directions', () => {
  assert.equal(neighbour(rect('A'), others('A'), 'right'), 'B');
  assert.equal(neighbour(rect('A'), others('A'), 'down'), 'C');
  assert.equal(neighbour(rect('D'), others('D'), 'left'), 'C');
  assert.equal(neighbour(rect('D'), others('D'), 'up'), 'B');
  // Nothing beyond the edge.
  assert.equal(neighbour(rect('A'), others('A'), 'left'), null);
  assert.equal(neighbour(rect('B'), others('B'), 'right'), null);
});

test('neighbour prefers the overlapping tile, then the nearer centre', () => {
  // An L: a tall tile on the left, two stacked on the right.
  const L = [
    { id: 'tall', rect: R(0, 0, 100, 200) },
    { id: 'top', rect: R(100, 0, 200, 100) },
    { id: 'bottom', rect: R(100, 100, 200, 200) },
  ];
  // From the tall tile both overlap and tie on gap; the tall tile's centre
  // (y=100) is equidistant, so the first in candidate order wins — the
  // caller passes tiles in reading order, so that is the top one.
  assert.equal(neighbour(L[0].rect, [L[1], L[2]], 'right'), 'top');
  // From a rect sitting lower, the bottom one is nearer.
  assert.equal(neighbour(R(0, 120, 100, 200), [L[1], L[2]], 'right'), 'bottom');
  // A tile with no overlap at all is still reachable as a fallback.
  assert.equal(neighbour(R(0, 0, 50, 50), [{ id: 'far', rect: R(300, 300, 400, 400) }], 'right'), 'far');
  assert.equal(neighbour(R(0, 0, 50, 50), [{ id: 'behind', rect: R(300, 300, 400, 400) }], 'left'), null);
});

// --- per-meter pane ids -------------------------------------------------------

test('fromJSON keeps a meter-scoped pane id it does not know yet, so an unplugged meter keeps its tiles', () => {
  // The stored tree names a meter's panes; the page loading it has not
  // heard of that meter (yet). The leaf survives as that id — rendered as
  // "not attached" — rather than degrading to a placeholder that forgets.
  const l = Layout.fromJSON({ tree: ['row', 0.5, 'graphs@81645', 'events'], floating: [{ p: 'values@81645', x: 0.2, y: 0.2, w: 0.5, h: 0.5 }] }, PANES);
  assert.deepEqual(shape(l), ['row', 0.5, 'graphs@81645', 'events']);
  assert.deepEqual(l.toJSON().floating.map((f) => f.p), ['values@81645']);
  assert.equal(l.isKnown('graphs@81645'), true);
  assert.equal(l.isKnown('values@81645'), true);
});

test('fromJSON reads the old peers pane as the local editor', () => {
  const panes = ['editor', 'scanner', 'console'];
  const l = Layout.fromJSON({
    tree: ['row', 0.6, 'peers', ['col', 0.55, 'scanner', 'console']],
  }, panes);
  assert.deepEqual(shape(l), ['row', 0.6, 'editor', ['col', 0.55, 'scanner', 'console']]);
  const floated = Layout.fromJSON({
    tree: 'scanner',
    floating: [{ p: 'peers', x: 0.1, y: 0.1, w: 0.4, h: 0.4 }],
  }, panes);
  assert.deepEqual(floated.toJSON().floating.map((f) => f.p), ['editor']);
});

test('fromJSON still drops an unknown bare id, and a malformed meter id', () => {
  const l = Layout.fromJSON({ tree: ['row', 0.5, 'nope', 'graphs@'] }, PANES);
  assert.deepEqual(shape(l), ['row', 0.5, null, null]);
  assert.equal(l.isKnown('nope'), false);
});

test('addKnown makes a pane placeable; renamePane moves a leaf and a float', () => {
  const l = Layout.single('graphs', PANES);
  const b = l.split(l.root.id, 'row');
  // Unknown: refused, the leaf stays empty.
  l.setPane(b, 'graphs@81645');
  assert.deepEqual(shape(l), ['row', 0.5, 'graphs', null]);
  l.addKnown('graphs@81645');
  l.setPane(b, 'graphs@81645');
  assert.deepEqual(shape(l), ['row', 0.5, 'graphs', 'graphs@81645']);
  // The one-time migration of a bare id to the first meter's.
  assert.equal(l.renamePane('graphs', 'graphs@AB12CD34'), true);
  assert.deepEqual(shape(l), ['row', 0.5, 'graphs@AB12CD34', 'graphs@81645']);
  assert.equal(l.isKnown('graphs@AB12CD34'), true);
  // Floats too.
  const f = l.float(b);
  assert.equal(l.renamePane('graphs@81645', 'graphs@X'), true);
  assert.equal(l.find(f).pane, 'graphs@X');
  assert.equal(l.renamePane('missing', 'x'), false);
});

// --- moving a pane onto another one, by splitting rather than swapping -------

// The arrangement the feature was asked for: two panes across the top, one
// along the bottom. Returns the ids so a test can name them.
function topTwoBottomOne() {
  const l = Layout.single('graphs', PANES);           // graphs
  const bottom = l.split(l.root.id, 'col');           // graphs / bottom
  l.setPane(bottom, 'values');
  const [top] = l.leaves();
  const topRight = l.split(top.id, 'row');            // (graphs | topRight) / values
  l.setPane(topRight, 'events');
  return { l, a: top.id, b: topRight, c: bottom };
}

test('moveInto splits the target and lands the pane on the edge it came to', () => {
  // The user's own case: drag a top pane down onto the bottom one. It
  // arrives at the bottom pane's top edge, so it takes the top half — and
  // the row it left collapses onto its sibling rather than leaving a hole.
  const { l, a, c } = topTwoBottomOne();
  assert.deepEqual(shape(l), ['col', 0.5, ['row', 0.5, 'graphs', 'events'], 'values']);
  const fresh = l.moveInto(a, c, 'up');
  assert.deepEqual(shape(l), ['col', 0.5, 'events', ['col', 0.5, 'graphs', 'values']]);
  assert.equal(l.find(fresh).pane, 'graphs');
});

test('moveInto puts the pane on whichever of the four edges it is given', () => {
  for (const [edge, want] of [
    ['up', ['col', 0.5, 'events', ['col', 0.5, 'graphs', 'values']]],
    ['down', ['col', 0.5, 'events', ['col', 0.5, 'values', 'graphs']]],
    ['left', ['col', 0.5, 'events', ['row', 0.5, 'graphs', 'values']]],
    ['right', ['col', 0.5, 'events', ['row', 0.5, 'values', 'graphs']]],
  ]) {
    const { l, a, c } = topTwoBottomOne();
    l.moveInto(a, c, edge);
    assert.deepEqual(shape(l), want, edge);
  }
});

test('moveInto refuses a move that would rebuild the same shape', () => {
  // graphs | events, with the split dragged to 0.7. Asking graphs to enter
  // events from the left puts it exactly where it is — so it is refused
  // rather than rebuilt at 0.5.
  const l = Layout.single('graphs', PANES);
  const b = l.split(l.root.id, 'row');
  l.setPane(b, 'events');
  const [a] = l.leaves();
  l.setRatio(l.root.id, 0.7);
  assert.equal(l.moveInto(a.id, b, 'left'), null);
  assert.deepEqual(shape(l), ['row', 0.7, 'graphs', 'events']);
  // The other side of the same split is a real move.
  assert.notEqual(l.moveInto(a.id, b, 'right'), null);
  assert.deepEqual(shape(l), ['row', 0.5, 'events', 'graphs']);
  // So is the same edge with a different direction.
  const l2 = Layout.single('graphs', PANES);
  const b2 = l2.split(l2.root.id, 'row');
  l2.setPane(b2, 'events');
  assert.notEqual(l2.moveInto(l2.leaves()[0].id, b2, 'up'), null);
  assert.deepEqual(shape(l2), ['col', 0.5, 'graphs', 'events']);
});

test('moveInto refuses what it cannot move, and keeps every pane in one place', () => {
  const { l, a, b, c } = topTwoBottomOne();
  assert.equal(l.moveInto(a, a, 'up'), null, 'onto itself');
  assert.equal(l.moveInto(a, l.root.id, 'up'), null, 'onto a split');
  assert.equal(l.moveInto(999, c, 'up'), null, 'no such window');
  assert.equal(l.moveInto(a, c, 'sideways'), null, 'no such edge');
  const empty = l.split(b, 'row');
  assert.equal(l.moveInto(empty, c, 'up'), null, 'a placeholder has nothing to move');
  // Nothing above did anything.
  assert.deepEqual(shape(l), ['col', 0.5, ['row', 0.5, 'graphs', ['row', 0.5, 'events', null]], 'values']);
  l.moveInto(a, c, 'up');
  const panes = l.windows().map((w) => w.pane).filter(Boolean);
  assert.equal(new Set(panes).size, panes.length, 'a pane appears at most once');
});

test('moveInto brings a floating window back into the tree, and respects the leaf cap', () => {
  const { l, a, c } = topTwoBottomOne();
  const f = l.float(a);
  assert.equal(l.floating.length, 1);
  l.moveInto(f, c, 'down');
  assert.deepEqual(shape(l), ['col', 0.5, 'events', ['col', 0.5, 'values', 'graphs']]);
  assert.equal(l.floating.length, 0, 'it left the float list');

  // A float is the one source that adds a leaf, so it is the one that can
  // hit the cap. Fill up, then try.
  const full = Layout.single('graphs', PANES);
  let id = full.root.id;
  for (let i = 0; i < 40; i++) {
    const n = full.split(id, 'row');
    if (n === null) break;
    id = n;
  }
  assert.equal(full.leaves().length, 16);
  full.setPane(id, 'events');
  const ff = full.float(id);
  assert.equal(full.leaves().length, 15);
  full.split(full.leaves()[0].id, 'row'); // back to 16
  assert.equal(full.moveInto(ff, full.leaves()[0].id, 'up'), null, 'would be a 17th leaf');
  assert.equal(full.floating.length, 1, 'and the float is untouched');
});

test('moveOutward gives the pane a whole edge of the workspace', () => {
  // A is top-left; there is nothing to its left, so moving left takes it out
  // of the top row and down the whole left side. Note the side is the
  // mirror of moveInto's: out goes to the far edge.
  const { l, a } = topTwoBottomOne();
  const fresh = l.moveOutward(a, 'left');
  assert.deepEqual(shape(l), ['row', 0.5, 'graphs', ['col', 0.5, 'events', 'values']]);
  assert.equal(l.find(fresh).pane, 'graphs');

  const down = topTwoBottomOne();
  down.l.moveOutward(down.a, 'down');
  assert.deepEqual(shape(down.l), ['col', 0.5, ['col', 0.5, 'events', 'values'], 'graphs']);
});

test('moveOutward refuses a lone tile and a pane that is already that whole edge', () => {
  const one = Layout.single('graphs', PANES);
  assert.equal(one.moveOutward(one.root.id, 'left'), null, 'nothing to move out of');

  const l = Layout.single('graphs', PANES);
  const b = l.split(l.root.id, 'row');
  l.setPane(b, 'events');
  const [a] = l.leaves();
  l.setRatio(l.root.id, 0.7);
  // `graphs` is already the whole left edge — re-wrapping would rebuild the
  // same shape and lose the 0.7 on the way.
  assert.equal(l.moveOutward(a.id, 'left'), null);
  assert.deepEqual(shape(l), ['row', 0.7, 'graphs', 'events']);
  // Moving it the other way is a real move.
  assert.notEqual(l.moveOutward(a.id, 'right'), null);
  assert.deepEqual(shape(l), ['row', 0.5, 'events', 'graphs']);
});

test('transpose flips every split, keeps every ratio and leaves floats alone', () => {
  const { l, a, b } = topTwoBottomOne();
  l.setRatio(l.root.id, 0.7);
  l.setRatio(l.parentOf(a).id, 0.3);
  const f = l.float(b);
  const before = l.find(f);
  const at = { x: before.x, y: before.y, w: before.w, h: before.h };

  assert.equal(l.transpose(), true);
  assert.deepEqual(shape(l), ['row', 0.7, 'graphs', 'values']);
  assert.equal(l.transpose(), true);
  assert.deepEqual(shape(l), ['col', 0.7, 'graphs', 'values'], 'twice is where it started');

  const after = l.find(f);
  assert.deepEqual({ x: after.x, y: after.y, w: after.w, h: after.h }, at, 'the float did not move');

  // Nothing to flip: one tile, no splits.
  assert.equal(Layout.single('graphs', PANES).transpose(), false);
});

test('moveAcross gives the pane a band beside both halves of a split', () => {
  // graphs | (events / values): dropping graphs on the line between events
  // and values should put it beside BOTH of them, not halve either one.
  const l = Layout.single('graphs', PANES);
  const right = l.split(l.root.id, 'row');
  l.setPane(right, 'events');
  const lower = l.split(right, 'col');
  l.setPane(lower, 'values');
  const pair = l.parentOf(lower);
  const [graphs] = l.leaves();
  assert.deepEqual(shape(l), ['row', 0.5, 'graphs', ['col', 0.5, 'events', 'values']]);

  const fresh = l.moveAcross(graphs.id, pair.id, 'right');
  assert.deepEqual(shape(l), ['row', 0.5, ['col', 0.5, 'events', 'values'], 'graphs']);
  assert.equal(l.find(fresh).pane, 'graphs');
});

test('moveAcross refuses a band it already is, and copes with the split dissolving', () => {
  const l = Layout.single('graphs', PANES);
  const right = l.split(l.root.id, 'row');
  l.setPane(right, 'events');
  const lower = l.split(right, 'col');
  l.setPane(lower, 'values');
  const pair = l.parentOf(lower);
  const [graphs] = l.leaves();

  // graphs is already the whole left side of that pair.
  assert.equal(l.moveAcross(graphs.id, pair.id, 'left'), null);
  assert.deepEqual(shape(l), ['row', 0.5, 'graphs', ['col', 0.5, 'events', 'values']]);

  // Dropped on the line of a split the pane is itself a child of: removing it
  // dissolves that split, so the band spans what is left of it instead.
  assert.notEqual(l.moveAcross(graphs.id, l.root.id, 'down'), null);
  assert.deepEqual(shape(l), ['col', 0.5, ['col', 0.5, 'events', 'values'], 'graphs']);

  // Not a split, not a pane: refused.
  const l2 = Layout.single('graphs', PANES);
  const b = l2.split(l2.root.id, 'row');
  assert.equal(l2.moveAcross(l2.leaves()[0].id, b, 'up'), null, 'target is a leaf');
  assert.equal(l2.moveAcross(b, l2.root.id, 'up'), null, 'a placeholder has nothing to move');
});

// --- the special resize: push an edge out past a border it does not own ------

// row( col( row(LVa,LVb), BAND ), LG022 ) — the arrangement the feature was
// asked for: a band across two panes, and a third window beside all of them.
function bandBesideThird() {
  const l = Layout.single('graphs', PANES);
  const third = l.split(l.root.id, 'row');
  l.setPane(third, 'offline-graphs');
  const top = l.ownerOf('graphs').id;
  const band = l.split(top, 'col');
  l.setPane(band, 'values');
  const second = l.split(top, 'row');
  l.setPane(second, 'events');
  return l;
}

test('edgeResize pushes one edge out past a border the pane does not own', () => {
  const l = bandBesideThird();
  assert.deepEqual(shape(l), ['row', 0.5, ['col', 0.5, ['row', 0.5, 'graphs', 'events'], 'values'], 'offline-graphs']);

  // The band's right edge goes out: it alone gets the width, and the window
  // it took from is re-nested with the two it was beside.
  const band = l.ownerOf('values').id;
  assert.notEqual(l.edgeResize(band, 'right', true), null);
  assert.deepEqual(shape(l), ['col', 0.5, ['row', 0.5, ['row', 0.5, 'graphs', 'events'], 'offline-graphs'], 'values']);

  // And back again, exactly.
  assert.notEqual(l.edgeResize(l.ownerOf('values').id, 'right', false), null);
  assert.deepEqual(shape(l), ['row', 0.5, ['col', 0.5, ['row', 0.5, 'graphs', 'events'], 'values'], 'offline-graphs']);
});

test('edgeResize keeps the dimension it is not growing', () => {
  const l = bandBesideThird();
  const band = l.ownerOf('values').id;
  l.setRatio(l.parentOf(band).id, 0.7); // the band is 30% of the column's height
  l.setRatio(l.root.id, 0.6); // the column is 60% of the width

  assert.notEqual(l.edgeResize(band, 'right', true), null);
  // Still 30% of the height, now across the whole width; the 60% it used to
  // share moved inside, so the other three keep their proportions too.
  assert.deepEqual(shape(l), ['col', 0.7, ['row', 0.6, ['row', 0.5, 'graphs', 'events'], 'offline-graphs'], 'values']);
  assert.notEqual(l.edgeResize(l.ownerOf('values').id, 'right', false), null);
  assert.deepEqual(shape(l), ['row', 0.6, ['col', 0.7, ['row', 0.5, 'graphs', 'events'], 'values'], 'offline-graphs']);
});

test('edgeResize is refused where the ordinary resize already does the job', () => {
  // Two panes side by side: the one across the border is exactly this pane's
  // size, so moving that border is all anyone wants. Alt+-/= does it.
  const l = Layout.single('graphs', PANES);
  const b = l.split(l.root.id, 'row');
  l.setPane(b, 'events');
  assert.equal(l.edgeResize(l.ownerOf('graphs').id, 'right', true), null);
  // Nothing on the other side at all: the pane is against the workspace edge.
  assert.equal(l.edgeResize(l.ownerOf('graphs').id, 'left', true), null);
  assert.equal(l.edgeResize(l.ownerOf('graphs').id, 'down', true), null);
  assert.deepEqual(shape(l), ['row', 0.5, 'graphs', 'events']);
  // Nothing to pull back into either: the neighbour is a bare pane.
  assert.equal(l.edgeResize(l.ownerOf('graphs').id, 'right', false), null);
  // A placeholder has nothing to move, and a bad edge is a bad edge.
  const ph = l.split(b, 'col');
  assert.equal(l.edgeResize(ph, 'right', true), null);
  assert.equal(l.edgeResize(l.ownerOf('graphs').id, 'sideways', true), null);
});

test('edgeResize crosses one border per call, and back the same way', () => {
  // row( row( col(graphs,events), values ), offline ): graphs has two
  // vertical borders to its right, one inside the other.
  const l = Layout.single('graphs', PANES);
  const z = l.split(l.root.id, 'row');
  l.setPane(z, 'offline-graphs');
  const g = l.ownerOf('graphs').id;
  l.setPane(l.split(g, 'row'), 'values');
  l.setPane(l.split(g, 'col'), 'events');
  const start = shape(l);
  assert.deepEqual(start, ['row', 0.5, ['row', 0.5, ['col', 0.5, 'graphs', 'events'], 'values'], 'offline-graphs']);

  // One press crosses the inner border: graphs spans the left group.
  assert.notEqual(l.edgeResize(l.ownerOf('graphs').id, 'right', true), null);
  assert.deepEqual(shape(l), ['row', 0.5, ['col', 0.5, 'graphs', ['row', 0.5, 'events', 'values']], 'offline-graphs']);

  // A second press crosses the outer one: now the whole width.
  assert.notEqual(l.edgeResize(l.ownerOf('graphs').id, 'right', true), null);
  assert.deepEqual(shape(l), ['col', 0.5, 'graphs', ['row', 0.5, ['row', 0.5, 'events', 'values'], 'offline-graphs']]);

  // A third has nowhere left to go.
  assert.equal(l.edgeResize(l.ownerOf('graphs').id, 'right', true), null);

  // Two of the reverse walk back to exactly where it started, handing the
  // room back to the windows that gave it up.
  assert.notEqual(l.edgeResize(l.ownerOf('graphs').id, 'right', false), null);
  assert.notEqual(l.edgeResize(l.ownerOf('graphs').id, 'right', false), null);
  assert.deepEqual(shape(l), start);

  // Every pane is still in exactly one place.
  const panes = l.windows().map((w) => w.pane).filter(Boolean);
  assert.equal(new Set(panes).size, panes.length);
});
