import { test } from 'node:test';
import assert from 'node:assert/strict';
import { initialState, handle, modeLabel, searchMatches } from '../../js/editor/vim.js';

// Run keys over text; `|` marks the cursor before and after. Keys are single
// characters, or <Esc> <Enter> <BS> <C-r>. Edits are applied exactly as the
// textarea adapter applies them: one {from, to, insert} range each.
function run(src, keys, st = initialState()) {
  let cursor = src.indexOf('|');
  let text = src.replace('|', '');
  const actions = [];
  const yanks = [];
  for (const k of keys.match(/<[^>]+>|./gs)) {
    const ctrl = k.startsWith('<C-');
    const key = { '<Esc>': 'Escape', '<Enter>': 'Enter', '<BS>': 'Backspace' }[k] || (ctrl ? k[3] : k);
    const r = handle(st, key, text, cursor, ctrl);
    st = r.state;
    if (r.action === 'pass' && key.length === 1) {       // typed in insert mode
      text = text.slice(0, cursor) + key + text.slice(cursor);
      cursor += 1;
      continue;
    }
    if (r.edit) text = text.slice(0, r.edit.from) + r.edit.insert + text.slice(r.edit.to);
    // a : command is recorded with its detail -- 'ex:w! path'
    if (r.action) actions.push(r.ex ? `ex:${r.ex.cmd}${r.ex.force ? '!' : ''}${r.ex.arg ? ' ' + r.ex.arg : ''}` : r.action);
    if (r.yank !== undefined) yanks.push(r.yank);
    cursor = r.cursor;
  }
  return { out: text.slice(0, cursor) + '|' + text.slice(cursor), st, actions, yanks };
}

const CASES = [
  // [name, before, keys, after]
  ['l stops at the line end', 'ab|c\nde', 'lll', 'ab|c\nde'],
  ['j keeps the column', 'ab|cd\nxy\nqrst', 'jj', 'abcd\nxy\nqr|st'],
  ['w b e', '|foo bar.baz', 'w', 'foo |bar.baz'],
  ['w over punctuation', 'foo |bar.baz', 'w', 'foo bar|.baz'],
  ['b back a word', 'foo bar|', 'b', 'foo |bar'],
  ['e to word end', '|foo bar', 'e', 'fo|o bar'],
  ['counts', '|a b c d', '3w', 'a b c |d'],
  ['0 ^ $', '  ab|cd', '0', '|  abcd'],
  ['^ first non-blank', '  ab|cd', '^', '  |abcd'],
  ['$ end of line', '|abcd\nx', '$', 'abc|d\nx'],
  ['gg and G', 'a\nb\n|c', 'gg', '|a\nb\nc'],
  ['G to last line', '|a\nb\nc', 'G', 'a\nb\n|c'],
  ['2G to line 2', '|a\nb\nc', '2G', 'a\n|b\nc'],
  ['x deletes under the cursor', 'a|bc', 'x', 'a|c'],
  ['3x', '|abcd', '3x', '|d'],
  ['dw', '|foo bar', 'dw', '|bar'],
  ['de', '|foo bar', 'de', '| bar'],
  ['d$ and D', 'ab|cd', 'D', 'a|b'],
  ['dd deletes the line', 'a\n|b\nc', 'dd', 'a\n|c'],
  ['dd on the last line', 'a\nb\n|c', 'dd', 'a\n|b'],
  ['2dd', '|a\nb\nc', '2dd', '|c'],
  ['dj deletes two lines', '|a\nb\nc', 'dj', '|c'],
  ['cw changes to the word end', '|foo bar', 'cwX<Esc>', '|X bar'],
  ['cc empties the line, keeps it', 'a\n  |bc\nd', 'ccX<Esc>', 'a\n|X\nd'],
  ['C changes to line end', 'ab|cd', 'CX<Esc>', 'ab|X'],
  ['i a', 'a|c', 'ib<Esc>', 'a|bc'],
  ['a appends after', 'a|c', 'ab<Esc>', 'ac|b'],
  ['I and A', '  a|b', 'Ix<Esc>A!<Esc>', '  xab|!'],
  ['o opens below, keeps indent', '  a|b\nc', 'ox<Esc>', '  ab\n  |x\nc'],
  ['O opens above', 'a\n|b', 'Ox<Esc>', 'a\n|x\nb'],
  ['yy p pastes the line below', '|a\nb', 'yyp', 'a\n|a\nb'],
  ['yy P pastes the line above', 'a\n|b', 'yyP', 'a\n|b\nb'],
  ['yw P pastes before, cursor on its last char', '|ab cd', 'ywP', 'ab| ab cd'],
  ['yw p pastes after the cursor', '|ab cd', 'ywp', 'aab| b cd'],
  ['dd then p moves a line down', '|a\nb\nc', 'ddp', 'b\n|a\nc'],
  ['J joins, squeezing indent', '|a\n   b', 'J', 'a| b'],
  ['v selects, d deletes the selection', 'a|bcd', 'vld', 'a|d'],
  ['V selects lines', 'a\n|b\nc', 'Vd', 'a\n|c'],
  ['v y then P', '|ab', 'vlyP', 'a|bab'],
  ['yy on the last line, then P, pastes a whole line', 'a\n|b', 'yyP', 'a\n|b\nb'],
  ['dd on the last line, then P, restores it', 'a\n|b', 'ddP', '|b\na'],
  ['/ searches, n repeats', '|x foo y foo', '/foo<Enter>n', 'x foo y |foo'],
  ['N searches back', 'x foo y |foo', '/foo<Enter>N', 'x foo y |foo'],
  ['n wraps past the end', 'x |foo y foo', '/foo<Enter>n', 'x |foo y foo'],
  ['? searches backwards, n keeps going back', 'foo a foo b |foo', '?foo<Enter>n', '|foo a foo b foo'],
  ['N after ? goes forwards', '|foo a foo b foo', '?foo<Enter>N', '|foo a foo b foo'],
  ['? from the very start wraps to the last match', '|foo a foo', '?foo<Enter>', 'foo a |foo'],
  ['an empty / repeats the last search', '|ab ab ab', '/ab<Enter>/<Enter>', 'ab ab |ab'],
  ['* finds the next whole word', '|foo foobar foo', '*', 'foo foobar |foo'],
  ['# finds the previous whole word', 'foo foobar |foo', '#', '|foo foobar foo'],
  ['* from inside a word', 'f|oo x foo', '*', 'foo x |foo'],
  [':$ goes to the last line', '|a\nb\n  c', ':$<Enter>', 'a\nb\n  |c'],
  ['v p replaces the selection', '|ab cd', 'yewvep', 'ab a|b'],
  ['v P replaces and keeps the register for again', '|x y z', 'ylwvPwvP', 'x x |x'],
  ['v p leaves the replaced text in the register', '|ab cd', 'yewvepbp', 'ab ac|db'],
  ['V p with a line replaces the line', '|a\nb\nc', 'yyjVp', 'a\n|a\nc'],
  [':3 goes to a line', '|a\nb\nc', ':3<Enter>', 'a\nb\n|c'],
  ['Esc cancels a pending operator', '|abc', 'd<Esc>x', '|bc'],
];

for (const [name, before, keys, after] of CASES) {
  test(`vim: ${name}`, () => assert.equal(run(before, keys).out, after));
}

test('vim: u and Ctrl+R are the browser\'s undo and redo', () => {
  assert.deepEqual(run('|a', 'u<C-r>').actions, ['undo', 'redo']);
});

test('vim: : commands are handed to the editor with their ! and argument', () => {
  assert.deepEqual(run('|a', ':w<Enter>:q<Enter>:q!<Enter>:wq<Enter>:x<Enter>:foo<Enter>').actions,
    ['ex:w', 'ex:q', 'ex:q!', 'ex:wq', 'ex:x', 'unknown']);
  assert.deepEqual(run('|a', ':w lua/new.lua<Enter>:e! <Enter>:e cfg/x.json<Enter>:run<Enter>:stop<Enter>').actions,
    ['ex:w lua/new.lua', 'ex:e!', 'ex:e cfg/x.json', 'ex:run', 'ex:stop']);
  assert.deepEqual(run('|a', ':write<Enter>:quit!<Enter>:xit<Enter>').actions, ['ex:w', 'ex:q!', 'ex:x']);
});

test('vim: ZZ is :x and ZQ is :q!', () => {
  assert.deepEqual(run('|a', 'ZZZQ').actions, ['ex:x', 'ex:q!']);
  assert.deepEqual(run('|a', 'Zx').actions, [], 'Z then anything else does nothing');
});

test('vim: search highlights until :noh, and says when it wraps or misses', () => {
  let r = run('|a b a', '/a<Enter>');
  assert.equal(r.st.hl, true);
  assert.equal(run('|a b a', '/a<Enter>:noh<Enter>').st.hl, false);
  assert.equal(run('|a b a', '/a<Enter>:noh<Enter>n').st.hl, true, 'n turns it back on');
  r = run('a b |a', '/a<Enter>');
  assert.deepEqual(r.actions, ['info'], 'wrapped past the end');
  r = run('|abc', '/zz<Enter>');
  assert.deepEqual(r.actions, ['info']);
  assert.equal(r.out, '|abc', 'not found: the cursor stays');
  assert.deepEqual(run('|abc', 'n').actions, ['info'], 'n with no search yet');
});

test('vim: searchMatches finds every match, whole words when asked', () => {
  assert.deepEqual(searchMatches('foo foobar foo', 'foo'), [0, 4, 11]);
  assert.deepEqual(searchMatches('foo foobar foo', 'foo', true), [0, 11]);
  assert.deepEqual(searchMatches('aaaa', 'aa'), [0, 1, 2], 'overlapping, as vim highlights them');
  assert.deepEqual(searchMatches('abc', ''), []);
  assert.equal(searchMatches('x'.repeat(100), 'x', false, 10).length, 10, 'capped');
});

test('vim: yanks are reported so the editor can copy them to the clipboard', () => {
  assert.deepEqual(run('|ab cd', 'yw').yanks, ['ab ']);
  assert.deepEqual(run('|ab\ncd', 'yy').yanks, ['ab\n']);
  assert.deepEqual(run('|ab\ncd', 'Y').yanks, ['ab\n']);
  assert.deepEqual(run('a|bcd', 'vly').yanks, ['bc']);
  assert.deepEqual(run('|abc', 'x').yanks, [], 'a delete is not a yank');
});

test('vim: Esc with nothing to cancel means "leave the text"', () => {
  assert.deepEqual(run('|a', '<Esc>').actions, ['leave']);
  assert.deepEqual(run('|a', 'i<Esc>').actions, [], 'insert -> normal is not leaving');
  assert.deepEqual(run('|a', 'v<Esc>').actions, [], 'visual -> normal is not leaving');
});

test('vim: insert mode leaves typing, and Ctrl+R alone (it reloads the page)', () => {
  const st = handle(initialState(), 'i', 'a', 0).state;
  assert.equal(handle(st, 'x', 'a', 0).action, 'pass');
  assert.equal(handle(st, 'r', 'a', 0, true).action, 'swallow');
});

test('vim: the mode label', () => {
  assert.equal(modeLabel(initialState()), '-- NORMAL --');
  assert.equal(modeLabel(run('|a', 'i').st), '-- INSERT --');
  assert.equal(modeLabel(run('|a', 'V').st), '-- VISUAL LINE --');
  assert.equal(modeLabel(run('|a', '2d').st), '-- NORMAL --  2d');
  assert.equal(modeLabel(run('|a', ':wq').st), ':wq');
  assert.equal(modeLabel(run('|a', '?ab').st), '?ab');
});
