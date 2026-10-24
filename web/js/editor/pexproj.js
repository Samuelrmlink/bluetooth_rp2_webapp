// Load a Pexfile from the open file's directory (or a parent) and build the
// target the page selected. The compiler is buildModule in pexbuild.js.
// This module does the card reads, the log, and the run step.

import {
  parsePexfile, parseBoard, findPexfile, findBoard, bindTarget, selectTarget,
  macroLines, prefixSource, artifactRel, logRel, joinProject, sdLoadPath,
  flashConfirm, ramDecision, layoutGuest, BuildLog, badRel,
  sdInstallCap, verifiedPexRel, verifiedNoteRel,
} from './pexfile.js';
import { badSdPath } from './pexbuilddlg.js';
import { imageOffOf, installMapNote } from './pexmap.js';

const SIGN = 'python3 pex/tools/pexlink.py sign';

async function walkFiles(list, root) {
  const out = [];
  const walk = async (dir, rel) => {
    const r = await list(dir);
    if (!r?.ok) throw new Error(r?.error || `could not list ${dir}`);
    for (const f of r.files || []) {
      const child = dir ? `${dir}/${f.name}` : f.name;
      const childRel = rel ? `${rel}/${f.name}` : f.name;
      if (f.dir) await walk(child, childRel);
      else out.push({ path: child, rel: childRel, name: f.name });
    }
  };
  await walk(root, '');
  return out;
}

/**
 * Find and parse the project that owns filePath, and bind every target's
 * board file. `readText` is the editor's fs.readText, or a buffer overlay.
 */
export async function loadProject({ filePath, readText, arch, board = null, pin = null }) {
  const found = await findPexfile(filePath, readText);
  if (!found) return { filePath, project: null, targets: [], selection: null, errors: [] };
  if (found.error) {
    return { filePath, project: null, targets: [], selection: null, errors: [found.error], pexPath: found.path };
  }
  const parsed = parsePexfile(found.text);
  const dir = found.path.split('/').slice(0, -1).join('/');
  const errors = parsed.errors.map((e) => (e.line ? `Pexfile:${e.line}: ${e.message}` : e.message));
  const cache = new Map();
  const targets = [];
  for (const t of parsed.project.targets) {
    let boardFile = null;
    if (t.board) {
      if (!cache.has(t.board)) {
        const got = await findBoard(dir, t.board, readText);
        if (!got) cache.set(t.board, { error: `board file ${t.board}.board was not found` });
        else if (got.error) cache.set(t.board, { error: got.error });
        else {
          const pb = parseBoard(got.text);
          if (!pb.ok) cache.set(t.board, { error: pb.errors.map((e) => e.message).join('; ') });
          else cache.set(t.board, { board: { ...pb.board, path: got.path } });
        }
      }
      const hit = cache.get(t.board);
      if (hit.error) errors.push(hit.error);
      else boardFile = hit.board;
    }
    const bound = bindTarget(t, boardFile, t.board ? null : arch);
    errors.push(...bound.errors);
    targets.push(bound);
  }
  const project = parsed.project.name
    ? { ...parsed.project, dir, pexPath: found.path, targets }
    : null;
  const unique = [...new Set(errors)];
  const selection = !project || unique.length
    ? { status: 'none', target: null, reason: unique[0] || 'No target' }
    : selectTarget(project, { arch, board, pin });
  return { filePath, project, targets, selection, errors: unique, dir, pexPath: found.path };
}

function headerDirs(target) {
  const dirs = new Set(target.inc || []);
  for (const rel of target.src || []) {
    const i = rel.lastIndexOf('/');
    if (i > 0) dirs.add(rel.slice(0, i));
  }
  return [...dirs];
}

function linkedOf(compiled) {
  if (compiled instanceof Uint8Array) return { bytes: compiled, map: '', dis: '', symbols: [] };
  if (compiled?.bytes instanceof Uint8Array) return compiled;
  throw new Error('compiler returned no module');
}

/** "name.arch.pex" at the end of an editor path. */
export function pexIdentity(path) {
  const base = String(path || '').split('/').pop();
  const m = /^([A-Za-z_][A-Za-z0-9_-]*)\.([A-Za-z0-9]+)\.pex$/.exec(base || '');
  return m ? { name: m[1], arch: m[2] } : null;
}

/**
 * Copy a module to sd/pex/good/ and write the note only after that copy is
 * complete. The previous verified file stays until the new bytes are on the
 * card under a sibling name.
 */
export async function markVerified({ name, arch, bytes, buildId = '', when, write, rename, remove }) {
  const base = verifiedPexRel(name, arch);
  const incoming = `${base}.incoming`;
  const prev = `${base}.prev`;
  const notePath = verifiedNoteRel(name, arch);
  const put = await write(incoming, new Blob([bytes]));
  if (!put?.ok) return { ok: false, error: put?.error || 'could not write the verified copy' };
  if (remove) await remove(prev);
  if (rename) {
    const mv = await rename(base, prev);
    if (!mv?.ok && mv?.status !== 404) {
      return { ok: false, error: mv?.error || 'could not move the previous verified file', incoming };
    }
    const fin = await rename(incoming, base);
    if (!fin?.ok) return { ok: false, error: fin?.error || 'verified copy stayed as .incoming', incoming };
    if (remove) await remove(prev);
  } else {
    const fin = await write(base, new Blob([bytes]));
    if (!fin?.ok) return { ok: false, error: fin?.error || 'could not write the verified file' };
    if (remove) await remove(incoming);
  }
  const note = `build ${buildId}\nsize ${bytes.length}\nwhen ${when}\n`;
  const n = await write(notePath, note);
  if (!n?.ok) return { ok: false, error: n?.error || 'note not written', file: base };
  return { ok: true, file: base, note: notePath };
}

async function waitSd(ws, call) {
  const done = watchEvent(ws, 'pex.sd_done', 60000);
  let reply;
  try { reply = await call(); }
  catch (e) { done.cancel(); throw e; }
  if (reply?.pending) {
    const ev = await done.promise;
    if (!ev?.ok) throw new Error(ev?.msg || ev?.error || 'install failed');
    return ev;
  }
  done.cancel();
  return reply;
}

async function installedOffset(ws, name) {
  try {
    const list = await ws.call('pex.installed');
    const hit = (list?.modules || []).find((m) => m?.name === name);
    return hit && typeof hit.offset === 'number' ? hit : null;
  } catch {
    return null;
  }
}

/**
 * Install only when the module is not loaded. A file larger than the pool
 * is copied to LittleFS and installed by name. A smaller file on the card
 * is installed from its sd: path, which stages the whole file in the pool.
 */
export async function installCandidate({ ws, write, name, arch, bytes, sdPath, cap }) {
  const internal = `pex/${name}.${arch}.pex`;
  if (!sdPath || bytes.length > cap) {
    const copy = await write(internal, new Blob([bytes]));
    if (!copy?.ok) throw new Error(copy?.error || `could not write ${internal}`);
    return ws.call('pex.install', { name });
  }
  await waitSd(ws, () => ws.call('pex.install', { path: sdPath, name }));
  const hit = await installedOffset(ws, name);
  return hit || { name };
}

async function recordOffset(write, stem, linked, inst) {
  const offset = typeof inst?.offset === 'number' ? inst.offset : null;
  if (offset == null || !linked?.map || !stem) return;
  const symbol = (linked.symbols || []).find((s) => s.name === 'pex_main') || (linked.symbols || [])[0] || null;
  const imageOff = linked.imageOff ?? imageOffOf(linked.bytes);
  const extra = installMapNote({ offset, imageOff, symbol });
  await write(`${stem}.map`, `${linked.map}${extra}`);
}

/**
 * Put the verified file back through the same unload-first gate.
 * `read` returns `{ ok, bytes }` for the verified .pex.
 */
export async function restoreVerified({
  name, arch, read, write, confirm = () => false, unload = null, ws = null,
  loaded = false, running = false, poolLargest = null,
}) {
  const path = verifiedPexRel(name, arch);
  const got = await read(path);
  if (!got?.ok) return { ok: false, error: got?.error || `${path} was not found` };
  const bytes = got.bytes instanceof Uint8Array ? got.bytes : new Uint8Array(got.bytes || []);
  const cap = sdInstallCap(poolLargest);
  const busy = loaded || running;
  if (!confirm(flashConfirm({
    name, arch, loaded: busy, bytes: bytes.length, poolCap: cap, sdPath: sdLoadPath(path) || '',
  }))) {
    return { ok: false, cancelled: true, error: busy ? `Left ${name} loaded.` : 'Restore cancelled.' };
  }
  if (busy) {
    if (!unload) return { ok: false, error: `cannot unload ${name}` };
    let u;
    try { u = await unload(name); }
    catch (e) { return { ok: false, error: e?.message || String(e) }; }
    if (!u?.unloaded) return { ok: false, error: u?.error || `Unload of ${name} failed.` };
  }
  if (!ws) return { ok: false, error: 'not connected' };
  const inst = await installCandidate({
    ws, write, name, arch, bytes, sdPath: sdLoadPath(path), cap,
  });
  return { ok: inst?.xip_ready !== false, installed: inst, file: path };
}

function watchEvent(ws, topic, timeoutMs) {
  let off = () => {};
  let timer = 0;
  let settle = () => {};
  const promise = new Promise((resolve, reject) => {
    settle = resolve;
    timer = setTimeout(() => { off(); reject(new Error(`${topic} timed out`)); }, timeoutMs);
    off = ws.on(topic, (data) => { clearTimeout(timer); off(); resolve(data); });
  });
  return { promise, cancel() { clearTimeout(timer); off(); settle(null); } };
}

/**
 * Build one target into its `out` on the card. `andRun` continues into
 * `run ram` or `run flash`. `compile` is buildModule. The log file is
 * replaced even when the compile fails.
 */
export async function executeTarget({
  project, target, andRun = false, active = null,
  readText, write, list, compile, confirm = () => false, ws = null,
  poolLargest = null, policy = '', running = false, loaded = false,
  unload = null,
  now = new Date(), onLine = () => {},
}) {
  const log = new BuildLog(target.level || project.level || 'info');
  let errorLine = '';
  const note = (level, text) => {
    if (!log.add(level, text)) return;
    if (level === 'error') errorLine = String(text).split('\n').filter(Boolean).pop() || errorLine;
    onLine(level, text);
  };
  const started = Date.now();
  const dir = project.dir;
  const artifact = joinProject(dir, artifactRel(project, target));
  const logPath = joinProject(dir, logRel(target));
  note('info', `Build ${project.name} · ${target.name}  ${now.toISOString()}`);
  note('info', `Project ${project.pexPath}`);
  note('info', `Target ${target.name}  arch ${target.arch || '?'}  board ${target.board || '—'}  run ${target.run}  out ${target.out}`);

  const finish = async (ok) => {
    const sec = ((Date.now() - started) / 1000).toFixed(1);
    note('info', ok ? `Finished in ${sec}s.` : `Stopped after ${sec}s.`);
    const body = log.text();
    if (logPath.startsWith('sd/')) {
      const why = badSdPath(logPath);
      if (why) note('error', `Build log not written: ${why}`);
      else {
        const w = await write(logPath, body);
        if (!w?.ok) note('error', `Build log not written: ${w?.error || logPath}`);
      }
    }
    return { ok, error: errorLine, log: log.text(), last: log.last(), artifact, logPath };
  };

  try {
    if (target.arch !== 'm33') {
      note('error', `pexcc emits Cortex-M33 only. Target ${target.name} arch ${target.arch} was not compiled.`);
      return finish(false);
    }
    const lines = macroLines({
      arch: target.arch,
      boardName: target.board,
      boardDefines: target.boardFile?.define || [],
      targetDefines: target.define || [],
      header: !!target.boardFile?.header,
    });
    for (const line of lines) note('info', `${line.line}  (${line.from})`);

    const units = [];
    for (const rel of target.src) {
      const full = joinProject(dir, rel);
      let text;
      if (active && active.path === full) {
        text = active.text;
        if (active.dirty) note('warn', `${rel} is unsaved. Compiling the editor copy.`);
      } else {
        const r = await readText(full);
        if (!r?.ok) throw new Error(r?.error || `could not read ${full}`);
        text = r.text;
      }
      note('info', `source ${rel}`);
      units.push({ path: rel, text: prefixSource(text, lines) });
    }

    const headers = [];
    const includeSeen = new Map();
    for (const inc of headerDirs(target)) {
      const root = joinProject(dir, inc);
      let files = [];
      try { files = await walkFiles(list, root); }
      catch (e) { throw new Error(e.message || `could not list ${root}`); }
      for (const f of files) {
        if (!f.name.endsWith('.h')) continue;
        const projectRel = `${inc}/${f.rel}`.replace(/\/{2,}/g, '/');
        if (badRel(projectRel)) continue;
        const r = await readText(f.path);
        if (!r?.ok) throw new Error(r?.error || `could not read ${f.path}`);
        const prev = includeSeen.get(f.rel);
        if (prev && prev !== r.text) {
          throw new Error(`two headers would both be ${f.rel} on the include path`);
        }
        includeSeen.set(f.rel, r.text);
        headers.push({ path: projectRel, text: r.text, includeAs: f.rel });
        note('debug', `header ${projectRel}`);
      }
    }

    const boardHeaders = [];
    if (target.boardFile?.header) {
      const bdir = target.boardFile.path.split('/').slice(0, -1).join('/');
      const hp = joinProject(bdir, target.boardFile.header);
      const r = await readText(hp);
      if (!r?.ok) throw new Error(r?.error || `board header ${hp} was not found`);
      const base = target.boardFile.header.split('/').pop();
      boardHeaders.push({ name: 'board.h', text: r.text });
      if (base !== 'board.h') boardHeaders.push({ name: base, text: r.text });
      note('info', `board header ${hp}`);
    }

    const guest = layoutGuest({ units, headers, boardHeaders });
    note('debug', `pexcc /tmp/out.elf ${guest.inputs.join(' ')}`);
    const linked = linkedOf(await compile({
      name: project.name,
      version: project.version,
      kind: project.kind,
      model: project.model,
      arch: target.arch,
      units,
      inputs: guest.inputs,
      tree: guest.tree,
      include: guest.include,
      onStatus: (m) => note('debug', m),
    }));
    const bytes = linked.bytes;

    if (String(policy).includes('signed-only')) {
      note('warn', `Unsigned module. A signed-only board refuses it until ${SIGN}`);
    }
    if (artifact.startsWith('sd/')) {
      const why = badSdPath(artifact);
      if (why) throw new Error(`output ${artifact}: ${why}`);
    }
    const put = await write(artifact, new Blob([bytes]));
    if (!put?.ok) throw new Error(put?.error || `could not write ${artifact}`);
    note('info', `Saved ${artifact} (${bytes.length} bytes).`);
    const stem = artifact.replace(/\.pex$/, '');
    if (linked.map) {
      const mw = await write(`${stem}.map`, linked.map);
      if (!mw?.ok) note('warn', mw?.error || `could not write ${stem}.map`);
      else note('info', `Saved ${stem}.map`);
    }
    if (linked.dis) {
      const dw = await write(`${stem}.dis`, linked.dis);
      if (!dw?.ok) note('warn', dw?.error || `could not write ${stem}.dis`);
      else note('info', `Saved ${stem}.dis`);
    }

    if (!andRun) return finish(true);

    if (target.run === 'ram') {
      const gate = ramDecision({ bytes: bytes.length, poolLargest });
      if (!gate.ok) {
        note('warn', gate.message);
        return finish(false);
      }
      if (running || loaded) {
        note('error', `${project.name} is already ${running ? 'running' : 'loaded'}. A RAM run does not start a second copy.`);
        return finish(false);
      }
      const path = sdLoadPath(artifact);
      if (!path) throw new Error('a RAM load reads the module from the SD card');
      if (!ws) throw new Error('not connected');
      note('info', `Loading ${path} into RAM.`);
      const done = watchEvent(ws, 'pex.sd_done', 30000);
      let reply;
      try {
        reply = await ws.call('pex.load', { path, name: project.name });
      } catch (e) {
        done.cancel();
        throw e;
      }
      if (reply?.pending) {
        const ev = await done.promise;
        if (!ev?.ok) throw new Error(ev?.msg || ev?.error || 'RAM load failed');
      } else done.cancel();
      await ws.call('pex.start', { name: project.name });
      note('info', `Started ${project.name} from RAM.`);
      return finish(true);
    }

    const cap = sdInstallCap(poolLargest);
    const sdPath = sdLoadPath(artifact);
    const busy = running || loaded;
    const ok = confirm(flashConfirm({
      name: project.name, arch: target.arch, loaded: busy,
      bytes: bytes.length, poolCap: cap, sdPath: sdPath || '',
    }));
    if (!ok) {
      note('info', busy
        ? `Left ${project.name} loaded. ${artifact} is on the card.`
        : `Install cancelled. ${artifact} is on the card.`);
      return finish(true);
    }
    if (busy) {
      if (!unload) throw new Error(`cannot unload ${project.name}`);
      let u;
      try { u = await unload(project.name); }
      catch (e) {
        note('error', e?.message || String(e));
        note('info', `${artifact} is on the card.`);
        return finish(false);
      }
      if (!u?.unloaded) {
        note('error', u?.error || `Unload of ${project.name} failed.`);
        note('info', `The module was left in place. ${artifact} is on the card.`);
        return finish(false);
      }
      note('info', `Unloaded ${project.name}.`);
    }
    if (!ws) throw new Error('not connected');
    const inst = await installCandidate({
      ws, write, name: project.name, arch: target.arch, bytes, sdPath, cap,
    });
    if (inst?.xip_ready === false) {
      note('warn', `Installed ${project.name}, but it is not ready to run from flash. Not started.`);
      await recordOffset(write, stem, linked, inst);
      return finish(false);
    }
    note('info', `Installed ${project.name}.`);
    await recordOffset(write, stem, linked, inst);
    await ws.call('pex.load', { name: project.name });
    await ws.call('pex.start', { name: project.name });
    note('info', `Started ${project.name}.`);
    return finish(true);
  } catch (e) {
    const msg = e?.message || String(e);
    note('error', msg);
    if (String(policy).includes('signed-only') && /sign|unsigned|policy/i.test(msg)) {
      note('warn', SIGN);
    }
    return finish(false);
  }
}
