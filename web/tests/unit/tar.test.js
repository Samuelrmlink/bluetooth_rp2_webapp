import { test } from 'node:test';
import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import { mkdtempSync, mkdirSync, writeFileSync, chmodSync, symlinkSync, readFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { gzipSync } from 'node:zlib';
import { parseTar, planRestore, badPath, foldersOf, FILE_MAX } from '../../js/tar.js';

// A folder the way a Linux PC leaves one: odd permissions, a script marked
// executable, a private file, an empty folder, a symlink, a compressed file
// carried as is, and names long enough to need GNU / pax long-name records.
const LONG = 'a-module-name-long-enough-to-overflow-the-one-hundred-byte-ustar-name-field-on-its-own.lua';
function sampleTree() {
  const root = mkdtempSync(join(tmpdir(), 'btgw-tar-'));
  const lua = join(root, 'lua');
  mkdirSync(join(lua, 'profiles', 'deep', 'er'), { recursive: true });
  mkdirSync(join(lua, 'empty'));
  writeFileSync(join(lua, 'run.lua'), 'print("hi")\n');
  chmodSync(join(lua, 'run.lua'), 0o755);
  writeFileSync(join(lua, 'secret.lua'), 'return 1\n');
  chmodSync(join(lua, 'secret.lua'), 0o600);
  writeFileSync(join(lua, 'profiles', 'deep', 'er', LONG), 'return {}\n');
  writeFileSync(join(lua, 'mod.lua.gz'), gzipSync('return 42\n'));
  symlinkSync('run.lua', join(lua, 'link.lua'));
  chmodSync(join(lua, 'profiles'), 0o700);
  return root;
}

function tarWith(args, root) {
  return new Uint8Array(execFileSync(args[0], [...args.slice(1), '-cf', '-', '-C', root, 'lua'], { maxBuffer: 1 << 24 }));
}

const byPath = (r) => Object.fromEntries(r.entries.map((e) => [e.path, e]));

for (const [label, args] of [
  ['GNU tar (gnu format)', ['tar', '--format=gnu']],
  ['GNU tar (posix / pax)', ['tar', '--format=posix']],
  ['GNU tar (default)', ['tar']],
  ['bsdtar', ['bsdtar']],
]) {
  test(`an archive from ${label}: files, folders, long names; permissions ignored, links skipped`, (t) => {
    let bytes;
    const root = sampleTree();
    try { bytes = tarWith(args, root); } catch { t.skip(`${args[0]} not available`); return; }
    finally { /* the bytes are all we need */ }
    try {
      const r = parseTar(bytes);
      const m = byPath(r);
      assert.equal(m['lua'].dir, true);
      assert.equal(m['lua/empty'].dir, true);
      assert.equal(new TextDecoder().decode(m['lua/run.lua'].data), 'print("hi")\n');
      assert.equal(new TextDecoder().decode(m['lua/secret.lua'].data), 'return 1\n');   // 0600: read all the same
      assert.equal(new TextDecoder().decode(m[`lua/profiles/deep/er/${LONG}`].data), 'return {}\n');
      // the .gz is carried exactly as stored, not unpacked
      assert.deepEqual([...m['lua/mod.lua.gz'].data], [...readFileSync(join(root, 'lua', 'mod.lua.gz'))]);
      assert.deepEqual(r.skipped, [{ path: 'lua/link.lua', why: 'symlink (not a file)' }]);
      for (const e of r.entries) assert.deepEqual(Object.keys(e).sort(), e.dir ? ['dir', 'path'] : ['data', 'dir', 'path']);
    } finally {
      rmSync(root, { recursive: true, force: true });
    }
  });
}

test('the device\'s own archive shape round-trips (Python tarfile, ustar)', (t) => {
  let bytes;
  try {
    bytes = execFileSync('python3', ['-c', `
import io, sys, tarfile
b = io.BytesIO()
with tarfile.open(fileobj=b, mode='w', format=tarfile.USTAR_FORMAT) as t:
    d = tarfile.TarInfo('js'); d.type = tarfile.DIRTYPE; t.addfile(d)
    for n, data in [('js/app.js.gz', b'\\x1f\\x8b' + bytes(300)), ('js/a' + '/b' * 60 + '.js', b'x')]:
        i = tarfile.TarInfo(n); i.size = len(data); i.mode = 0o644; t.addfile(i, io.BytesIO(data))
sys.stdout.buffer.write(b.getvalue())`]);
  } catch { t.skip('python3 not available'); return; }
  const r = parseTar(new Uint8Array(bytes));
  assert.deepEqual(r.entries.map((e) => e.path), ['js', 'js/app.js.gz', 'js/a' + '/b'.repeat(60) + '.js']);
  assert.equal(r.entries[1].data.length, 302);
});

test('not a tar, or cut short, is an error rather than a partial restore', () => {
  assert.throws(() => parseTar(new TextEncoder().encode('hello'.repeat(200))), /not a tar/);
  const root = sampleTree();
  try {
    const full = tarWith(['tar', '--format=gnu'], root);
    const r = parseTar(full);
    const big = r.entries.find((e) => e.path === 'lua/run.lua');
    assert.ok(big);
    // cut in the middle of a data block
    assert.throws(() => parseTar(full.subarray(0, 512 + 100)), /ends in the middle|checksum|incomplete/);
  } finally {
    rmSync(root, { recursive: true, force: true });
  }
});

test('a damaged header is reported', () => {
  const root = sampleTree();
  try {
    const b = tarWith(['tar', '--format=gnu'], root).slice();
    b[512 * 3 + 10] ^= 0xff;                       // somewhere in a later header or data
    // either it hit data (still parses) or a header (throws) -- never garbage paths
    try { for (const e of parseTar(b).entries) assert.equal(badPath(e.path) === null || e.path.includes('�'), true); }
    catch (e) { assert.match(e.message, /checksum|ends|incomplete/); }
  } finally {
    rmSync(root, { recursive: true, force: true });
  }
});

test('badPath: no escape from the target, no absolute paths, the device\'s length', () => {
  assert.equal(badPath('lua/x.lua'), null);
  assert.match(badPath('../etc/passwd'), /outside/);
  assert.match(badPath('lua/../../x'), /outside/);
  assert.match(badPath('/etc/passwd'), /absolute/);
  assert.match(badPath('a//b'), /empty/);
  assert.match(badPath('x'.repeat(128)), /longer/);
  assert.equal(badPath('x'.repeat(127)), null);
});

test('planRestore: adds vs overwrites, empty folders, cache/ and oversize skipped, nothing deleted', () => {
  const f = (path, n = 3) => ({ path, dir: false, data: new Uint8Array(n) });
  const parsed = {
    entries: [
      { path: 'lua', dir: true }, f('lua/a.lua'), f('lua/b.lua'), { path: 'lua/empty', dir: true },
      { path: 'lua/e2', dir: true }, { path: 'lua/e2/inner', dir: true },
      f('cache/lua/a.luac'), f('lua/huge.lua', FILE_MAX + 1), f('../evil'), f('lua/a.lua', 5),
    ],
    skipped: [{ path: 'lua/link', why: 'symlink (not a file)' }],
  };
  const p = planRestore(parsed, '', new Set(['lua/a.lua', 'lua/other.lua']));
  assert.deepEqual(p.adds.map((w) => w.path), ['lua/b.lua']);
  assert.deepEqual(p.overwrites.map((w) => w.path), ['lua/a.lua']);
  assert.equal(p.overwrites[0].data.length, 5);           // the later copy wins
  assert.deepEqual(p.mkdirs, ['lua/e2/inner', 'lua/empty']);
  assert.deepEqual(p.skipped.map((s) => s.path).sort(), ['../evil', 'cache/lua/a.luac', 'lua/huge.lua', 'lua/link']);
  // into a folder: everything lands under it
  const q = planRestore(parsed, 'scratch', new Set());
  assert.deepEqual(q.adds.map((w) => w.path).sort(), ['scratch/cache/lua/a.luac', 'scratch/lua/a.lua', 'scratch/lua/b.lua']);
  assert.ok(!q.skipped.some((s) => s.path === 'cache/lua/a.luac'));   // scratch/cache/... is just a folder
  assert.deepEqual(foldersOf(parsed, 'scratch'), ['scratch/cache/lua', 'scratch/lua']);
});
