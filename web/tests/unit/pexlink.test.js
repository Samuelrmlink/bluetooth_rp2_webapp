// JS pexlink against host pexlink.py. The ELF is the one arm-none-eabi-gcc
// produces for pex/test/modules/hello.c with the reloc flags in pex.cmake.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import { mkdtempSync, readFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { linkElf } from '../../js/editor/pexlink.js';

const WEB = join(dirname(fileURLToPath(import.meta.url)), '../..');
const ROOT = join(WEB, '..');
const GCC = join(process.env.HOME, '.pico-sdk/toolchain/15_2_Rel1/bin/arm-none-eabi-gcc');
const CPU = ['-mthumb', '-mcpu=cortex-m33', '-march=armv8-m.main+fp+dsp', '-mfloat-abi=softfp'];
const API = join(ROOT, 'pex/host_api.json');

function pyLink(elf, out, args) {
  execFileSync('python3', [
    join(ROOT, 'pex/tools/pexlink.py'), 'link', elf, '-o', out,
    '--target', 'm33', '--model', 'reloc', '--api', API, ...args,
  ], { stdio: 'pipe' });
  return readFileSync(out);
}

test('JS pexlink matches pexlink.py on hello.elf', async () => {
  const dir = mkdtempSync(join(tmpdir(), 'pexlink-'));
  try {
    const obj = join(dir, 'hello.o');
    const elf = join(dir, 'hello.elf');
    const py = join(dir, 'hello.py.pex');
    execFileSync(GCC, [
      ...CPU, '-Os', '-g', '-std=gnu11', '-ffreestanding', '-ffunction-sections', '-fdata-sections',
      '-fno-common', '-fno-exceptions', '-fno-unwind-tables', '-fno-asynchronous-unwind-tables',
      '-fstack-usage', '-Wall', '-Wextra', '-Wno-unused-parameter',
      '-I', join(ROOT, 'pex/sdk/include'), '-I', join(ROOT, 'build/pex_gen/include'),
      '-DPEX_TARGET_m33=1', '-c', join(ROOT, 'pex/test/modules/hello.c'), '-o', obj,
    ]);
    execFileSync(GCC, [
      ...CPU, '-nostdlib', '-nostartfiles',
      '-T', join(ROOT, 'pex/sdk/ld/pex_m33_reloc.ld'),
      '-L', join(ROOT, 'pex/sdk/ld'),
      '-Wl,--emit-relocs', '-Wl,--gc-sections', '-Wl,-e,pex_main',
      obj, join(ROOT, 'build/pex_gen/pex_imports_m33_reloc.o'), '-lgcc', '-o', elf,
    ]);
    const pyBytes = pyLink(elf, py, ['--name', 'hello', '--version', '1.0.0', '--kind', 'lua']);
    const api = JSON.parse(readFileSync(API, 'utf8'));
    const { bytes, info, map, dis, symbols, imageOff } = await linkElf(readFileSync(elf), {
      target: 'm33', model: 'reloc', api, name: 'hello', version: '1.0.0', kind: 'lua',
    });
    const nm = execFileSync(join(process.env.HOME, '.pico-sdk/toolchain/15_2_Rel1/bin/arm-none-eabi-nm'), ['-n', elf], { encoding: 'utf8' });
    const row = nm.split('\n').find((l) => l.endsWith(' pex_main'));
    // This nm prints a Thumb function with bit 0 clear. The ELF st_value,
    // which the map records, keeps it set.
    const nmAddr = parseInt(row.split(/\s+/)[0], 16);
    const sym = symbols.find((s) => s.name === 'pex_main');
    assert.equal(sym.value & ~1, nmAddr & ~1);
    assert.equal(sym.value & 1, 1);
    assert.match(map, /0x10400000 \+ partition_offset \+ image_off/);
    assert.match(dis, /<pex_main>:/);
    // Paper check, not a probe. code_addr is exec_base + sector*4096 + image_off,
    // and a Thumb symbol's bit 0 is not part of the address.
    const offset = 94208;
    const swd = (0x10400000 + offset + imageOff + (sym.value & ~1)) >>> 0;
    assert.equal(swd, (0x10400000 + 94208 + imageOff + (nmAddr & ~1)) >>> 0);
    assert.equal(info.imports, 9);
    assert.equal(info.exports, 1);
    assert.equal(info.text, 332);
    assert.equal(info.ro, 84);
    assert.equal(info.data, 52);
    assert.equal(info.bss, 0);
    assert.deepEqual(Buffer.from(bytes), pyBytes);
  } finally {
    rmSync(dir, { recursive: true, force: true });
  }
});
