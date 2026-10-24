// Browser PEX build. Loaded only when Build is clicked (dynamic import from
// the editor), so the wasm compiler is not in the first-load bundle. The
// toolchain lives on the SD card under pex-cc/ and is fetched once.
//
// pexcc.wasm is a WASI program. It compiles C to an ARM ELF. linkElf turns
// that ELF into an unsigned module. The page does not have the OTA signing
// key. Sign on the host: python3 pex/tools/pexlink.py sign

import { linkElf } from './pexlink.js';
import { compileToElf } from './pexcc_run.js';
import { driverOf } from './pexmod.js';

const SD = [
  'pex-cc/pexcc.wasm',
  'pex-cc/host_api.json',
  'pex-cc/include/pex.h',
  'pex-cc/modinc/stddef.h',
  'pex-cc/modinc/stdint.h',
  'pex-cc/modinc/string.h',
];

let toolchain = null;

async function fetchBytes(path) {
  const url = '/api/sd/files/' + path.split('/').map(encodeURIComponent).join('/');
  let res;
  try {
    res = await fetch(url, { cache: 'no-store' });
  } catch {
    throw new Error('The compiler lives on the SD card and this firmware has no SD.');
  }
  if (res.status === 404) {
    throw new Error(`compiler file missing on the SD card: ${path}`);
  }
  if (!res.ok) {
    throw new Error(`SD ${path}: HTTP ${res.status}`);
  }
  return new Uint8Array(await res.arrayBuffer());
}

async function loadToolchain(onStatus) {
  if (toolchain) return toolchain;
  onStatus?.('Fetching the compiler from the SD card…');
  const blobs = {};
  for (const path of SD) blobs[path] = await fetchBytes(path);
  const api = JSON.parse(new TextDecoder().decode(blobs['pex-cc/host_api.json']));
  toolchain = {
    wasm: blobs['pex-cc/pexcc.wasm'],
    api,
    files: {},
    includePex: blobs['pex-cc/include/pex.h'],
    modinc: {
      'stddef.h': blobs['pex-cc/modinc/stddef.h'],
      'stdint.h': blobs['pex-cc/modinc/stdint.h'],
      'string.h': blobs['pex-cc/modinc/string.h'],
    },
  };
  return toolchain;
}

async function driverBytes(tc, file) {
  const path = `pex-cc/drivers/${file}`;
  if (!tc.files[path]) tc.files[path] = await fetchBytes(path);
  return tc.files[path];
}

/**
 * Compile one C buffer to an unsigned name.m33.pex. Does not write the
 * source and does not load or start the module.
 * openFile is the buffer's basename. A driver translation unit pulls the
 * rest of that module from the SD card; the open buffer is the copy that
 * gets compiled for its own file. siblingRoot, when set, is the card
 * directory those siblings come from (pex for sd/pex/). Otherwise they
 * come from the compiler pack at pex-cc/drivers/.
 * Returns a Uint8Array.
 */
export async function buildPex(source, name, onStatus, openFile, siblingRoot) {
  const base = openFile || `${name}.c`;
  const mod = driverOf(base);
  const outName = mod ? mod.name : name;
  const tc = await loadToolchain(onStatus);
  const srcFiles = {};
  if (mod) {
    onStatus?.('Fetching module sources…');
    for (const file of [...mod.headers, ...mod.sources]) {
      if (file === base) continue;
      srcFiles[file] = siblingRoot
        ? await fetchBytes(`${siblingRoot}/${file}`)
        : await driverBytes(tc, file);
    }
  }
  srcFiles[base] = new TextEncoder().encode(source);
  onStatus?.('Compiling…');
  const names = mod ? mod.sources : [base];
  const elf = await compileToElf(tc.wasm, [
    { guest: '/include', files: { 'pex.h': tc.includePex } },
    { guest: '/modinc', files: tc.modinc },
    { guest: '/src', files: srcFiles },
  ], names.map((file) => `/src/${file}`));
  onStatus?.('Linking the unsigned module…');
  const opts = {
    target: 'm33',
    model: 'reloc',
    api: tc.api,
    name: outName,
    version: mod ? mod.version : '0.1.0',
    kind: 'app',
    apiName: 'pex-cc/host_api.json',
  };
  if (mod?.lua) opts.lua = true;
  if (mod?.desc) opts.desc = mod.desc;
  const linked = await linkElf(elf, opts);
  return linked.bytes;
}

/**
 * Compile one project's translation units. `tree` is the guest /p directory
 * (sources already prefixed, headers beside them). `include` is extra
 * headers for /include (board.h and each inc file). pex.h stays the
 * toolchain header. Returns `{ bytes, map, dis }` for the unsigned module.
 */
export async function buildModule({
  name, version = '0.1.0', kind = 'app', model = 'reloc', arch = 'm33',
  inputs, tree, include = {}, onStatus,
}) {
  if (arch !== 'm33') throw new Error(`pexcc emits Cortex-M33 only; arch ${arch} was not compiled`);
  const tc = await loadToolchain(onStatus);
  onStatus?.('Compiling…');
  const elf = await compileToElf(tc.wasm, [
    { guest: '/include', files: { ...include, 'pex.h': tc.includePex } },
    { guest: '/modinc', files: tc.modinc },
    { guest: '/p', files: tree },
  ], inputs);
  onStatus?.('Linking the unsigned module…');
  const linked = await linkElf(elf, {
    target: arch,
    model,
    api: tc.api,
    name,
    version,
    kind,
    apiName: 'pex-cc/host_api.json',
  });
  return { bytes: linked.bytes, map: linked.map, dis: linked.dis, symbols: linked.symbols, imageOff: linked.imageOff };
}
