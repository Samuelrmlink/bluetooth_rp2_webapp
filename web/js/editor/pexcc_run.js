// Run the WASI pexcc command. The wasm is a program: pexcc OUT.elf SRC.c ...
// Headers live in the preopens (/include, /modinc). The ELF is /tmp/out.elf.

import { startWasi, readGuest } from './wasi.js';

const OUT = '/tmp/out.elf';

export async function compileToElf(wasmBytes, preopens, inputs) {
  const dirs = preopens.map((p) => ({ guest: p.guest, files: p.files }));
  if (!dirs.some((d) => d.guest === '/tmp')) dirs.push({ guest: '/tmp', files: {} });
  const args = ['pexcc', OUT, ...inputs];
  let ran;
  try {
    ran = await startWasi(wasmBytes, { args, preopens: dirs });
  } catch (e) {
    const text = e.stdout ? new TextDecoder().decode(e.stdout).trim() : '';
    throw new Error(text || e.message || 'compile failed');
  }
  const text = new TextDecoder().decode(ran.stdout).trim();
  if (ran.exit !== 0) throw new Error(text || 'compile failed');
  const elf = readGuest(ran.preopens, OUT);
  if (!elf || elf.length < 52) throw new Error(text || 'compiler did not write an ELF');
  return elf;
}
