import { test } from 'node:test';
import assert from 'node:assert/strict';
import { loadProject, executeTarget } from '../../js/editor/pexproj.js';

const PEX = `name blink
version 0.1.0
kind app
model reloc
level debug
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
  define BLINK_RAM

target other
  board other_m33
  out build/other
  run flash
  src src/other.c

target wide
  board wide
  out build/wide
  run flash
  arch m0p
  src src/wide.c
`;

const BOARD = 'name pico2_w\narch m33\nheader pico2_w.h\ndefine PEX_HAS_WIFI=1\n';
const HEADER = '#define PEX_E2E_HEADER 1\n';

function world(extra = {}) {
  const files = {
    'sd/projects/blink/Pexfile': PEX,
    'sd/projects/blink/src/main.c': 'int flash_src;\n',
    'sd/projects/blink/src/main.h': '#define H 1\n',
    'sd/projects/blink/src/ram_main.c': 'int ram_src;\n',
    'sd/boards/pico2_w.board': BOARD,
    'sd/boards/pico2_w.h': HEADER,
    'sd/boards/other_m33.board': 'name other_m33\narch m33\n',
    'sd/boards/wide.board': 'name wide\narch m0p\n',
    ...extra,
  };
  const writes = [];
  const readText = async (path) => (path in files
    ? { ok: true, text: files[path], status: 200 }
    : { ok: false, status: 404, error: 'not found' });
  const list = async (dir) => {
    const pre = `${dir}/`;
    const names = new Map();
    for (const k of Object.keys(files)) {
      if (!k.startsWith(pre)) continue;
      const rest = k.slice(pre.length);
      const name = rest.split('/')[0];
      names.set(name, rest.includes('/'));
    }
    return {
      ok: true,
      files: [...names].map(([name, dir]) => ({ name, dir })),
    };
  };
  return {
    files,
    writes,
    readText,
    list,
    write: async (path, data) => { writes.push({ path, data }); return { ok: true }; },
  };
}

test('loadProject binds the board and does not select a second board while the name is unreported', async () => {
  const w = world();
  const spec = await loadProject({
    filePath: 'sd/projects/blink/src/main.c',
    readText: w.readText,
    arch: 'm33',
    board: null,
    pin: null,
  });
  assert.deepEqual(spec.errors, []);
  assert.equal(spec.selection.status, 'unreported');
  assert.equal(spec.selection.target.name, 'pico2_w');
  assert.equal(spec.selection.target.arch, 'm33');
  assert.equal(spec.selection.target.boardFile.header, 'pico2_w.h');
  const known = await loadProject({
    filePath: 'sd/projects/blink/src/main.c',
    readText: w.readText,
    arch: 'm33',
    board: 'pico2_w',
    pin: 'wide',
  });
  assert.equal(known.selection.target.name, 'pico2_w');
  assert.equal(known.selection.status, 'matched');
});

test('a build writes the SD artifact and the log, and compiles the unsaved editor copy', async () => {
  const w = world();
  let compiled = null;
  const spec = await loadProject({
    filePath: 'sd/projects/blink/src/main.c', readText: w.readText, arch: 'm33', board: null,
  });
  const result = await executeTarget({
    project: spec.project,
    target: spec.selection.target,
    active: { path: 'sd/projects/blink/src/main.c', text: 'int unsaved_flash;\n', dirty: true },
    readText: w.readText,
    write: w.write,
    list: w.list,
    policy: 'signed-only',
    compile: async (opts) => {
      compiled = opts;
      return new Uint8Array([1, 2, 3, 4]);
    },
    now: new Date('2026-10-23T12:00:00Z'),
  });
  assert.equal(result.ok, true, result.log);
  assert.equal(compiled.inputs[0], '/p/src/main.c');
  const unit = new TextDecoder().decode(compiled.tree.src['main.c']);
  assert.match(unit, /PEX_TARGET_m33/);
  assert.match(unit, /PEX_BOARD_PICO2_W/);
  assert.match(unit, /PEX_HAS_WIFI/);
  assert.match(unit, /unsaved_flash/);
  assert.doesNotMatch(unit, /flash_src/);
  assert.ok(compiled.include['board.h']);
  assert.equal(w.writes[0].path, 'sd/projects/blink/build/pico2_w/blink.m33.pex');
  assert.equal(w.writes[0].data.size, 4);
  const log = w.writes.find((x) => x.path.endsWith('build.log'));
  assert.ok(log);
  assert.match(log.data, /unsaved/);
  assert.match(log.data, /2026-10-23T12:00:00.000Z/);
  assert.match(log.data, /pexlink.py sign/);
  assert.match(result.log, /Saved sd\/projects\/blink\/build\/pico2_w\/blink\.m33\.pex \(4 bytes\)/);
});

test('the RAM target compiles its own source, and a file larger than the pool is not loaded', async () => {
  const w = world();
  const spec = await loadProject({
    filePath: 'sd/projects/blink/Pexfile', readText: w.readText, arch: 'm33', board: 'pico2_w', pin: 'ram',
  });
  assert.equal(spec.selection.target.name, 'ram');
  let text = '';
  const built = await executeTarget({
    project: spec.project,
    target: spec.selection.target,
    readText: w.readText,
    write: w.write,
    list: w.list,
    compile: async (opts) => {
      text = new TextDecoder().decode(opts.tree.src['ram_main.c']);
      return new Uint8Array(8);
    },
  });
  assert.equal(built.ok, true, built.log);
  assert.match(text, /#define BLINK_RAM 1/);
  assert.match(text, /ram_src/);
  assert.equal(w.writes[0].path, 'sd/projects/blink/build/ram/blink.m33.pex');

  w.writes.length = 0;
  let loaded = false;
  const big = await executeTarget({
    project: spec.project,
    target: spec.selection.target,
    andRun: true,
    readText: w.readText,
    write: w.write,
    list: w.list,
    poolLargest: 4,
    ws: { call: async () => { loaded = true; return {}; }, on: () => () => {} },
    compile: async () => new Uint8Array(16),
  });
  assert.equal(big.ok, false);
  assert.match(big.log, /largest block is 4/);
  assert.equal(loaded, false);
});

test('run flash asks before copying internally, and m0p is refused before compile', async () => {
  const w = world();
  const spec = await loadProject({
    filePath: 'sd/projects/blink/src/main.c', readText: w.readText, arch: 'm33', board: 'pico2_w',
  });
  let asked = '';
  const cancelled = await executeTarget({
    project: spec.project,
    target: spec.targets.find((t) => t.name === 'pico2_w'),
    andRun: true,
    readText: w.readText,
    write: w.write,
    list: w.list,
    confirm: (text) => { asked = text; return false; },
    compile: async () => new Uint8Array([9]),
  });
  assert.equal(cancelled.ok, true);
  assert.match(asked, /pex\/blink\.m33\.pex/);
  assert.ok(w.writes.some((x) => x.path.endsWith('blink.m33.pex')));
  assert.equal(w.writes.some((x) => x.path === 'pex/blink.m33.pex'), false);
  assert.match(cancelled.log, /Install cancelled/);

  let compiled = false;
  const refused = await executeTarget({
    project: spec.project,
    target: spec.targets.find((t) => t.name === 'wide'),
    readText: w.readText,
    write: w.write,
    list: w.list,
    compile: async () => { compiled = true; return new Uint8Array(); },
  });
  assert.equal(refused.ok, false);
  assert.equal(compiled, false);
  assert.match(refused.log, /Cortex-M33 only/);
});
