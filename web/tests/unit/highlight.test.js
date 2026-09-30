import { test } from 'node:test';
import assert from 'node:assert/strict';
import { highlightLua, highlightJson, highlight, languageOf } from '../../js/editor/highlight.js';

// The text a highlighter produces must be the text it was given: every
// character, once, in order. Anything else and the <pre> under the
// transparent textarea drifts out of alignment with what is typed.
const plain = (html) => html.replace(/<[^>]+>/g, '').replace(/&lt;/g, '<').replace(/&gt;/g, '>').replace(/&amp;/g, '&');

const LUA = `-- a comment
local x = 0x1F + 3.5e2 -- trailing
--[==[ block
comment ]==]
local s = "q\\"uote" .. 'single' .. [[long
string]]
function f(a) return a < 2 and nil or true end
sys.sleep(5); bytes.hex(x)`;

test('Lua: round-trips every character, escaped', () => {
  const html = highlightLua(LUA);
  assert.equal(plain(html), LUA);
  assert.ok(!/<(?!\/?span)/.test(html), 'only span tags; < in code is escaped');
});

test('Lua: tokens land in the right classes', () => {
  const h = highlightLua(LUA);
  assert.match(h, /<span class="tk-com">-- a comment<\/span>/);
  assert.match(h, /<span class="tk-com">--\[==\[ block\ncomment \]==\]<\/span>/);
  assert.match(h, /<span class="tk-str">\[\[long\nstring\]\]<\/span>/);
  assert.match(h, /<span class="tk-str">"q\\"uote"<\/span>/);
  assert.match(h, /<span class="tk-num">0x1F<\/span>/);
  assert.match(h, /<span class="tk-num">3.5e2<\/span>/);
  assert.match(h, /<span class="tk-kw">local<\/span>/);
  assert.match(h, /<span class="tk-lit">nil<\/span>/);
  assert.match(h, /<span class="tk-lib">sys<\/span>/);
  assert.match(h, /<span class="tk-fn">f<\/span>/);
});

test('Lua: a field named like a library is not one', () => {
  assert.doesNotMatch(highlightLua('t.string = 1'), /tk-lib">string/);
});

test('Lua: an unterminated string or block ends cleanly', () => {
  for (const src of ['x = "open', 'x = [[never closed', '--[[ never closed']) {
    assert.equal(plain(highlightLua(src)), src);
  }
});

test('JSON: keys, strings, numbers, literals', () => {
  const src = '{ "private_key": "abc", "port": 51820, "on": true, "x": null, "f": -1.5e3 }';
  const h = highlightJson(src);
  assert.equal(plain(h), src);
  assert.match(h, /tk-key">"private_key"/);
  assert.match(h, /tk-str">"abc"/);
  assert.match(h, /tk-num">51820/);
  assert.match(h, /tk-lit">true/);
  assert.match(h, /tk-num">-1.5e3/);
});

test('plain text is escaped and nothing else', () => {
  assert.equal(highlight('<a & b>', 'text'), '&lt;a &amp; b&gt;');
  assert.equal(languageOf('lua/anker.lua'), 'lua');
  assert.equal(languageOf('cfg/wifi.json'), 'json');
  assert.equal(languageOf('index.html.gz'), 'text');
});
