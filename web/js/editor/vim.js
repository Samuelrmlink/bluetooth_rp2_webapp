// Vim keys for the editor, as a pure reducer.
//
//   handle(state, key, text, cursor) -> { state, edit?, cursor, action? }
//
// No DOM here, so the whole thing is tested as tables of text-before,
// keys, text-after (tests/unit/vim.test.js). The textarea adapter applies
// the result: an edit is ONE {from, to, insert} range replacement, which it
// performs with insertText so the browser's own undo history holds it --
// `u` and Ctrl+R then simply drive native undo and redo.
//
// The core set, as agreed:
//   modes      normal, insert, visual (v), visual line (V)
//   motions    h j k l  w b e  0 ^ $  gg G  (with counts), arrows
//   operators  d c y + motion; dd cc yy D C Y x X p P J
//   insert     i a I A o O
//   undo       u, Ctrl+R
//   commands   :w :q :wq :<line>   /pattern  n N
// Not included (later, if wanted): macros, named registers, text objects,
// block visual, `.` repeat.

export function initialState() {
  return { mode: 'normal', count: '', op: null, g: false, reg: { text: '', linewise: false }, anchor: 0, want: null, cmd: null, search: '' };
}

// --- text geometry -------------------------------------------------------------
const lineStart = (t, i) => t.lastIndexOf('\n', i - 1) + 1;
const lineEnd = (t, i) => { const k = t.indexOf('\n', i); return k < 0 ? t.length : k; };
const lineNo = (t, i) => { let n = 0; for (let k = t.indexOf('\n'); k >= 0 && k < i; k = t.indexOf('\n', k + 1)) n++; return n; };
function lineAt(t, n) {
  let s = 0;
  for (let k = 0; k < n; k++) { const e = t.indexOf('\n', s); if (e < 0) return s; s = e + 1; }
  return s;
}
const lineCount = (t) => t.split('\n').length;
const firstNonBlank = (t, i) => { let k = lineStart(t, i); const e = lineEnd(t, i); while (k < e && (t[k] === ' ' || t[k] === '\t')) k++; return k; };

// In normal mode the cursor sits ON a character, never on a line's newline
// (an empty line is the exception: there is nothing else to sit on).
export function clampNormal(t, i) {
  i = Math.max(0, Math.min(i, t.length));
  const s = lineStart(t, i);
  if ((i >= t.length || t[i] === '\n') && i > s) return i - 1;
  return i;
}

const cls = (c) => (c === undefined ? -1 : /\s/.test(c) ? 0 : /\w/.test(c) ? 1 : 2);

function wordFwd(t, i) {
  const c = cls(t[i]);
  if (c > 0) while (i < t.length && cls(t[i]) === c) i++;
  while (i < t.length && cls(t[i]) === 0) i++;
  return i;
}
function wordBack(t, i) {
  i--;
  while (i > 0 && cls(t[i]) === 0) i--;
  const c = cls(t[i]);
  while (i > 0 && cls(t[i - 1]) === c) i--;
  return Math.max(0, i);
}
function wordEnd(t, i) {
  i++;
  while (i < t.length && cls(t[i]) === 0) i++;
  const c = cls(t[i]);
  while (i + 1 < t.length && cls(t[i + 1]) === c) i++;
  return Math.min(i, t.length - 1);
}

// A motion from `c`, `n` times: { pos, inclusive, linewise } or null.
function motion(key, t, c, n, st) {
  const col = st.want ?? c - lineStart(t, c);
  const vert = (dl) => {
    const target = Math.max(0, Math.min(lineCount(t) - 1, lineNo(t, c) + dl));
    const s = lineAt(t, target);
    const len = lineEnd(t, s) - s;
    return { pos: s + Math.min(col, Math.max(0, len - 1)), linewise: true, keepWant: true };
  };
  switch (key) {
    case 'h': case 'ArrowLeft': return { pos: Math.max(lineStart(t, c), c - n) };
    case 'l': case 'ArrowRight': return { pos: Math.min(lineEnd(t, c), c + n) };
    case 'j': case 'ArrowDown': return vert(n);
    case 'k': case 'ArrowUp': return vert(-n);
    case 'w': { let p = c; for (let k = 0; k < n; k++) p = wordFwd(t, p); return { pos: p }; }
    case 'b': { let p = c; for (let k = 0; k < n; k++) p = wordBack(t, p); return { pos: p }; }
    case 'e': { let p = c; for (let k = 0; k < n; k++) p = wordEnd(t, p); return { pos: p, inclusive: true }; }
    case '0': case 'Home': return { pos: lineStart(t, c) };
    case '^': return { pos: firstNonBlank(t, c) };
    case '$': case 'End': {
      const s = lineAt(t, Math.min(lineCount(t) - 1, lineNo(t, c) + n - 1));
      return { pos: lineEnd(t, s), inclusive: true, eol: true };
    }
    case 'G': {
      const ln = st.count ? Number(st.count) - 1 : lineCount(t) - 1;
      return { pos: firstNonBlank(t, lineAt(t, Math.min(ln, lineCount(t) - 1))), linewise: true };
    }
    case 'gg': {
      const ln = st.count ? Number(st.count) - 1 : 0;
      return { pos: firstNonBlank(t, lineAt(t, Math.min(ln, lineCount(t) - 1))), linewise: true };
    }
    default: return null;
  }
}

// The text range an operator acts on, from the cursor to a motion.
function opRange(t, c, m) {
  if (m.linewise) {
    const a = Math.min(c, m.pos), b = Math.max(c, m.pos);
    let from = lineStart(t, a);
    let to = lineEnd(t, b);
    if (to < t.length) to += 1;              // take the newline with the lines
    else if (from > 0) from -= 1;            // last line: take the one before
    return { from, to, linewise: true };
  }
  let from = Math.min(c, m.pos), to = Math.max(c, m.pos);
  if (m.inclusive) to = Math.min(t.length, to + 1);
  return { from, to, linewise: false };
}

const apply = (t, e) => t.slice(0, e.from) + e.insert + t.slice(e.to);

// Returns { state, edit, cursor, action, sel }. `key` is the KeyboardEvent's
// key; ctrl is passed separately. In insert mode everything but Esc is left
// to the textarea ('pass').
export function handle(state, key, text, cursor, ctrl = false) {
  const st = { ...state };
  const out = (extra) => ({ state: st, cursor, ...extra });
  const n = Math.max(1, Number(st.count) || 1);
  const reset = () => { st.count = ''; st.op = null; st.g = false; };

  // --- command line (: and /) ---------------------------------------------------
  if (st.cmd !== null) {
    if (key === 'Escape') { st.cmd = null; return out({}); }
    if (key === 'Backspace') { if (st.cmd.length <= 1) st.cmd = null; else st.cmd = st.cmd.slice(0, -1); return out({}); }
    if (key === 'Enter') {
      const line = st.cmd; st.cmd = null;
      if (line[0] === '/') {
        st.search = line.slice(1);
        return out({ cursor: find(text, st.search, cursor, 1) ?? cursor });
      }
      const c = line.slice(1).trim();
      if (/^\d+$/.test(c)) return out({ cursor: firstNonBlank(text, lineAt(text, Math.max(0, Number(c) - 1))) });
      if (c === 'w') return out({ action: 'save' });
      if (c === 'q' || c === 'q!') return out({ action: 'quit' });
      if (c === 'wq' || c === 'x') return out({ action: 'save-quit' });
      return out({ action: 'unknown', message: `Not an editor command: ${c}` });
    }
    if (key.length === 1) st.cmd += key;
    return out({});
  }

  // --- insert -------------------------------------------------------------------
  if (st.mode === 'insert') {
    if (key !== 'Escape') return out({ action: ctrl && key === 'r' ? 'swallow' : 'pass' });
    st.mode = 'normal';
    const back = cursor > lineStart(text, cursor) ? cursor - 1 : cursor;
    return out({ cursor: clampNormal(text, back) });
  }

  // --- normal and visual --------------------------------------------------------
  if (ctrl) {
    if (key === 'r') { reset(); return out({ action: 'redo' }); }
    return out({ action: 'pass' });
  }
  const visual = st.mode === 'visual' || st.mode === 'vline';

  if (key === 'Escape') {
    const had = visual || st.op || st.count || st.g;
    reset();
    if (visual) { st.mode = 'normal'; }
    // Esc with nothing to cancel is "leave the text" -- the editor's Esc
    // ladder; the adapter declines it so the key layer can blur the field.
    return out({ action: had ? undefined : 'leave' });
  }

  // counts: digits, but a leading 0 is the motion
  if (/^[0-9]$/.test(key) && !(key === '0' && !st.count)) { st.count += key; return out({}); }

  if (st.g) {
    st.g = false;
    if (key !== 'g') { reset(); return out({}); }
    key = 'gg';
  } else if (key === 'g') { st.g = true; return out({}); }

  if (key === ':' || key === '/') { reset(); st.cmd = key; return out({}); }
  if (key === 'n' || key === 'N') {
    reset();
    return out({ cursor: st.search ? find(text, st.search, cursor, key === 'n' ? 1 : -1) ?? cursor : cursor });
  }

  // visual-mode verbs act on the selection
  if (visual) {
    const a = Math.min(st.anchor, cursor), b = Math.max(st.anchor, cursor);
    const range = st.mode === 'vline'
      ? opRange(text, a, { pos: b, linewise: true })
      : { from: a, to: Math.min(text.length, b + 1), linewise: false };
    if (key === 'o') { const k = st.anchor; st.anchor = cursor; return out({ cursor: k }); }
    if (key === 'v' || key === 'V') {
      const m = key === 'v' ? 'visual' : 'vline';
      st.mode = st.mode === m ? 'normal' : m;
      return out({});
    }
    if (key === 'd' || key === 'x' || key === 'y' || key === 'c') {
      st.reg = { text: text.slice(range.from, range.to), linewise: range.linewise };
      reset();
      if (key === 'y') { st.mode = 'normal'; return out({ cursor: range.from }); }
      const edit = { from: range.from, to: range.to, insert: key === 'c' && range.linewise ? '\n' : '' };
      if (key === 'c') { st.mode = 'insert'; return out({ edit, cursor: range.from }); }
      st.mode = 'normal';
      return out({ edit, cursor: clampNormal(apply(text, edit), range.from) });
    }
  }

  // motions (and operator + motion)
  const m = motion(key, text, cursor, n, st);
  if (m) {
    st.want = m.keepWant ? (st.want ?? cursor - lineStart(text, cursor)) : (m.eol ? Infinity : null);
    if (st.op) {
      const op = st.op;
      // `cw` on a word changes to its end, not to the next word's start
      let mm = m;
      if (op === 'c' && key === 'w' && cls(text[cursor]) > 0) mm = motion('e', text, cursor, n, st);
      const r = opRange(text, cursor, mm);
      reset();
      return operate(st, op, r, text, out);
    }
    reset();
    return out({ cursor: visual ? m.pos : clampNormal(text, m.pos) });
  }

  // operators wait for their motion; dd / cc / yy act on whole lines
  if (key === 'd' || key === 'c' || key === 'y') {
    if (st.op === key) {
      const s = lineAt(text, lineNo(text, cursor));
      const e = lineAt(text, Math.min(lineCount(text) - 1, lineNo(text, cursor) + n - 1));
      const r = opRange(text, s, { pos: e, linewise: true });
      reset();
      return operate(st, key, r, text, out);
    }
    st.op = key;
    return out({});
  }
  if (st.op) { reset(); return out({}); }       // operator + something else: cancel

  st.want = null;
  const ls = lineStart(text, cursor), le = lineEnd(text, cursor);
  switch (key) {
    case 'i': reset(); st.mode = 'insert'; return out({});
    case 'a': reset(); st.mode = 'insert'; return out({ cursor: Math.min(le, cursor + (cursor < le ? 1 : 0)) });
    case 'I': reset(); st.mode = 'insert'; return out({ cursor: firstNonBlank(text, cursor) });
    case 'A': reset(); st.mode = 'insert'; return out({ cursor: le });
    case 'o': case 'O': {
      reset();
      st.mode = 'insert';
      const indent = /^[ \t]*/.exec(text.slice(ls, le))[0];
      if (key === 'o') return out({ edit: { from: le, to: le, insert: '\n' + indent }, cursor: le + 1 + indent.length });
      return out({ edit: { from: ls, to: ls, insert: indent + '\n' }, cursor: ls + indent.length });
    }
    case 'v': case 'V': reset(); st.mode = key === 'v' ? 'visual' : 'vline'; st.anchor = cursor; return out({});
    case 'x': case 'X': {
      reset();
      const r = key === 'x'
        ? { from: cursor, to: Math.min(le, cursor + n) }
        : { from: Math.max(ls, cursor - n), to: cursor };
      if (r.from === r.to) return out({});
      st.reg = { text: text.slice(r.from, r.to), linewise: false };
      const edit = { ...r, insert: '' };
      return out({ edit, cursor: clampNormal(apply(text, edit), r.from) });
    }
    case 'D': case 'C': {
      reset();
      const edit = { from: cursor, to: le, insert: '' };
      st.reg = { text: text.slice(cursor, le), linewise: false };
      if (key === 'C') { st.mode = 'insert'; return out({ edit, cursor }); }
      return out({ edit, cursor: clampNormal(apply(text, edit), cursor) });
    }
    case 'Y': {
      reset();
      st.reg = { text: text.slice(ls, Math.min(text.length, le + 1)) + (le >= text.length ? '\n' : ''), linewise: true };
      return out({});
    }
    case 'p': case 'P': {
      reset();
      const { text: r, linewise } = st.reg;
      if (!r) return out({});
      const body = r.repeat(n);
      if (linewise) {
        const lines = body.endsWith('\n') ? body : body + '\n';
        if (key === 'P') return out({ edit: { from: ls, to: ls, insert: lines }, cursor: ls });
        if (le >= text.length) return out({ edit: { from: le, to: le, insert: '\n' + lines.slice(0, -1) }, cursor: le + 1 });
        return out({ edit: { from: le + 1, to: le + 1, insert: lines }, cursor: le + 1 });
      }
      const at = key === 'p' && cursor < le ? cursor + 1 : cursor;
      return out({ edit: { from: at, to: at, insert: body }, cursor: at + body.length - 1 });
    }
    case 'J': {
      reset();
      if (le >= text.length) return out({});
      let k = le + 1;
      while (k < text.length && (text[k] === ' ' || text[k] === '\t')) k++;
      return out({ edit: { from: le, to: k, insert: ' ' }, cursor: le });
    }
    case 'u': reset(); return out({ action: 'undo' });
    default: reset(); return out({});
  }
}

function operate(st, op, r, text, out) {
  let body = text.slice(r.from, r.to);
  if (r.linewise) {
    // The register holds whole lines ending in a newline, however the range
    // got them. Deleting the LAST line borrows the newline before it (so no
    // blank line is left behind), which would otherwise leave the register
    // as "\nline" -- and a later P would paste an empty line first.
    if (body.startsWith('\n') && r.to === text.length) body = body.slice(1) + '\n';
    if (!body.endsWith('\n')) body += '\n';
  }
  st.reg = { text: body, linewise: r.linewise };
  if (op === 'y') return out({ cursor: r.linewise ? lineStart(text, r.from + (text[r.from] === '\n' ? 1 : 0)) : r.from });
  if (op === 'c') {
    st.mode = 'insert';
    // cc keeps the line itself, empties it: replace with the line break
    const insert = r.linewise ? (r.to <= text.length && text[r.to - 1] === '\n' ? '\n' : '') : '';
    const edit = { from: r.from, to: r.to, insert };
    return out({ edit, cursor: r.from });
  }
  const edit = { from: r.from, to: r.to, insert: '' };
  const after = apply(text, edit);
  const at = r.linewise ? firstNonBlank(after, Math.min(r.from, after.length)) : r.from;
  return out({ edit, cursor: clampNormal(after, at) });
}

// Plain-text search, wrapping around; dir 1 forward from after `from`, -1 back.
function find(text, pat, from, dir) {
  if (!pat) return null;
  if (dir > 0) {
    const k = text.indexOf(pat, from + 1);
    return k >= 0 ? k : (text.indexOf(pat) >= 0 ? text.indexOf(pat) : null);
  }
  const k = text.lastIndexOf(pat, from - 1);
  return k >= 0 ? k : (text.lastIndexOf(pat) >= 0 ? text.lastIndexOf(pat) : null);
}

export function modeLabel(st) {
  if (st.cmd !== null) return st.cmd;
  return { normal: '-- NORMAL --', insert: '-- INSERT --', visual: '-- VISUAL --', vline: '-- VISUAL LINE --' }[st.mode]
    + (st.count || st.op || st.g ? `  ${st.count}${st.op || ''}${st.g ? 'g' : ''}` : '');
}
