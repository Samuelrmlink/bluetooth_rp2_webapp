// The rules of a table's columns, without the DOM: order, visibility, widths,
// sorting. controls/grid.js draws them; tests/unit/grid.test.js holds them.
//
// A table's state, as remembered per table in btgw.prefs.tables[id]:
//   { widths: {key: px}, sort: {key, dir} | null, order: [key...], hidden: [key...] }

export const MIN_W = 24;
export const MAX_W = 1200;

// Nulls and empty strings sort last whichever way, so "unknown" never crowds
// out the values you sorted to see. Numbers compare as numbers, text
// case-insensitively and naturally ("dev2" before "dev10").
const blank = (v) => v == null || v === '';
export function compareValues(a, b, type) {
  if (blank(a) || blank(b)) return blank(a) === blank(b) ? 0 : blank(a) ? 1 : -1;
  if (type === 'num') return Number(a) - Number(b);
  return String(a).localeCompare(String(b), undefined, { numeric: true, sensitivity: 'base' });
}

// A stable sort of `rows` by column `col` in direction `dir` (1 or -1),
// blanks last in both directions.
export function sortRows(rows, col, dir) {
  return rows
    .map((r, i) => [r, i, col.get(r)])
    .sort(([, ia, va], [, ib, vb]) => {
      if (blank(va) || blank(vb)) return (blank(va) === blank(vb) ? 0 : blank(va) ? 1 : -1) || ia - ib;
      return dir * compareValues(va, vb, col.type) || ia - ib;
    })
    .map(([r]) => r);
}

// The full order: the remembered one, minus columns that no longer exist,
// plus any new ones in their declared place (after their declared
// predecessor), so adding a column to a table never scrambles a user's order.
// `pinned` columns (the scanner's Connect) always lead, in declared order.
export function columnOrder(columns, order = []) {
  const pinned = columns.filter((c) => c.pinned).map((c) => c.key);
  const keys = columns.filter((c) => !c.pinned).map((c) => c.key);
  const known = new Set(keys);
  const out = (order || []).filter((k) => known.has(k));
  for (let i = 0; i < keys.length; i++) {
    if (out.includes(keys[i])) continue;
    const prev = keys.slice(0, i).reverse().find((k) => out.includes(k));
    out.splice(prev ? out.indexOf(prev) + 1 : 0, 0, keys[i]);
  }
  return [...pinned, ...out];
}

const isPinned = (columns, key) => !!columns.find((c) => c.key === key)?.pinned;

// The hidden keys: what was chosen, or -- never chosen -- the columns
// declared `hidden: true` (an optional column starts off).
export const hiddenKeys = (columns, state = {}) =>
  state.hidden ?? columns.filter((c) => c.hidden).map((c) => c.key);

// The columns to draw, in order. A `fixed` (or `pinned`) column cannot be hidden.
export function visibleColumns(columns, state = {}) {
  const byKey = new Map(columns.map((c) => [c.key, c]));
  const hidden = new Set(hiddenKeys(columns, state));
  return columnOrder(columns, state.order)
    .map((k) => byKey.get(k))
    .filter((c) => c && (c.fixed || c.pinned || !hidden.has(c.key)));
}

// Move `key` by `delta` places among the VISIBLE columns (hidden ones keep
// their relative place in the full order). Returns the new full order.
export function moveColumn(columns, state, key, delta) {
  const order = columnOrder(columns, state.order);
  if (isPinned(columns, key)) return order;
  const vis = visibleColumns(columns, state).map((c) => c.key).filter((k) => !isPinned(columns, k));
  const i = vis.indexOf(key);
  const j = Math.max(0, Math.min(vis.length - 1, i + delta));
  if (i < 0 || i === j) return order;
  const target = vis[j];
  const without = order.filter((k) => k !== key);
  const at = without.indexOf(target) + (delta > 0 ? 1 : 0);
  without.splice(at, 0, key);
  return without;
}

// Put `key` right before `beforeKey` (null = at the end) -- a drag's drop.
export function placeColumn(columns, state, key, beforeKey) {
  if (isPinned(columns, key)) return columnOrder(columns, state.order);
  if (beforeKey != null && isPinned(columns, beforeKey)) beforeKey = columnOrder(columns, state.order).find((k) => !isPinned(columns, k)) ?? null;
  const order = columnOrder(columns, state.order).filter((k) => k !== key);
  const at = beforeKey == null ? order.length : order.indexOf(beforeKey);
  order.splice(at < 0 ? order.length : at, 0, key);
  return order;
}

export const clampWidth = (w) => Math.max(MIN_W, Math.min(MAX_W, Math.round(w)));

// The width a column is drawn at.
export const widthOf = (col, state = {}) => (state.widths && state.widths[col.key]) || col.width;

// A new widths map with `key` at `px` (the default width is not stored).
export function setWidth(columns, state, key, px) {
  const col = columns.find((c) => c.key === key);
  const w = clampWidth(px);
  const next = { ...(state.widths || {}) };
  if (col && w === col.width) delete next[key]; else next[key] = w;
  return next;
}

// Header click / Enter: ascending -> descending -> back to the table's default.
export function nextSort(sort, key) {
  if (!sort || sort.key !== key) return { key, dir: 'asc' };
  if (sort.dir === 'asc') return { key, dir: 'desc' };
  return null;
}
