import { test } from 'node:test';
import assert from 'node:assert/strict';
import { loadProject, executeTarget, markVerified, restoreVerified, installFromCard, installCandidate } from '../../js/editor/pexproj.js';

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
  assert.match(asked, /Install sd:\/projects\/blink\/build\/pico2_w\/blink\.m33\.pex/);
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

function callsOf() {
  const calls = [];
  return {
    calls,
    ws: {
      call: async (op, params) => {
        calls.push({ op, params });
        if (op === 'pex.install' && params?.path) return { pending: true };
        if (op === 'pex.installed') return { modules: [{ name: 'blink', offset: 94208 }] };
        return { offset: 8192, xip_ready: true };
      },
      on: (topic, fn) => {
        if (topic === 'pex.sd_done') queueMicrotask(() => fn({ ok: true, name: 'blink' }));
        return () => {};
      },
    },
  };
}

test('install from the card waits for pex.sd_done and does not copy to LittleFS', async () => {
  const { ws, calls } = callsOf();
  const ev = await installFromCard({
    ws, sdPath: 'sd:/projects/blink/build/pico2_w/blink.m33.pex', name: 'blink',
  });
  assert.equal(ev.ok, true);
  assert.equal(ev.name, 'blink');
  assert.deepEqual(calls, [
    { op: 'pex.install', params: { path: 'sd:/projects/blink/build/pico2_w/blink.m33.pex', name: 'blink' } },
  ]);
});

test('a running module is not installed until unload succeeds', async () => {
  const w = world();
  const spec = await loadProject({
    filePath: 'sd/projects/blink/src/main.c', readText: w.readText, arch: 'm33', board: 'pico2_w',
  });
  const target = spec.targets.find((t) => t.name === 'pico2_w');
  const { ws, calls } = callsOf();
  const left = await executeTarget({
    project: spec.project, target, andRun: true, running: true, loaded: true,
    readText: w.readText, write: w.write, list: w.list, ws,
    confirm: () => false,
    compile: async () => new Uint8Array([1, 2, 3, 4]),
  });
  assert.equal(left.ok, true, left.log);
  assert.match(left.log, /Left blink loaded/);
  assert.equal(calls.length, 0);
  assert.equal(w.writes.some((x) => x.path === 'pex/blink.m33.pex'), false);

  let unloaded = false;
  const failed = await executeTarget({
    project: spec.project, target, andRun: true, running: true, loaded: true,
    readText: w.readText, write: w.write, list: w.list, ws,
    confirm: () => true,
    unload: async () => { unloaded = true; return { stopped: true, unloaded: false, error: 'busy' }; },
    compile: async () => new Uint8Array([1, 2, 3, 4]),
  });
  assert.equal(failed.ok, false);
  assert.equal(unloaded, true);
  assert.match(failed.log, /left in place/);
  assert.equal(calls.some((c) => c.op === 'pex.install'), false);
});

test('a card file larger than the pool is still installed from the card', async () => {
  const w = world();
  const spec = await loadProject({
    filePath: 'sd/projects/blink/src/main.c', readText: w.readText, arch: 'm33', board: 'pico2_w',
  });
  const target = spec.targets.find((t) => t.name === 'pico2_w');
  const { ws, calls } = callsOf();
  let asked = false;
  let unloaded = false;
  const result = await executeTarget({
    project: spec.project, target, andRun: true, running: true, loaded: true,
    readText: w.readText, write: w.write, list: w.list, ws,
    poolLargest: 64,
    confirm: () => { asked = true; return true; },
    unload: async () => { unloaded = true; return { stopped: true, unloaded: true }; },
    compile: async () => ({
      bytes: new Uint8Array(200),
      map: '# map\n00000001 T pex_main\n',
      dis: '00000000 <pex_main>:\n',
      symbols: [{ name: 'pex_main', value: 1, kind: 'T' }],
      imageOff: 128,
    }),
  });
  assert.equal(result.ok, true, result.log);
  assert.equal(asked, true);
  assert.equal(unloaded, true);
  assert.equal(w.writes.some((x) => x.path === 'pex/blink.m33.pex'), false);
  assert.equal(calls.some((c) => c.op === 'pex.install'), true);
});

test('installCandidate streams a card file without a pool-size gate', async () => {
  const calls = [];
  const hit = await installCandidate({
    ws: {
      call: async (op, params) => {
        calls.push({ op, params });
        if (op === 'pex.install') return { pending: true };
        if (op === 'pex.installed') {
          return { modules: [{ name: 'anker', offset: 4096, xip_ready: true }] };
        }
        return {};
      },
      on: (topic, fn) => {
        if (topic === 'pex.sd_done') queueMicrotask(() => fn({ ok: true, name: 'anker' }));
        return () => {};
      },
    },
    name: 'anker', arch: 'm33', bytes: new Uint8Array(100),
    sdPath: 'sd:/projects/anker/build/anker.m33.pex', cap: 40,
  });
  assert.equal(calls.some((c) => c.op === 'pex.install' && c.params.path.startsWith('sd:/')), true);
  assert.equal(hit.xip_ready, true);
});

test('a card file that fits is installed from the card, and anker gets its manifest', async () => {
  const w = world();
  const spec = await loadProject({
    filePath: 'sd/projects/blink/src/main.c', readText: w.readText, arch: 'm33', board: 'pico2_w',
  });
  const target = spec.targets.find((t) => t.name === 'pico2_w');
  const project = { ...spec.project, name: 'anker' };
  const { ws, calls } = callsOf();
  const result = await executeTarget({
    project, target, andRun: true,
    readText: w.readText, write: w.write, list: w.list, ws,
    poolLargest: 4096,
    confirm: () => true,
    compile: async () => new Uint8Array([1, 2, 3, 4]),
  });
  assert.equal(result.ok, true, result.log);
  assert.equal(w.writes.some((x) => x.path === 'pex/anker.m33.pex'), false);
  const manifest = w.writes.find((x) => x.path === 'pex/anker.driver.json');
  assert.ok(manifest);
  assert.equal(JSON.parse(manifest.data).id, 'anker');
  assert.equal(JSON.parse(manifest.data).kind, 'pex');
  const install = calls.find((c) => c.op === 'pex.install');
  assert.equal(install.params.path, 'sd:/projects/blink/build/pico2_w/anker.m33.pex');
  assert.equal(install.params.name, 'anker');
  assert.match(result.log, /Wrote pex\/anker.driver.json/);
});

test('mark verified keeps the previous file until the new copy is on the card', async () => {
  const writes = [];
  const renames = [];
  const removes = [];
  const bytes = new Uint8Array(92);
  bytes.set([0xab, 0xcd], 76);
  const res = await markVerified({
    name: 'anker', arch: 'm33', bytes, buildId: 'abcd', when: '2026-10-23T00:00:00.000Z',
    write: async (path, data) => { writes.push(path); return { ok: true, data }; },
    rename: async (from, to) => { renames.push([from, to]); return { ok: true }; },
    remove: async (path) => { removes.push(path); return { ok: true }; },
  });
  assert.equal(res.ok, true);
  assert.equal(writes[0], 'sd/pex/good/anker.m33.pex.incoming');
  assert.deepEqual(renames[0], ['sd/pex/good/anker.m33.pex', 'sd/pex/good/anker.m33.pex.prev']);
  assert.deepEqual(renames[1], ['sd/pex/good/anker.m33.pex.incoming', 'sd/pex/good/anker.m33.pex']);
  assert.equal(writes[1], 'sd/pex/good/anker.m33.txt');
  assert.ok(removes.includes('sd/pex/good/anker.m33.pex.prev'));

  const calls = [];
  const restored = await restoreVerified({
    name: 'blink', arch: 'm33', loaded: true, running: true, poolLargest: 48 * 1024,
    confirm: () => true,
    unload: async () => ({ unloaded: false, error: 'still referenced' }),
    read: async () => ({ ok: true, bytes: new Uint8Array([1, 2, 3, 4]) }),
    write: async () => { calls.push('write'); return { ok: true }; },
    ws: { call: async () => { calls.push('call'); return {}; }, on: () => () => {} },
  });
  assert.equal(restored.ok, false);
  assert.match(restored.error, /still referenced/);
  assert.deepEqual(calls, []);

  let confirmed = false;
  let unloaded = false;
  const streamed = await restoreVerified({
    name: 'anker', arch: 'm33', loaded: true, running: true, poolLargest: 64,
    confirm: () => { confirmed = true; return true; },
    unload: async () => { unloaded = true; return { unloaded: true }; },
    read: async () => ({ ok: true, bytes: new Uint8Array(200) }),
    write: async () => ({ ok: true }),
    ws: { call: async () => ({ xip_ready: true }), on: () => () => {} },
  });
  assert.equal(streamed.ok, true, streamed.error);
  assert.equal(confirmed, true);
  assert.equal(unloaded, true);
});
