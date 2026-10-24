// Which open .c file is one translation unit of a multi-file module.
// Anker is four files and Pinecil is two. Opening any of them builds the
// whole module. Other .c files build as a single translation unit.

export const DRIVERS = {
  anker: {
    sources: ['anker_app.c', 'anker_adv.c', 'anker_wire.c', 'anker_models.c'],
    headers: ['anker_adv.h', 'anker_wire.h', 'anker_models.h'],
    version: '1.0.0',
    lua: true,
    desc: 'Anker SOLIX / Prime driver',
  },
  pinecil: {
    sources: ['pinecil_app.c', 'pinecil_live.c'],
    headers: ['pinecil_live.h'],
    version: '1.0.0',
    lua: false,
    desc: 'Pinecil V2 driver',
  },
};

const BY_FILE = {};
for (const [name, mod] of Object.entries(DRIVERS)) {
  for (const file of mod.sources) BY_FILE[file] = name;
}

export function driverOf(filename) {
  const base = String(filename || '').slice(String(filename || '').lastIndexOf('/') + 1);
  const name = BY_FILE[base];
  if (!name) return null;
  return { name, file: base, ...DRIVERS[name] };
}

export function pexOutName(filename) {
  const found = driverOf(filename);
  if (found) return found.name;
  const base = String(filename || '').slice(String(filename || '').lastIndexOf('/') + 1);
  return base.endsWith('.c') ? base.slice(0, -2) : base;
}

// A C file the user means as a PEX module: top-level *.c, pex/*.c, or
// sd/pex/*.c. Not lua/, staged/, or the rest of the card. The stem is the
// module name. Compiled output still goes to littlefs pex/.
export function pexCName(path) {
  if (!path || !path.endsWith('.c')) return null;
  if (path.startsWith('lua/') || path.startsWith('staged/')) return null;
  if (path.startsWith('sd/') && !/^sd\/pex\/[^/]+\.c$/.test(path)) return null;
  if (!path.startsWith('sd/') && path.includes('/') && !path.startsWith('pex/')) return null;
  const base = path.slice(path.lastIndexOf('/') + 1);
  if (!/^[A-Za-z0-9_-]+\.c$/.test(base)) return null;
  return base.slice(0, -2);
}
