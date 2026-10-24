// The editor's WASI shim drives the same pexcc.wasm as Node's WASI.
// A browser-sample ELF must match byte for byte, and linkElf must match
// pexlink.py. Pinecil and Anker are separate translation units.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import { mkdtempSync, readFileSync, writeFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { WASI } from 'node:wasi';
import { compileToElf } from '../../js/editor/pexcc_run.js';
import { linkElf } from '../../js/editor/pexlink.js';
import { driverOf, pexCName, pexOutName } from '../../js/editor/pexmod.js';

const WEB = join(dirname(fileURLToPath(import.meta.url)), '../..');
const ROOT = join(WEB, '..');
const PEXCC = join(ROOT, '../pexcc');
const WASM = join(PEXCC, 'build/pexcc.wasm');
const API = join(ROOT, 'pex/host_api.json');
const DRV = join(ROOT, 'pex/modules/drivers');

const BROWSER = `#include "pex.h"
int pex_main(const pex_host_t *host, uint32_t reason, void *arg) {
    (void)arg;
    if (reason == PEX_INIT) pex_log(PEX_LOG_INFO, "built in the browser");
    if (reason == PEX_APP_RUN) return PEX_OK;
    return PEX_OK;
}
`;

function headers() {
  return [
    { guest: '/include', files: { 'pex.h': readFileSync(join(PEXCC, 'include/pex.h')) } },
    { guest: '/modinc', files: {
      'stddef.h': readFileSync(join(PEXCC, 'modinc/stddef.h')),
      'stdint.h': readFileSync(join(PEXCC, 'modinc/stdint.h')),
      'string.h': readFileSync(join(PEXCC, 'modinc/string.h')),
    } },
  ];
}

function pyLink(elf, out, args) {
  execFileSync('python3', [
    join(ROOT, 'pex/tools/pexlink.py'), 'link', elf, '-o', out,
    '--target', 'm33', '--model', 'reloc', '--api', API, ...args,
  ], { stdio: 'pipe' });
  return readFileSync(out);
}

async function nodeElf(dir, guestInputs) {
  const wasm = readFileSync(WASM);
  const outGuest = '/tmp/out.elf';
  const wasi = new WASI({
    version: 'preview1',
    args: ['pexcc', outGuest, ...guestInputs],
    env: {},
    preopens: {
      '/tmp': dir,
      '/include': join(PEXCC, 'include'),
      '/modinc': join(PEXCC, 'modinc'),
      '/drv': DRV,
    },
    returnOnExit: true,
  });
  const mod = await WebAssembly.compile(wasm);
  const inst = await WebAssembly.instantiate(mod, wasi.getImportObject());
  const code = wasi.start(inst);
  assert.equal(code, 0);
  return readFileSync(join(dir, 'out.elf'));
}

test('driver file names map onto anker and pinecil', () => {
  assert.equal(pexOutName('browser.c'), 'browser');
  assert.equal(pexOutName('pex/browser.c'), 'browser');
  const anker = driverOf('anker_wire.c');
  assert.equal(anker.name, 'anker');
  assert.equal(pexOutName('pex/anker_app.c'), 'anker');
  assert.equal(pexCName('sd/pex/anker_app.c'), 'anker_app');
  assert.equal(pexCName('pex/anker_app.c'), 'anker_app');
  assert.equal(pexCName('sd/pex-cc/drivers/anker_app.c'), null);
  assert.equal(pexCName('sd/logs/note.c'), null);
  assert.deepEqual(anker.sources, ['anker_app.c', 'anker_adv.c', 'anker_wire.c', 'anker_models.c']);
  assert.equal(anker.version, '1.0.0');
  assert.equal(anker.lua, true);
  const pine = driverOf('pinecil_live.c');
  assert.equal(pine.name, 'pinecil');
  assert.equal(pine.lua, false);
  assert.equal(driverOf('browser.c'), null);
});

test('WASI shim matches node:wasi and linkElf on the browser sample', async () => {
  const wasm = readFileSync(WASM);
  const dir = mkdtempSync(join(tmpdir(), 'pexcc-wasi-'));
  try {
    const src = new TextEncoder().encode(BROWSER);
    writeFileSync(join(dir, 'browser.c'), src);
    const viaNode = await nodeElf(dir, ['/tmp/browser.c']);
    const viaShim = await compileToElf(wasm, [
      ...headers(),
      { guest: '/tmp', files: { 'browser.c': src } },
    ], ['/tmp/browser.c']);
    assert.deepEqual(Buffer.from(viaShim), viaNode);

    const elfPath = join(dir, 'browser.elf');
    writeFileSync(elfPath, viaShim);
    const py = pyLink(elfPath, join(dir, 'browser.pex'), ['--name', 'browser', '--version', '0.1.0', '--kind', 'app']);
    const api = JSON.parse(readFileSync(API, 'utf8'));
    const { bytes, info } = await linkElf(viaShim, {
      target: 'm33', model: 'reloc', api, name: 'browser', version: '0.1.0', kind: 'app',
      apiName: 'pex-cc/host_api.json',
    });
    assert.deepEqual(Buffer.from(bytes), py);
    assert.equal(Buffer.from(bytes.subarray(0, 4)).toString('latin1'), 'PEX\x1a');
    assert.equal(Buffer.from(bytes).includes(Buffer.from('built in the browser')), true);
    assert.equal(info.imports, 1);

    const bad = new TextEncoder().encode('int pex_main(void) { return 1 }\n');
    await assert.rejects(
      () => compileToElf(wasm, [...headers(), { guest: '/src', files: { 'bad.c': bad } }], ['/src/bad.c']),
      /expected ';'/,
    );
  } finally {
    rmSync(dir, { recursive: true, force: true });
  }
});

test('WASI shim matches node:wasi on Pinecil and Anker', { timeout: 300000 }, async () => {
  const wasm = readFileSync(WASM);
  const dir = mkdtempSync(join(tmpdir(), 'pexcc-drv-'));
  const read = (name) => readFileSync(join(DRV, name));
  const drvFiles = {};
  for (const name of [
    'anker_app.c', 'anker_adv.c', 'anker_adv.h', 'anker_wire.c', 'anker_wire.h',
    'anker_models.c', 'anker_models.h', 'pinecil_app.c', 'pinecil_live.c', 'pinecil_live.h',
  ]) drvFiles[name] = read(name);
  try {
    const pineInputs = ['/drv/pinecil_live.c', '/drv/pinecil_app.c'];
    const pineNode = await nodeElf(dir, pineInputs);
    const pineShim = await compileToElf(wasm, [
      ...headers(),
      { guest: '/drv', files: drvFiles },
    ], pineInputs);
    assert.deepEqual(Buffer.from(pineShim), pineNode);

    const ankerInputs = ['/drv/anker_app.c', '/drv/anker_adv.c', '/drv/anker_wire.c', '/drv/anker_models.c'];
    const ankerNode = await nodeElf(dir, ankerInputs);
    const ankerShim = await compileToElf(wasm, [
      ...headers(),
      { guest: '/drv', files: drvFiles },
    ], ankerInputs);
    assert.deepEqual(Buffer.from(ankerShim), ankerNode);

    writeFileSync(join(dir, 'pinecil.elf'), pineShim);
    writeFileSync(join(dir, 'anker.elf'), ankerShim);
    const pinePex = pyLink(join(dir, 'pinecil.elf'), join(dir, 'pinecil.pex'), [
      '--name', 'pinecil', '--version', '1.0.0', '--kind', 'app', '--desc', 'Pinecil V2 driver',
    ]);
    const ankerPex = pyLink(join(dir, 'anker.elf'), join(dir, 'anker.pex'), [
      '--name', 'anker', '--version', '1.0.0', '--kind', 'app', '--lua', '--desc', 'Anker SOLIX / Prime driver',
    ]);
    assert.equal(pinePex.subarray(0, 4).toString('latin1'), 'PEX\x1a');
    assert.equal(ankerPex.subarray(0, 4).toString('latin1'), 'PEX\x1a');
    const api = JSON.parse(readFileSync(API, 'utf8'));
    const pineJs = await linkElf(pineShim, {
      target: 'm33', model: 'reloc', api, name: 'pinecil', version: '1.0.0', kind: 'app',
      desc: 'Pinecil V2 driver',
    });
    const ankerJs = await linkElf(ankerShim, {
      target: 'm33', model: 'reloc', api, name: 'anker', version: '1.0.0', kind: 'app', lua: true,
      desc: 'Anker SOLIX / Prime driver',
    });
    assert.deepEqual(Buffer.from(pineJs.bytes), pinePex);
    assert.deepEqual(Buffer.from(ankerJs.bytes), ankerPex);
    assert.equal(ankerJs.info.imports > 0, true);
    const dump = execFileSync('python3', [
      join(ROOT, 'pex/tools/pexlink.py'), 'dump', join(dir, 'anker.pex'), '--api', API,
    ], { encoding: 'utf8' });
    assert.equal(dump.includes('__aeabi_'), false);
    assert.match(dump, /pex_log/);
  } finally {
    rmSync(dir, { recursive: true, force: true });
  }
});
