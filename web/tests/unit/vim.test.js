import { test } from 'node:test';
import assert from 'node:assert/strict';
import { initialState, handle, modeLabel } from '../../js/editor/vim.js';

// Run keys over text; `|` marks the cursor before and after. Keys are single
// characters, or <Esc> <Enter> <BS> <C-r>. Edits are applied exactly as the
// textarea adapter applies them: one {from, to, insert} range each.
function run(src, keys, st = initialState()) {
  let cursor = src.indexOf('|');
  let text = src.replace('|', '');
  const actions = [];
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
    if (r.action) actions.push(r.action);
    cursor = r.cursor;
  }
  return { out: text.slice(0, cursor) + '|' + text.slice(cursor), st, actions };
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
  [':3 goes to a line', '|a\nb\nc', ':3<Enter>', 'a\nb\n|c'],
  ['Esc cancels a pending operator', '|abc', 'd<Esc>x', '|bc'],
];

for (const [name, before, keys, after] of CASES) {
  test(`vim: ${name}`, () => assert.equal(run(before, keys).out, after));
}

test('vim: u and Ctrl+R are the browser\'s undo and redo', () => {
  assert.deepEqual(run('|a', 'u<C-r>').actions, ['undo', 'redo']);
});

test('vim: :w :q :wq ask the editor, and unknown commands are reported', () => {
  assert.deepEqual(run('|a', ':w<Enter>:q<Enter>:wq<Enter>:foo<Enter>').actions, ['save', 'quit', 'save-quit', 'unknown']);
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
});
