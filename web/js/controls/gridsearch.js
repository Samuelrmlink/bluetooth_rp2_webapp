// Search over what a table shows: every cell's displayed text, case-
// insensitively. DOM-free (tests/unit/gridsearch.test.js); the window draws
// the marks and moves the cursor.
//
//   findMatches(rows, columns, q) -> [{ row, col, start, len }]   row-major
//   matchCells(matches)          -> [{ row, col, ranges: [[start, len]...] }]
//   stepMatch(cells, from, dir)  -> index of the next / previous cell, wrapping

export const cellText = (row, c) => {
  const v = c.show ? c.show(row) : c.get ? c.get(row) : '';
  return v == null ? '' : String(v);
};

export function findMatches(rows, columns, q) {
  const needle = String(q || '').toLowerCase();
  const out = [];
  if (!needle) return out;
  rows.forEach((r, i) => {
    columns.forEach((c, j) => {
      if (c.searchable === false) return;
      const hay = cellText(r, c).toLowerCase();
      for (let k = hay.indexOf(needle); k >= 0; k = hay.indexOf(needle, k + needle.length)) {
        out.push({ row: i, col: j, start: k, len: needle.length });
      }
    });
  });
  return out;
}

export function matchCells(matches) {
  const out = [];
  for (const m of matches) {
    const last = out[out.length - 1];
    if (last && last.row === m.row && last.col === m.col) last.ranges.push([m.start, m.len]);
    else out.push({ row: m.row, col: m.col, ranges: [[m.start, m.len]] });
  }
  return out;
}

// From `from` ({row, col}, or null for "before the first"), the next cell in
// direction dir (1 / -1), wrapping. -1 when there is none.
export function stepMatch(cells, from, dir) {
  if (!cells.length) return -1;
  const key = (c) => c.row * 1e4 + c.col;
  if (!from) return dir > 0 ? 0 : cells.length - 1;
  const f = key(from);
  if (dir > 0) {
    const i = cells.findIndex((c) => key(c) > f);
    return i >= 0 ? i : 0;
  }
  for (let i = cells.length - 1; i >= 0; i--) if (key(cells[i]) < f) return i;
  return cells.length - 1;
}

const esc = (s) => s.replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));

// The cell's text as HTML with each range in <mark>; everything escaped.
export function highlightHTML(text, ranges) {
  let html = '';
  let k = 0;
  for (const [s, n] of [...ranges].sort((a, b) => a[0] - b[0])) {
    if (s < k) continue;
    html += esc(text.slice(k, s)) + `<mark>${esc(text.slice(s, s + n))}</mark>`;
    k = s + n;
  }
  return html + esc(text.slice(k));
}
