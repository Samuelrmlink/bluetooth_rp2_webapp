import { test } from 'node:test';
import assert from 'node:assert/strict';
import {
  parsePexfile, parseBoard, pexfileCandidates, bindTarget, selectTarget,
  projectBuildItems, buildButtonState, macroLines, prefixSource, boardMacro,
  artifactRel, flashConfirm, ramDecision, layoutGuest,
} from '../../js/editor/pexfile.js';

const ANKER = `
name anker
version 1.0.0
kind app
model reloc
level info
inc src
src src/anker_app.c
src src/anker_models.c
src src/anker_wire.c
src src/anker_adv.c

target pico2_w
  board pico2_w
  out build
  run flash
`;

const BLINK = `
name blink
version 0.1.0
kind app
model reloc
inc src
src src/main.c
default pico2_w

target pico2_w
  board pico2_w
  out build/pico2_w
  run flash

target ram
  board pico2_w
  out build/ram
  run ram
  src src/ram_main.c
  define BLINK_RAM=1

target other
  board other_m33
  out build/other
  run flash
  src src/other.c
`;

const PICO = `
name pico2_w
arch m33
header pico2_w.h
define PEX_HAS_WIFI=1
define PEX_HAS_SD=1
`;

function blink() {
  const parsed = parsePexfile(BLINK);
  assert.equal(parsed.ok, true, parsed.errors.map((e) => e.message).join('; '));
  const board = parseBoard(PICO);
  assert.equal(board.ok, true);
  const other = parseBoard('name other_m33\narch m33\n');
  const targets = parsed.project.targets.map((t) => {
    const file = t.board === 'pico2_w' ? board.board : other.board;
    return bindTarget(t, file, 'm33');
  });
  return { ...parsed.project, targets };
}

test('anker Pexfile: one module, one target, shared sources', () => {
  const p = parsePexfile(ANKER);
  assert.equal(p.ok, true, p.errors.map((e) => e.message).join('; '));
  assert.equal(p.project.name, 'anker');
  assert.equal(p.project.version, '1.0.0');
  assert.equal(p.project.defaultTarget, 'pico2_w');
  assert.equal(p.project.targets.length, 1);
  const t = p.project.targets[0];
  assert.equal(t.board, 'pico2_w');
  assert.equal(t.out, 'build');
  assert.equal(t.run, 'flash');
  assert.deepEqual(t.src, ['src/anker_app.c', 'src/anker_models.c', 'src/anker_wire.c', 'src/anker_adv.c']);
  assert.deepEqual(t.inc, ['src']);
});

const BRACKETS = `
name anker
version 1.0.0
kind app
model reloc
level info
inc [ src ]
src [
  src/anker_app.c
  src/anker_models.c,
  # wire is next
  src/anker_wire.c
  src/anker_adv.c
]

target pico2_w {
  board pico2_w
  out build
  run flash
}

target ram {
  src [ src/ram_main.c, src/ram_board.c ]
  define [ BLINK_RAM=1, LED_PIN=25 ]
  out build/ram
  run ram
}
`;

test('bracket lists are the whole list, and braces end a target', () => {
  const p = parsePexfile(BRACKETS);
  assert.equal(p.ok, true, p.errors.map((e) => e.message).join('; '));
  assert.equal(p.project.defaultTarget, 'pico2_w');
  const flash = p.project.targets.find((t) => t.name === 'pico2_w');
  const ram = p.project.targets.find((t) => t.name === 'ram');
  assert.deepEqual(flash.src, ['src/anker_app.c', 'src/anker_models.c', 'src/anker_wire.c', 'src/anker_adv.c']);
  assert.deepEqual(flash.inc, ['src']);
  assert.equal(flash.board, 'pico2_w');
  assert.equal(flash.out, 'build');
  assert.deepEqual(ram.src, ['src/ram_main.c', 'src/ram_board.c']);
  assert.deepEqual(ram.define, [{ name: 'BLINK_RAM', value: '1' }, { name: 'LED_PIN', value: '25' }]);
  assert.equal(ram.out, 'build/ram');
  assert.equal(ram.run, 'ram');
  assert.equal(Object.hasOwn(flash, 'listKind'), false);
});

test('a bracket list cannot mix with repeated keys, and an open block is an error', () => {
  const mixed = parsePexfile('name blink\nsrc a.c\nsrc [\n  b.c\n]\n');
  assert.equal(mixed.ok, false);
  assert.ok(mixed.errors.some((e) => /src is already set/.test(e.message)));
  const flipped = parsePexfile('name blink\nsrc [\n  a.c\n]\nsrc b.c\n');
  assert.ok(flipped.errors.some((e) => /src is already set/.test(e.message)));
  const open = parsePexfile('name blink\nsrc [\n  a.c\n');
  assert.ok(open.errors.some((e) => /src list is missing \]/.test(e.message)));
  const brace = parsePexfile('name blink\nsrc a.c\ntarget t {\n  out build\n');
  assert.ok(brace.errors.some((e) => /target t is missing \}/.test(e.message)));
  const stray = parsePexfile('name blink\nsrc a.c\n}\n');
  assert.ok(stray.errors.some((e) => /closing brace is outside a target/.test(e.message)));
});

test('a RAM target replaces src and define instead of appending', () => {
  const p = blink();
  const flash = p.targets.find((t) => t.name === 'pico2_w');
  const ram = p.targets.find((t) => t.name === 'ram');
  assert.deepEqual(flash.src, ['src/main.c']);
  assert.deepEqual(flash.define, []);
  assert.deepEqual(ram.src, ['src/ram_main.c']);
  assert.deepEqual(ram.define, [{ name: 'BLINK_RAM', value: '1' }]);
  assert.equal(ram.run, 'ram');
  assert.equal(flash.out, 'build/pico2_w');
});

test('no target line is one implicit default target', () => {
  const p = parsePexfile('name blink\nsrc main.c\n');
  assert.equal(p.ok, true, p.errors.map((e) => e.message).join('; '));
  assert.equal(p.project.targets.length, 1);
  assert.equal(p.project.targets[0].name, 'default');
  assert.equal(p.project.targets[0].out, 'build');
  assert.equal(p.project.targets[0].run, 'flash');
  const bound = bindTarget(p.project.targets[0], null, 'm33');
  assert.equal(bound.arch, 'm33');
  assert.deepEqual(bound.errors, []);
});

test('unknown keys, .., a repeated out, and a disagreeing arch are errors', () => {
  const bad = parsePexfile('name blink\nfoo 1\nsrc ../x.c\ntarget a\n  out build\n  src a.c\ntarget b\n  out build\n  src b.c\n');
  assert.equal(bad.ok, false);
  assert.ok(bad.errors.some((e) => /unknown key foo/.test(e.message)));
  assert.ok(bad.errors.some((e) => e.message.includes('..')));
  assert.ok(bad.errors.some((e) => /both write build/.test(e.message)));
  const board = parseBoard('name pico2_w\narch m33\n');
  const t = bindTarget({ name: 't', arch: 'm0p', board: 'pico2_w', src: ['a.c'], out: 'build', run: 'flash', define: [], inc: [] }, board.board, 'm33');
  assert.ok(t.errors.some((e) => /does not match/.test(e)));
  assert.equal(parseBoard('name pico2_w\narch m33\nclock 125\n').ok, false);
});

test('the walk stops at the card root and at a Pexfile', () => {
  assert.deepEqual(pexfileCandidates('sd/projects/anker/src/anker_app.c'), [
    'sd/projects/anker/src/Pexfile',
    'sd/projects/anker/Pexfile',
    'sd/projects/Pexfile',
    'sd/Pexfile',
  ]);
  assert.deepEqual(pexfileCandidates('sd/projects/anker/Pexfile'), [
    'sd/projects/anker/Pexfile',
    'sd/projects/Pexfile',
    'sd/Pexfile',
  ]);
  assert.deepEqual(pexfileCandidates('lua/anker.lua'), ['lua/Pexfile', 'Pexfile']);
});

test('without a board name, default picks pico2_w and a second m33 board is not selected', () => {
  const p = blink();
  const sel = selectTarget(p, { arch: 'm33' });
  assert.equal(sel.status, 'unreported');
  assert.equal(sel.target.name, 'pico2_w');
  assert.equal(sel.target.run, 'flash');
  const button = buildButtonState(p, sel, { boardKnown: false });
  assert.equal(button.label, 'Build blink · pico2_w');
  assert.match(button.title, /not reported/);
  assert.equal(button.disabled, false);
});

test('two targets for the same board resolve by default, and a pin keeps ram', () => {
  const p = blink();
  p.targets = p.targets.filter((t) => t.board === 'pico2_w');
  const sel = selectTarget(p, { arch: 'm33' });
  assert.equal(sel.status, 'matched');
  assert.equal(sel.target.name, 'pico2_w');
  const pin = selectTarget(p, { arch: 'm33', pin: 'ram' });
  assert.equal(pin.status, 'pinned');
  assert.equal(pin.target.name, 'ram');
  assert.equal(pin.target.src[0], 'src/ram_main.c');
  assert.match(buildButtonState(p, pin).title, /pinned/);
});

test('a reported board drops the other board, including a pin of that board', () => {
  const p = blink();
  const sel = selectTarget(p, { arch: 'm33', board: 'pico2_w', pin: 'other' });
  assert.equal(sel.status, 'matched');
  assert.equal(sel.target.name, 'pico2_w');
  const ram = selectTarget(p, { arch: 'm33', board: 'pico2_w', pin: 'ram' });
  assert.equal(ram.status, 'pinned');
  assert.equal(ram.target.name, 'ram');
  const none = selectTarget(p, { arch: 'm33', board: 'no_such' });
  assert.equal(none.status, 'none');
  assert.match(none.reason, /no_such/);
  assert.equal(buildButtonState(p, none).disabled, true);
  assert.match(buildButtonState(p, sel, { boardKnown: true }).title, /this board/);
});

test('the menu lists arch matches, then other archs, then Build all', () => {
  const p = blink();
  p.targets.push(bindTarget({
    name: 'old', arch: 'm0p', board: null, out: 'build/old', run: 'flash',
    src: ['src/old.c'], inc: [], define: [], level: 'info', log: null,
  }, null, null));
  const items = projectBuildItems(p, { arch: 'm33', board: 'pico2_w' });
  assert.deepEqual(items.map((i) => i.label), [
    'Build pico2_w', 'Build and run pico2_w',
    'Build ram', 'Build and run ram',
    'Build other', 'Build and run other',
    'Build old', 'Build and run old',
    'Build all',
  ]);
});

test('macros: arch, board, board defines, then target defines, then the header', () => {
  const lines = macroLines({
    arch: 'm33',
    boardName: 'pico2_w',
    boardDefines: [{ name: 'LED_PIN', value: '25' }, { name: 'PEX_HAS_WIFI', value: '1' }],
    targetDefines: [{ name: 'LED_PIN', value: '0' }, { name: 'BLINK_RAM', value: '1' }],
    header: true,
  });
  assert.deepEqual(lines.map((l) => l.line), [
    '#define PEX_TARGET_m33 1',
    '#define PEX_BOARD_PICO2_W 1',
    '#define LED_PIN 25',
    '#define PEX_HAS_WIFI 1',
    '#define LED_PIN 0',
    '#define BLINK_RAM 1',
    '#include "board.h"',
  ]);
  assert.equal(boardMacro('pico2_w'), 'PEX_BOARD_PICO2_W');
  const src = prefixSource('int x;\n', lines);
  assert.ok(src.startsWith('#define PEX_TARGET_m33 1\n'));
  assert.ok(src.endsWith('int x;\n'));
  assert.equal(artifactRel({ name: 'blink' }, { out: 'build/ram', arch: 'm33' }), 'build/ram/blink.m33.pex');
});

test('flash confirm names the internal file, and anker says it replaces the signed module', () => {
  assert.match(flashConfirm({ name: 'blink', arch: 'm33' }), /pex\/blink\.m33\.pex/);
  assert.doesNotMatch(flashConfirm({ name: 'blink', arch: 'm33' }), /signed/);
  assert.match(flashConfirm({ name: 'anker', arch: 'm33' }), /replaces the running signed module/);
  assert.equal(ramDecision({ bytes: 100, poolLargest: 48 * 1024 }).ok, true);
  assert.match(ramDecision({ bytes: 164596, poolLargest: 48 * 1024 }).message, /164596/);
});

test('guest files keep the project path, and the board header is board.h', () => {
  const g = layoutGuest({
    units: [{ path: 'src/main.c', text: 'int main;\n' }],
    headers: [{ path: 'src/main.h', text: '#define H 1\n', includeAs: 'main.h' }],
    boardHeaders: [{ name: 'board.h', text: '#define PEX_E2E_HEADER 1\n' }, { name: 'pico2_w.h', text: '#define PEX_E2E_HEADER 1\n' }],
  });
  assert.deepEqual(g.inputs, ['/p/src/main.c']);
  assert.ok(g.tree.src['main.c'] instanceof Uint8Array);
  assert.ok(g.tree.src['main.h'] instanceof Uint8Array);
  assert.ok(g.include['board.h'] instanceof Uint8Array);
  assert.ok(g.include['main.h'] instanceof Uint8Array);
  assert.ok(g.include['pico2_w.h'] instanceof Uint8Array);
});
