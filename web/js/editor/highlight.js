// Syntax highlighting for the editor: text in, escaped HTML with <span>s out.
//
// A single pass over the WHOLE text, not line by line. Lua's long strings
// and block comments ([[ ... ]], --[==[ ... ]==]) span lines, and a
// line-at-a-time highlighter has to carry "inside a long comment" state from
// line to line and invalidate it correctly on every edit; one pass over the
// text has no such state to get wrong. Files here are small (a Lua draft is
// capped at 32 KB by the firmware), so the whole pass costs a few ms and
// runs at most once a frame.
//
// DOM-free, so it is unit-tested directly (tests/unit/highlight.test.js).

const esc = (s) => s.replace(/[&<>]/g, (c) => (c === '&' ? '&amp;' : c === '<' ? '&lt;' : '&gt;'));
const span = (cls, s) => `<span class="tk-${cls}">${esc(s)}</span>`;

const LUA_KW = new Set(('and break do else elseif end for function goto if in local not or ' +
  'repeat return then until while').split(' '));
const LUA_LIT = new Set(['nil', 'true', 'false']);
// The gateway's own bindings and the Lua builtins scripts actually use: a
// hint that a name is provided, not an exhaustive list.
const LUA_LIB = new Set(('sys bt bytes crypto ops log require print pairs ipairs next type ' +
  'tostring tonumber select error assert pcall xpcall setmetatable getmetatable rawget rawset ' +
  'string table math coroutine utf8 os').split(' '));

// Long bracket opener at `i` ("[", n "=", "[")? Returns the level or -1.
function longOpen(t, i) {
  if (t[i] !== '[') return -1;
  let j = i + 1;
  while (t[j] === '=') j++;
  return t[j] === '[' ? j - i - 1 : -1;
}

function longEnd(t, from, level) {
  const close = ']' + '='.repeat(level) + ']';
  const k = t.indexOf(close, from);
  return k < 0 ? t.length : k + close.length;
}

export function highlightLua(t) {
  let out = '';
  let i = 0;
  const n = t.length;
  while (i < n) {
    const c = t[i];
    // comments: --[[ block ]] or -- to end of line
    if (c === '-' && t[i + 1] === '-') {
      const lv = longOpen(t, i + 2);
      const end = lv >= 0 ? longEnd(t, i + 4 + lv, lv) : (t.indexOf('\n', i) < 0 ? n : t.indexOf('\n', i));
      out += span('com', t.slice(i, end));
      i = end;
      continue;
    }
    // long strings
    if (c === '[') {
      const lv = longOpen(t, i);
      if (lv >= 0) {
        const end = longEnd(t, i + 2 + lv, lv);
        out += span('str', t.slice(i, end));
        i = end;
        continue;
      }
    }
    // quoted strings, with escapes; an unterminated one stops at the line end
    if (c === '"' || c === "'") {
      let j = i + 1;
      while (j < n && t[j] !== c && t[j] !== '\n') j += t[j] === '\\' ? 2 : 1;
      if (t[j] === c) j++;
      out += span('str', t.slice(i, j));
      i = j;
      continue;
    }
    // numbers: hex, decimal, exponents
    if (/[0-9]/.test(c) || (c === '.' && /[0-9]/.test(t[i + 1] || ''))) {
      const m = /^(0[xX][0-9a-fA-F]*(\.[0-9a-fA-F]*)?([pP][+-]?\d+)?|\d*\.?\d*([eE][+-]?\d+)?)/.exec(t.slice(i, i + 64));
      const len = Math.max(1, m[0].length);
      out += span('num', t.slice(i, i + len));
      i += len;
      continue;
    }
    // names
    if (/[A-Za-z_]/.test(c)) {
      let j = i + 1;
      while (j < n && /[A-Za-z0-9_]/.test(t[j])) j++;
      const w = t.slice(i, j);
      const prevDot = t[i - 1] === '.' || t[i - 1] === ':';
      if (LUA_KW.has(w)) out += span('kw', w);
      else if (LUA_LIT.has(w)) out += span('lit', w);
      else if (!prevDot && LUA_LIB.has(w)) out += span('lib', w);
      else if (t[j] === '(' || (t[j] === ' ' && t[j + 1] === '(')) out += span('fn', w);
      else out += esc(w);
      i = j;
      continue;
    }
    // everything else, a run at a time
    let j = i + 1;
    while (j < n && !/[-\[\"'0-9A-Za-z_.]/.test(t[j])) j++;
    out += esc(t.slice(i, j));
    i = j;
  }
  return out;
}

export function highlightJson(t) {
  let out = '';
  let i = 0;
  const n = t.length;
  while (i < n) {
    const c = t[i];
    if (c === '"') {
      let j = i + 1;
      while (j < n && t[j] !== '"' && t[j] !== '\n') j += t[j] === '\\' ? 2 : 1;
      if (t[j] === '"') j++;
      // a key is a string followed (after spaces) by a colon
      let k = j;
      while (k < n && (t[k] === ' ' || t[k] === '\t')) k++;
      out += span(t[k] === ':' ? 'key' : 'str', t.slice(i, j));
      i = j;
      continue;
    }
    const m = /^(-?\d+(\.\d+)?([eE][+-]?\d+)?|true|false|null)/.exec(t.slice(i, i + 40));
    if (m && (i === 0 || !/[A-Za-z0-9_]/.test(t[i - 1]))) {
      out += span(/\d/.test(m[0][m[0].length - 1]) ? 'num' : 'lit', m[0]);
      i += m[0].length;
      continue;
    }
    let j = i + 1;
    while (j < n && !/["0-9tfn-]/.test(t[j])) j++;
    out += esc(t.slice(i, j));
    i = j;
  }
  return out;
}

export function languageOf(path) {
  if (/\.lua$/i.test(path)) return 'lua';
  if (/\.json$/i.test(path)) return 'json';
  return 'text';
}

export function highlight(text, lang) {
  if (lang === 'lua') return highlightLua(text);
  if (lang === 'json') return highlightJson(text);
  return esc(text);
}
